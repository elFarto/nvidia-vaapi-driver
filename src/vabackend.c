#define _GNU_SOURCE

#include "vabackend.h"
#include "backend-common.h"
#include "external-surface.h"
#include "encode/h264_encode.h"
#include "encode/encode_common.h"
#include "encode/encode_pipeline.h"
#include "encode/encode_loader.h"
#include "encode/encode_driver.h"
#include "encode/encode_image.h"
#include "encode/encode_buffer.h"
#include "encode/encode_surface.h"
#include "encode/encode_va.h"
#include "direct/nv-driver.h"

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <malloc.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <va/va_backend.h>
#include <va/va_drmcommon.h>

#include <drm_fourcc.h>
#include <linux/dma-buf.h>

#include <unistd.h>
#include <sys/types.h>
#include <stdarg.h>

#include <time.h>

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#ifndef __has_include
#define __has_include(x) 0
#endif

#if __has_include(<pthread_np.h>)
#include <pthread_np.h>
#define gettid pthread_getthreadid_np
#define HAVE_GETTID 1
#endif

#ifndef HAVE_GETTID
#include <sys/syscall.h>
/* Bionic and glibc >= 2.30 declare gettid() system call wrapper in unistd.h and
 * has a definition for it */
#ifdef __BIONIC__
#define HAVE_GETTID 1
#elif !defined(__GLIBC_PREREQ)
#define HAVE_GETTID 0
#elif !__GLIBC_PREREQ(2,30)
#define HAVE_GETTID 0
#else
#define HAVE_GETTID 1
#endif
#endif

static pid_t nv_gettid(void)
{
#if HAVE_GETTID
    return gettid();
#else
    return syscall(__NR_gettid);
#endif
}

static pthread_mutex_t concurrency_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t instances;
static uint32_t max_instances;

CudaFunctions *cu;
static CuvidFunctions *cv;
static NvencFunctions *nvenc;

extern const NVCodec __start_nvd_codecs[];
extern const NVCodec __stop_nvd_codecs[];

static FILE *LOG_OUTPUT;

static int gpu = -1;
static enum {
    EGL, DIRECT
} backend = DIRECT;

extern const NVBackend DIRECT_BACKEND;
extern const NVBackend EGL_BACKEND;

const NVFormatInfo formatsInfo[NV_FORMAT_COUNT] =
{
    [NV_FORMAT_NONE] = {0},
    [NV_FORMAT_NV12] = {1, 2, DRM_FORMAT_NV12,     false, false, {{1, DRM_FORMAT_R8,       {0,0}}, {2, DRM_FORMAT_RG88,   {1,1}}},                            {VA_FOURCC_NV12, VA_LSB_FIRST,   12, 0,0,0,0,0}},
    [NV_FORMAT_P010] = {2, 2, DRM_FORMAT_P010,     true,  false, {{1, DRM_FORMAT_R16,      {0,0}}, {2, DRM_FORMAT_RG1616, {1,1}}},                            {VA_FOURCC_P010, VA_LSB_FIRST,   24, 0,0,0,0,0}},
    [NV_FORMAT_P012] = {2, 2, DRM_FORMAT_P012,     true,  false, {{1, DRM_FORMAT_R16,      {0,0}}, {2, DRM_FORMAT_RG1616, {1,1}}},                            {VA_FOURCC_P012, VA_LSB_FIRST,   24, 0,0,0,0,0}},
    [NV_FORMAT_P016] = {2, 2, DRM_FORMAT_P016,     true,  false, {{1, DRM_FORMAT_R16,      {0,0}}, {2, DRM_FORMAT_RG1616, {1,1}}},                            {VA_FOURCC_P016, VA_LSB_FIRST,   24, 0,0,0,0,0}},
    [NV_FORMAT_444P] = {1, 3, DRM_FORMAT_YUV444,   false, true,  {{1, DRM_FORMAT_R8,       {0,0}}, {1, DRM_FORMAT_R8,     {0,0}}, {1, DRM_FORMAT_R8, {0,0}}}, {VA_FOURCC_444P, VA_LSB_FIRST,   24, 0,0,0,0,0}},
#if VA_CHECK_VERSION(1, 20, 0)
    [NV_FORMAT_Q416] = {2, 3, DRM_FORMAT_INVALID,  true,  true,  {{1, DRM_FORMAT_R16,      {0,0}}, {1, DRM_FORMAT_R16,    {0,0}}, {1, DRM_FORMAT_R16,{0,0}}}, {VA_FOURCC_Q416, VA_LSB_FIRST,   48, 0,0,0,0,0}},
#endif
};

uint32_t nvGetExportLayerDrmFormat(NVFormat format, uint32_t plane_idx, uint32_t fallback_fourcc)
{
    if (plane_idx == 0) {
        switch (format) {
        case NV_FORMAT_NV12:
            return DRM_FORMAT_R8;
        case NV_FORMAT_P010:
        case NV_FORMAT_P012:
        case NV_FORMAT_P016:
            return DRM_FORMAT_R16;
        default:
            return fallback_fourcc;
        }
    }

    switch (format) {
    case NV_FORMAT_NV12:
        return DRM_FORMAT_RG88;
    case NV_FORMAT_P010:
    case NV_FORMAT_P012:
    case NV_FORMAT_P016:
        return DRM_FORMAT_RG1616;
    default:
        return fallback_fourcc;
    }
}

static NVFormat nvFormatFromVaFormat(uint32_t fourcc) {
    for (uint32_t i = NV_FORMAT_NONE + 1; i < ARRAY_SIZE(formatsInfo); i++) {
        if (formatsInfo[i].vaFormat.fourcc == fourcc) {
            return i;
        }
    }
    return NV_FORMAT_NONE;
}

__attribute__ ((constructor))
static void init() {
    char *nvdLog = getenv("NVD_LOG");
    if (nvdLog != NULL) {
        if (strcmp(nvdLog, "1") == 0) {
            LOG_OUTPUT = stdout;
        } else {
            LOG_OUTPUT = fopen(nvdLog, "a");
            if (LOG_OUTPUT == NULL) {
                LOG_OUTPUT = stdout;
            }
        }
    }

    char *nvdGpu = getenv("NVD_GPU");
    if (nvdGpu != NULL) {
        gpu = atoi(nvdGpu);
    }

    char *nvdMaxInstances = getenv("NVD_MAX_INSTANCES");
    if (nvdMaxInstances != NULL) {
        max_instances = atoi(nvdMaxInstances);
    }

    char *nvdBackend = getenv("NVD_BACKEND");
    if (nvdBackend != NULL) {
        if (strncmp(nvdBackend, "direct", 6) == 0) {
            backend = DIRECT;
        } else if (strncmp(nvdBackend, "egl", 6) == 0) {
            backend = EGL;
        }
    }

#ifdef __linux__
    //try to detect the Firefox sandbox and skip loading CUDA if detected
    int fd = open("/proc/version", O_RDONLY);
    if (fd < 0) {
        LOG("ERROR: Potential Firefox sandbox detected, failing to init!");
        LOG("If running in Firefox, set env var MOZ_DISABLE_RDD_SANDBOX=1 to disable sandbox.");
        //exit here so we don't init CUDA, unless an env var has been set to force us to init even though we've detected a sandbox
        if (getenv("NVD_FORCE_INIT") == NULL) {
            return;
        }
    } else {
        //we're not in a sandbox
        //continue as normal
        close(fd);
    }
#endif

    //initialise the CUDA and NVDEC functions
    int ret = cuda_load_functions(&cu, NULL);
    if (ret != 0) {
        cu = NULL;
        LOG("Failed to load CUDA functions");
        return;
    }
    ret = cuvid_load_functions(&cv, NULL);
    if (ret != 0) {
        cv = NULL;
        LOG("Failed to load NVDEC functions");
        return;
    }
    nvenc_loader_init(&nvenc);

    //Not really much we can do here to abort the loading of the library
    CHECK_CUDA_RESULT(cu->cuInit(0));
}

__attribute__ ((destructor))
static void cleanup() {
    if (cv != NULL) {
        cuvid_free_functions(&cv);
    }
    nvenc_loader_cleanup(&nvenc);
    if (cu != NULL) {
        cuda_free_functions(&cu);
    }
}


#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

#if __has_attribute(gnu_printf) || (defined(__GNUC__) && !defined(__clang__))
__attribute((format(gnu_printf, 4, 5)))
#endif
void logger(const char *filename, const char *function, int line, const char *msg, ...) {
    if (LOG_OUTPUT == 0) {
        return;
    }

    va_list argList;
    char formattedMessage[1024];

    va_start(argList, msg);
    vsnprintf(formattedMessage, 1024, msg, argList);
    va_end(argList);

    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);

    fprintf(LOG_OUTPUT, "%10ld.%09ld [%d-%d] %s:%4d %24s %s\n", (long)tp.tv_sec, tp.tv_nsec, getpid(), nv_gettid(), filename, line, function, formattedMessage);
    fflush(LOG_OUTPUT);
}

bool checkCudaErrors(CUresult err, const char *file, const char *function, const int line) {
    if (CUDA_SUCCESS != err) {
        const char *errStr = NULL;
        cu->cuGetErrorString(err, &errStr);
        logger(file, function, line, "CUDA ERROR '%s' (%d)\n", errStr, err);
        return true;
    }
    return false;
}

void appendBuffer(AppendableBuffer *ab, const void *buf, uint64_t size) {
  if (ab->buf == NULL) {
      ab->allocated = size*2;
      ab->buf = memalign(16, ab->allocated);
      ab->size = 0;
  } else if (ab->size + size > ab->allocated) {
      while (ab->size + size > ab->allocated) {
        ab->allocated += ab->allocated >> 1;
      }
      void *nb = memalign(16, ab->allocated);
      memcpy(nb, ab->buf, ab->size);
      free(ab->buf);
      ab->buf = nb;
  }
  memcpy(PTROFF(ab->buf, ab->size), buf, size);
  ab->size += size;
}

static void freeBuffer(AppendableBuffer *ab) {
  if (ab->buf != NULL) {
      free(ab->buf);
      ab->buf = NULL;
      ab->size = 0;
      ab->allocated = 0;
  }
}

static Object allocateObject(NVDriver *drv, ObjectType type, size_t allocatePtrSize) {
    Object newObj = (Object) calloc(1, sizeof(struct Object_t));

    newObj->type = type;

    if (allocatePtrSize > 0) {
        newObj->obj = calloc(1, allocatePtrSize);
    }

    pthread_mutex_lock(&drv->objectCreationMutex);
    newObj->id = (++drv->nextObjId);
    add_element(&drv->objects, newObj);
    pthread_mutex_unlock(&drv->objectCreationMutex);

    return newObj;
}

static Object getObject(NVDriver *drv, ObjectType type, VAGenericID id) {
    Object ret = NULL;
    if (id != VA_INVALID_ID) {
        pthread_mutex_lock(&drv->objectCreationMutex);
        ARRAY_FOR_EACH(Object, o, &drv->objects)
            if (o->id == id && o->type == type) {
                ret = o;
                break;
            }
        END_FOR_EACH
        pthread_mutex_unlock(&drv->objectCreationMutex);
    }
    return ret;
}

static void* getObjectPtr(NVDriver *drv, ObjectType type, VAGenericID id) {
    if (id != VA_INVALID_ID) {
        Object o = getObject(drv, type, id);
        if (o != NULL) {
            return o->obj;
        }
    }
    return NULL;
}

static Object getObjectByPtr(NVDriver *drv, ObjectType type, void *ptr) {
    Object ret = NULL;
    if (ptr != NULL) {
        pthread_mutex_lock(&drv->objectCreationMutex);
        ARRAY_FOR_EACH(Object, o, &drv->objects)
            if (o->obj == ptr && o->type == type) {
                ret = o;
                break;
            }
        END_FOR_EACH
        pthread_mutex_unlock(&drv->objectCreationMutex);
    }
    return ret;
}

static void deleteObject(NVDriver *drv, VAGenericID id) {
    if (id == VA_INVALID_ID) {
        return;
    }

    pthread_mutex_lock(&drv->objectCreationMutex);
    ARRAY_FOR_EACH(Object, o, &drv->objects)
        if (o->id == id) {
            remove_element_at(&drv->objects, o_idx);
            free(o->obj);
            free(o);
            //we've found the object, no need to continue
            break;
        }
    END_FOR_EACH
    pthread_mutex_unlock(&drv->objectCreationMutex);
}

static void nv_clear_surface_context_refs(NVDriver *drv, NVContext *nvCtx)
{
    if (!drv || !nvCtx) {
        return;
    }
    pthread_mutex_lock(&drv->objectCreationMutex);
    ARRAY_FOR_EACH(Object, o, &drv->objects)
        if (o->type != OBJECT_TYPE_SURFACE || !o->obj) {
            continue;
        }
        NVSurface *surface = (NVSurface*) o->obj;
        if (surface->context == nvCtx) {
            surface->context = NULL;
        }
        if (surface->encCtx == nvCtx) {
            surface->encCtx = NULL;
            surface->encReg = NULL;
        }
    END_FOR_EACH
    pthread_mutex_unlock(&drv->objectCreationMutex);
}

static void nv_clear_encode_buffer_refs(NVDriver *drv, NVEncodeContext *enc)
{
    if (!drv || !enc) {
        return;
    }
    pthread_mutex_lock(&drv->objectCreationMutex);
    ARRAY_FOR_EACH(Object, o, &drv->objects)
        if (o->type != OBJECT_TYPE_BUFFER || !o->obj) {
            continue;
        }
        NVBuffer *buf = (NVBuffer*) o->obj;
        if (buf->bufferType == VAEncCodedBufferType && buf->encCtx == enc) {
            buf->encCtx = NULL;
        }
    END_FOR_EACH
    pthread_mutex_unlock(&drv->objectCreationMutex);
}

void nvenc_unregister_surface(NVEncodeContext *enc, NVEncSurfaceReg *reg)
{
    if (!enc || !reg || !enc->encoder || !enc->funcs) {
        return;
    }
    if (reg->registered) {
        NVENCSTATUS st = enc->funcs->nvEncUnregisterResource(enc->encoder, reg->registered);
        if (st != NV_ENC_SUCCESS) {
            LOG("NvEncUnregisterResource failed: %d", st);
        }
    }
    if (enc->drv) {
        NVSurface *surface = getObjectPtr(enc->drv, OBJECT_TYPE_SURFACE, reg->surfaceId);
        if (surface) {
            surface->encReg = NULL;
            surface->encCtx = NULL;
        }
    }
}

void nvenc_remove_surface_reg(NVEncodeContext *enc, NVEncSurfaceReg *reg)
{
    if (!enc || !reg) {
        return;
    }
    for (uint32_t i = 0; i < enc->registeredSurfaces.size; i++) {
        if (enc->registeredSurfaces.buf[i] == reg) {
            remove_element_at(&enc->registeredSurfaces, i);
            return;
        }
    }
}

static void nvenc_abandon_encoder_resources(NVEncodeContext *enc)
{
    if (!enc) {
        return;
    }

    ARRAY_FOR_EACH(NVEncSurfaceReg*, reg, &enc->registeredSurfaces)
        if (reg && enc->drv) {
            NVSurface *surface = getObjectPtr(enc->drv, OBJECT_TYPE_SURFACE, reg->surfaceId);
            if (surface && surface->encReg == reg) {
                surface->encReg = NULL;
            }
        }
        free(reg);
    END_FOR_EACH
    free(enc->registeredSurfaces.buf);
    enc->registeredSurfaces.buf = NULL;
    enc->registeredSurfaces.size = 0;
    enc->registeredSurfaces.capacity = 0;

    pthread_mutex_lock(&enc->bitstreamPoolMutex);
    free(enc->bitstreamPool.buf);
    enc->bitstreamPool.buf = NULL;
    enc->bitstreamPool.size = 0;
    enc->bitstreamPool.capacity = 0;
    pthread_mutex_unlock(&enc->bitstreamPoolMutex);

    enc->encoder = NULL;
}

static bool destroyEncodeContext(NVDriver *drv, NVContext *nvCtx)
{
    NVEncodeContext *enc = nvCtx->enc;
    bool ok = true;
    if (!enc) {
        return true;
    }

    if (!CHECK_CUDA_RESULT(cu->cuCtxPushCurrent(drv->cudaContext))) {
        if ((nvenc_has_queued_pics(enc) || nvenc_has_pending_outputs(enc)) &&
            enc->context == nvCtx) {
            VAStatus st = nvenc_drain_encoder(drv, nvCtx);
            if (st != VA_STATUS_SUCCESS) {
                ok = false;
            }
        }
        nvenc_clear_pending_outputs(enc);
        nvenc_clear_queued_pics(enc);
        nvenc_context_release_encoder_resources(enc);
        if (CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL))) {
            ok = false;
        }
    } else {
        ok = false;
        nvenc_abandon_pending_outputs(enc);
        nvenc_abandon_queued_pics(enc);
        nvenc_abandon_encoder_resources(enc);
    }

    nv_clear_encode_buffer_refs(drv, enc);
    nv_clear_surface_context_refs(drv, nvCtx);

    nvenc_context_release_host_resources(enc);
    free(enc);
    nvCtx->enc = NULL;
    return ok;
}

static void nvenc_destroy_detached_context(NVDriver *drv, NVEncodeContext *enc)
{
    if (!drv || !enc) {
        return;
    }

    if (!checkCudaErrors(cu->cuCtxPushCurrent(drv->cudaContext), __FILE__, __func__, __LINE__)) {
        nvenc_context_release_encoder_resources(enc);
        checkCudaErrors(cu->cuCtxPopCurrent(NULL), __FILE__, __func__, __LINE__);
    } else {
        nvenc_abandon_encoder_resources(enc);
    }

    nvenc_context_release_host_resources(enc);
    free(enc);
}

static void nvenc_bind_render_targets(NVDriver *drv,
                                      NVContext *nvCtx,
                                      VASurfaceID *render_targets,
                                      int num_render_targets)
{
    if (!drv || !nvCtx || !render_targets || num_render_targets <= 0) {
        return;
    }

    for (int i = 0; i < num_render_targets; i++) {
        NVSurface *surface = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, render_targets[i]);
        if (surface) {
            surface->context = nvCtx;
        }
    }
}

static bool destroyContext(NVDriver *drv, NVContext *nvCtx) {
    if (nvCtx->isEncode) {
        return destroyEncodeContext(drv, nvCtx);
    }
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), false);

    LOG("Signaling resolve thread to exit");
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 5;
    nvCtx->exiting = true;
    pthread_cond_signal(&nvCtx->resolveCondition);
    LOG("Waiting for resolve thread to exit");
    int ret = pthread_timedjoin_np(nvCtx->resolveThread, NULL, &timeout);
    LOG("Finished waiting for resolve thread with %d", ret);

    free(nvCtx->codecData);
    nvCtx->codecData = NULL;

    freeBuffer(&nvCtx->sliceOffsets);
    freeBuffer(&nvCtx->bitstreamBuffer);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), false);

    nv_clear_surface_context_refs(drv, nvCtx);

    return true;
}

static void destroyAllContexts(NVDriver *drv)
{
    if (!drv) {
        return;
    }

    pthread_mutex_lock(&drv->objectCreationMutex);
    ARRAY_FOR_EACH(Object, o, &drv->objects)
        if (!o || o->type != OBJECT_TYPE_CONTEXT || !o->obj) {
            continue;
        }
        LOG("Destroying context object %d during driver teardown", o->id);
        if (!destroyContext(drv, (NVContext*) o->obj)) {
            LOG("Context object %d teardown reported failure", o->id);
        }
    END_FOR_EACH
    pthread_mutex_unlock(&drv->objectCreationMutex);
}

static void deleteAllObjects(NVDriver *drv)
{
    if (!drv) {
        return;
    }

    pthread_mutex_lock(&drv->objectCreationMutex);
    while (drv->objects.size > 0) {
        Object o = (Object) get_element_at(&drv->objects, 0);
        if (!o) {
            remove_element_at(&drv->objects, 0);
            continue;
        }
        LOG("Deleting object %d of type %d", o->id, o->type);
        deleteObject(drv, o->id);
    }
    pthread_mutex_unlock(&drv->objectCreationMutex);
}

NVSurface* nvSurfaceFromSurfaceId(NVDriver *drv, VASurfaceID surf) {
    Object obj = getObject(drv, OBJECT_TYPE_SURFACE, surf);
    if (obj != NULL) {
        NVSurface *suf = (NVSurface*) obj->obj;
        return suf;
    }
    return NULL;
}

int pictureIdxFromSurfaceId(NVDriver *drv, VASurfaceID surfId) {
    NVSurface *surf = nvSurfaceFromSurfaceId(drv, surfId);
    if (surf != NULL) {
        return surf->pictureIdx;
    }
    return -1;
}

static cudaVideoCodec vaToCuCodec(VAProfile profile) {
    for (const NVCodec *c = __start_nvd_codecs; c < __stop_nvd_codecs; c++) {
        cudaVideoCodec cvc = c->computeCudaCodec(profile);
        if (cvc != cudaVideoCodec_NONE) {
            return cvc;
        }
    }

    return cudaVideoCodec_NONE;
}

static bool doesGPUSupportCodec(NVDriver *drv, cudaVideoCodec codec, int bitDepth, cudaVideoChromaFormat chromaFormat, uint32_t *width, uint32_t *height)
{
    CUVIDDECODECAPS videoDecodeCaps = {
        .eCodecType      = codec,
        .eChromaFormat   = chromaFormat,
        .nBitDepthMinus8 = bitDepth - 8
    };

    bool failed = false;
    pthread_mutex_lock(&drv->cudaMutex);
    if (CHECK_CUDA_RESULT(cu->cuCtxPushCurrent(drv->cudaContext))) {
        pthread_mutex_unlock(&drv->cudaMutex);
        return false;
    }
    if (CHECK_CUDA_RESULT(cv->cuvidGetDecoderCaps(&videoDecodeCaps))) {
        failed = true;
    }
    if (CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL))) {
        failed = true;
    }
    pthread_mutex_unlock(&drv->cudaMutex);
    if (failed) {
        return false;
    }

    if (width != NULL) {
        *width = videoDecodeCaps.nMaxWidth;
    }
    if (height != NULL) {
        *height = videoDecodeCaps.nMaxHeight;
    }
    return (videoDecodeCaps.bIsSupported == 1);
}

static void* resolveSurfaces(void *param) {
    NVContext *ctx = (NVContext*) param;
    NVDriver *drv = ctx->drv;
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), NULL);

    LOG("[RT] Resolve thread for %p started", ctx);
    while (!ctx->exiting) {
        //wait for frame on queue
        pthread_mutex_lock(&ctx->resolveMutex);
        while (ctx->surfaceQueueReadIdx == ctx->surfaceQueueWriteIdx) {
            pthread_cond_wait(&ctx->resolveCondition, &ctx->resolveMutex);
            if (ctx->exiting) {
                pthread_mutex_unlock(&ctx->resolveMutex);
                goto out;
            }
        }
        pthread_mutex_unlock(&ctx->resolveMutex);
        //find the last item
        //LOG("Reading from queue: %d %d", ctx->surfaceQueueReadIdx, ctx->surfaceQueueWriteIdx);
        NVSurface *surface = ctx->surfaceQueue[ctx->surfaceQueueReadIdx++];
        if (ctx->surfaceQueueReadIdx >= SURFACE_QUEUE_SIZE) {
            ctx->surfaceQueueReadIdx = 0;
        }

        CUdeviceptr deviceMemory = (CUdeviceptr) NULL;
        unsigned int pitch = 0;

        //map frame
        CUVIDPROCPARAMS procParams = {
            .progressive_frame = surface->progressiveFrame,
            .top_field_first = surface->topFieldFirst,
            .second_field = surface->secondField
        };

        //LOG("Mapping surface %d", surface->pictureIdx);
        if (surface->decodeFailed || CHECK_CUDA_RESULT(cv->cuvidMapVideoFrame(ctx->decoder, surface->pictureIdx, &deviceMemory, &pitch, &procParams))) {
            pthread_mutex_lock(&surface->mutex);
            surface->resolving = 0;
            pthread_cond_signal(&surface->cond);
            pthread_mutex_unlock(&surface->mutex);
            continue;
        }
        //LOG("Mapped surface %d to %p (%d)", surface->pictureIdx, (void*)deviceMemory, pitch);

        //update cuarray
        drv->backend->exportCudaPtr(drv, deviceMemory, surface, pitch);
        //LOG("Surface %d exported", surface->pictureIdx);
        //unmap frame

        CHECK_CUDA_RESULT(cv->cuvidUnmapVideoFrame(ctx->decoder, deviceMemory));
    }
out:
    //release the decoder here to prevent multiple threads attempting it
    if (ctx->decoder != NULL) {
        CUresult result = cv->cuvidDestroyDecoder(ctx->decoder);
        ctx->decoder = NULL;
        if (result != CUDA_SUCCESS) {
            LOG("cuvidDestroyDecoder failed: %d", result);
        }
    }
    LOG("[RT] Resolve thread for %p exiting", ctx);
    return NULL;
}


static VAStatus nvQueryConfigProfiles(
        VADriverContextP ctx,
        VAProfile *profile_list,	/* out */
        int *num_profiles			/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    //now filter out the codecs we don't support
    for (int i = 0; i < drv->profileCount; i++) {
        profile_list[i] = drv->profiles[i];
    }

    *num_profiles = drv->profileCount;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvQueryConfigProfiles2(
        VADriverContextP ctx,
        VAProfile *profile_list,	/* out */
        int *num_profiles			/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    int profiles = 0;
    if (doesGPUSupportCodec(drv, cudaVideoCodec_MPEG2, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileMPEG2Simple;
        profile_list[profiles++] = VAProfileMPEG2Main;
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_MPEG4, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileMPEG4Simple;
        profile_list[profiles++] = VAProfileMPEG4AdvancedSimple;
        profile_list[profiles++] = VAProfileMPEG4Main;
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_VC1, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVC1Simple;
        profile_list[profiles++] = VAProfileVC1Main;
        profile_list[profiles++] = VAProfileVC1Advanced;
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_H264, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileH264Main;
        profile_list[profiles++] = VAProfileH264High;
        profile_list[profiles++] = VAProfileH264ConstrainedBaseline;
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_JPEG, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileJPEGBaseline;
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_H264_SVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileH264StereoHigh;
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_H264_MVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileH264MultiviewHigh;
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_HEVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain;
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_VP8, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVP8Version0_3;
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_VP9, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVP9Profile0; //color depth: 8 bit, 4:2:0
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_AV1, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileAV1Profile0;
    }

    if (drv->supports16BitSurface) {
        if (doesGPUSupportCodec(drv, cudaVideoCodec_HEVC, 10, cudaVideoChromaFormat_420, NULL, NULL)) {
            profile_list[profiles++] = VAProfileHEVCMain10;
        }
        if (doesGPUSupportCodec(drv, cudaVideoCodec_HEVC, 12, cudaVideoChromaFormat_420, NULL, NULL)) {
            profile_list[profiles++] = VAProfileHEVCMain12;
        }
        if (doesGPUSupportCodec(drv, cudaVideoCodec_VP9, 10, cudaVideoChromaFormat_420, NULL, NULL)) {
            profile_list[profiles++] = VAProfileVP9Profile2; //color depth: 10–12 bit, 4:2:0
        }
    }

    if (drv->supports444Surface) {
        if (doesGPUSupportCodec(drv, cudaVideoCodec_HEVC, 8, cudaVideoChromaFormat_444, NULL, NULL)) {
            profile_list[profiles++] = VAProfileHEVCMain444;
        }
        if (doesGPUSupportCodec(drv, cudaVideoCodec_VP9, 8, cudaVideoChromaFormat_444, NULL, NULL)) {
            profile_list[profiles++] = VAProfileVP9Profile1; //color depth: 8 bit, 4:2:2, 4:4:0, 4:4:4
        }
        if (doesGPUSupportCodec(drv, cudaVideoCodec_AV1, 8, cudaVideoChromaFormat_444, NULL, NULL)) {
            profile_list[profiles++] = VAProfileAV1Profile1;
        }

#if VA_CHECK_VERSION(1, 20, 0)
        if (drv->supports16BitSurface) {
            if (doesGPUSupportCodec(drv, cudaVideoCodec_HEVC, 10, cudaVideoChromaFormat_444, NULL, NULL)) {
                profile_list[profiles++] = VAProfileHEVCMain444_10;
            }
            if (doesGPUSupportCodec(drv, cudaVideoCodec_HEVC, 12, cudaVideoChromaFormat_444, NULL, NULL)) {
                profile_list[profiles++] = VAProfileHEVCMain444_12;
            }
            if (doesGPUSupportCodec(drv, cudaVideoCodec_VP9, 10, cudaVideoChromaFormat_444, NULL, NULL)) {
                profile_list[profiles++] = VAProfileVP9Profile3; //color depth: 10–12 bit, 4:2:2, 4:4:0, 4:4:4
            }
        }
#endif
    }

    // Nvidia decoder doesn't support 422 chroma layout
#if 0
    if (doesGPUSupportCodec(drv, cudaVideoCodec_HEVC, 10, cudaVideoChromaFormat_422, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain422_10;
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_HEVC, 12, cudaVideoChromaFormat_422, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain422_12;
    }
#endif

    //now filter out the codecs we don't support
    for (int i = 0; i < profiles; i++) {
        if (vaToCuCodec(profile_list[i]) == cudaVideoCodec_NONE) {
            for (int x = i; x < profiles-1; x++) {
                profile_list[x] = profile_list[x+1];
            }
            profiles--;
            i--;
        }
    }

    *num_profiles = profiles;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvQueryConfigEntrypoints(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint  *entrypoint_list,	/* out */
        int *num_entrypoints			/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    int count = 0;

    if (vaToCuCodec(profile) != cudaVideoCodec_NONE) {
        entrypoint_list[count++] = VAEntrypointVLD;
    }
    if (nvenc_supports_profile(drv, profile)) {
        entrypoint_list[count++] = VAEntrypointEncSlice;
    }

    *num_entrypoints = count;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvGetConfigAttributes(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint entrypoint,
        VAConfigAttrib *attrib_list,	/* in/out */
        int num_attribs
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    if (entrypoint == VAEntrypointEncSlice) {
        if (!nvenc_supports_profile(drv, profile)) {
            return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        }
        nvenc_get_config_attributes(drv, attrib_list, num_attribs);
        return VA_STATUS_SUCCESS;
    }

    if (entrypoint != VAEntrypointVLD) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }
    if (vaToCuCodec(profile) == cudaVideoCodec_NONE) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    //LOG("Got here with profile: %d == %d", profile, vaToCuCodec(profile));

    for (int i = 0; i < num_attribs; i++)
    {
        if (attrib_list[i].type == VAConfigAttribRTFormat)
        {
            attrib_list[i].value = VA_RT_FORMAT_YUV420;
            switch (profile) {
            case VAProfileHEVCMain12:
            case VAProfileVP9Profile2:
                attrib_list[i].value |= VA_RT_FORMAT_YUV420_12;
                // Fall-through
            case VAProfileHEVCMain10:
            case VAProfileAV1Profile0:
                attrib_list[i].value |= VA_RT_FORMAT_YUV420_10;
                break;

            case VAProfileHEVCMain444_12:
            case VAProfileVP9Profile3:
                attrib_list[i].value |= VA_RT_FORMAT_YUV444_12 | VA_RT_FORMAT_YUV420_12;
                // Fall-through
            case VAProfileHEVCMain444_10:
            case VAProfileAV1Profile1:
                attrib_list[i].value |= VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV420_10;
                // Fall-through
            case VAProfileHEVCMain444:
            case VAProfileVP9Profile1:
                attrib_list[i].value |= VA_RT_FORMAT_YUV444;
                break;
            default:
                //do nothing
                break;
            }

            if (!drv->supports16BitSurface) {
                attrib_list[i].value &= ~(VA_RT_FORMAT_YUV420_10 | VA_RT_FORMAT_YUV420_12 | VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV444_12);
            }
            if (!drv->supports444Surface) {
                attrib_list[i].value &= ~(VA_RT_FORMAT_YUV444 | VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV444_12);
            }
        }
        else if (attrib_list[i].type == VAConfigAttribMaxPictureWidth)
        {
            doesGPUSupportCodec(drv, vaToCuCodec(profile), 8, cudaVideoChromaFormat_420, &attrib_list[i].value, NULL);
        }
        else if (attrib_list[i].type == VAConfigAttribMaxPictureHeight)
        {
            doesGPUSupportCodec(drv, vaToCuCodec(profile), 8, cudaVideoChromaFormat_420, NULL, &attrib_list[i].value);
        }
        else
        {
            LOG("unhandled config attribute: %d", attrib_list[i].type);
        }
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateConfig(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint entrypoint,
        VAConfigAttrib *attrib_list,
        int num_attribs,
        VAConfigID *config_id		/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    //LOG("got profile: %d with %d attributes", profile, num_attribs);
    cudaVideoCodec cudaCodec = vaToCuCodec(profile);

    if (entrypoint == VAEntrypointEncSlice) {
        if (!nvenc_supports_profile(drv, profile)) {
            LOG("Encode profile not supported: %d", profile);
            return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        }
    } else if (entrypoint != VAEntrypointVLD) {
        LOG("Entrypoint not supported: %d", entrypoint);
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    } else {
        if (cudaCodec == cudaVideoCodec_NONE) {
            //we don't support this yet
            LOG("Profile not supported: %d", profile);
            return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        }
    }

    Object obj = allocateObject(drv, OBJECT_TYPE_CONFIG, sizeof(NVConfig));
    if (obj == NULL || obj->obj == NULL) {
        LOG("nvCreateConfig: allocateObject failed");
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    NVConfig *cfg = (NVConfig*) obj->obj;
    //this will contain all the attributes the client cares about
    //for (int i = 0; i < num_attribs; i++) {
    //  LOG("got config attrib: %d %d %d", i, attrib_list[i].type, attrib_list[i].value);
    //}
    nvenc_config_init_defaults(cfg, profile, entrypoint, cudaCodec);
    if (entrypoint == VAEntrypointEncSlice) {
        VAStatus rt_st = nvenc_config_apply_rt_format_attribs(cfg, attrib_list, num_attribs);
        if (rt_st != VA_STATUS_SUCCESS) {
            deleteObject(drv, obj->id);
            return rt_st;
        }
        VAStatus rc_st = nvenc_config_apply_rate_control_attribs(cfg, attrib_list, num_attribs);
        if (rc_st != VA_STATUS_SUCCESS) {
            deleteObject(drv, obj->id);
            return rc_st;
        }
    }

    if (drv->supports16BitSurface) {
        switch(cfg->profile) {
        case VAProfileHEVCMain10:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
            cfg->bitDepth = 10;
            break;
        case VAProfileHEVCMain12:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
            cfg->bitDepth = 12;
            break;
        case VAProfileVP9Profile2:
        case VAProfileAV1Profile0:
            // If the user provides an RTFormat, we can use that to identify which decoder
            // configuration is appropriate. If a format is not required here, the caller
            // must pass render targets to createContext so we can use those to establish
            // the surface format and bit depth.
            if (num_attribs > 0 && attrib_list[0].type == VAConfigAttribRTFormat) {
                switch(attrib_list[0].value) {
                case VA_RT_FORMAT_YUV420_12:
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
                    cfg->bitDepth = 12;
                    break;
                case VA_RT_FORMAT_YUV420_10:
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
                    cfg->bitDepth = 10;
                    break;
                default:
                    break;
                }
            } else {
                if (cfg->profile == VAProfileVP9Profile2) {
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
                    cfg->bitDepth = 10;
                } else {
                    LOG("Unable to determine surface type for VP9/AV1 codec due to no RTFormat specified.");
                }
            }
        default:
            break;
        }
    }

    if (drv->supports444Surface) {
        switch(cfg->profile) {
        case VAProfileHEVCMain444:
        case VAProfileVP9Profile1:
        case VAProfileAV1Profile1:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444;
            cfg->chromaFormat = cudaVideoChromaFormat_444;
            cfg->bitDepth = 8;
            break;
        default:
            break;
        }
    }

    if (drv->supports444Surface && drv->supports16BitSurface) {
        switch(cfg->profile) {
        case VAProfileHEVCMain444_10:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
            cfg->chromaFormat = cudaVideoChromaFormat_444;
            cfg->bitDepth = 10;
            break;
        case VAProfileHEVCMain444_12:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
            cfg->chromaFormat = cudaVideoChromaFormat_444;
            cfg->bitDepth = 12;
            break;
        case VAProfileVP9Profile3:
        case VAProfileAV1Profile1:
            // If the user provides an RTFormat, we can use that to identify which decoder
            // configuration is appropriate. If a format is not required here, the caller
            // must pass render targets to createContext so we can use those to establish
            // the surface format and bit depth.
            if (num_attribs > 0 && attrib_list[0].type == VAConfigAttribRTFormat) {
                switch(attrib_list[0].value) {
                case VA_RT_FORMAT_YUV444_12:
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
                    cfg->chromaFormat = cudaVideoChromaFormat_444;
                    cfg->bitDepth = 12;
                    break;
                case VA_RT_FORMAT_YUV444_10:
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
                    cfg->chromaFormat = cudaVideoChromaFormat_444;
                    cfg->bitDepth = 10;
                    break;
                case VA_RT_FORMAT_YUV444:
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444;
                    cfg->chromaFormat = cudaVideoChromaFormat_444;
                    cfg->bitDepth = 8;
                    break;
                default:
                    break;
                }
            } else {
                if (cfg->profile == VAProfileVP9Profile3) {
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
                    cfg->chromaFormat = cudaVideoChromaFormat_444;
                    cfg->bitDepth = 10;
                }
            }
        default:
            break;
        }
    }

    *config_id = obj->id;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvDestroyConfig(
        VADriverContextP ctx,
        VAConfigID config_id
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    deleteObject(drv, config_id);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvQueryConfigAttributes(
        VADriverContextP ctx,
        VAConfigID config_id,
        VAProfile *profile,		/* out */
        VAEntrypoint *entrypoint, 	/* out */
        VAConfigAttrib *attrib_list,	/* out */
        int *num_attribs		/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVConfig *cfg = (NVConfig*) getObjectPtr(drv, OBJECT_TYPE_CONFIG, config_id);

    if (cfg == NULL) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    if (cfg->entrypoint == VAEntrypointEncSlice) {
        nvenc_query_config_attributes(cfg, profile, entrypoint, attrib_list, num_attribs);
        return VA_STATUS_SUCCESS;
    }

    *profile = cfg->profile;
    *entrypoint = cfg->entrypoint;
    int i = 0;
    attrib_list[i].value = VA_RT_FORMAT_YUV420;
    attrib_list[i].type = VAConfigAttribRTFormat;
    switch (cfg->profile) {
    case VAProfileHEVCMain12:
    case VAProfileVP9Profile2:
        attrib_list[i].value |= VA_RT_FORMAT_YUV420_12;
        // Fall-through
    case VAProfileHEVCMain10:
    case VAProfileAV1Profile0:
        attrib_list[i].value |= VA_RT_FORMAT_YUV420_10;
        break;

    case VAProfileHEVCMain444_12:
    case VAProfileVP9Profile3:
        attrib_list[i].value |= VA_RT_FORMAT_YUV444_12 | VA_RT_FORMAT_YUV420_12;
        // Fall-through
    case VAProfileHEVCMain444_10:
    case VAProfileAV1Profile1:
        attrib_list[i].value |= VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV420_10;
        // Fall-through
    case VAProfileHEVCMain444:
    case VAProfileVP9Profile1:
        attrib_list[i].value |= VA_RT_FORMAT_YUV444;
        break;
    default:
        //do nothing
        break;
    }

    if (!drv->supports16BitSurface) {
        attrib_list[i].value &= ~(VA_RT_FORMAT_YUV420_10 | VA_RT_FORMAT_YUV420_12 | VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV444_12);
    }
    if (!drv->supports444Surface) {
        attrib_list[i].value &= ~(VA_RT_FORMAT_YUV444 | VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV444_12);
    }

    i++;
    *num_attribs = i;
    return VA_STATUS_SUCCESS;
}

static void nv_copy_plane_rows(void *dst_base,
                               size_t dst_pitch,
                               const void *src_base,
                               size_t src_pitch,
                               size_t row_bytes,
                               uint32_t height)
{
    for (uint32_t y = 0; y < height; y++) {
        memcpy((char *)dst_base + dst_pitch * y,
               (const char *)src_base + src_pitch * y,
               row_bytes);
    }
}

static VAStatus nvCreateSurfaces2(
            VADriverContextP    ctx,
            unsigned int        format,
            unsigned int        width,
            unsigned int        height,
            VASurfaceID        *surfaces,
            unsigned int        num_surfaces,
            VASurfaceAttrib    *attrib_list,
            unsigned int        num_attribs
        )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    uint32_t mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
    const void *external_desc = NULL;
    const VASurfaceAttribExternalBuffers *extbuf = NULL;
    const VADRMPRIMESurfaceDescriptor *prime2_desc = NULL;
    if (attrib_list != NULL && num_attribs > 0) {
        for (unsigned int i = 0; i < num_attribs; i++) {
            if (attrib_list[i].type == VASurfaceAttribMemoryType &&
                attrib_list[i].value.type == VAGenericValueTypeInteger) {
                mem_type = (uint32_t)attrib_list[i].value.value.i;
            } else if (attrib_list[i].type == VASurfaceAttribExternalBufferDescriptor &&
                       attrib_list[i].value.type == VAGenericValueTypePointer) {
                external_desc = attrib_list[i].value.value.p;
            }
        }
    }

    if (external_desc != NULL) {
        VAStatus ext_parse_st =
            nv_parse_external_import_descriptor(mem_type, external_desc, &extbuf, &prime2_desc);
        if (ext_parse_st != VA_STATUS_SUCCESS) {
            return ext_parse_st;
        }
    }

    const bool use_external_prime2 = prime2_desc != NULL;
    const bool use_external_legacy = extbuf != NULL;
    const bool use_external = use_external_legacy || use_external_prime2;
    if (use_external) {
        if (num_surfaces != 1) {
            LOG("External buffer import only supports 1 surface (requested %u)", num_surfaces);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
    }

    cudaVideoSurfaceFormat nvFormat;
    cudaVideoChromaFormat chromaFormat;
    int bitdepth;

    switch (format)
    {
    case VA_RT_FORMAT_YUV420:
        nvFormat = cudaVideoSurfaceFormat_NV12;
        chromaFormat = cudaVideoChromaFormat_420;
        bitdepth = 8;
        break;
    case VA_RT_FORMAT_YUV420_10:
        nvFormat = cudaVideoSurfaceFormat_P016;
        chromaFormat = cudaVideoChromaFormat_420;
        bitdepth = 10;
        break;
    case VA_RT_FORMAT_YUV420_12:
        nvFormat = cudaVideoSurfaceFormat_P016;
        chromaFormat = cudaVideoChromaFormat_420;
        bitdepth = 12;
        break;
    case VA_RT_FORMAT_YUV444:
        nvFormat = cudaVideoSurfaceFormat_YUV444;
        chromaFormat = cudaVideoChromaFormat_444;
        bitdepth = 8;
        break;
    case VA_RT_FORMAT_YUV444_10:
        nvFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
        chromaFormat = cudaVideoChromaFormat_444;
        bitdepth = 10;
        break;
    case VA_RT_FORMAT_YUV444_12:
        nvFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
        chromaFormat = cudaVideoChromaFormat_444;
        bitdepth = 12;
        break;
    
    default:
        LOG("Unknown format: %X", format);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    if (use_external_legacy) {
        VAStatus ext_st = nv_validate_external_buffer_import(format, extbuf, nvFormat, bitdepth);
        if (ext_st != VA_STATUS_SUCCESS) {
            return ext_st;
        }
    }
    if (use_external_prime2) {
        VAStatus ext_st = nv_validate_prime2_buffer_import(format, prime2_desc, nvFormat, bitdepth);
        if (ext_st != VA_STATUS_SUCCESS) {
            return ext_st;
        }
    }

    // If there is subsampled chroma make the size a multple of it
    switch(chromaFormat) {
        case cudaVideoChromaFormat_422:
            width = ROUND_UP(width, 2);
            break;
        case cudaVideoChromaFormat_420:
            width = ROUND_UP(width, 2);
            height = ROUND_UP(height, 2);
            break;
        default:
            // no change needed
            break;
    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    for (uint32_t i = 0; i < num_surfaces; i++) {
        Object surfaceObject = allocateObject(drv, OBJECT_TYPE_SURFACE, sizeof(NVSurface));
        surfaces[i] = surfaceObject->id;
        NVSurface *suf = (NVSurface*) surfaceObject->obj;
        suf->width = width;
        suf->height = height;
        suf->format = nvFormat;
        suf->pictureIdx = -1;
        suf->bitDepth = bitdepth;
        suf->context = NULL;
        suf->chromaFormat = chromaFormat;
        pthread_mutex_init(&suf->mutex, NULL);
        pthread_cond_init(&suf->cond, NULL);
        LOG("Creating surface %ux%u, format %X (%p)", width, height, format, suf);

        if (use_external) {
            BackingImage *img = use_external_prime2 ?
                                nv_import_external_prime2_buffer(drv, suf, prime2_desc) :
                                nv_import_external_buffer(drv, suf, extbuf);
            if (!img) {
                LOG("External buffer import failed");
                for (uint32_t j = 0; j <= i; j++) {
                    NVSurface *cleanup = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surfaces[j]);
                    if (cleanup && cleanup->backingImage) {
                        drv->backend->detachBackingImageFromSurface(drv, cleanup);
                    }
                    if (surfaces[j]) {
                        deleteObject(drv, surfaces[j]);
                    }
                }
                CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            img->surface = suf;
            suf->backingImage = img;
        }
    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateSurfaces(
        VADriverContextP ctx,
        int width,
        int height,
        int format,
        int num_surfaces,
        VASurfaceID *surfaces		/* out */
    )
{
    return nvCreateSurfaces2(ctx, format, width, height, surfaces, num_surfaces, NULL, 0);
}

static VAStatus nvSyncSurface(
        VADriverContextP ctx,
        VASurfaceID render_target
    );

static NVContext *nvenc_surface_get_context(const NVSurface *surface)
{
    if (!surface) {
        return NULL;
    }
    if (surface->encCtx && surface->encCtx->isEncode) {
        return surface->encCtx;
    }
    if (surface->context && surface->context->isEncode) {
        return surface->context;
    }
    return NULL;
}

static VAStatus nvDestroySurfaces(
        VADriverContextP ctx,
        VASurfaceID *surface_list,
        int num_surfaces
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    for (int i = 0; i < num_surfaces; i++) {
        NVSurface *surface = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surface_list[i]);
        if (!surface) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (nvenc_surface_get_context(surface)) {
            VAStatus st = nvSyncSurface(ctx, surface_list[i]);
            if (st != VA_STATUS_SUCCESS) {
                return st;
            }
        }
    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    for (int i = 0; i < num_surfaces; i++) {
        NVSurface *surface = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surface_list[i]);
        if (!surface) {
            CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        LOG("Destroying surface %d (%p)", surface->pictureIdx, surface);

        NVContext *encCtx = nvenc_surface_get_context(surface);
        if (surface->encReg && encCtx && encCtx->enc) {
            NVEncodeContext *enc = encCtx->enc;
            nvenc_unregister_surface(enc, surface->encReg);
            nvenc_remove_surface_reg(enc, surface->encReg);
            free(surface->encReg);
            surface->encReg = NULL;
        }
        if (surface->encDevPtr) {
            cu->cuMemFree(surface->encDevPtr);
            surface->encDevPtr = 0;
            surface->encPitch = 0;
            surface->encAllocHeight = 0;
            surface->encInputValid = false;
        }
        if (surface->encCopyEventValid) {
            cu->cuEventDestroy(surface->encCopyEvent);
            surface->encCopyEventValid = false;
            surface->encCopyEventPending = false;
        }

        drv->backend->detachBackingImageFromSurface(drv, surface);

        deleteObject(drv, surface_list[i]);
    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateEncodeContext(
        VADriverContextP ctx,
        NVDriver *drv,
        NVConfig *cfg,
        int picture_width,
        int picture_height,
        VASurfaceID *render_targets,
        int num_render_targets,
        VAContextID *context)
{
    (void)ctx;
    uint32_t coded_width = nvenc_profile_coded_dim(cfg->profile, (uint32_t)picture_width);
    uint32_t coded_height = nvenc_profile_coded_dim(cfg->profile, (uint32_t)picture_height);

    if (!nvenc_supports_profile(drv, cfg->profile)) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    if (picture_width <= 0 || picture_height <= 0) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (drv->nvencCaps.widthMin > 0 && drv->nvencCaps.heightMin > 0 &&
        drv->nvencCaps.widthMax > 0 && drv->nvencCaps.heightMax > 0) {
        if ((int)coded_width < drv->nvencCaps.widthMin ||
            (int)coded_width > drv->nvencCaps.widthMax ||
            (int)coded_height < drv->nvencCaps.heightMin ||
            (int)coded_height > drv->nvencCaps.heightMax) {
            LOG("Encode context resolution %dx%d outside NVENC caps %dx%d..%dx%d",
                coded_width, coded_height,
                drv->nvencCaps.widthMin, drv->nvencCaps.heightMin,
                drv->nvencCaps.widthMax, drv->nvencCaps.heightMax);
            return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
        }
    }

    VAStatus st = nvenc_validate_render_targets(drv,
                                                (uint32_t)picture_width,
                                                (uint32_t)picture_height,
                                                coded_width,
                                                coded_height,
                                                render_targets,
                                                num_render_targets,
                                                true);
    if (st != VA_STATUS_SUCCESS) {
        return st;
    }

    NVEncodeContext *enc = NULL;
    st = nvenc_context_create(drv, cfg, picture_width, picture_height, &enc);
    if (st != VA_STATUS_SUCCESS) {
        return st;
    }

    Object contextObj = allocateObject(drv, OBJECT_TYPE_CONTEXT, sizeof(NVContext));
    if (!contextObj || !contextObj->obj) {
        if (contextObj) {
            deleteObject(drv, contextObj->id);
        }
        nvenc_destroy_detached_context(drv, enc);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    NVContext *nvCtx = (NVContext*) contextObj->obj;
    nvCtx->drv = drv;
    nvCtx->profile = cfg->profile;
    nvCtx->entrypoint = cfg->entrypoint;
    nvCtx->ownerTid = (uint64_t)nv_gettid();
    nvCtx->width = picture_width;
    nvCtx->height = picture_height;
    nvCtx->isEncode = true;
    nvCtx->enc = enc;
    enc->context = nvCtx;
    nvenc_bind_render_targets(drv, nvCtx, render_targets, num_render_targets);

    *context = contextObj->id;
    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateContext(
        VADriverContextP ctx,
        VAConfigID config_id,
        int picture_width,
        int picture_height,
        int flag,
        VASurfaceID *render_targets,
        int num_render_targets,
        VAContextID *context		/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVConfig *cfg = (NVConfig*) getObjectPtr(drv, OBJECT_TYPE_CONFIG, config_id);

    if (cfg == NULL) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    LOG("Creating context with %d render targets, at %dx%d", num_render_targets, picture_width, picture_height);

    if (cfg->entrypoint == VAEntrypointEncSlice) {
        return nvCreateEncodeContext(ctx, drv, cfg, picture_width, picture_height,
                                     render_targets, num_render_targets, context);
    }
    //find the codec they've selected
    const NVCodec *selectedCodec = NULL;
    for (const NVCodec *c = __start_nvd_codecs; c < __stop_nvd_codecs; c++) {
        for (int i = 0; i < c->supportedProfileCount; i++) {
            if (c->supportedProfiles[i] == cfg->profile) {
                selectedCodec = c;
                break;
            }
        }
    }
    if (selectedCodec == NULL) {
        LOG("Unable to find codec for profile: %d", cfg->profile);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    if (num_render_targets) {
        // Update the decoder configuration to match the passed in surfaces.
        NVSurface *surface = (NVSurface *) getObjectPtr(drv, OBJECT_TYPE_SURFACE, render_targets[0]);
        if (!surface) {
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        cfg->surfaceFormat = surface->format;
        cfg->chromaFormat = surface->chromaFormat;
        cfg->bitDepth = surface->bitDepth;
    }

     // Setting to maximun value if num_render_targets == 0 to prevent picture index overflow as additional surfaces can be created after calling nvCreateContext
    int surfaceCount = num_render_targets > 0 ? num_render_targets : 32;

    if (surfaceCount > 32) {
        LOG("Application requested %d surface(s), limiting to 32. This may cause issues.", surfaceCount);
        surfaceCount = 32;
    }

    int display_area_width = picture_width;
    int display_area_height = picture_height;

    // If we're increasing the surface size for the chroma subsampling,
    // increase the displayArea to match
    switch(cfg->chromaFormat) {
        case cudaVideoChromaFormat_422:
            display_area_width = ROUND_UP(display_area_width, 2);
            break;
        case cudaVideoChromaFormat_420:
            display_area_width = ROUND_UP(display_area_width, 2);
            display_area_height = ROUND_UP(display_area_height, 2);
            break;
        default:
            // no change needed
            break;
    }

    CUVIDDECODECREATEINFO vdci = {
        .ulWidth             = vdci.ulMaxWidth  = vdci.ulTargetWidth  = picture_width,
        .ulHeight            = vdci.ulMaxHeight = vdci.ulTargetHeight = picture_height,
        .CodecType           = cfg->cudaCodec,
        .ulCreationFlags     = cudaVideoCreate_PreferCUVID,
        .ulIntraDecodeOnly   = 0, //TODO (flag & VA_PROGRESSIVE) != 0
        .display_area.right  = display_area_width,
        .display_area.bottom = display_area_height,
        .ChromaFormat        = cfg->chromaFormat,
        .OutputFormat        = cfg->surfaceFormat,
        .bitDepthMinus8      = cfg->bitDepth - 8,
        .DeinterlaceMode     = cudaVideoDeinterlaceMode_Weave,

        //we only ever map one frame at a time, so we can set this to 1
        //it isn't particually efficient to do this, but it is simple
        .ulNumOutputSurfaces = 1,
        //just allocate as many surfaces as have been created since we can never have as much information as the decode to guess correctly
        .ulNumDecodeSurfaces = surfaceCount,
        //.vidLock             = drv->vidLock
    };

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    CUvideodecoder decoder;
    CHECK_CUDA_RESULT_RETURN(cv->cuvidCreateDecoder(&decoder, &vdci), VA_STATUS_ERROR_ALLOCATION_FAILED);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    Object contextObj = allocateObject(drv, OBJECT_TYPE_CONTEXT, sizeof(NVContext));
    LOG("Creating decoder: %p for context id: %d", decoder, contextObj ? contextObj->id : VA_INVALID_ID);

    NVContext *nvCtx = (NVContext*) contextObj->obj;
    nvCtx->drv = drv;
    nvCtx->decoder = decoder;
    nvCtx->profile = cfg->profile;
    nvCtx->entrypoint = cfg->entrypoint;
    nvCtx->ownerTid = (uint64_t)nv_gettid();
    nvCtx->width = picture_width;
    nvCtx->height = picture_height;
    nvCtx->isEncode = false;
    nvCtx->enc = NULL;
    nvCtx->codec = selectedCodec;
    nvCtx->surfaceCount = surfaceCount;
    nvCtx->firstKeyframeValid = false;
    
    pthread_mutexattr_t attrib;
    pthread_mutexattr_init(&attrib);
    pthread_mutexattr_settype(&attrib, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&nvCtx->surfaceCreationMutex, &attrib);

    pthread_mutex_init(&nvCtx->resolveMutex, NULL);
    pthread_cond_init(&nvCtx->resolveCondition, NULL);
    int err = pthread_create(&nvCtx->resolveThread, NULL, &resolveSurfaces, nvCtx);
    if (err != 0) {
        LOG("Unable to create resolve thread: %d", err);
        deleteObject(drv, contextObj->id);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    *context = contextObj->id;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvDestroyContext(
        VADriverContextP ctx,
        VAContextID context)
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("Destroying context: %d", context);
    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);

    if (nvCtx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    VAStatus ret = VA_STATUS_SUCCESS;

    if (!destroyContext(drv, nvCtx)) {
        ret = VA_STATUS_ERROR_OPERATION_FAILED;
    }

    deleteObject(drv, context);

    return ret;
}

static VAStatus nvCreateBuffer(
        VADriverContextP ctx,
        VAContextID context,		/* in */
        VABufferType type,		/* in */
        unsigned int size,		/* in */
        unsigned int num_elements,	/* in */
        void *data,			/* in */
        VABufferID *buf_id
    )
{
    //LOG("got buffer %p, type %x, size %u, elements %u", data, type, size, num_elements);
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);
    if (nvCtx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    //HACK: This is an awful hack to support VP8 videos when running within FFMPEG.
    //VA-API doesn't pass enough information for NVDEC to work with, but the information is there
    //just before the start of the buffer that was passed to us.
    size_t offset = 0;
    if (nvCtx->profile == VAProfileVP8Version0_3 && type == VASliceDataBufferType) {
        offset = ((uintptr_t) data) & 0xf;
        data = ((char *) data) - offset;
        size += (unsigned int)offset;
    }

    //TODO should pool these as most of the time these should be the same size
    Object bufferObject = allocateObject(drv, OBJECT_TYPE_BUFFER, sizeof(NVBuffer));
    if (bufferObject == NULL || bufferObject->obj == NULL) {
        if (bufferObject != NULL) {
            deleteObject(drv, bufferObject->id);
        }
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    *buf_id = bufferObject->id;

    NVBuffer *buf = (NVBuffer*) bufferObject->obj;
    buf->bufferType = type;
    buf->elements = num_elements;
    buf->size = num_elements * size;
    if (type == VAEncCodedBufferType) {
        VAStatus st = nvenc_alloc_coded_buffer(buf, buf->size, data);
        if (st != VA_STATUS_SUCCESS) {
            deleteObject(drv, bufferObject->id);
            return st;
        }
    } else {
        buf->ptr = memalign(16, buf->size);
    }
    buf->offset = offset;

    if (type != VAEncCodedBufferType && buf->ptr == NULL) {
        LOG("Unable to allocate buffer of %zu bytes", buf->size);
        deleteObject(drv, bufferObject->id);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    if (data != NULL)
    {
        if (type != VAEncCodedBufferType) {
            memcpy(buf->ptr, data, buf->size);
        }
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvBufferSetNumElements(
        VADriverContextP ctx,
        VABufferID buf_id,	/* in */
        unsigned int num_elements	/* in */
    )
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSyncBuffer(
        VADriverContextP ctx,
        VABufferID buf_id,	/* in */
        uint64_t timeout_ns
    );

static VAStatus nvSyncSurface(
        VADriverContextP ctx,
        VASurfaceID render_target
    );

static VAStatus nvPutImage(
        VADriverContextP ctx,
        VASurfaceID surface,
        VAImageID image,
        int src_x,
        int src_y,
        unsigned int src_width,
        unsigned int src_height,
        int dest_x,
        int dest_y,
        unsigned int dest_width,
        unsigned int dest_height
    );

static VAStatus nvMapBuffer(
        VADriverContextP ctx,
        VABufferID buf_id,	/* in */
        void **pbuf         /* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVBuffer *buf = getObjectPtr(drv, OBJECT_TYPE_BUFFER, buf_id);

    if (buf == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    if (buf->bufferType == VAEncCodedBufferType) {
        if (!buf->codedReady && buf->encCtx) {
            VAStatus sync_st = nvSyncBuffer(ctx, buf_id, NVD_VA_TIMEOUT_INFINITE);
            if (sync_st != VA_STATUS_SUCCESS) {
                return sync_st;
            }
        }
        nvenc_map_coded_buffer(buf, pbuf);
    } else {
        *pbuf = buf->ptr;
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvUnmapBuffer(
        VADriverContextP ctx,
        VABufferID buf_id	/* in */
    )
{
    return VA_STATUS_SUCCESS;
}

static VAStatus nvDestroyBuffer(
        VADriverContextP ctx,
        VABufferID buffer_id
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVBuffer *buf = getObjectPtr(drv, OBJECT_TYPE_BUFFER, buffer_id);

    if (buf == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    if (buf->bufferType == VAEncCodedBufferType && buf->encCtx) {
        NVEncodeContext *enc = buf->encCtx;
        if (!buf->codedReady && enc &&
            !nvenc_coded_buffer_has_pending_output(enc, buf) &&
            nvenc_coded_buffer_is_queued_or_reordered(enc, buf)) {
            nvenc_discard_unsubmitted_coded_buffer(enc, buf);
        } else {
            VAStatus st = nvenc_sync_buffer(drv, buf, NVD_VA_TIMEOUT_INFINITE);
            if (st != VA_STATUS_SUCCESS) {
                return st;
            }
        }
    }

    if (buf->ptr != NULL) {
        free(buf->ptr);
        buf->ptr = NULL;
    }
    if (buf->codedBuf != NULL || buf->packedHeader != NULL) {
        nvenc_release_coded_buffer(buf);
    }

    deleteObject(drv, buffer_id);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvBeginPicture(
        VADriverContextP ctx,
        VAContextID context,
        VASurfaceID render_target
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);
    NVSurface *surface = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, render_target);

    if (nvCtx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    if (surface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (nvCtx->isEncode) {
        NVEncodeContext *enc = nvCtx->enc;
        if (!enc) {
            return VA_STATUS_ERROR_INVALID_CONTEXT;
        }
        VASurfaceID begin_target = render_target;
        VAStatus st = nvenc_validate_render_targets(drv,
                                                    enc->width,
                                                    enc->height,
                                                    nvenc_context_coded_width(nvCtx),
                                                    nvenc_context_coded_height(nvCtx),
                                                    &begin_target,
                                                    1,
                                                    !enc->haveSeq);
        if (st != VA_STATUS_SUCCESS) {
            return st;
        }
        nvenc_begin_picture_state(nvCtx, surface, render_target);
        return VA_STATUS_SUCCESS;
    }

    if (surface->context != NULL && surface->context != nvCtx) {
        //this surface was last used on a different context, we need to free up the backing image (it might not be the correct size)
        if (surface->backingImage != NULL) {
            nvenc_surface_invalidate_encode_input(surface);
            drv->backend->detachBackingImageFromSurface(drv, surface);
        }
        //...and reset the pictureIdx
        surface->pictureIdx = -1;
    }

    //if this surface hasn't been used before, give it a new picture index
    if (surface->pictureIdx == -1) {
        if (nvCtx->currentPictureId == nvCtx->surfaceCount) {
            return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
        }
        surface->pictureIdx = nvCtx->currentPictureId++;
    }

    //I don't know if we actually need to lock here, nothing should be waiting
    //until after this function returns...
    pthread_mutex_lock(&surface->mutex);
    surface->resolving = 1;
    pthread_mutex_unlock(&surface->mutex);

    memset(&nvCtx->pPicParams, 0, sizeof(CUVIDPICPARAMS));
    nvCtx->renderTarget = surface;
    nvCtx->renderTarget->progressiveFrame = true; //assume we're producing progressive frame unless the codec says otherwise
    nvCtx->pPicParams.CurrPicIdx = nvCtx->renderTarget->pictureIdx;
    if (nvCtx->codec != NULL && nvCtx->codec->beginPicture != NULL) {
        nvCtx->codec->beginPicture(nvCtx);
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvEncodeRenderPicture(
        NVDriver *drv,
        NVContext *nvCtx,
        VABufferID *buffers,
        int num_buffers)
{
    NVEncodeContext *enc = nvCtx->enc;
    VAStatus st;
    if (!enc) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    for (int i = 0; i < num_buffers; i++) {
        NVBuffer *buf = (NVBuffer*) getObjectPtr(drv, OBJECT_TYPE_BUFFER, buffers[i]);
        if (buf == NULL) {
            LOG("Invalid encode buffer id: %d", buffers[i]);
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }

        st = nvenc_handle_render_buffer(enc, buf);
        if (st != VA_STATUS_SUCCESS) {
            LOG("Rejecting encode buffer %d type=%d status=%d", buffers[i], buf->bufferType, st);
            return st;
        }
    }

    nvenc_finalize_render_picture(enc);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvRenderPicture(
        VADriverContextP ctx,
        VAContextID context,
        VABufferID *buffers,
        int num_buffers
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);

    if (nvCtx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    if (nvCtx->isEncode) {
        return nvEncodeRenderPicture(drv, nvCtx, buffers, num_buffers);
    }

    CUVIDPICPARAMS *picParams = &nvCtx->pPicParams;

    for (int i = 0; i < num_buffers; i++) {
        NVBuffer *buf = (NVBuffer*) getObjectPtr(drv, OBJECT_TYPE_BUFFER, buffers[i]);
        if (buf == NULL || buf->ptr == NULL) {
            LOG("Invalid buffer detected, skipping: %d", buffers[i]);
            continue;
        }
        HandlerFunc func = nvCtx->codec->handlers[buf->bufferType];
        if (func != NULL) {
            func(nvCtx, buf, picParams);
        } else {
            LOG("Unhandled buffer type: %d", buf->bufferType);
        }
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvEncodeEndPicture(
        NVDriver *drv,
        NVContext *nvCtx)
{
    NVEncodeContext *enc = nvCtx->enc;
    if (!enc) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    VAStatus st = nvenc_validate_picture_submit(enc);
    if (st != VA_STATUS_SUCCESS) {
        return st;
    }

    NVSurface *surface = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, enc->inputSurface);
    if (!surface) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    NVBuffer *codedBuf = (NVBuffer*) getObjectPtr(drv, OBJECT_TYPE_BUFFER, enc->codedBufId);
    if (!codedBuf || codedBuf->bufferType != VAEncCodedBufferType) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    VASurfaceID input_surface = enc->inputSurface;
    st = nvenc_validate_render_targets(drv,
                                       enc->width,
                                       enc->height,
                                       nvenc_context_coded_width(nvCtx),
                                       nvenc_context_coded_height(nvCtx),
                                       &input_surface,
                                       1,
                                       false);
    if (st != VA_STATUS_SUCCESS) {
        return st;
    }
    st = nvenc_prepare_coded_buffer(enc, codedBuf);
    if (st != VA_STATUS_SUCCESS) {
        return st;
    }

    return nvenc_submit_prepared_picture(drv, nvCtx, surface, codedBuf);
}

static VAStatus nvEndPicture(
        VADriverContextP ctx,
        VAContextID context
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);

    if (nvCtx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (nvCtx->isEncode) {
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
        VAStatus st = nvEncodeEndPicture(drv, nvCtx);
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
        return st;
    }
    if (nvCtx->decoder == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    CUVIDPICPARAMS *picParams = &nvCtx->pPicParams;

    picParams->pBitstreamData = nvCtx->bitstreamBuffer.buf;
    picParams->pSliceDataOffsets = nvCtx->sliceOffsets.buf;
    nvCtx->bitstreamBuffer.size = 0;
    nvCtx->sliceOffsets.size = 0;

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
    CUresult result = cv->cuvidDecodePicture(nvCtx->decoder, picParams);
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    VAStatus status = VA_STATUS_SUCCESS;

    if (result != CUDA_SUCCESS) {
        LOG("cuvidDecodePicture failed: %d", result);
        status = VA_STATUS_ERROR_DECODING_ERROR;
    }
    //LOG("Decoded frame successfully to idx: %d (%p)", picParams->CurrPicIdx, nvCtx->renderTarget);

    NVSurface *surface = nvCtx->renderTarget;

    surface->context = nvCtx;
    surface->topFieldFirst = !picParams->bottom_field_flag;
    surface->secondField = picParams->second_field;
    surface->decodeFailed = status != VA_STATUS_SUCCESS;

    //TODO check we're not overflowing the queue
    pthread_mutex_lock(&nvCtx->resolveMutex);
    nvCtx->surfaceQueue[nvCtx->surfaceQueueWriteIdx++] = nvCtx->renderTarget;
    if (nvCtx->surfaceQueueWriteIdx >= SURFACE_QUEUE_SIZE) {
        nvCtx->surfaceQueueWriteIdx = 0;
    }
    pthread_mutex_unlock(&nvCtx->resolveMutex);

    //Wake up the resolve thread
    pthread_cond_signal(&nvCtx->resolveCondition);

    return status;
}

static bool nvenc_surface_in_queued_pics(NVEncodeContext *enc, NVSurface *surface)
{
    if (!enc || !surface) {
        return false;
    }
    bool found = false;
    pthread_mutex_lock(&enc->queueMutex);
    for (uint32_t i = 0; i < enc->queuedPics.size; i++) {
        NVEncQueuedPic *queued = (NVEncQueuedPic*) get_element_at(&enc->queuedPics, i);
        if (queued && queued->surface == surface) {
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&enc->queueMutex);
    return found;
}

static VAStatus nvSyncSurface(
        VADriverContextP ctx,
        VASurfaceID render_target
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVSurface *surface = getObjectPtr(drv, OBJECT_TYPE_SURFACE, render_target);

    if (surface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    NVContext *encCtx = nvenc_surface_get_context(surface);
    if (encCtx) {
        NVEncodeContext *enc = encCtx ? encCtx->enc : NULL;
        if (enc && encCtx) {
            CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
            VAStatus st = VA_STATUS_SUCCESS;
            bool in_flight = nvenc_surface_has_pending_output(enc, surface) ||
                             nvenc_surface_in_queued_pics(enc, surface);
            while (surface->resolving && in_flight) {
                if (nvenc_use_internal_reorder(enc)) {
                    nvenc_process_reorder_queue(drv, encCtx, false);
                }
                bool resolved_any = false;
                while (true) {
                    int32_t pending_idx = nvenc_find_pending_output_index_for_surface(enc, surface);
                    if (pending_idx < 0) {
                        break;
                    }
                    st = nvenc_resolve_pending_output_ex(enc, (uint32_t)pending_idx, false, NULL);
                    if (st != VA_STATUS_SUCCESS) {
                        break;
                    }
                    resolved_any = true;
                }
                if (st != VA_STATUS_SUCCESS) {
                    break;
                }
                if (!resolved_any) {
                    struct timespec ts = { 0, 1000000 };
                    nanosleep(&ts, NULL);
                }
                in_flight = nvenc_surface_has_pending_output(enc, surface) ||
                            nvenc_surface_in_queued_pics(enc, surface);
            }
            CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
            if (st != VA_STATUS_SUCCESS) {
                return st;
            }
        }
        pthread_mutex_lock(&surface->mutex);
        while (surface->resolving) {
            pthread_cond_wait(&surface->cond, &surface->mutex);
        }
        pthread_mutex_unlock(&surface->mutex);
        if (surface->decodeFailed) {
            return VA_STATUS_ERROR_DECODING_ERROR;
        }
        return VA_STATUS_SUCCESS;
    }

    //LOG("Syncing on surface: %d (%p)", surface->pictureIdx, surface);

    //wait for resolve to occur before synchronising
    pthread_mutex_lock(&surface->mutex);
    if (surface->resolving) {
        //LOG("Surface %d not resolved, waiting", surface->pictureIdx);
        pthread_cond_wait(&surface->cond, &surface->mutex);
    }
    pthread_mutex_unlock(&surface->mutex);

    //LOG("Surface %d resolved (%p)", surface->pictureIdx, surface);

    if (surface->decodeFailed) {
        return VA_STATUS_ERROR_DECODING_ERROR;
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvSyncBuffer(
        VADriverContextP ctx,
        VABufferID buf_id,
        uint64_t timeout_ns
    )
{
    if (buf_id == VA_INVALID_ID) {
        return VA_STATUS_SUCCESS;
    }
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVBuffer *buf = getObjectPtr(drv, OBJECT_TYPE_BUFFER, buf_id);
    if (buf == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (buf->bufferType == VAEncCodedBufferType && buf->encCtx) {
        return nvenc_sync_buffer(drv, buf, timeout_ns);
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus nvQuerySurfaceStatus(
        VADriverContextP ctx,
        VASurfaceID render_target,
        VASurfaceStatus *status	/* out */
    )
{
    if (status == NULL) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVSurface *surface = getObjectPtr(drv, OBJECT_TYPE_SURFACE, render_target);
    if (surface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    pthread_mutex_lock(&surface->mutex);
    if (surface->resolving) {
        *status = VASurfaceRendering;
    } else {
        *status = VASurfaceReady;
    }
    pthread_mutex_unlock(&surface->mutex);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvQuerySurfaceError(
        VADriverContextP ctx,
        VASurfaceID render_target,
        VAStatus error_status,
        void **error_info /*out*/
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvPutSurface(
        VADriverContextP ctx,
        VASurfaceID surface,
        void* draw, /* Drawable of window system */
        short srcx,
        short srcy,
        unsigned short srcw,
        unsigned short srch,
        short destx,
        short desty,
        unsigned short destw,
        unsigned short desth,
        VARectangle *cliprects, /* client supplied clip list */
        unsigned int number_cliprects, /* number of clip rects in the clip list */
        unsigned int flags /* de-interlacing flags */
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQueryImageFormats(
        VADriverContextP ctx,
        VAImageFormat *format_list,        /* out */
        int *num_formats           /* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    //LOG("In %s", __func__);

    *num_formats = 0;
    for (unsigned int i = NV_FORMAT_NONE + 1; i < ARRAY_SIZE(formatsInfo); i++) {
        if (formatsInfo[i].is16bits && !drv->supports16BitSurface) {
            continue;
        }
        if (formatsInfo[i].isYuv444 && !drv->supports444Surface) {
            continue;
        }
        format_list[(*num_formats)++] = formatsInfo[i].vaFormat;
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateImage(
        VADriverContextP ctx,
        VAImageFormat *format,
        int width,
        int height,
        VAImage *image     /* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVFormat nvFormat = nvFormatFromVaFormat(format->fourcc);
    const NVFormatInfo *fmtInfo = &formatsInfo[nvFormat];

    if (nvFormat == NV_FORMAT_NONE) {
        return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
    }

    Object imageObj = allocateObject(drv, OBJECT_TYPE_IMAGE, sizeof(NVImage));
    image->image_id = imageObj->id;

    //LOG("created image id: %d", imageObj->id);

    NVImage *img = (NVImage*) imageObj->obj;
    img->width = width;
    img->height = height;
    img->format = nvFormat;
    //allocate buffer to hold image when we copy down from the GPU
    //TODO could probably put these in a pool, they appear to be allocated, used, then freed
    Object imageBufferObject = allocateObject(drv, OBJECT_TYPE_BUFFER, sizeof(NVBuffer));
    NVBuffer *imageBuffer = (NVBuffer*) imageBufferObject->obj;
    imageBuffer->bufferType = VAImageBufferType;
    imageBuffer->size = 0;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        imageBuffer->size += nv_format_plane_size(fmtInfo, i, width, height);
    }
    if (imageBuffer->size > UINT32_MAX) {
        deleteObject(drv, imageBufferObject->id);
        deleteObject(drv, imageObj->id);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    imageBuffer->elements = 1;
    imageBuffer->ptr = memalign(16, imageBuffer->size);
    if (imageBuffer->ptr == NULL) {
        LOG("Unable to allocate image buffer of %zu bytes", imageBuffer->size);
        deleteObject(drv, imageBufferObject->id);
        deleteObject(drv, imageObj->id);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    img->imageBuffer = imageBuffer;

    memcpy(&image->format, format, sizeof(VAImageFormat));
    image->buf = imageBufferObject->id;	/* image data buffer */
    /*
     * Image data will be stored in a buffer of type VAImageBufferType to facilitate
     * data store on the server side for optimal performance. The buffer will be
     * created by the CreateImage function, and proper storage allocated based on the image
     * size and format. This buffer is managed by the library implementation, and
     * accessed by the client through the buffer Map/Unmap functions.
     */
    image->width = width;
    image->height = height;
    image->data_size = imageBuffer->size;
    image->num_planes = fmtInfo->numPlanes;	/* can not be greater than 3 */
    /*
     * An array indicating the scanline pitch in bytes for each plane.
     * Each plane may have a different pitch. Maximum 3 planes for planar formats
     */
    memset(image->pitches, 0, sizeof(image->pitches));
    memset(image->offsets, 0, sizeof(image->offsets));
    memset(img->pitches, 0, sizeof(img->pitches));
    memset(img->offsets, 0, sizeof(img->offsets));
    /*
     * An array indicating the byte offset from the beginning of the image data
     * to the start of each plane.
     */
    size_t plane_offset = 0;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        size_t plane_pitch = nv_format_plane_row_bytes(fmtInfo, i, width);
        if (plane_pitch > UINT32_MAX || plane_offset > UINT32_MAX) {
            free(imageBuffer->ptr);
            imageBuffer->ptr = NULL;
            deleteObject(drv, imageBufferObject->id);
            deleteObject(drv, imageObj->id);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        image->pitches[i] = (uint32_t)plane_pitch;
        image->offsets[i] = (uint32_t)plane_offset;
        img->pitches[i] = image->pitches[i];
        img->offsets[i] = image->offsets[i];
        plane_offset += nv_format_plane_size(fmtInfo, i, width, height);
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvDeriveImage(
        VADriverContextP ctx,
        VASurfaceID surface,
        VAImage *image     /* out */
    )
{
    //LOG("In %s", __func__);
    //FAILED because we don't support it
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

static VAStatus nvDestroyImage(
        VADriverContextP ctx,
        VAImageID image
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVImage *img = (NVImage*) getObjectPtr(drv, OBJECT_TYPE_IMAGE, image);

    if (img == NULL) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    Object imageBufferObj = getObjectByPtr(drv, OBJECT_TYPE_BUFFER, img->imageBuffer);

    if (imageBufferObj != NULL) {
        if (img->imageBuffer && img->imageBuffer->ptr != NULL) {
            free(img->imageBuffer->ptr);
            img->imageBuffer->ptr = NULL;
        }

        deleteObject(drv, imageBufferObj->id);
    }

    deleteObject(drv, image);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvSetImagePalette(
            VADriverContextP ctx,
            VAImageID image,
            /*
                 * pointer to an array holding the palette data.  The size of the array is
                 * num_palette_entries * entry_bytes in size.  The order of the components
                 * in the palette is described by the component_order in VAImage struct
                 */
                unsigned char *palette
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvGetImage(
        VADriverContextP ctx,
        VASurfaceID surface,
        int x,     /* coordinates of the upper left source pixel */
        int y,
        unsigned int width, /* width and height of the region */
        unsigned int height,
        VAImageID image
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    NVSurface *surfaceObj = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surface);
    NVImage *imageObj = (NVImage*) getObjectPtr(drv, OBJECT_TYPE_IMAGE, image);

    if (surfaceObj == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (imageObj == NULL) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    bool have_live_context = surfaceObj->context != NULL ||
                             nvenc_surface_has_encode_context(surfaceObj);
    const NVFormatInfo *fmtInfo = &formatsInfo[imageObj->format];
    NVFormat surface_fmt = nvenc_surface_expected_format(surfaceObj);
    const NVFormatInfo *surface_fmt_info = surface_fmt != NV_FORMAT_NONE ?
                                           &formatsInfo[surface_fmt] :
                                           fmtInfo;

    if (have_live_context) {
        VAStatus sync_st = nvSyncSurface(ctx, surface);
        if (sync_st != VA_STATUS_SUCCESS) {
            return sync_st;
        }
    } else if (surfaceObj->resolving) {
        LOG("GetImage called on unresolved retained surface %u without a live context", surface);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    bool use_encode_linear = nvenc_surface_uses_encode_path(surfaceObj) &&
                             surfaceObj->encDevPtr != 0 &&
                             (surfaceObj->backingImage == NULL || surfaceObj->encDirectUploadLatest);
    bool use_backing_linear = surfaceObj->backingImage &&
                              surfaceObj->backingImage->linearPtr != 0 &&
                              !surfaceObj->encDirectUploadLatest;
    bool use_backing_host = surfaceObj->backingImage &&
                            nvBackingImageIsHostMappedExternal(surfaceObj->backingImage) &&
                            !surfaceObj->encDirectUploadLatest;

    if (!use_encode_linear && !use_backing_linear && !use_backing_host && !surfaceObj->backingImage) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (use_backing_host) {
        const BackingImage *img = surfaceObj->backingImage;
        if (!nvSyncBackingImageHostAccess(img, false, true)) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
            size_t src_pitch = img->strides[i] > 0 ?
                               (size_t)img->strides[i] :
                               nv_format_plane_row_bytes(surface_fmt_info, i, surfaceObj->width);
            size_t dst_pitch = imageObj->pitches[i] ?
                               imageObj->pitches[i] :
                               nv_format_plane_row_bytes(fmtInfo, i, width);
            size_t row_bytes = nv_format_plane_row_bytes(fmtInfo, i, width);
            uint32_t plane_h = nv_format_plane_height(fmtInfo, i, height);
            const void *src_base = nv_get_host_external_plane_ptr(img, i);
            void *dst_base = (char *)imageObj->imageBuffer->ptr + imageObj->offsets[i];
            if (!src_base) {
                (void)nvSyncBackingImageHostAccess(img, false, false);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            nv_copy_plane_rows(dst_base, dst_pitch, src_base, src_pitch, row_bytes, plane_h);
        }
        if (!nvSyncBackingImageHostAccess(img, false, false)) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        return VA_STATUS_SUCCESS;
    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
    size_t encode_plane_offset = 0;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        size_t dst_pitch = imageObj->pitches[i] ?
                           imageObj->pitches[i] :
                           nv_format_plane_row_bytes(fmtInfo, i, width);
        CUDA_MEMCPY2D memcpy2d = {
            .srcXInBytes = 0, .srcY = 0,
            .dstXInBytes = 0, .dstY = 0,
            .dstMemoryType = CU_MEMORYTYPE_HOST,
            .dstHost = (char *)imageObj->imageBuffer->ptr + imageObj->offsets[i],
            .dstPitch = dst_pitch,
            .WidthInBytes = nv_format_plane_row_bytes(fmtInfo, i, width),
            .Height = nv_format_plane_height(fmtInfo, i, height)
        };

        if (use_encode_linear) {
            memcpy2d.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            memcpy2d.srcDevice = surfaceObj->encDevPtr + encode_plane_offset;
            memcpy2d.srcPitch = surfaceObj->encPitch;
        } else if (use_backing_linear) {
            size_t src_pitch = surfaceObj->backingImage->strides[i] > 0 ?
                               (size_t)surfaceObj->backingImage->strides[i] :
                               nv_format_plane_row_bytes(surface_fmt_info, i, surfaceObj->width);
            memcpy2d.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            memcpy2d.srcDevice = surfaceObj->backingImage->linearPtr + surfaceObj->backingImage->offsets[i];
            memcpy2d.srcPitch = src_pitch;
        } else {
            memcpy2d.srcMemoryType = CU_MEMORYTYPE_ARRAY;
            memcpy2d.srcArray = surfaceObj->backingImage->arrays[i];
        }

        CUresult result = cu->cuMemcpy2D(&memcpy2d);
        if (result != CUDA_SUCCESS) {
            LOG("cuMemcpy2D failed: %d", result);
            CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
            return VA_STATUS_ERROR_DECODING_ERROR;
        }
        if (use_encode_linear) {
            encode_plane_offset += surfaceObj->encPitch *
                                   nv_format_plane_height(surface_fmt_info, i, surfaceObj->height);
        }
    }
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    return VA_STATUS_SUCCESS;
}

static VAStatus nv_copy_image_to_surface(NVDriver *drv,
                                         const NVSurface *surfaceObj,
                                         const NVImage *imageObj,
                                         unsigned int copy_width,
                                         unsigned int copy_height)
{
    if (!drv || !surfaceObj || !surfaceObj->backingImage || !imageObj || !imageObj->imageBuffer ||
        !imageObj->imageBuffer->ptr) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    const BackingImage *img = surfaceObj->backingImage;
    const NVFormatInfo *fmtInfo = &formatsInfo[imageObj->format];
    const NVFormatInfo *surface_fmt_info = &formatsInfo[img->format];
    bool use_linear = img->linearPtr != 0;
    bool use_host_map = nvBackingImageIsHostMappedExternal(img);

    if (use_host_map) {
        if (!nvSyncBackingImageHostAccess(img, true, true)) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
            size_t plane_width = nv_format_plane_row_bytes(fmtInfo, i, copy_width);
            uint32_t plane_height = nv_format_plane_height(fmtInfo, i, copy_height);
            size_t src_pitch = imageObj->pitches[i] ? imageObj->pitches[i] : plane_width;
            size_t src_offset = imageObj->offsets[i];
            size_t dst_pitch = img->strides[i] > 0 ?
                               (size_t)img->strides[i] :
                               nv_format_plane_row_bytes(surface_fmt_info, i, surfaceObj->width);
            void *dst_base = nv_get_host_external_plane_ptr_mut((BackingImage *)img, i);
            const void *src_base = (const char *)imageObj->imageBuffer->ptr + src_offset;
            if (!dst_base) {
                (void)nvSyncBackingImageHostAccess(img, true, false);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            nv_copy_plane_rows(dst_base, dst_pitch, src_base, src_pitch, plane_width, plane_height);
        }
        if (!nvSyncBackingImageHostAccess(img, true, false)) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        return VA_STATUS_SUCCESS;
    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        size_t plane_width = nv_format_plane_row_bytes(fmtInfo, i, copy_width);
        uint32_t plane_height = nv_format_plane_height(fmtInfo, i, copy_height);
        size_t src_pitch = imageObj->pitches[i] ? imageObj->pitches[i] : plane_width;
        size_t src_offset = imageObj->offsets[i];
        CUDA_MEMCPY2D memcpy2d = {
            .srcXInBytes = 0, .srcY = 0,
            .srcMemoryType = CU_MEMORYTYPE_HOST,
            .srcHost = (char *)imageObj->imageBuffer->ptr + src_offset,
            .srcPitch = src_pitch,
            .dstXInBytes = 0, .dstY = 0,
            .WidthInBytes = plane_width,
            .Height = plane_height
        };

        if (use_linear) {
            size_t dst_pitch = img->strides[i] > 0 ?
                               (size_t)img->strides[i] :
                               nv_format_plane_row_bytes(surface_fmt_info, i, surfaceObj->width);
            memcpy2d.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            memcpy2d.dstDevice = img->linearPtr + img->offsets[i];
            memcpy2d.dstPitch = dst_pitch;
        } else {
            if (!img->arrays[i]) {
                CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            memcpy2d.dstMemoryType = CU_MEMORYTYPE_ARRAY;
            memcpy2d.dstArray = img->arrays[i];
        }

        if (CHECK_CUDA_RESULT(cu->cuMemcpy2D(&memcpy2d))) {
            CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
    return VA_STATUS_SUCCESS;
}

static VAStatus nvPutImage(
        VADriverContextP ctx,
        VASurfaceID surface,
        VAImageID image,
        int src_x,
        int src_y,
        unsigned int src_width,
        unsigned int src_height,
        int dest_x,
        int dest_y,
        unsigned int dest_width,
        unsigned int dest_height
    )
{
    LOG("In %s", __func__);
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    uint32_t copy_width = src_width;
    uint32_t copy_height = src_height;

    NVSurface *surfaceObj = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surface);
    NVImage *imageObj = (NVImage*) getObjectPtr(drv, OBJECT_TYPE_IMAGE, image);

    if (surfaceObj == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (imageObj == NULL) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    bool is_encode_surface = nvenc_surface_uses_encode_path(surfaceObj);
    if (!is_encode_surface && imageObj->format == NV_FORMAT_NV12) {
        if (nvenc_try_bind_surface_to_encode_context(drv, surfaceObj)) {
            is_encode_surface = nvenc_surface_uses_encode_path(surfaceObj);
        }
    }

    if (nvenc_surface_has_encode_context(surfaceObj) && surfaceObj->resolving) {
        VAStatus sync_st = nvSyncSurface(ctx, surface);
        if (sync_st != VA_STATUS_SUCCESS) {
            return sync_st;
        }
    }

    if (src_x != 0 || src_y != 0 || dest_x != 0 || dest_y != 0 ||
        src_width != dest_width || src_height != dest_height) {
        LOG("vaPutImage only supports origin-aligned uploads: src=(%d,%d %ux%u) dst=(%d,%d %ux%u)",
            src_x, src_y, src_width, src_height, dest_x, dest_y, dest_width, dest_height);
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }

    NVFormat surfaceFmt = nvenc_surface_expected_format(surfaceObj);
    if (surfaceFmt == NV_FORMAT_NONE) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    if (is_encode_surface) {
        if (surfaceFmt != NV_FORMAT_NV12) {
            LOG("Encode vaPutImage requires NV12 surface, got format %d", surfaceFmt);
            return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        }
        if (imageObj->format != NV_FORMAT_NV12) {
            return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
        }

        /*
         * Chromium and similar clients can upload only the visible rect while
         * keeping the VAImage allocated at the coded size. Preserve the caller's
         * requested copy dimensions and let the encode upload path clear the
         * remaining coded padding deterministically.
         */
    } else if (imageObj->format != surfaceFmt) {
        LOG("vaPutImage image/surface format mismatch: image=%d surface=%d",
            imageObj->format, surfaceFmt);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    if (is_encode_surface) {
        return nvenc_put_image(drv, surfaceObj, imageObj, copy_width, copy_height);
    }

    if (copy_width != surfaceObj->width || copy_height != surfaceObj->height) {
        LOG("vaPutImage non-encode upload size mismatch: visible=%ux%u surface=%ux%u",
            copy_width, copy_height, surfaceObj->width, surfaceObj->height);
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }

    if (!surfaceObj->backingImage) {
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
        if (!drv->backend->realiseSurface(drv, surfaceObj)) {
            CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
    }

    if (!surfaceObj->backingImage) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (surfaceObj->backingImage->format != surfaceFmt) {
        LOG("vaPutImage backing image format mismatch: backing=%d expected=%d",
            surfaceObj->backingImage->format, surfaceFmt);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    VAStatus copy_st = nv_copy_image_to_surface(drv, surfaceObj, imageObj, copy_width, copy_height);
    if (copy_st != VA_STATUS_SUCCESS) {
        return copy_st;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus nvQuerySubpictureFormats(
        VADriverContextP ctx,
        VAImageFormat *format_list,        /* out */
        unsigned int *flags,       /* out */
        unsigned int *num_formats  /* out */
    )
{
    LOG("In %s", __func__);
    *num_formats = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateSubpicture(
        VADriverContextP ctx,
        VAImageID image,
        VASubpictureID *subpicture   /* out */
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvDestroySubpicture(
        VADriverContextP ctx,
        VASubpictureID subpicture
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetSubpictureImage(
                VADriverContextP ctx,
                VASubpictureID subpicture,
                VAImageID image
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetSubpictureChromakey(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        unsigned int chromakey_min,
        unsigned int chromakey_max,
        unsigned int chromakey_mask
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetSubpictureGlobalAlpha(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        float global_alpha
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvAssociateSubpicture(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        VASurfaceID *target_surfaces,
        int num_surfaces,
        short src_x, /* upper left offset in subpicture */
        short src_y,
        unsigned short src_width,
        unsigned short src_height,
        short dest_x, /* upper left offset in surface */
        short dest_y,
        unsigned short dest_width,
        unsigned short dest_height,
        /*
         * whether to enable chroma-keying or global-alpha
         * see VA_SUBPICTURE_XXX values
         */
        unsigned int flags
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvDeassociateSubpicture(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        VASurfaceID *target_surfaces,
        int num_surfaces
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQueryDisplayAttributes(
        VADriverContextP ctx,
        VADisplayAttribute *attr_list,	/* out */
        int *num_attributes		/* out */
        )
{
    LOG("In %s", __func__);
    *num_attributes = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus nvGetDisplayAttributes(
        VADriverContextP ctx,
        VADisplayAttribute *attr_list,	/* in/out */
        int num_attributes
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetDisplayAttributes(
        VADriverContextP ctx,
                VADisplayAttribute *attr_list,
                int num_attributes
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQuerySurfaceAttributes(
        VADriverContextP    ctx,
	    VAConfigID          config,
	    VASurfaceAttrib    *attrib_list,
	    unsigned int       *num_attribs
	)
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVConfig *cfg = (NVConfig*) getObjectPtr(drv, OBJECT_TYPE_CONFIG, config);

    if (cfg == NULL) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    if (cfg->entrypoint == VAEntrypointEncSlice) {
        return nvenc_query_surface_attributes(drv, attrib_list, num_attribs);
    }

    //LOG("with %d (%d) %p %d", cfg->cudaCodec, cfg->bitDepth, attrib_list, *num_attribs);

    if (cfg->chromaFormat != cudaVideoChromaFormat_420 && cfg->chromaFormat != cudaVideoChromaFormat_444) {
        //TODO not sure what pixel formats are needed for 422 formats
        LOG("Unknown chrome format: %d", cfg->chromaFormat);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    if ((cfg->chromaFormat == cudaVideoChromaFormat_444 || cfg->surfaceFormat == cudaVideoSurfaceFormat_YUV444_16Bit) && !drv->supports444Surface) {
        //TODO not sure what pixel formats are needed for 422 and 444 formats
        LOG("YUV444 surfaces not supported: %d", cfg->chromaFormat);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    if (cfg->surfaceFormat == cudaVideoSurfaceFormat_P016 && !drv->supports16BitSurface) {
        //TODO not sure what pixel formats are needed for 422 and 444 formats
        LOG("16 bits surfaces not supported: %d", cfg->chromaFormat);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    if (num_attribs == NULL) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    unsigned int cnt = 4;
    if (cfg->chromaFormat == cudaVideoChromaFormat_444) {
        cnt += 1;
#if VA_CHECK_VERSION(1, 20, 0)
        cnt += 1;
#endif
    } else {
        cnt += 1;
        if (drv->supports16BitSurface) {
            cnt += 3;
        }
    }
    if (attrib_list == NULL) {
        *num_attribs = cnt;
        return VA_STATUS_SUCCESS;
    }
    if (*num_attribs < cnt) {
        *num_attribs = cnt;
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    *num_attribs = cnt;

    if (attrib_list != NULL) {
        CUVIDDECODECAPS videoDecodeCaps = {
            .eCodecType      = cfg->cudaCodec,
            .eChromaFormat   = cfg->chromaFormat,
            .nBitDepthMinus8 = cfg->bitDepth - 8
        };

        bool failed = false;
        pthread_mutex_lock(&drv->cudaMutex);
        if (CHECK_CUDA_RESULT(cu->cuCtxPushCurrent(drv->cudaContext))) {
            pthread_mutex_unlock(&drv->cudaMutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (CHECK_CUDA_RESULT(cv->cuvidGetDecoderCaps(&videoDecodeCaps))) {
            failed = true;
        }
        if (CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL))) {
            failed = true;
        }
        pthread_mutex_unlock(&drv->cudaMutex);
        if (failed) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        attrib_list[0].type = VASurfaceAttribMinWidth;
        attrib_list[0].flags = 0;
        attrib_list[0].value.type = VAGenericValueTypeInteger;
        attrib_list[0].value.value.i = videoDecodeCaps.nMinWidth;

        attrib_list[1].type = VASurfaceAttribMinHeight;
        attrib_list[1].flags = 0;
        attrib_list[1].value.type = VAGenericValueTypeInteger;
        attrib_list[1].value.value.i = videoDecodeCaps.nMinHeight;

        attrib_list[2].type = VASurfaceAttribMaxWidth;
        attrib_list[2].flags = 0;
        attrib_list[2].value.type = VAGenericValueTypeInteger;
        attrib_list[2].value.value.i = videoDecodeCaps.nMaxWidth;

        attrib_list[3].type = VASurfaceAttribMaxHeight;
        attrib_list[3].flags = 0;
        attrib_list[3].value.type = VAGenericValueTypeInteger;
        attrib_list[3].value.value.i = videoDecodeCaps.nMaxHeight;

        //LOG("Returning constraints: width: %d - %d, height: %d - %d", attrib_list[0].value.value.i, attrib_list[2].value.value.i, attrib_list[1].value.value.i, attrib_list[3].value.value.i);

        int attrib_idx = 4;

        /* returning all the surfaces here probably isn't the best thing we could do
         * but we don't always have enough information to determine exactly which
         * pixel formats should be used (for instance, AV1 10-bit videos) */
        if (cfg->chromaFormat == cudaVideoChromaFormat_444) {
            attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
            attrib_list[attrib_idx].flags = 0;
            attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
            attrib_list[attrib_idx].value.value.i = VA_FOURCC_444P;
            attrib_idx += 1;
#if VA_CHECK_VERSION(1, 20, 0)
            attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
            attrib_list[attrib_idx].flags = 0;
            attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
            attrib_list[attrib_idx].value.value.i = VA_FOURCC_Q416;
            attrib_idx += 1;
#endif
        } else {
            attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
            attrib_list[attrib_idx].flags = 0;
            attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
            attrib_list[attrib_idx].value.value.i = VA_FOURCC_NV12;
            attrib_idx += 1;
            if (drv->supports16BitSurface) {
                attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
                attrib_list[attrib_idx].flags = 0;
                attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
                attrib_list[attrib_idx].value.value.i = VA_FOURCC_P010;
                attrib_idx += 1;
                attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
                attrib_list[attrib_idx].flags = 0;
                attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
                attrib_list[attrib_idx].value.value.i = VA_FOURCC_P012;
                attrib_idx += 1;
                attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
                attrib_list[attrib_idx].flags = 0;
                attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
                attrib_list[attrib_idx].value.value.i = VA_FOURCC_P016;
                attrib_idx += 1;
            }
        }
    }

    return VA_STATUS_SUCCESS;
}

/* used by va trace */
static VAStatus nvBufferInfo(
           VADriverContextP ctx,      /* in */
           VABufferID buf_id,         /* in */
           VABufferType *type,        /* out */
           unsigned int *size,        /* out */
           unsigned int *num_elements /* out */
)
{
    LOG("In %s", __func__);
    *size=0;
    *num_elements=0;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvAcquireBufferHandle(
            VADriverContextP    ctx,
            VABufferID          buf_id,         /* in */
            VABufferInfo *      buf_info        /* in/out */
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvReleaseBufferHandle(
            VADriverContextP    ctx,
            VABufferID          buf_id          /* in */
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

//        /* lock/unlock surface for external access */
static VAStatus nvLockSurface(
        VADriverContextP ctx,
        VASurfaceID surface,
        unsigned int *fourcc, /* out  for follow argument */
        unsigned int *luma_stride,
        unsigned int *chroma_u_stride,
        unsigned int *chroma_v_stride,
        unsigned int *luma_offset,
        unsigned int *chroma_u_offset,
        unsigned int *chroma_v_offset,
        unsigned int *buffer_name, /* if it is not NULL, assign the low lever
                                    * surface buffer name
                                    */
        void **buffer /* if it is not NULL, map the surface buffer for
                       * CPU access
                       */
)
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvUnlockSurface(
        VADriverContextP ctx,
                VASurfaceID surface
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvCreateMFContext(
            VADriverContextP ctx,
            VAMFContextID *mfe_context    /* out */
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvMFAddContext(
            VADriverContextP ctx,
            VAMFContextID mf_context,
            VAContextID context
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvMFReleaseContext(
            VADriverContextP ctx,
            VAMFContextID mf_context,
            VAContextID context
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvMFSubmit(
            VADriverContextP ctx,
            VAMFContextID mf_context,
            VAContextID *contexts,
            int num_contexts
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
static VAStatus nvCreateBuffer2(
            VADriverContextP ctx,
            VAContextID context,                /* in */
            VABufferType type,                  /* in */
            unsigned int width,                 /* in */
            unsigned int height,                /* in */
            unsigned int *unit_size,            /* out */
            unsigned int *pitch,                /* out */
            VABufferID *buf_id                  /* out */
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQueryProcessingRate(
            VADriverContextP ctx,               /* in */
            VAConfigID config_id,               /* in */
            VAProcessingRateParameter *proc_buf,/* in */
            unsigned int *processing_rate	/* out */
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvExportSurfaceHandle(
            VADriverContextP    ctx,
            VASurfaceID         surface_id,     /* in */
            uint32_t            mem_type,       /* in */
            uint32_t            flags,          /* in */
            void               *descriptor      /* out */
)
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    VAStatus status = VA_STATUS_SUCCESS;

    if ((mem_type & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) == 0) {
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    if ((flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) == 0) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    NVSurface *surface = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surface_id);
    if (surface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    //LOG("Exporting surface: %d (%p)", surface->pictureIdx, surface);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    if (!drv->backend->realiseSurface(drv, surface)) {
        LOG("Unable to export surface");
        status = VA_STATUS_ERROR_ALLOCATION_FAILED;
        goto out;
    }

    if (nvenc_surface_uses_encode_path(surface) && surface->encDirectUploadLatest) {
        if (!surface->encDevPtr || surface->encPitch == 0 || surface->encPitch > UINT32_MAX) {
            LOG("Unable to export encode surface %u with stale direct upload state", surface_id);
            status = VA_STATUS_ERROR_OPERATION_FAILED;
            goto out;
        }
        if (!drv->backend->exportCudaPtr(drv, surface->encDevPtr, surface, (uint32_t)surface->encPitch)) {
            LOG("Unable to sync encode upload before export");
            status = VA_STATUS_ERROR_OPERATION_FAILED;
            goto out;
        }
    }

    VADRMPRIMESurfaceDescriptor *ptr = (VADRMPRIMESurfaceDescriptor*) descriptor;

    if (!drv->backend->fillExportDescriptor(drv, surface, ptr)) {
        LOG("Unable to fill export descriptor");
        status = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out;
    }

    // LOG("Exporting with w:%d h:%d o:%d p:%d m:%" PRIx64 " o:%d p:%d m:%" PRIx64, ptr->width, ptr->height, ptr->layers[0].offset[0],
    //                                                             ptr->layers[0].pitch[0], ptr->objects[0].drm_format_modifier,
    //                                                             ptr->layers[1].offset[0], ptr->layers[1].pitch[0],
    //                                                             ptr->objects[1].drm_format_modifier);

out:
    if (CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL))) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    return status;
}

static VAStatus nvTerminate( VADriverContextP ctx )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("Terminating %p", ctx);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    /* Drain/destroy contexts before render targets and backing storage disappear. */
    destroyAllContexts(drv);
    drv->backend->destroyAllBackingImage(drv);

    deleteAllObjects(drv);
    nv_destroy_imported_object_cache(drv);

    drv->backend->releaseExporter(drv);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    pthread_mutex_lock(&concurrency_mutex);
    instances--;
    LOG("Now have %d (%d max) instances", instances, max_instances);
    pthread_mutex_unlock(&concurrency_mutex);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxDestroy(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
    drv->cudaContext = NULL;

    free(drv);

    return VA_STATUS_SUCCESS;
}

#define VTABLE(func) .va ## func = &nv ## func
static const struct VADriverVTable vtable = {
    VTABLE(Terminate),
    VTABLE(QueryConfigProfiles),
    VTABLE(QueryConfigEntrypoints),
    VTABLE(QueryConfigAttributes),
    VTABLE(CreateConfig),
    VTABLE(DestroyConfig),
    VTABLE(GetConfigAttributes),
    VTABLE(CreateSurfaces),
    VTABLE(CreateSurfaces2),
    VTABLE(DestroySurfaces),
    VTABLE(CreateContext),
    VTABLE(DestroyContext),
    VTABLE(CreateBuffer),
    VTABLE(BufferSetNumElements),
    VTABLE(MapBuffer),
    VTABLE(UnmapBuffer),
    VTABLE(DestroyBuffer),
    VTABLE(BeginPicture),
    VTABLE(RenderPicture),
    VTABLE(EndPicture),
    VTABLE(SyncSurface),
#if NVD_HAVE_VA_SYNCBUFFER
    VTABLE(SyncBuffer),
#endif
    VTABLE(QuerySurfaceStatus),
    VTABLE(QuerySurfaceError),
    VTABLE(PutSurface),
    VTABLE(QueryImageFormats),
    VTABLE(CreateImage),
    VTABLE(DeriveImage),
    VTABLE(DestroyImage),
    VTABLE(SetImagePalette),
    VTABLE(GetImage),
    VTABLE(PutImage),
    VTABLE(QuerySubpictureFormats),
    VTABLE(CreateSubpicture),
    VTABLE(DestroySubpicture),
    VTABLE(SetSubpictureImage),
    VTABLE(SetSubpictureChromakey),
    VTABLE(SetSubpictureGlobalAlpha),
    VTABLE(AssociateSubpicture),
    VTABLE(DeassociateSubpicture),
    VTABLE(QueryDisplayAttributes),
    VTABLE(GetDisplayAttributes),
    VTABLE(SetDisplayAttributes),
    VTABLE(QuerySurfaceAttributes),
    VTABLE(BufferInfo),
    VTABLE(AcquireBufferHandle),
    VTABLE(ReleaseBufferHandle),
    VTABLE(LockSurface),
    VTABLE(UnlockSurface),
    VTABLE(CreateMFContext),
    VTABLE(MFAddContext),
    VTABLE(MFReleaseContext),
    VTABLE(MFSubmit),
    VTABLE(CreateBuffer2),
    VTABLE(QueryProcessingRate),
    VTABLE(ExportSurfaceHandle),
};

__attribute__((visibility("default")))
VAStatus __vaDriverInit_1_0(VADriverContextP ctx);

__attribute__((visibility("default")))
VAStatus __vaDriverInit_1_0(VADriverContextP ctx) {
    LOG("Initialising NVIDIA VA-API Driver");

    //drm_state can be passed in with any display type, including X11. But if it's X11, we don't
    //want to use the fd as it'll likely be an Intel GPU, as NVIDIA doesn't support DRI3 at the moment
    bool isDrm = ctx->drm_state != NULL && ((struct drm_state*) ctx->drm_state)->fd > 0;
    int drmFd = (gpu == -1 && isDrm) ? ((struct drm_state*) ctx->drm_state)->fd : -1;

    //check if the drmFd is actually an nvidia one
    LOG("Got DRM FD: %d %d", isDrm, drmFd)
    if (drmFd != -1) {
        if (!isNvidiaDrmFd(drmFd, true)) {
            LOG("Passed in DRM FD does not belong to the NVIDIA driver, ignoring");
            drmFd = -1;
        } else if (!checkModesetParameterFromFd(drmFd)) {
            //we have an NVIDIA fd but no modeset (which means no DMA-BUF support)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    pthread_mutex_lock(&concurrency_mutex);
    LOG("Now have %d (%d max) instances", instances, max_instances);
    if (max_instances > 0 && instances >= max_instances) {
        pthread_mutex_unlock(&concurrency_mutex);
        return VA_STATUS_ERROR_HW_BUSY;
    }
    instances++;
    pthread_mutex_unlock(&concurrency_mutex);

    //check to make sure we initialised the CUDA functions correctly
    if (cu == NULL || cv == NULL) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    NVDriver *drv = (NVDriver*) calloc(1, sizeof(NVDriver));
    ctx->pDriverData = drv;

    drv->cu = cu;
    drv->cv = cv;
    drv->nvenc = nvenc;
    nvenc_driver_init(drv);
    drv->useCorrectNV12Format = true;
    drv->cudaGpuId = gpu;
    //make sure that we want the default GPU, and that a DRM fd that we care about is passed in
    drv->drmFd = drmFd;

    if (drv->cudaGpuId == -1 && drmFd != -1 && isNvidiaDrmFd(drmFd, true)) {
        int probeFd = dup(drmFd);
        if (probeFd != -1 && init_nvdriver(&drv->driverContext, probeFd)) {
            uint8_t drmUuid[16];
            if (get_device_uuid(&drv->driverContext, drmUuid)) {
                int gpuCount = 0;
                if (!CHECK_CUDA_RESULT(cu->cuDeviceGetCount(&gpuCount))) {
                    for (int i = 0; i < gpuCount; i++) {
                        CUuuid uuid;
                        if (!CHECK_CUDA_RESULT(cu->cuDeviceGetUuid(&uuid, i)) &&
                            memcmp(drmUuid, uuid.bytes, sizeof(drmUuid)) == 0) {
                            drv->cudaGpuId = i;
                            LOG("Matched DRM UUID to CUDA GPU index %d", drv->cudaGpuId);
                            break;
                        }
                    }
                }
            }
            free_nvdriver(&drv->driverContext);
        } else if (probeFd != -1) {
            close(probeFd);
        }
        if (drv->cudaGpuId == -1) {
            drv->cudaGpuId = 0;
        }
    }

    if (backend == EGL) {
        LOG("Selecting EGL backend");
        drv->backend = &EGL_BACKEND;
    } else if (backend == DIRECT) {
        LOG("Selecting Direct backend");
        drv->backend = &DIRECT_BACKEND;
        /*
         * Direct export is more reliable when we own the render-node fd instead
         * of reusing the caller's libva display fd.
         */
        drv->drmFd = -1;
    }

    ctx->max_profiles = MAX_PROFILES;
    ctx->max_entrypoints = 2;
    ctx->max_attributes = VAConfigAttribTypeMax;
    ctx->max_display_attributes = 1;
    ctx->max_image_formats = ARRAY_SIZE(formatsInfo) - 1;
    ctx->max_subpic_formats = 1;

    if (backend == DIRECT) {
        ctx->str_vendor = "VA-API NVDEC driver [direct backend]";
    } else if (backend == EGL) {
        ctx->str_vendor = "VA-API NVDEC driver [egl backend]";
    }

    pthread_mutexattr_t attrib;
    pthread_mutexattr_init(&attrib);
    pthread_mutexattr_settype(&attrib, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&drv->objectCreationMutex, &attrib);
    pthread_mutex_init(&drv->imagesMutex, &attrib);
    pthread_mutex_init(&drv->exportMutex, NULL);
    pthread_mutex_init(&drv->nvencCapsMutex, &attrib);
    pthread_mutex_init(&drv->cudaMutex, &attrib);
    pthread_mutex_init(&drv->importedObjectCacheMutex, &attrib);

    if (!drv->backend->initExporter(drv)) {
        LOG("Exporter failed");
        free(drv);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (CHECK_CUDA_RESULT(cu->cuCtxCreate(&drv->cudaContext, CU_CTX_SCHED_BLOCKING_SYNC, drv->cudaGpuId))) {
        drv->backend->releaseExporter(drv);
        free(drv);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    //CHECK_CUDA_RESULT_RETURN(cv->cuvidCtxLockCreate(&drv->vidLock, drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    nvQueryConfigProfiles2(ctx, drv->profiles, &drv->profileCount);

    *ctx->vtable = vtable;
    return VA_STATUS_SUCCESS;
}
