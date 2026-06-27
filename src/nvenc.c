#include "nvenc.h"
#include "vabackend.h"

#include <string.h>
#include <stdlib.h>

static bool check_nvenc_status(NVENCSTATUS status, const char *func, int line)
{
    if (status != NV_ENC_SUCCESS) {
        LOG("NVENC error %d at %s:%d", status, func, line);
        return false;
    }
    return true;
}
#define CHECK_NVENC(status) check_nvenc_status(status, __func__, __LINE__)

bool nvenc_load(NvencFunctions **nvenc_dl)
{
    int ret = nvenc_load_functions(nvenc_dl, NULL);
    if (ret != 0) {
        LOG("Failed to load NVENC functions (libnvidia-encode.so)");
        *nvenc_dl = NULL;
        return false;
    }
    //version format: API returns (major << 4 | minor)
    uint32_t maxVersion = 0;
    NVENCSTATUS st = (*nvenc_dl)->NvEncodeAPIGetMaxSupportedVersion(&maxVersion);
    if (st != NV_ENC_SUCCESS) {
        LOG("NvEncodeAPIGetMaxSupportedVersion failed: %d", st);
        nvenc_free_functions(nvenc_dl);
        *nvenc_dl = NULL;
        return false;
    }
    uint32_t currentVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    LOG("NVENC max supported version: %u.%u, header version: %u.%u",
        maxVersion >> 4, maxVersion & 0xf,
        NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);

    if (currentVersion > maxVersion) {
        LOG("NVENC header version (%u) is newer than driver supports (%u)",
            currentVersion, maxVersion);
        nvenc_free_functions(nvenc_dl);
        *nvenc_dl = NULL;
        return false;
    }
    return true;
}

void nvenc_unload(NvencFunctions **nvenc_dl)
{
    if (*nvenc_dl != NULL) {
        nvenc_free_functions(nvenc_dl);
        *nvenc_dl = NULL;
    }
}

bool nvenc_open_session(NVENCContext *nvencCtx, NvencFunctions *nvenc_dl, CUcontext cudaCtx)
{
    memset(nvencCtx, 0, sizeof(*nvencCtx));

    /* Fill function list */
    nvencCtx->funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS st = nvenc_dl->NvEncodeAPICreateInstance(&nvencCtx->funcs);
    if (!CHECK_NVENC(st)) {
        return false;
    }

    /* Open encode session */
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = {0};
    sessionParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    sessionParams.device = cudaCtx;
    sessionParams.apiVersion = NVENCAPI_VERSION;

    st = nvencCtx->funcs.nvEncOpenEncodeSessionEx(&sessionParams, &nvencCtx->encoder);
    if (!CHECK_NVENC(st)) {
        nvencCtx->encoder = NULL;
        return false;
    }

    LOG("NVENC session opened: %p", nvencCtx->encoder);
    return true;
}

void nvenc_close_session(NVENCContext *nvencCtx)
{
    if (nvencCtx->encoder == NULL) {
        return;
    }

    /* Send EOS to flush encoder before freeing any buffers */
    if (nvencCtx->initialized) {
        NV_ENC_PIC_PARAMS picParams = {0};
        picParams.version = NV_ENC_PIC_PARAMS_VER;
        picParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        nvencCtx->funcs.nvEncEncodePicture(nvencCtx->encoder, &picParams);
    }

    /* Free output buffer after flush */
    nvenc_free_output_buffer(nvencCtx);

    /* Destroy encoder */
    NVENCSTATUS st = nvencCtx->funcs.nvEncDestroyEncoder(nvencCtx->encoder);
    if (st != NV_ENC_SUCCESS) {
        LOG("nvEncDestroyEncoder failed: %d", st);
    }

    LOG("NVENC session closed");
    nvencCtx->encoder = NULL;
    nvencCtx->initialized = false;
}

bool nvenc_init_encoder(NVENCContext *nvencCtx, uint32_t width, uint32_t height,
                        GUID codecGuid, GUID profileGuid, GUID presetGuid,
                        NV_ENC_TUNING_INFO tuningInfo)
{
    NVENCSTATUS st;

    nvencCtx->codecGuid = codecGuid;
    nvencCtx->profileGuid = profileGuid;
    nvencCtx->width = width;
    nvencCtx->height = height;

    //get preset config
    NV_ENC_PRESET_CONFIG presetConfig = {0};
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

    st = nvencCtx->funcs.nvEncGetEncodePresetConfigEx(
        nvencCtx->encoder, codecGuid, presetGuid, tuningInfo, &presetConfig);
    if (!CHECK_NVENC(st)) {
        return false;
    }

    //apply overrides
    memcpy(&nvencCtx->encodeConfig, &presetConfig.presetCfg, sizeof(NV_ENC_CONFIG));
    nvencCtx->encodeConfig.version = NV_ENC_CONFIG_VER;
    nvencCtx->encodeConfig.profileGUID = profileGuid;

    if (nvencCtx->rcMode != 0) {
        nvencCtx->encodeConfig.rcParams.rateControlMode = (NV_ENC_PARAMS_RC_MODE)nvencCtx->rcMode;
    }
    if (nvencCtx->bitrate > 0) {
        nvencCtx->encodeConfig.rcParams.averageBitRate = nvencCtx->bitrate;
    }
    if (nvencCtx->maxBitrate > 0) {
        nvencCtx->encodeConfig.rcParams.maxBitRate = nvencCtx->maxBitrate;
    }
    if (nvencCtx->vbvBufferSize > 0) {
        nvencCtx->encodeConfig.rcParams.vbvBufferSize = nvencCtx->vbvBufferSize;
    }
    if (nvencCtx->vbvInitialDelay > 0) {
        nvencCtx->encodeConfig.rcParams.vbvInitialDelay = nvencCtx->vbvInitialDelay;
    }

    if (nvencCtx->intraPeriod > 0) {
        nvencCtx->encodeConfig.gopLength = nvencCtx->intraPeriod;
    }
    //no B-frames: NVENC needs DPB management or returns NEED_MORE_INPUT which ffmpeg 6.x can't handle
    nvencCtx->encodeConfig.frameIntervalP = 1;

    memset(&nvencCtx->initParams, 0, sizeof(nvencCtx->initParams));
    nvencCtx->initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    nvencCtx->initParams.encodeGUID = codecGuid;
    nvencCtx->initParams.presetGUID = presetGuid;
    nvencCtx->initParams.encodeWidth = width;
    nvencCtx->initParams.encodeHeight = height;
    nvencCtx->initParams.darWidth = width;
    nvencCtx->initParams.darHeight = height;
    nvencCtx->initParams.frameRateNum = nvencCtx->frameRateNum > 0 ? nvencCtx->frameRateNum : 30;
    nvencCtx->initParams.frameRateDen = nvencCtx->frameRateDen > 0 ? nvencCtx->frameRateDen : 1;
    nvencCtx->initParams.enablePTD = 1;
    nvencCtx->initParams.encodeConfig = &nvencCtx->encodeConfig;
    nvencCtx->initParams.maxEncodeWidth = width;
    nvencCtx->initParams.maxEncodeHeight = height;
    nvencCtx->initParams.tuningInfo = tuningInfo;

    st = nvencCtx->funcs.nvEncInitializeEncoder(nvencCtx->encoder, &nvencCtx->initParams);
    if (!CHECK_NVENC(st)) {
        return false;
    }

    nvencCtx->initialized = true;
    LOG("NVENC encoder initialized: %ux%u codec=%s",
        width, height,
        memcmp(&codecGuid, &NV_ENC_CODEC_H264_GUID, sizeof(GUID)) == 0 ? "H.264" : "HEVC");

    return true;
}

bool nvenc_alloc_output_buffer(NVENCContext *nvencCtx)
{
    if (nvencCtx->outputBuffer.allocated) {
        return true;
    }

    NV_ENC_CREATE_BITSTREAM_BUFFER createBuf = {0};
    createBuf.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

    NVENCSTATUS st = nvencCtx->funcs.nvEncCreateBitstreamBuffer(
        nvencCtx->encoder, &createBuf);
    if (!CHECK_NVENC(st)) {
        return false;
    }

    nvencCtx->outputBuffer.bitstreamBuffer = createBuf.bitstreamBuffer;
    nvencCtx->outputBuffer.allocated = true;
    nvencCtx->outputBuffer.locked = false;
    nvencCtx->outputBuffer.lockedPtr = NULL;
    nvencCtx->outputBuffer.lockedSize = 0;

    return true;
}

void nvenc_free_output_buffer(NVENCContext *nvencCtx)
{
    if (!nvencCtx->outputBuffer.allocated || nvencCtx->encoder == NULL) {
        return;
    }

    /* Unlock if still locked */
    if (nvencCtx->outputBuffer.locked) {
        nvenc_unlock_bitstream(nvencCtx);
    }

    nvencCtx->funcs.nvEncDestroyBitstreamBuffer(
        nvencCtx->encoder, nvencCtx->outputBuffer.bitstreamBuffer);
    nvencCtx->outputBuffer.bitstreamBuffer = NULL;
    nvencCtx->outputBuffer.allocated = false;
}

bool nvenc_register_cuda_resource(NVENCContext *nvencCtx, CUdeviceptr devPtr,
                                  uint32_t width, uint32_t height, uint32_t pitch,
                                  NV_ENC_BUFFER_FORMAT format,
                                  NV_ENC_REGISTERED_PTR *outRegistered)
{
    NV_ENC_REGISTER_RESOURCE regRes = {0};
    regRes.version = NV_ENC_REGISTER_RESOURCE_VER;
    regRes.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    regRes.resourceToRegister = (void*)devPtr;
    regRes.width = width;
    regRes.height = height;
    regRes.pitch = pitch;
    regRes.bufferFormat = format;
    regRes.bufferUsage = NV_ENC_INPUT_IMAGE;

    NVENCSTATUS st = nvencCtx->funcs.nvEncRegisterResource(
        nvencCtx->encoder, &regRes);
    if (!CHECK_NVENC(st)) {
        return false;
    }

    *outRegistered = regRes.registeredResource;
    return true;
}

bool nvenc_map_resource(NVENCContext *nvencCtx, NV_ENC_REGISTERED_PTR registered,
                        NV_ENC_INPUT_PTR *outMapped, NV_ENC_BUFFER_FORMAT *outFmt)
{
    NV_ENC_MAP_INPUT_RESOURCE mapRes = {0};
    mapRes.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapRes.registeredResource = registered;

    NVENCSTATUS st = nvencCtx->funcs.nvEncMapInputResource(
        nvencCtx->encoder, &mapRes);
    if (!CHECK_NVENC(st)) {
        return false;
    }

    *outMapped = mapRes.mappedResource;
    if (outFmt) {
        *outFmt = mapRes.mappedBufferFmt;
    }
    return true;
}

bool nvenc_unmap_resource(NVENCContext *nvencCtx, NV_ENC_INPUT_PTR mapped)
{
    NVENCSTATUS st = nvencCtx->funcs.nvEncUnmapInputResource(
        nvencCtx->encoder, mapped);
    return CHECK_NVENC(st);
}

bool nvenc_unregister_resource(NVENCContext *nvencCtx, NV_ENC_REGISTERED_PTR registered)
{
    NVENCSTATUS st = nvencCtx->funcs.nvEncUnregisterResource(
        nvencCtx->encoder, registered);
    return CHECK_NVENC(st);
}

/*
 * Encode a frame. Returns:
 *  1 = encoded successfully, output available
 *  0 = needs more input (B-frame buffering), no output yet
 * -1 = error
 */
int nvenc_encode_frame(NVENCContext *nvencCtx, NV_ENC_INPUT_PTR inputBuffer,
                       NV_ENC_BUFFER_FORMAT bufferFmt,
                       uint32_t inputWidth, uint32_t inputHeight, uint32_t inputPitch,
                       NV_ENC_PIC_TYPE picType, uint32_t picFlags)
{
    if (!nvencCtx->outputBuffer.allocated) {
        if (!nvenc_alloc_output_buffer(nvencCtx)) {
            return -1;
        }
    }

    NV_ENC_PIC_PARAMS picParams = {0};
    picParams.version = NV_ENC_PIC_PARAMS_VER;
    picParams.inputBuffer = inputBuffer;
    picParams.bufferFmt = bufferFmt;
    picParams.inputWidth = inputWidth;
    picParams.inputHeight = inputHeight;
    picParams.inputPitch = inputPitch;
    picParams.outputBitstream = nvencCtx->outputBuffer.bitstreamBuffer;
    picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picParams.pictureType = picType;
    picParams.encodePicFlags = picFlags;
    picParams.frameIdx = (uint32_t)nvencCtx->frameCount;
    picParams.inputTimeStamp = nvencCtx->frameCount;

    NVENCSTATUS st = nvencCtx->funcs.nvEncEncodePicture(
        nvencCtx->encoder, &picParams);

    nvencCtx->frameCount++;

    if (st == NV_ENC_ERR_NEED_MORE_INPUT) {
        /* B-frame reordering: NVENC needs more frames before producing output */
        return 0;
    }
    if (st != NV_ENC_SUCCESS) {
        LOG("nvEncEncodePicture failed: %d", st);
        return -1;
    }

    return 1;
}

bool nvenc_lock_bitstream(NVENCContext *nvencCtx, void **outPtr, uint32_t *outSize)
{
    NV_ENC_LOCK_BITSTREAM lockParams = {0};
    lockParams.version = NV_ENC_LOCK_BITSTREAM_VER;
    lockParams.outputBitstream = nvencCtx->outputBuffer.bitstreamBuffer;
    lockParams.doNotWait = 0;

    NVENCSTATUS st = nvencCtx->funcs.nvEncLockBitstream(
        nvencCtx->encoder, &lockParams);
    if (!CHECK_NVENC(st)) {
        return false;
    }

    *outPtr = lockParams.bitstreamBufferPtr;
    *outSize = lockParams.bitstreamSizeInBytes;
    nvencCtx->outputBuffer.locked = true;
    nvencCtx->outputBuffer.lockedPtr = lockParams.bitstreamBufferPtr;
    nvencCtx->outputBuffer.lockedSize = lockParams.bitstreamSizeInBytes;

    return true;
}

bool nvenc_unlock_bitstream(NVENCContext *nvencCtx)
{
    if (!nvencCtx->outputBuffer.locked) {
        return true;
    }

    NVENCSTATUS st = nvencCtx->funcs.nvEncUnlockBitstream(
        nvencCtx->encoder, nvencCtx->outputBuffer.bitstreamBuffer);
    nvencCtx->outputBuffer.locked = false;
    nvencCtx->outputBuffer.lockedPtr = NULL;
    nvencCtx->outputBuffer.lockedSize = 0;

    return CHECK_NVENC(st);
}

/* Profile/entrypoint helpers */

bool nvenc_is_encode_profile(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
    case VAProfileH264Main:
    case VAProfileH264High:
    case VAProfileHEVCMain:
    case VAProfileHEVCMain10:
        return true;
    default:
        return false;
    }
}

GUID nvenc_va_profile_to_codec_guid(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
    case VAProfileH264Main:
    case VAProfileH264High:
        return NV_ENC_CODEC_H264_GUID;
    case VAProfileHEVCMain:
    case VAProfileHEVCMain10:
        return NV_ENC_CODEC_HEVC_GUID;
    default: {
        GUID empty = {0};
        return empty;
    }
    }
}

GUID nvenc_va_profile_to_profile_guid(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
        return NV_ENC_H264_PROFILE_BASELINE_GUID;
    case VAProfileH264Main:
        return NV_ENC_H264_PROFILE_MAIN_GUID;
    case VAProfileH264High:
        return NV_ENC_H264_PROFILE_HIGH_GUID;
    case VAProfileHEVCMain:
        return NV_ENC_HEVC_PROFILE_MAIN_GUID;
    case VAProfileHEVCMain10:
        return NV_ENC_HEVC_PROFILE_MAIN10_GUID;
    default: {
        GUID empty = {0};
        return empty;
    }
    }
}

NV_ENC_BUFFER_FORMAT nvenc_surface_format(VAProfile profile)
{
    switch (profile) {
    case VAProfileHEVCMain10:
        return NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
    default:
        return NV_ENC_BUFFER_FORMAT_NV12;
    }
}
