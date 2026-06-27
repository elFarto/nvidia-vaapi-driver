/*
 * nvenc-helper: 64-bit NVENC encode helper daemon.
 *
 * This standalone process runs as 64-bit, where CUDA works on all GPUs.
 * It receives raw NV12/P010 frames from the VA-API driver via
 * a Unix domain socket, encodes them with NVENC, and returns the
 * encoded bitstream.
 *
 * Usage: nvenc-helper [--foreground]
 * The socket is created at $XDG_RUNTIME_DIR/nvenc-helper.sock
 *
 * The helper runs persistently until stopped via SIGTERM/SIGINT.
 * It is managed by a systemd user service (nvenc-helper.service).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/mman.h>

#include <ffnvcodec/dynlink_loader.h>
#include <ffnvcodec/nvEncodeAPI.h>
#include "nvenc-ipc.h"

static CudaFunctions *cu;
static NvencFunctions *nv_dl;
static volatile sig_atomic_t running = 1;
static int log_enabled = 0;

/* Force an IDR keyframe every N frames for streaming error recovery.
 * At 60fps this is ~1 second. At 30fps this is ~2 seconds. */
#define NVENC_HELPER_IDR_INTERVAL 60

static inline bool check_cuda_helper(CUresult err, const char *func, int line) {
    if (err != CUDA_SUCCESS) {
        const char *s = NULL;
        cu->cuGetErrorString(err, &s);
        fprintf(stderr, "[nvenc-helper] CUDA error: %s (%d) at %s:%d\n",
                s ? s : "?", err, func, line);
        return true;
    }
    return false;
}
#define CHECK_CUDA_RESULT_HELPER(err) check_cuda_helper(err, __func__, __LINE__)

static void helper_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void helper_log(const char *fmt, ...) {
    if (!log_enabled) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[nvenc-helper %ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}
#define HELPER_LOG helper_log

/* Per-client encoder state */
typedef struct {
    CUcontext                   cudaCtx;
    void                       *encoder;
    NV_ENCODE_API_FUNCTION_LIST funcs;
    bool                        initialized;
    NV_ENC_INPUT_PTR            inputBuffer;   /* NVENC-managed (fallback) */
    NV_ENC_OUTPUT_PTR           outputBuffer;
    /* Persistent CUDA buffer for GPU-side encode (avoids nvEncLockInputBuffer) */
    CUdeviceptr                 gpuBuf;        /* Linear CUDA VRAM buffer */
    uint32_t                    gpuBufPitch;   /* Aligned pitch */
    uint32_t                    gpuBufSize;    /* Total allocation size */
    NV_ENC_REGISTERED_PTR       gpuBufReg;     /* Persistent NVENC registration */
    bool                        gpuBufReady;   /* true if GPU path available */
    uint32_t                    width;
    uint32_t                    height;
    uint32_t                    is10bit;
    uint64_t                    frameCount;
    uint8_t                    *bsBuf;         /* pre-allocated bitstream output */
    uint32_t                    bsBufSize;
} HelperEncoder;

/* Reliable I/O */
static bool send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool recv_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool send_response(int fd, int32_t status, const void *data, uint32_t size)
{
    NVEncIPCRespHeader resp = { .status = status, .payload_size = size };
    if (!send_all(fd, &resp, sizeof(resp))) return false;
    if (size > 0 && data != NULL) {
        if (!send_all(fd, data, size)) return false;
    }
    return true;
}

/* Send response header with an fd attached via SCM_RIGHTS */
static bool send_response_with_fd(int sock, int32_t status, int send_fd,
                                   const void *data, uint32_t size)
{
    NVEncIPCRespHeader resp = { .status = status, .payload_size = size };

    struct iovec iov = { .iov_base = &resp, .iov_len = sizeof(resp) };
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_buf;
    memset(&cmsg_buf, 0, sizeof(cmsg_buf));

    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg_buf.buf,
        .msg_controllen = sizeof(cmsg_buf.buf),
    };

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &send_fd, sizeof(int));

    ssize_t n = sendmsg(sock, &msg, MSG_NOSIGNAL);
    if (n != sizeof(resp)) return false;

    if (size > 0 && data != NULL) {
        if (!send_all(sock, data, size)) return false;
    }
    return true;
}

/* Encoder lifecycle */
static bool encoder_init(HelperEncoder *enc, const NVEncIPCInitParams *params)
{
    HELPER_LOG("Init: %ux%u codec=%u profile=%u bitrate=%u",
               params->width, params->height, params->codec, params->profile,
               params->bitrate);

    /* Create CUDA context */
    if (CHECK_CUDA_RESULT_HELPER(cu->cuCtxCreate(&enc->cudaCtx, 0, 0))) {
        return false;
    }

    /* Get NVENC function list */
    enc->funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS st = nv_dl->NvEncodeAPICreateInstance(&enc->funcs);
    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("NvEncodeAPICreateInstance failed: %d", st);
        cu->cuCtxDestroy(enc->cudaCtx);
        return false;
    }

    /* Open NVENC session */
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessParams = {0};
    sessParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sessParams.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    sessParams.device = enc->cudaCtx;
    sessParams.apiVersion = NVENCAPI_VERSION;

    st = enc->funcs.nvEncOpenEncodeSessionEx(&sessParams, &enc->encoder);
    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("nvEncOpenEncodeSessionEx failed: %d", st);
        cu->cuCtxDestroy(enc->cudaCtx);
        return false;
    }

    /* Select codec and profile GUIDs */
    GUID codecGuid = (params->codec == 0) ? NV_ENC_CODEC_H264_GUID : NV_ENC_CODEC_HEVC_GUID;
    GUID profileGuid;
    if (params->codec == 0) {
        /* H.264 */
        profileGuid = NV_ENC_H264_PROFILE_HIGH_GUID;
    } else {
        /* HEVC */
        profileGuid = params->is10bit ? NV_ENC_HEVC_PROFILE_MAIN10_GUID : NV_ENC_HEVC_PROFILE_MAIN_GUID;
    }

    /* Get preset config */
    NV_ENC_PRESET_CONFIG presetConfig = {0};
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

    st = enc->funcs.nvEncGetEncodePresetConfigEx(
        enc->encoder, codecGuid, NV_ENC_PRESET_P4_GUID,
        NV_ENC_TUNING_INFO_LOW_LATENCY, &presetConfig);
    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("nvEncGetEncodePresetConfigEx failed: %d", st);
        goto fail;
    }

    NV_ENC_CONFIG encConfig;
    memcpy(&encConfig, &presetConfig.presetCfg, sizeof(encConfig));
    encConfig.version = NV_ENC_CONFIG_VER;
    encConfig.profileGUID = profileGuid;
    encConfig.frameIntervalP = 1; /* No B-frames for synchronous encode */

    if (params->bitrate > 0) {
        encConfig.rcParams.averageBitRate = params->bitrate;
    }
    if (params->maxBitrate > 0) {
        encConfig.rcParams.maxBitRate = params->maxBitrate;
    }
    if (params->gopLength > 0) {
        encConfig.gopLength = params->gopLength;
    }

    /* Initialize encoder */
    NV_ENC_INITIALIZE_PARAMS initParams = {0};
    initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams.encodeGUID = codecGuid;
    initParams.presetGUID = NV_ENC_PRESET_P4_GUID;
    initParams.encodeWidth = params->width;
    initParams.encodeHeight = params->height;
    initParams.darWidth = params->width;
    initParams.darHeight = params->height;
    initParams.frameRateNum = params->frameRateNum > 0 ? params->frameRateNum : 30;
    initParams.frameRateDen = params->frameRateDen > 0 ? params->frameRateDen : 1;
    initParams.enablePTD = 1;
    initParams.encodeConfig = &encConfig;
    initParams.maxEncodeWidth = params->width;
    initParams.maxEncodeHeight = params->height;
    initParams.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;

    st = enc->funcs.nvEncInitializeEncoder(enc->encoder, &initParams);
    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("nvEncInitializeEncoder failed: %d", st);
        goto fail;
    }

    /* Create NVENC-managed input buffer */
    NV_ENC_CREATE_INPUT_BUFFER createIn = {0};
    createIn.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
    createIn.width = params->width;
    createIn.height = params->height;
    createIn.bufferFmt = params->is10bit ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;

    st = enc->funcs.nvEncCreateInputBuffer(enc->encoder, &createIn);
    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("nvEncCreateInputBuffer failed: %d", st);
        goto fail;
    }
    enc->inputBuffer = createIn.inputBuffer;

    /* Create output bitstream buffer */
    NV_ENC_CREATE_BITSTREAM_BUFFER createOut = {0};
    createOut.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

    st = enc->funcs.nvEncCreateBitstreamBuffer(enc->encoder, &createOut);
    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("nvEncCreateBitstreamBuffer failed: %d", st);
        enc->funcs.nvEncDestroyInputBuffer(enc->encoder, enc->inputBuffer);
        goto fail;
    }
    enc->outputBuffer = createOut.bitstreamBuffer;

    enc->width = params->width;
    enc->height = params->height;
    enc->is10bit = params->is10bit;
    enc->frameCount = 0;
    enc->bsBufSize = 4 * 1024 * 1024;
    enc->bsBuf = malloc(enc->bsBufSize);
    enc->initialized = true;

    /* Allocate persistent CUDA linear buffer for GPU-side encode.
     * This replaces nvEncLockInputBuffer (host memory) with a CUDA device
     * buffer registered once with NVENC. Per-frame: single cuMemcpy2D
     * (host→device with pitch conversion) + nvEncMapInputResource. */
    uint32_t bpp = params->is10bit ? 2 : 1;
    enc->gpuBufPitch = params->width * bpp;
    enc->gpuBufPitch = (enc->gpuBufPitch + 255) & ~255; /* Align to 256 */
    enc->gpuBufSize = enc->gpuBufPitch * params->height * 3 / 2;
    enc->gpuBufReady = false;

    CUresult cres = cu->cuMemAlloc(&enc->gpuBuf, enc->gpuBufSize);
    if (cres == CUDA_SUCCESS) {
        NV_ENC_BUFFER_FORMAT bufFmt = params->is10bit
            ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;

        NV_ENC_REGISTER_RESOURCE regRes = {0};
        regRes.version = NV_ENC_REGISTER_RESOURCE_VER;
        regRes.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
        regRes.resourceToRegister = (void *)enc->gpuBuf;
        regRes.width = params->width;
        regRes.height = params->height;
        regRes.pitch = enc->gpuBufPitch;
        regRes.bufferFormat = bufFmt;
        regRes.bufferUsage = NV_ENC_INPUT_IMAGE;

        st = enc->funcs.nvEncRegisterResource(enc->encoder, &regRes);
        if (st == NV_ENC_SUCCESS) {
            enc->gpuBufReg = regRes.registeredResource;
            enc->gpuBufReady = true;
            HELPER_LOG("GPU buffer: %u bytes, pitch=%u (persistent CUDA+NVENC)",
                       enc->gpuBufSize, enc->gpuBufPitch);
        } else {
            HELPER_LOG("GPU buffer register failed (%d), falling back to host path", st);
            cu->cuMemFree(enc->gpuBuf);
            enc->gpuBuf = 0;
        }
    } else {
        HELPER_LOG("GPU buffer alloc failed (%d), falling back to host path", cres);
        enc->gpuBuf = 0;
    }

    HELPER_LOG("Encoder initialized: %ux%u %s %s (gpu=%s)",
               params->width, params->height,
               params->codec == 0 ? "H.264" : "HEVC",
               params->is10bit ? "10-bit" : "8-bit",
               enc->gpuBufReady ? "yes" : "no");
    return true;

fail:
    enc->funcs.nvEncDestroyEncoder(enc->encoder);
    enc->encoder = NULL;
    cu->cuCtxDestroy(enc->cudaCtx);
    enc->cudaCtx = NULL;
    return false;
}

static bool encoder_encode(HelperEncoder *enc, const void *frame_data,
                           uint32_t frame_width, uint32_t frame_height,
                           uint32_t frame_size, bool force_idr,
                           void **out_data, uint32_t *out_size)
{
    NVENCSTATUS st;
    uint32_t bpp = enc->is10bit ? 2 : 1;
    uint32_t srcPitch = frame_width * bpp;
    NV_ENC_INPUT_PTR encodeInput;
    NV_ENC_BUFFER_FORMAT encFmt = enc->is10bit
        ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
    uint32_t encodePitch;
    bool usedGpuPath = false;

    if (enc->gpuBufReady) {
        /* GPU FAST PATH: cuMemcpy2D host→device with pitch conversion.
         * Single CUDA call replaces 1080+ individual memcpy calls.
         * GPU DMA engine handles pitch conversion in hardware.
         * NVENC reads from VRAM — no PCIe upload at encode time. */
        uint32_t padLines = enc->height - frame_height;

        /* Luma: host SHM → GPU buffer */
        CUDA_MEMCPY2D cpyLuma = {0};
        cpyLuma.srcMemoryType = CU_MEMORYTYPE_HOST;
        cpyLuma.srcHost = frame_data;
        cpyLuma.srcPitch = srcPitch;
        cpyLuma.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cpyLuma.dstDevice = enc->gpuBuf;
        cpyLuma.dstPitch = enc->gpuBufPitch;
        cpyLuma.WidthInBytes = srcPitch;
        cpyLuma.Height = frame_height;

        CUresult cres = cu->cuMemcpy2D(&cpyLuma);
        if (cres != CUDA_SUCCESS) {
            HELPER_LOG("GPU path: luma cuMemcpy2D failed: %d, falling back", cres);
            goto host_fallback;
        }

        /* Chroma: host SHM → GPU buffer */
        uint32_t chromaOff_src = srcPitch * frame_height;
        uint32_t chromaOff_dst = enc->gpuBufPitch * enc->height;
        uint32_t chromaHeight = frame_height / 2;

        CUDA_MEMCPY2D cpyChroma = {0};
        cpyChroma.srcMemoryType = CU_MEMORYTYPE_HOST;
        cpyChroma.srcHost = (const uint8_t *)frame_data + chromaOff_src;
        cpyChroma.srcPitch = srcPitch;
        cpyChroma.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cpyChroma.dstDevice = enc->gpuBuf + chromaOff_dst;
        cpyChroma.dstPitch = enc->gpuBufPitch;
        cpyChroma.WidthInBytes = srcPitch;
        cpyChroma.Height = chromaHeight;

        cres = cu->cuMemcpy2D(&cpyChroma);
        if (cres != CUDA_SUCCESS) {
            HELPER_LOG("GPU path: chroma cuMemcpy2D failed: %d, falling back", cres);
            goto host_fallback;
        }

        /* Zero padding rows on GPU (async, only if needed) */
        if (padLines > 0) {
            cu->cuMemsetD8Async(enc->gpuBuf + enc->gpuBufPitch * frame_height,
                                0, enc->gpuBufPitch * padLines, 0);
            cu->cuMemsetD8Async(enc->gpuBuf + chromaOff_dst + enc->gpuBufPitch * chromaHeight,
                                128, enc->gpuBufPitch * (padLines / 2), 0);
        }

        /* Map the persistent registered resource */
        NV_ENC_MAP_INPUT_RESOURCE mapRes = {0};
        mapRes.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mapRes.registeredResource = enc->gpuBufReg;

        st = enc->funcs.nvEncMapInputResource(enc->encoder, &mapRes);
        if (st != NV_ENC_SUCCESS) {
            HELPER_LOG("GPU path: nvEncMapInputResource failed: %d, falling back", st);
            goto host_fallback;
        }

        encodeInput = mapRes.mappedResource;
        encFmt = mapRes.mappedBufferFmt;
        encodePitch = enc->gpuBufPitch;
        usedGpuPath = true;
        goto do_encode;
    }

host_fallback:
    /* HOST FALLBACK: nvEncLockInputBuffer + memcpy (original path) */
    {
        NV_ENC_LOCK_INPUT_BUFFER lockIn = {0};
        lockIn.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
        lockIn.inputBuffer = enc->inputBuffer;

        st = enc->funcs.nvEncLockInputBuffer(enc->encoder, &lockIn);
        if (st != NV_ENC_SUCCESS) {
            HELPER_LOG("nvEncLockInputBuffer failed: %d", st);
            return false;
        }

        uint32_t dstPitch = lockIn.pitch;
        uint8_t *src = (uint8_t *)frame_data;
        uint8_t *dst = (uint8_t *)lockIn.bufferDataPtr;
        uint32_t chromaOffset_src = srcPitch * frame_height;
        uint32_t chromaOffset_dst = dstPitch * enc->height;
        uint32_t chromaHeight = frame_height / 2;
        uint32_t padLines = enc->height - frame_height;

        if (srcPitch == dstPitch) {
            memcpy(dst, src, srcPitch * frame_height);
            memcpy(dst + chromaOffset_dst, src + chromaOffset_src, srcPitch * chromaHeight);
        } else {
            for (uint32_t y = 0; y < frame_height; y++)
                memcpy(dst + y * dstPitch, src + y * srcPitch, srcPitch);
            for (uint32_t y = 0; y < chromaHeight; y++)
                memcpy(dst + chromaOffset_dst + y * dstPitch,
                       src + chromaOffset_src + y * srcPitch, srcPitch);
        }

        if (padLines > 0) {
            memset(dst + dstPitch * frame_height, 0, dstPitch * padLines);
            memset(dst + chromaOffset_dst + dstPitch * chromaHeight, 128, dstPitch * (padLines / 2));
        }

        enc->funcs.nvEncUnlockInputBuffer(enc->encoder, enc->inputBuffer);
        encodeInput = enc->inputBuffer;
        encodePitch = dstPitch;
    }

do_encode:;
    /* Encode */
    NV_ENC_PIC_PARAMS picParams = {0};
    picParams.version = NV_ENC_PIC_PARAMS_VER;
    picParams.inputBuffer = encodeInput;
    picParams.bufferFmt = encFmt;
    picParams.inputWidth = enc->width;
    picParams.inputHeight = enc->height;
    picParams.inputPitch = encodePitch;
    picParams.outputBitstream = enc->outputBuffer;
    picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picParams.pictureType = NV_ENC_PIC_TYPE_UNKNOWN;
    /* Force IDR: on first frame, on explicit request, or every 60 frames
     * for streaming recovery. Without periodic IDR, a single lost packet
     * causes the client to freeze until the next intra_period (up to 60s). */
    bool needIDR = (enc->frameCount == 0) || force_idr || (enc->frameCount % NVENC_HELPER_IDR_INTERVAL == 0);
    picParams.encodePicFlags = needIDR
        ? (NV_ENC_PIC_FLAG_OUTPUT_SPSPPS | NV_ENC_PIC_FLAG_FORCEIDR)
        : 0;
    picParams.frameIdx = (uint32_t)enc->frameCount;
    picParams.inputTimeStamp = enc->frameCount;

    st = enc->funcs.nvEncEncodePicture(enc->encoder, &picParams);

    /* Unmap the GPU resource after encode (must happen before next map) */
    if (usedGpuPath) {
        enc->funcs.nvEncUnmapInputResource(enc->encoder, encodeInput);
    }

    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("nvEncEncodePicture failed: %d", st);
        return false;
    }

    enc->frameCount++;

    if (enc->frameCount % 300 == 0) {
        HELPER_LOG("Encoded %lu frames", (unsigned long)enc->frameCount);
    }

    /* Lock output bitstream */
    NV_ENC_LOCK_BITSTREAM lockOut = {0};
    lockOut.version = NV_ENC_LOCK_BITSTREAM_VER;
    lockOut.outputBitstream = enc->outputBuffer;

    st = enc->funcs.nvEncLockBitstream(enc->encoder, &lockOut);
    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("nvEncLockBitstream failed: %d", st);
        return false;
    }

    /* Copy bitstream data */
    *out_size = lockOut.bitstreamSizeInBytes;

    //grow pre-allocated buffer if needed
    if (lockOut.bitstreamSizeInBytes > enc->bsBufSize) {
        uint32_t newSize = lockOut.bitstreamSizeInBytes + (lockOut.bitstreamSizeInBytes >> 1);
        uint8_t *newBuf = realloc(enc->bsBuf, newSize);
        if (newBuf == NULL) {
            enc->funcs.nvEncUnlockBitstream(enc->encoder, enc->outputBuffer);
            return false;
        }
        enc->bsBuf = newBuf;
        enc->bsBufSize = newSize;
    }
    memcpy(enc->bsBuf, lockOut.bitstreamBufferPtr, lockOut.bitstreamSizeInBytes);
    *out_data = enc->bsBuf;

    enc->funcs.nvEncUnlockBitstream(enc->encoder, enc->outputBuffer);

    return true;
}

static void encoder_close(HelperEncoder *enc)
{
    if (enc->encoder == NULL) return;

    /* Flush */
    if (enc->initialized) {
        NV_ENC_PIC_PARAMS picParams = {0};
        picParams.version = NV_ENC_PIC_PARAMS_VER;
        picParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        enc->funcs.nvEncEncodePicture(enc->encoder, &picParams);
    }

    if (enc->outputBuffer) {
        enc->funcs.nvEncDestroyBitstreamBuffer(enc->encoder, enc->outputBuffer);
    }
    if (enc->inputBuffer) {
        enc->funcs.nvEncDestroyInputBuffer(enc->encoder, enc->inputBuffer);
    }
    /* Free persistent GPU buffer */
    if (enc->gpuBufReady) {
        enc->funcs.nvEncUnregisterResource(enc->encoder, enc->gpuBufReg);
        enc->gpuBufReady = false;
    }
    if (enc->gpuBuf) {
        cu->cuMemFree(enc->gpuBuf);
        enc->gpuBuf = 0;
    }

    enc->funcs.nvEncDestroyEncoder(enc->encoder);
    enc->encoder = NULL;

    if (enc->cudaCtx) {
        cu->cuCtxDestroy(enc->cudaCtx);
        enc->cudaCtx = NULL;
    }

    free(enc->bsBuf);
    enc->bsBuf = NULL;
    enc->bsBufSize = 0;
    enc->initialized = false;
    HELPER_LOG("Encoder closed (encoded %lu frames)", (unsigned long)enc->frameCount);
}

/* Handle one client connection */
static void handle_client(int client_fd)
{
    HelperEncoder enc = {0};
    void *shm_ptr = MAP_FAILED;
    uint32_t shm_size = 0;
    int shm_fd = -1;

    HELPER_LOG("Client connected (fd=%d)", client_fd);

    while (running) {
        //wait for data with 5s timeout (detect dead clients)
        struct pollfd cpfd = { .fd = client_fd, .events = POLLIN };
        int pr = poll(&cpfd, 1, 5000);
        if (pr == 0) {
            HELPER_LOG("Client timeout (5s), disconnecting");
            break;
        }
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        NVEncIPCMsgHeader hdr;
        if (!recv_all(client_fd, &hdr, sizeof(hdr))) {
            HELPER_LOG("Client disconnected");
            break;
        }

        switch (hdr.cmd) {
        case NVENC_IPC_CMD_INIT: {
            if (hdr.payload_size != sizeof(NVEncIPCInitParams)) {
                send_response(client_fd, -1, NULL, 0);
                break;
            }
            NVEncIPCInitParams params;
            if (!recv_all(client_fd, &params, sizeof(params))) goto done;

            if (enc.initialized) {
                encoder_close(&enc);
            }

            /* Clean up old shm if any */
            if (shm_ptr != MAP_FAILED) {
                munmap(shm_ptr, shm_size);
                shm_ptr = MAP_FAILED;
            }
            if (shm_fd >= 0) {
                close(shm_fd);
                shm_fd = -1;
            }

            bool ok = encoder_init(&enc, &params);
            if (!ok) {
                send_response(client_fd, -1, NULL, 0);
                break;
            }

            /* Create shared memory for frame transfer.
             * NV12 = w*h*1.5, P010 = w*h*3 */
            uint32_t bpp = params.is10bit ? 2 : 1;
            shm_size = params.width * bpp * params.height * 3 / 2;
            shm_fd = memfd_create("nvenc-frame", MFD_CLOEXEC);
            if (shm_fd < 0 || ftruncate(shm_fd, shm_size) < 0) {
                HELPER_LOG("Failed to create shm: %s", strerror(errno));
                if (shm_fd >= 0) { close(shm_fd); shm_fd = -1; }
                /* Fall back to socket-based transfer (no shm).
                 * Send normal response without fd (no SCM_RIGHTS with fd=-1). */
                NVEncIPCInitResponse iresp = { .shm_size = 0 };
                send_response(client_fd, 0, &iresp, sizeof(iresp));
                break;
            }

            shm_ptr = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
            if (shm_ptr == MAP_FAILED) {
                HELPER_LOG("Failed to mmap shm: %s", strerror(errno));
                close(shm_fd);
                shm_fd = -1;
                NVEncIPCInitResponse iresp = { .shm_size = 0 };
                send_response(client_fd, 0, &iresp, sizeof(iresp));
                break;
            }

            /* Send shm fd to client */
            int client_shm_fd = dup(shm_fd); /* dup because SCM_RIGHTS transfers ownership */
            NVEncIPCInitResponse iresp = { .shm_size = shm_size };
            HELPER_LOG("Created shm: %u bytes, fd=%d", shm_size, client_shm_fd);
            send_response_with_fd(client_fd, 0, client_shm_fd, &iresp, sizeof(iresp));
            close(client_shm_fd);
            break;
        }

        case NVENC_IPC_CMD_ENCODE: {
            if (!enc.initialized || hdr.payload_size > NVENC_IPC_MAX_FRAME_SIZE + sizeof(NVEncIPCEncodeParams)) {
                /* Drain the payload with a fixed buffer to avoid huge malloc */
                char drain[4096];
                uint32_t remaining = hdr.payload_size;
                while (remaining > 0) {
                    uint32_t chunk = remaining < sizeof(drain) ? remaining : sizeof(drain);
                    if (!recv_all(client_fd, drain, chunk)) goto done;
                    remaining -= chunk;
                }
                send_response(client_fd, -1, NULL, 0);
                break;
            }

            NVEncIPCEncodeParams ep;
            if (!recv_all(client_fd, &ep, sizeof(ep))) goto done;

            if (ep.frame_size > NVENC_IPC_MAX_FRAME_SIZE) {
                HELPER_LOG("CMD_ENCODE: frame_size %u exceeds max %u", ep.frame_size, NVENC_IPC_MAX_FRAME_SIZE);
                send_response(client_fd, -1, NULL, 0);
                goto done;
            }

            /* Receive frame data */
            void *frame = malloc(ep.frame_size);
            if (frame == NULL) {
                send_response(client_fd, -1, NULL, 0);
                goto done;
            }
            if (!recv_all(client_fd, frame, ep.frame_size)) {
                free(frame);
                goto done;
            }


            void *bitstream = NULL;
            uint32_t bsSize = 0;
            bool ok = encoder_encode(&enc, frame, ep.width, ep.height, ep.frame_size, ep.force_idr, &bitstream, &bsSize);
            free(frame);


            if (ok) {
                send_response(client_fd, 0, bitstream, bsSize);
            } else {
                send_response(client_fd, -1, NULL, 0);
            }
            break;
        }

        case NVENC_IPC_CMD_ENCODE_DMABUF: {
            if (!enc.initialized) {
                if (hdr.payload_size > 0) {
                    void *tmp = malloc(hdr.payload_size);
                    if (tmp) { recv_all(client_fd, tmp, hdr.payload_size); free(tmp); }
                }
                send_response(client_fd, -1, NULL, 0);
                break;
            }

            /* Receive params WITH per-plane DMA-BUF fds via SCM_RIGHTS */
            NVEncIPCEncodeDmaBufParams dp;
            int dmabuf_fds[4] = {-1, -1, -1, -1};
            int num_fds = 0;
            {
                struct iovec iov = { .iov_base = &dp, .iov_len = sizeof(dp) };
                union {
                    char buf[CMSG_SPACE(sizeof(int) * 4)];
                    struct cmsghdr align;
                } cmsg_buf;
                memset(&cmsg_buf, 0, sizeof(cmsg_buf));

                struct msghdr msg = {
                    .msg_iov = &iov,
                    .msg_iovlen = 1,
                    .msg_control = cmsg_buf.buf,
                    .msg_controllen = sizeof(cmsg_buf.buf),
                };

                ssize_t n = recvmsg(client_fd, &msg, 0);
                if (n != sizeof(dp)) {
                    HELPER_LOG("DMABUF: recvmsg failed: %zd (errno=%d)", n, errno);
                    send_response(client_fd, -1, NULL, 0);
                    break;
                }

                struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
                if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
                    cmsg->cmsg_type == SCM_RIGHTS) {
                    num_fds = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                    if (num_fds > 4) num_fds = 4;
                    memcpy(dmabuf_fds, CMSG_DATA(cmsg), (size_t)num_fds * sizeof(int));
                }
            }

            if (num_fds < 1 || dmabuf_fds[0] < 0) {
                HELPER_LOG("DMABUF: no fds received");
                send_response(client_fd, -1, NULL, 0);
                break;
            }


            if (enc.frameCount < 3) {
                HELPER_LOG("DMABUF: fds=[%d,%d] %ux%u planes=%u bppc=%u sizes=[%u,%u]",
                           dmabuf_fds[0], dmabuf_fds[1],
                           dp.width, dp.height, dp.num_planes, dp.bppc,
                           dp.sizes[0], dp.sizes[1]);
            }

            /* Import each plane's DMA-BUF into CUDA as a CUarray,
             * same as the driver's import_to_cuda in direct-export-buf.c */
            CUexternalMemory extMems[4] = {0};
            CUmipmappedArray mipmaps[4] = {0};
            CUarray arrays[4] = {0};
            bool importOk = true;

            for (int i = 0; i < (int)dp.num_planes && i < num_fds; i++) {
                CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
                    .type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD,
                    .handle.fd = dmabuf_fds[i],
                    .size = dp.sizes[i],
                    .flags = 0,
                };

                CUresult cres = cu->cuImportExternalMemory(&extMems[i], &extMemDesc);
                /* CUDA takes ownership of the fd on success */
                if (cres != CUDA_SUCCESS) {
                    HELPER_LOG("DMABUF: cuImportExternalMemory plane %d failed: %d", i, cres);
                    close(dmabuf_fds[i]);
                    importOk = false;
                    break;
                }

                /* Determine plane format */
                int bpc = 8 * dp.bppc;
                int channels = (i == 0) ? 1 : 2; /* Y=1ch, UV=2ch interleaved */
                uint32_t planeW = (i == 0) ? dp.width : dp.width / 2;
                uint32_t planeH = (i == 0) ? dp.height : dp.height / 2;

                CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC mipmapDesc = {
                    .arrayDesc = {
                        .Width = planeW,
                        .Height = planeH,
                        .Depth = 0,
                        .Format = (bpc == 8) ? CU_AD_FORMAT_UNSIGNED_INT8 : CU_AD_FORMAT_UNSIGNED_INT16,
                        .NumChannels = (unsigned int)channels,
                        .Flags = 0,
                    },
                    .numLevels = 1,
                    .offset = 0,
                };

                cres = cu->cuExternalMemoryGetMappedMipmappedArray(&mipmaps[i], extMems[i], &mipmapDesc);
                if (cres != CUDA_SUCCESS) {
                    HELPER_LOG("DMABUF: cuExternalMemoryGetMappedMipmappedArray plane %d failed: %d", i, cres);
                    importOk = false;
                    break;
                }

                cres = cu->cuMipmappedArrayGetLevel(&arrays[i], mipmaps[i], 0);
                if (cres != CUDA_SUCCESS) {
                    HELPER_LOG("DMABUF: cuMipmappedArrayGetLevel plane %d failed: %d", i, cres);
                    importOk = false;
                    break;
                }
            }

            if (!importOk) {
                for (int i = 0; i < 4; i++) {
                    if (mipmaps[i]) cu->cuMipmappedArrayDestroy(mipmaps[i]);
                    if (extMems[i]) cu->cuDestroyExternalMemory(extMems[i]);
                    /* Close any fds that CUDA didn't take ownership of */
                    else if (i < num_fds && dmabuf_fds[i] >= 0) close(dmabuf_fds[i]);
                }
                /* Close remaining fds beyond what we tried to import */
                for (int i = (int)dp.num_planes; i < num_fds; i++) {
                    if (dmabuf_fds[i] >= 0) close(dmabuf_fds[i]);
                }
                send_response(client_fd, -1, NULL, 0);
                break;
            }

            /* Copy CUarrays to linear buffer (same as nvEndPictureEncode direct path) */
            uint32_t bpp = dp.is10bit ? 2 : 1;
            uint32_t pitch = dp.width * bpp;
            pitch = (pitch + 255) & ~255; /* Align to 256 */
            uint32_t lumaSize = pitch * dp.height;
            uint32_t chromaSize = pitch * (dp.height / 2);
            uint32_t totalSize = lumaSize + chromaSize;

            CUdeviceptr linearBuf = 0;
            CUresult cres = cu->cuMemAlloc(&linearBuf, totalSize);
            if (cres != CUDA_SUCCESS) {
                HELPER_LOG("DMABUF: cuMemAlloc(%u) failed: %d", totalSize, cres);
                goto dmabuf_cleanup;
            }
            cu->cuMemsetD8Async(linearBuf, 0, totalSize, 0);

            /* Copy luma */
            CUDA_MEMCPY2D cpy = {0};
            cpy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
            cpy.srcArray = arrays[0];
            cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            cpy.dstDevice = linearBuf;
            cpy.dstPitch = pitch;
            cpy.WidthInBytes = dp.width * bpp;
            cpy.Height = dp.height;
            cres = cu->cuMemcpy2D(&cpy);
            if (cres != CUDA_SUCCESS) {
                HELPER_LOG("DMABUF: luma cuMemcpy2D failed: %d", cres);
                cu->cuMemFree(linearBuf);
                goto dmabuf_cleanup;
            }

            /* Copy chroma */
            if (dp.num_planes >= 2 && arrays[1]) {
                memset(&cpy, 0, sizeof(cpy));
                cpy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
                cpy.srcArray = arrays[1];
                cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
                cpy.dstDevice = linearBuf + lumaSize;
                cpy.dstPitch = pitch;
                cpy.WidthInBytes = dp.width * bpp;
                cpy.Height = dp.height / 2;
                cres = cu->cuMemcpy2D(&cpy);
                if (cres != CUDA_SUCCESS) {
                    HELPER_LOG("DMABUF: chroma cuMemcpy2D failed: %d", cres);
                    cu->cuMemFree(linearBuf);
                    goto dmabuf_cleanup;
                }
            }

            /* Register linear buffer with NVENC */
            NV_ENC_BUFFER_FORMAT bufFmt = dp.is10bit
                ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;

            NV_ENC_REGISTER_RESOURCE regRes = {0};
            regRes.version = NV_ENC_REGISTER_RESOURCE_VER;
            regRes.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
            regRes.resourceToRegister = (void *)linearBuf;
            regRes.width = dp.width;
            regRes.height = dp.height;
            regRes.pitch = pitch;
            regRes.bufferFormat = bufFmt;
            regRes.bufferUsage = NV_ENC_INPUT_IMAGE;

            NVENCSTATUS nvst = enc.funcs.nvEncRegisterResource(enc.encoder, &regRes);
            if (nvst != NV_ENC_SUCCESS) {
                HELPER_LOG("DMABUF: nvEncRegisterResource failed: %d", nvst);
                cu->cuMemFree(linearBuf);
                goto dmabuf_cleanup;
            }

            NV_ENC_MAP_INPUT_RESOURCE mapRes = {0};
            mapRes.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
            mapRes.registeredResource = regRes.registeredResource;
            nvst = enc.funcs.nvEncMapInputResource(enc.encoder, &mapRes);
            if (nvst != NV_ENC_SUCCESS) {
                enc.funcs.nvEncUnregisterResource(enc.encoder, regRes.registeredResource);
                cu->cuMemFree(linearBuf);
                goto dmabuf_cleanup;
            }

            /* Encode */
            NV_ENC_PIC_PARAMS picParams = {0};
            picParams.version = NV_ENC_PIC_PARAMS_VER;
            picParams.inputBuffer = mapRes.mappedResource;
            picParams.bufferFmt = mapRes.mappedBufferFmt;
            picParams.inputWidth = dp.width;
            picParams.inputHeight = dp.height;
            picParams.inputPitch = pitch;
            picParams.outputBitstream = enc.outputBuffer;
            picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
            picParams.pictureType = NV_ENC_PIC_TYPE_UNKNOWN;
            picParams.encodePicFlags = (enc.frameCount == 0)
                ? (NV_ENC_PIC_FLAG_OUTPUT_SPSPPS | NV_ENC_PIC_FLAG_FORCEIDR) : 0;
            picParams.frameIdx = (uint32_t)enc.frameCount;
            picParams.inputTimeStamp = enc.frameCount;

            nvst = enc.funcs.nvEncEncodePicture(enc.encoder, &picParams);

            enc.funcs.nvEncUnmapInputResource(enc.encoder, mapRes.mappedResource);
            enc.funcs.nvEncUnregisterResource(enc.encoder, regRes.registeredResource);
            cu->cuMemFree(linearBuf);

            if (nvst != NV_ENC_SUCCESS) {
                HELPER_LOG("DMABUF: nvEncEncodePicture failed: %d", nvst);
                goto dmabuf_cleanup;
            }

            enc.frameCount++;
            if (enc.frameCount % 300 == 0) {
                HELPER_LOG("Encoded %lu frames (DMABUF)", (unsigned long)enc.frameCount);
            }

            /* Lock and send bitstream */
            {
                NV_ENC_LOCK_BITSTREAM lockOut = {0};
                lockOut.version = NV_ENC_LOCK_BITSTREAM_VER;
                lockOut.outputBitstream = enc.outputBuffer;
                nvst = enc.funcs.nvEncLockBitstream(enc.encoder, &lockOut);
                if (nvst == NV_ENC_SUCCESS) {
                    send_response(client_fd, 0, lockOut.bitstreamBufferPtr,
                                  lockOut.bitstreamSizeInBytes);
                    enc.funcs.nvEncUnlockBitstream(enc.encoder, enc.outputBuffer);
                } else {
                    send_response(client_fd, -1, NULL, 0);
                }
            }

dmabuf_cleanup:
            for (int i = 0; i < 4; i++) {
                if (mipmaps[i]) cu->cuMipmappedArrayDestroy(mipmaps[i]);
                if (extMems[i]) cu->cuDestroyExternalMemory(extMems[i]);
            }
            break;
        }

        case NVENC_IPC_CMD_ENCODE_SHM: {
            if (!enc.initialized || shm_ptr == MAP_FAILED) {
                /* Drain payload */
                if (hdr.payload_size > 0) {
                    void *tmp = malloc(hdr.payload_size);
                    if (tmp) { recv_all(client_fd, tmp, hdr.payload_size); free(tmp); }
                }
                send_response(client_fd, -1, NULL, 0);
                break;
            }

            NVEncIPCEncodeShmParams sp;
            if (!recv_all(client_fd, &sp, sizeof(sp))) goto done;


            /* Encode directly from shared memory — no socket data transfer */
            void *bitstream = NULL;
            uint32_t bsSize = 0;
            bool ok = encoder_encode(&enc, shm_ptr, sp.width, sp.height,
                                     sp.frame_size, sp.force_idr,
                                     &bitstream, &bsSize);


            if (ok) {
                send_response(client_fd, 0, bitstream, bsSize);
            } else {
                send_response(client_fd, -1, NULL, 0);
            }
            break;
        }

        case NVENC_IPC_CMD_CLOSE:
            encoder_close(&enc);
            send_response(client_fd, 0, NULL, 0);
            goto done;

        default:
            HELPER_LOG("Unknown command: %u", hdr.cmd);
            send_response(client_fd, -1, NULL, 0);
            break;
        }
    }

done:
    if (enc.initialized) {
        cu->cuCtxPushCurrent(enc.cudaCtx);
        encoder_close(&enc);
        cu->cuCtxPopCurrent(NULL);
    }
    if (shm_ptr != MAP_FAILED) {
        munmap(shm_ptr, shm_size);
    }
    if (shm_fd >= 0) {
        close(shm_fd);
    }
    close(client_fd);
    HELPER_LOG("Client handler done");
}

static void sighandler(int sig)
{
    (void)sig;
    running = 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Always log to stderr — this is a daemon, logs are essential for diagnostics */
    log_enabled = 1;

    signal(SIGTERM, sighandler);
    signal(SIGINT, sighandler);
    signal(SIGPIPE, SIG_IGN);

    HELPER_LOG("Starting nvenc-helper (pid=%d)", getpid());

    /* Load CUDA */
    if (cuda_load_functions(&cu, NULL) != 0 || cu == NULL) {
        HELPER_LOG("Failed to load CUDA");
        return 1;
    }

    CUresult cres = cu->cuInit(0);
    if (cres != CUDA_SUCCESS) {
        HELPER_LOG("cuInit failed: %d", cres);
        cuda_free_functions(&cu);
        return 1;
    }

    /* Load NVENC */
    if (nvenc_load_functions(&nv_dl, NULL) != 0 || nv_dl == NULL) {
        HELPER_LOG("Failed to load NVENC");
        cuda_free_functions(&cu);
        return 1;
    }

    HELPER_LOG("CUDA and NVENC loaded");

    /* Create socket */
    char sock_path[256];
    if (!nvenc_ipc_get_socket_path(sock_path, sizeof(sock_path))) {
        HELPER_LOG("Failed to get socket path");
        return 1;
    }

    unlink(sock_path); /* Remove stale socket */

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        HELPER_LOG("socket: %s", strerror(errno));
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    mode_t old_umask = umask(0077); //socket created with 0700 permissions
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        HELPER_LOG("bind(%s): %s", sock_path, strerror(errno));
        umask(old_umask);
        close(listen_fd);
        return 1;
    }
    umask(old_umask);

    if (listen(listen_fd, 8) < 0) {
        HELPER_LOG("listen: %s", strerror(errno));
        close(listen_fd);
        unlink(sock_path);
        return 1;
    }

    HELPER_LOG("Listening on %s", sock_path);

    /* Accept loop — runs until SIGTERM/SIGINT */
    while (running) {
        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
        int ret = poll(&pfd, 1, -1); /* Block forever until connection or signal */

        if (ret < 0) {
            if (errno == EINTR) continue;
            HELPER_LOG("poll: %s", strerror(errno));
            break;
        }

        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            HELPER_LOG("accept: %s", strerror(errno));
            continue; /* Don't exit on accept error — keep listening */
        }

        /* Handle one client at a time (sufficient for Steam's single encode stream) */
        handle_client(client_fd);
        HELPER_LOG("Ready for next client");
    }

    close(listen_fd);
    unlink(sock_path);
    nvenc_free_functions(&nv_dl);
    cuda_free_functions(&cu);
    HELPER_LOG("Exiting");
    return 0;
}
