#define _GNU_SOURCE

#include "vabackend.h"
#include "backend-common.h"
#include "kernels.h"

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <va/va_backend.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>

#include <drm_fourcc.h>

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

#ifndef CU_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_FD
#define CU_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_FD ((CUexternalMemoryHandleType)7)
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

static CudaFunctions *cu;
static CuvidFunctions *cv;

extern const NVCodec __start_nvd_codecs[];
extern const NVCodec __stop_nvd_codecs[];

static FILE *LOG_OUTPUT;
static FILE *STATS_OUTPUT;
static bool LOG_DEBUG_ENABLED;

static int gpu = -1;
static enum {
    EGL, DIRECT
} backend = DIRECT;

const NVFormatInfo formatsInfo[] =
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
    [NV_FORMAT_ARGB] = {1, 1, VA_FOURCC_ARGB,      false, false, {{4, DRM_FORMAT_ARGB8888, {0,0}}},                            {VA_FOURCC_ARGB, VA_LSB_FIRST,   32, 0,0,0,0,0}},
};

static NVFormat nvFormatFromVaFormat(uint32_t fourcc) {
    for (uint32_t i = NV_FORMAT_NONE + 1; i < ARRAY_SIZE(formatsInfo); i++) {
        if (formatsInfo[i].vaFormat.fourcc == fourcc) {
            return i;
        }
    }
    return NV_FORMAT_NONE;
}

static bool isRgbFourcc(uint32_t fourcc) {
    return fourcc == VA_FOURCC_ARGB ||
           fourcc == VA_FOURCC_XRGB ||
           fourcc == VA_FOURCC_ABGR ||
           fourcc == VA_FOURCC_XBGR ||
           fourcc == VA_FOURCC_RGBA ||
           fourcc == VA_FOURCC_RGBX ||
           fourcc == VA_FOURCC_BGRA ||
           fourcc == VA_FOURCC_BGRX;
}

static NVFormat nvFormatFromSurfaceFourcc(uint32_t fourcc) {
    if (isRgbFourcc(fourcc)) {
        return NV_FORMAT_ARGB;
    }
    return nvFormatFromVaFormat(fourcc);
}

static const char *fourccString(uint32_t fourcc, char out[5]) {
    out[0] = (char) (fourcc & 0xff);
    out[1] = (char) ((fourcc >> 8) & 0xff);
    out[2] = (char) ((fourcc >> 16) & 0xff);
    out[3] = (char) ((fourcc >> 24) & 0xff);
    out[4] = '\0';
    return out;
}

static void cacheBackingImageFdStat(BackingImage *img, int index) {
    if (img == NULL || index < 0 || index >= 4 || img->fds[index] < 0) {
        return;
    }

    struct stat s;
    if (fstat(img->fds[index], &s) == 0) {
        img->st_dev[index] = s.st_dev;
        img->st_ino[index] = s.st_ino;
    }
}

static bool backingImageFdMatchesStat(const BackingImage *img, const struct stat *fdStat, int index) {
    return img->fds[index] >= 0 &&
           img->st_dev[index] == fdStat->st_dev &&
           img->st_ino[index] == fdStat->st_ino;
}

static bool backingImageMatchesImport(BackingImage *img, const struct stat *fdStat, NVFormat format, uint32_t width, uint32_t height) {
    if (img == NULL || img->isExternalBuffer || img->borrowedCudaResources ||
        img->format != format || img->width != width || img->height != height) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (backingImageFdMatchesStat(img, fdStat, i)) {
            return true;
        }
    }
    return false;
}

static BackingImage *retainBackingImageByFd(NVDriver *drv, int fd, NVFormat format, uint32_t width, uint32_t height) {
    struct stat fdStat;
    if (fd < 0 || fstat(fd, &fdStat) != 0) {
        return NULL;
    }

    BackingImage *ret = NULL;
    pthread_mutex_lock(&drv->imagesMutex);
    ARRAY_FOR_EACH(BackingImage*, img, &drv->images)
        if (backingImageMatchesImport(img, &fdStat, format, width, height)) {
            ret = img;
            atomic_fetch_add(&ret->borrowCount, 1);
            break;
        }
    END_FOR_EACH
    pthread_mutex_unlock(&drv->imagesMutex);

    return ret;
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
    char *nvdLogVerbose = getenv("NVD_LOG_VERBOSE");
    LOG_DEBUG_ENABLED = nvdLogVerbose != NULL && strcmp(nvdLogVerbose, "0") != 0;
    char *nvdStats = getenv("NVD_STATS");
    if (nvdStats != NULL && strcmp(nvdStats, "0") != 0) {
        char *nvdStatsLog = getenv("NVD_STATS_LOG");
        if (nvdStatsLog != NULL) {
            STATS_OUTPUT = fopen(nvdStatsLog, "a");
            if (STATS_OUTPUT == NULL) {
                STATS_OUTPUT = stdout;
            }
        } else if (LOG_OUTPUT != NULL) {
            STATS_OUTPUT = LOG_OUTPUT;
        } else {
            STATS_OUTPUT = stdout;
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

    //Not really much we can do here to abort the loading of the library
    CHECK_CUDA_RESULT(cu->cuInit(0));
}

__attribute__ ((destructor))
static void cleanup() {
    if (cv != NULL) {
        cuvid_free_functions(&cv);
    }
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

bool nvdLogDebugEnabled(void) {
    return LOG_DEBUG_ENABLED;
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

void nvStatsLog(NVDriver *drv, const char *reason) {
    if (drv == NULL || !drv->statsEnabled) {
        return;
    }

    FILE *out = STATS_OUTPUT != NULL ? STATS_OUTPUT : LOG_OUTPUT;
    if (out == NULL) {
        return;
    }

    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    fprintf(out,
        "%10ld.%09ld [%d-%d] Stats[%s]: decoder_creates=%llu decode_pictures=%llu resolve_frames=%llu export_copies=%llu export_host_copies=%llu export_descriptors=%llu single_descriptors=%llu multi_descriptors=%llu videoproc_requests=%llu videoproc_cuda=%llu videoproc_cuda_failures=%llu videoproc_cpu_fallback=%llu\n",
        (long)tp.tv_sec,
        tp.tv_nsec,
        getpid(),
        nv_gettid(),
        reason,
        (unsigned long long) atomic_load_explicit(&drv->stats[NV_STAT_DECODER_CREATES], memory_order_relaxed),
        (unsigned long long) atomic_load_explicit(&drv->stats[NV_STAT_DECODE_PICTURES], memory_order_relaxed),
        (unsigned long long) atomic_load_explicit(&drv->stats[NV_STAT_RESOLVE_FRAMES], memory_order_relaxed),
        (unsigned long long) atomic_load_explicit(&drv->stats[NV_STAT_EXPORT_COPIES], memory_order_relaxed),
        (unsigned long long) atomic_load_explicit(&drv->stats[NV_STAT_EXPORT_HOST_COPIES], memory_order_relaxed),
        (unsigned long long) atomic_load_explicit(&drv->stats[NV_STAT_EXPORT_DESCRIPTORS], memory_order_relaxed),
        (unsigned long long) atomic_load_explicit(&drv->stats[NV_STAT_EXPORT_DESCRIPTORS_SINGLE], memory_order_relaxed),
        (unsigned long long) atomic_load_explicit(&drv->stats[NV_STAT_EXPORT_DESCRIPTORS_MULTI], memory_order_relaxed),
        (unsigned long long) atomic_load_explicit(&drv->stats[NV_STAT_VIDEOPROC_REQUESTS], memory_order_relaxed),
        (unsigned long long) atomic_load_explicit(&drv->stats[NV_STAT_VIDEOPROC_CUDA], memory_order_relaxed),
        (unsigned long long) atomic_load_explicit(&drv->stats[NV_STAT_VIDEOPROC_CUDA_FAILURES], memory_order_relaxed),
        (unsigned long long) atomic_load_explicit(&drv->stats[NV_STAT_VIDEOPROC_CPU_FALLBACK], memory_order_relaxed));
    fflush(out);
}

void nvStatsIncrement(NVDriver *drv, NVStatCounter counter) {
    if (drv == NULL || !drv->statsEnabled || counter >= NV_STAT_COUNT) {
        return;
    }

    uint64_t value = atomic_fetch_add_explicit(&drv->stats[counter], 1, memory_order_relaxed) + 1;
    if (counter == NV_STAT_DECODE_PICTURES &&
        drv->statsLogInterval > 0 &&
        value % drv->statsLogInterval == 0) {
        nvStatsLog(drv, "periodic");
    }
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

static bool destroyContext(NVDriver *drv, NVContext *nvCtx) {
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), false);

    if (nvCtx->decoder != NULL) {
        LOG("Signaling resolve thread to exit");
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 5;
        nvCtx->exiting = true;
        pthread_cond_signal(&nvCtx->resolveCondition);
        LOG("Waiting for resolve thread to exit");
        int ret = pthread_timedjoin_np(nvCtx->resolveThread, NULL, &timeout);
        LOG("Finished waiting for resolve thread with %d", ret);
    }

    free(nvCtx->codecData);
    nvCtx->codecData = NULL;

    freeBuffer(&nvCtx->sliceOffsets);
    freeBuffer(&nvCtx->bitstreamBuffer);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), false);

    return true;
}

static void deleteAllObjects(NVDriver *drv) {
    pthread_mutex_lock(&drv->objectCreationMutex);
    ARRAY_FOR_EACH(Object, o, &drv->objects)
        LOG("Found object %d or type %d", o->id, o->type);
        if (o->type == OBJECT_TYPE_CONTEXT) {
            destroyContext(drv, (NVContext*) o->obj);
        }
        deleteObject(drv, o->id);
    END_FOR_EACH
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

static void setSurfaceBackingImageResolving(NVSurface *surface, bool resolving);

static cudaVideoCodec vaToCuCodec(VAProfile profile) {
    for (const NVCodec *c = __start_nvd_codecs; c < __stop_nvd_codecs; c++) {
        cudaVideoCodec cvc = c->computeCudaCodec(profile);
        if (cvc != cudaVideoCodec_NONE) {
            return cvc;
        }
    }

    return cudaVideoCodec_NONE;
}

static bool doesGPUSupportCodec(cudaVideoCodec codec, int bitDepth, cudaVideoChromaFormat chromaFormat, uint32_t *width, uint32_t *height)
{
    CUVIDDECODECAPS videoDecodeCaps = {
        .eCodecType      = codec,
        .eChromaFormat   = chromaFormat,
        .nBitDepthMinus8 = bitDepth - 8
    };

    CHECK_CUDA_RESULT_RETURN(cv->cuvidGetDecoderCaps(&videoDecodeCaps), false);

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
            setSurfaceBackingImageResolving(surface, false);
            pthread_mutex_lock(&surface->mutex);
            surface->resolving = 0;
            pthread_cond_signal(&surface->cond);
            pthread_mutex_unlock(&surface->mutex);
            continue;
        }
        //LOG("Mapped surface %d to %p (%d)", surface->pictureIdx, (void*)deviceMemory, pitch);

        //update cuarray
        nvStatsIncrement(drv, NV_STAT_RESOLVE_FRAMES);
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
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    int profiles = 0;
    if (doesGPUSupportCodec(cudaVideoCodec_MPEG2, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileMPEG2Simple;
        profile_list[profiles++] = VAProfileMPEG2Main;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_MPEG4, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileMPEG4Simple;
        profile_list[profiles++] = VAProfileMPEG4AdvancedSimple;
        profile_list[profiles++] = VAProfileMPEG4Main;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_VC1, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVC1Simple;
        profile_list[profiles++] = VAProfileVC1Main;
        profile_list[profiles++] = VAProfileVC1Advanced;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_H264, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileH264Main;
        profile_list[profiles++] = VAProfileH264High;
        profile_list[profiles++] = VAProfileH264ConstrainedBaseline;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_JPEG, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileJPEGBaseline;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_H264_SVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileH264StereoHigh;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_H264_MVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileH264MultiviewHigh;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_VP8, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVP8Version0_3;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_VP9, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVP9Profile0; //color depth: 8 bit, 4:2:0
    }
    if (doesGPUSupportCodec(cudaVideoCodec_AV1, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileAV1Profile0;
    }

    if (drv->supports16BitSurface) {
        if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 10, cudaVideoChromaFormat_420, NULL, NULL)) {
            profile_list[profiles++] = VAProfileHEVCMain10;
        }
        if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 12, cudaVideoChromaFormat_420, NULL, NULL)) {
            profile_list[profiles++] = VAProfileHEVCMain12;
        }
        if (doesGPUSupportCodec(cudaVideoCodec_VP9, 10, cudaVideoChromaFormat_420, NULL, NULL)) {
            profile_list[profiles++] = VAProfileVP9Profile2; //color depth: 10–12 bit, 4:2:0
        }
    }

    if (drv->supports444Surface) {
        if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 8, cudaVideoChromaFormat_444, NULL, NULL)) {
            profile_list[profiles++] = VAProfileHEVCMain444;
        }
        if (doesGPUSupportCodec(cudaVideoCodec_VP9, 8, cudaVideoChromaFormat_444, NULL, NULL)) {
            profile_list[profiles++] = VAProfileVP9Profile1; //color depth: 8 bit, 4:2:2, 4:4:0, 4:4:4
        }
        if (doesGPUSupportCodec(cudaVideoCodec_AV1, 8, cudaVideoChromaFormat_444, NULL, NULL)) {
            profile_list[profiles++] = VAProfileAV1Profile1;
        }

#if VA_CHECK_VERSION(1, 20, 0)
        if (drv->supports16BitSurface) {
            if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 10, cudaVideoChromaFormat_444, NULL, NULL)) {
                profile_list[profiles++] = VAProfileHEVCMain444_10;
            }
            if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 12, cudaVideoChromaFormat_444, NULL, NULL)) {
                profile_list[profiles++] = VAProfileHEVCMain444_12;
            }
            if (doesGPUSupportCodec(cudaVideoCodec_VP9, 10, cudaVideoChromaFormat_444, NULL, NULL)) {
                profile_list[profiles++] = VAProfileVP9Profile3; //color depth: 10–12 bit, 4:2:2, 4:4:0, 4:4:4
            }
        }
#endif
    }

    // Nvidia decoder doesn't support 422 chroma layout
#if 0
    if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 10, cudaVideoChromaFormat_422, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain422_10;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 12, cudaVideoChromaFormat_422, NULL, NULL)) {
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

    profile_list[profiles++] = VAProfileNone;

    *num_profiles = profiles;

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvQueryConfigEntrypoints(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint  *entrypoint_list,	/* out */
        int *num_entrypoints			/* out */
    )
{
    if (profile == VAProfileNone) {
        entrypoint_list[0] = VAEntrypointVideoProc;
        *num_entrypoints = 1;
        return VA_STATUS_SUCCESS;
    }

    entrypoint_list[0] = VAEntrypointVLD;
    *num_entrypoints = 1;

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
    if (entrypoint == VAEntrypointVideoProc && profile == VAProfileNone) {
        NVDriver *drv = (NVDriver*) ctx->pDriverData;
        for (int i = 0; i < num_attribs; i++) {
            if (attrib_list[i].type == VAConfigAttribRTFormat) {
                attrib_list[i].value = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32;
                if (drv->supports16BitSurface) {
                    attrib_list[i].value |= VA_RT_FORMAT_YUV420_10;
                }
            } else {
                LOG("unhandled vpp config attribute: %d", attrib_list[i].type);
            }
        }
        return VA_STATUS_SUCCESS;
    }

    if (entrypoint != VAEntrypointVLD) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }

    NVDriver *drv = (NVDriver*) ctx->pDriverData;
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
            doesGPUSupportCodec(vaToCuCodec(profile), 8, cudaVideoChromaFormat_420, &attrib_list[i].value, NULL);
        }
        else if (attrib_list[i].type == VAConfigAttribMaxPictureHeight)
        {
            doesGPUSupportCodec(vaToCuCodec(profile), 8, cudaVideoChromaFormat_420, NULL, &attrib_list[i].value);
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

    if (entrypoint == VAEntrypointVideoProc && profile == VAProfileNone) {
        Object obj = allocateObject(drv, OBJECT_TYPE_CONFIG, sizeof(NVConfig));
        NVConfig *cfg = (NVConfig*) obj->obj;
        cfg->profile = profile;
        cfg->entrypoint = entrypoint;
        cfg->cudaCodec = cudaVideoCodec_NONE;
        cfg->chromaFormat = cudaVideoChromaFormat_420;
        cfg->surfaceFormat = cudaVideoSurfaceFormat_NV12;
        cfg->bitDepth = 8;
        *config_id = obj->id;
        return VA_STATUS_SUCCESS;
    }

    if (cudaCodec == cudaVideoCodec_NONE) {
        //we don't support this yet
        LOG("Profile not supported: %d", profile);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    if (entrypoint != VAEntrypointVLD) {
        LOG("Entrypoint not supported: %d", entrypoint);
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }

    Object obj = allocateObject(drv, OBJECT_TYPE_CONFIG, sizeof(NVConfig));
    NVConfig *cfg = (NVConfig*) obj->obj;
    cfg->profile = profile;
    cfg->entrypoint = entrypoint;

    //this will contain all the attributes the client cares about
    //for (int i = 0; i < num_attribs; i++) {
    //  LOG("got config attrib: %d %d %d", i, attrib_list[i].type, attrib_list[i].value);
    //}

    cfg->cudaCodec = cudaCodec;
    cfg->chromaFormat = cudaVideoChromaFormat_420;
    cfg->surfaceFormat = cudaVideoSurfaceFormat_NV12;
    cfg->bitDepth = 8;
    uint32_t rtFormat = 0;
    for (int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == VAConfigAttribRTFormat) {
            rtFormat = attrib_list[i].value;
            break;
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
            if (rtFormat != 0) {
                if ((rtFormat & VA_RT_FORMAT_YUV420_12) != 0) {
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
                    cfg->bitDepth = 12;
                } else if ((rtFormat & VA_RT_FORMAT_YUV420_10) != 0) {
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
                    cfg->bitDepth = 10;
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
            if (rtFormat != 0) {
                if ((rtFormat & VA_RT_FORMAT_YUV444_12) != 0) {
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
                    cfg->chromaFormat = cudaVideoChromaFormat_444;
                    cfg->bitDepth = 12;
                } else if ((rtFormat & VA_RT_FORMAT_YUV444_10) != 0) {
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
                    cfg->chromaFormat = cudaVideoChromaFormat_444;
                    cfg->bitDepth = 10;
                } else if ((rtFormat & VA_RT_FORMAT_YUV444) != 0) {
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444;
                    cfg->chromaFormat = cudaVideoChromaFormat_444;
                    cfg->bitDepth = 8;
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

    if (cfg->entrypoint == VAEntrypointVideoProc) {
        *profile = cfg->profile;
        *entrypoint = cfg->entrypoint;
        int i = 0;
        attrib_list[i].value = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32;
        attrib_list[i].type = VAConfigAttribRTFormat;
        if (drv->supports16BitSurface) {
            attrib_list[i].value |= VA_RT_FORMAT_YUV420_10;
        }
        i++;
        *num_attribs = i;
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

typedef struct {
    bool valid;
    uint32_t memoryType;
    uint32_t pixelFormat;
    uint32_t width;
    uint32_t height;
    uint32_t dataSize;
    uint32_t numPlanes;
    uint32_t pitches[4];
    uint32_t offsets[4];
    int fds[4];
    uint32_t numFds;
} ImportedSurface;

static void initImportedSurface(ImportedSurface *imported) {
    memset(imported, 0, sizeof(*imported));
    for (int i = 0; i < 4; i++) {
        imported->fds[i] = -1;
    }
}

static void parseSurfaceImportAttributes(VASurfaceAttrib *attribList, unsigned int numAttribs, ImportedSurface *imported) {
    initImportedSurface(imported);
    void *externalDescriptor = NULL;

    for (unsigned int i = 0; i < numAttribs; i++) {
        switch (attribList[i].type) {
        case VASurfaceAttribMemoryType:
            imported->memoryType = (uint32_t) attribList[i].value.value.i;
            break;
        case VASurfaceAttribExternalBufferDescriptor:
            externalDescriptor = attribList[i].value.value.p;
            break;
        case VASurfaceAttribPixelFormat:
            imported->pixelFormat = (uint32_t) attribList[i].value.value.i;
            break;
        default:
            break;
        }
    }

    if (externalDescriptor == NULL) {
        return;
    }

    if ((imported->memoryType & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME) != 0) {
        VASurfaceAttribExternalBuffers *ext = (VASurfaceAttribExternalBuffers*) externalDescriptor;
        imported->pixelFormat = ext->pixel_format != 0 ? ext->pixel_format : imported->pixelFormat;
        imported->width = ext->width;
        imported->height = ext->height;
        imported->dataSize = ext->data_size;
        imported->numPlanes = ext->num_planes;
        imported->numFds = ext->num_buffers;
        if (imported->numPlanes > 4 || imported->numFds == 0 || imported->numFds > 4 || ext->buffers == NULL) {
            imported->valid = false;
            return;
        }
        for (uint32_t i = 0; i < imported->numPlanes; i++) {
            imported->pitches[i] = ext->pitches[i];
            imported->offsets[i] = ext->offsets[i];
        }
        for (uint32_t i = 0; i < imported->numFds; i++) {
            imported->fds[i] = (int) ext->buffers[i];
        }
        imported->valid = true;
    } else if ((imported->memoryType & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) != 0) {
        VADRMPRIMESurfaceDescriptor *desc = (VADRMPRIMESurfaceDescriptor*) externalDescriptor;
        imported->pixelFormat = desc->fourcc;
        imported->width = desc->width;
        imported->height = desc->height;
        imported->numFds = desc->num_objects;
        if (desc->num_layers == 0 || imported->numFds == 0 || imported->numFds > 4 || desc->layers[0].num_planes > 4) {
            imported->valid = false;
            return;
        }
        imported->numPlanes = desc->layers[0].num_planes;
        for (uint32_t i = 0; i < imported->numFds; i++) {
            imported->fds[i] = desc->objects[i].fd;
            imported->dataSize += desc->objects[i].size;
        }
        for (uint32_t i = 0; i < imported->numPlanes; i++) {
            imported->pitches[i] = desc->layers[0].pitch[i];
            imported->offsets[i] = desc->layers[0].offset[i];
        }
        imported->valid = true;
    }
}

static bool importExternalBackingToCuda(NVDriver *drv, BackingImage *img) {
    if (img->fds[0] < 0 || img->totalSize == 0) {
        return false;
    }

    int importFd = dup(img->fds[0]);
    if (importFd < 0) {
        return false;
    }

    CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
        .type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD,
        .handle.fd = importFd,
        .flags = 0,
        .size = img->totalSize
    };
    LOG_DEBUG("Importing external memory to CUDA: fd=%d size=%u", importFd, img->totalSize);
    if (CHECK_CUDA_RESULT(drv->cu->cuImportExternalMemory(&img->extMem, &extMemDesc))) {
        close(importFd);
        return false;
    }
    img->isSingleBuffer = true;

    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC mipmapArrayDesc = {
            .arrayDesc = {
                .Width = img->width >> fmtInfo->plane[i].ss.x,
                .Height = img->height >> fmtInfo->plane[i].ss.y,
                .Depth = 0,
                .Format = fmtInfo->bppc == 1 ? CU_AD_FORMAT_UNSIGNED_INT8 : CU_AD_FORMAT_UNSIGNED_INT16,
                .NumChannels = fmtInfo->plane[i].channelCount,
                .Flags = 0
            },
            .numLevels = 1,
            .offset = (unsigned long long) img->offsets[i]
        };
        if (CHECK_CUDA_RESULT(drv->cu->cuExternalMemoryGetMappedMipmappedArray(&img->cudaImages[i].mipmapArray, img->extMem, &mipmapArrayDesc))) {
            return false;
        }
        if (CHECK_CUDA_RESULT(drv->cu->cuMipmappedArrayGetLevel(&img->arrays[i], img->cudaImages[i].mipmapArray, 0))) {
            return false;
        }
    }

    return true;
}

static bool importExternalBufferToCuda(NVDriver *drv, BackingImage *img) {
    if (img->fds[0] < 0 || img->totalSize == 0 || drv->cu->cuExternalMemoryGetMappedBuffer == NULL) {
        return false;
    }

    int importFd = dup(img->fds[0]);
    if (importFd < 0) {
        return false;
    }

    CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
        .type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD,
        .handle.fd = importFd,
        .flags = 0,
        .size = img->totalSize
    };
    if (CHECK_CUDA_RESULT(drv->cu->cuImportExternalMemory(&img->extMem, &extMemDesc))) {
        close(importFd);
        return false;
    }

    CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufferDesc = {
        .offset = 0,
        .size = img->totalSize,
        .flags = 0
    };
    if (CHECK_CUDA_RESULT(drv->cu->cuExternalMemoryGetMappedBuffer(&img->externalDevicePtr, img->extMem, &bufferDesc))) {
        CHECK_CUDA_RESULT(drv->cu->cuDestroyExternalMemory(img->extMem));
        img->extMem = NULL;
        return false;
    }

    img->externalDeviceSize = img->totalSize;
    img->isSingleBuffer = true;
    char fourcc[5];
    LOG_DEBUG("Imported external %s surface as CUDA mapped buffer", fourccString((uint32_t) img->fourcc, fourcc));
    return true;
}

static bool mapExternalBacking(BackingImage *img) {
    if (img->fds[0] < 0 || img->totalSize == 0) {
        return false;
    }

    img->externalMapping = mmap(NULL, img->totalSize, PROT_READ | PROT_WRITE, MAP_SHARED, img->fds[0], 0);
    if (img->externalMapping == MAP_FAILED) {
        img->externalMapping = NULL;
        return false;
    }
    img->externalMappingSize = img->totalSize;
    return true;
}

static BackingImage *createImportedBackingImage(NVDriver *drv, const ImportedSurface *imported, uint32_t width, uint32_t height) {
    NVFormat format = nvFormatFromSurfaceFourcc(imported->pixelFormat);
    if (format == NV_FORMAT_NONE || imported->numPlanes == 0 || imported->fds[0] < 0) {
        return NULL;
    }

    BackingImage *img = calloc(1, sizeof(BackingImage));
    if (img == NULL) {
        return NULL;
    }
    for (int i = 0; i < 4; i++) {
        img->fds[i] = -1;
    }

    img->isExternalBuffer = true;
    img->format = format;
    img->width = width;
    img->height = height;
    img->fourcc = (int) imported->pixelFormat;
    img->totalSize = imported->dataSize;
    for (uint32_t i = 0; i < imported->numFds && i < 4; i++) {
        img->fds[i] = dup(imported->fds[i]);
        if (img->fds[i] < 0) {
            goto fail;
        }
        cacheBackingImageFdStat(img, (int) i);
    }
    off_t realSize = lseek(img->fds[0], 0, SEEK_END);
    if (realSize > 0) {
        img->totalSize = (uint32_t) realSize;
        lseek(img->fds[0], 0, SEEK_SET);
    }
    for (uint32_t i = 0; i < imported->numPlanes && i < 4; i++) {
        img->strides[i] = (int) imported->pitches[i];
        img->offsets[i] = (int) imported->offsets[i];
        img->size[i] = imported->dataSize;
    }

    BackingImage *existing = retainBackingImageByFd(drv, imported->fds[0], format, width, height);
    if (existing != NULL) {
        nvBackingImageCopyColorMetadata(img, existing);
        const NVFormatInfo *fmtInfo = &formatsInfo[format];
        for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
            img->arrays[i] = existing->arrays[i];
            img->strides[i] = existing->strides[i];
            img->offsets[i] = existing->offsets[i];
            img->size[i] = existing->size[i];
            img->mods[i] = existing->mods[i];
        }
        img->totalSize = existing->totalSize != 0 ? existing->totalSize : existing->size[0];
        img->borrowedCudaResources = true;
        img->borrowedBackingImage = existing;
        LOG_DEBUG("Imported surface reused backing image color metadata: imported=%p backing=%p color_standard=%s(%d) full_range=%d",
            img, existing, nvColorStandardName(img->colorStandard), img->colorStandard, img->colorRangeFull);
        return img;
    }

    if (isRgbFourcc(imported->pixelFormat) && importExternalBufferToCuda(drv, img)) {
        return img;
    }

    if (mapExternalBacking(img)) {
        return img;
    }

    if (isRgbFourcc(imported->pixelFormat)) {
        char fourcc[5];
        LOG("Unable to mmap imported RGB surface %s", fourccString(imported->pixelFormat, fourcc));
        goto fail;
    }

    if (!importExternalBackingToCuda(drv, img)) {
        char fourcc[5];
        LOG("Unable to import external surface %s to CUDA", fourccString(imported->pixelFormat, fourcc));
        goto fail;
    }

    return img;

fail:
    for (int i = 0; i < 4; i++) {
        if (img->fds[i] >= 0) {
            close(img->fds[i]);
        }
    }
    if (img->externalMapping != NULL) {
        munmap(img->externalMapping, img->externalMappingSize);
    }
    free(img);
    return NULL;
}

static BackingImage *surfaceSyncBackingImage(NVSurface *surface) {
    if (surface == NULL || surface->backingImage == NULL) {
        return NULL;
    }
    if (surface->backingImage->borrowedBackingImage != NULL) {
        return surface->backingImage->borrowedBackingImage;
    }
    return surface->backingImage;
}

static void setSurfaceBackingImageResolving(NVSurface *surface, bool resolving) {
    BackingImage *img = surfaceSyncBackingImage(surface);
    if (img == NULL || !img->syncInitialized) {
        return;
    }

    pthread_mutex_lock(&img->mutex);
    img->resolving = resolving;
    if (!resolving) {
        pthread_cond_broadcast(&img->cond);
    }
    pthread_mutex_unlock(&img->mutex);
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
    ImportedSurface imported;
    parseSurfaceImportAttributes(attrib_list, num_attribs, &imported);
    const bool importSurface = imported.valid;
    uint32_t surfaceFourcc = importSurface ? imported.pixelFormat : 0;

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
    case VA_RT_FORMAT_RGB32:
        nvFormat = cudaVideoSurfaceFormat_NV12;
        chromaFormat = cudaVideoChromaFormat_444;
        bitdepth = 8;
        break;
    
    default:
        LOG("Unknown format: %X", format);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
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
        suf->fourcc = surfaceFourcc != 0 ? (int) surfaceFourcc : (format == VA_RT_FORMAT_RGB32 ? VA_FOURCC_ARGB : 0);
        suf->pictureIdx = -1;
        suf->bitDepth = bitdepth;
        suf->context = NULL;
        suf->chromaFormat = chromaFormat;
        pthread_mutex_init(&suf->mutex, NULL);
        pthread_cond_init(&suf->cond, NULL);

        if (importSurface) {
            BackingImage *img = createImportedBackingImage(drv, &imported, width, height);
            if (img == NULL) {
                CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
                return VA_STATUS_ERROR_ALLOCATION_FAILED;
            }
            suf->backingImage = img;
            img->surface = suf;
            nvSurfaceCopyColorMetadataFromBackingImage(suf, img);
            char fourcc[5];
            LOG_DEBUG("Importing surface %ux%u, format %X/%s (%p) color_standard=%s(%d) full_range=%d",
                width, height, format, fourccString(surfaceFourcc, fourcc), suf,
                nvColorStandardName(suf->colorStandard), suf->colorStandard, suf->colorRangeFull);
        } else {
            LOG_DEBUG("Creating surface %ux%u, format %X (%p)", width, height, format, suf);
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

        LOG_DEBUG("Destroying surface %d (%p)", surface->pictureIdx, surface);

        drv->backend->detachBackingImageFromSurface(drv, surface);

        deleteObject(drv, surface_list[i]);
    }

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

    if (cfg->entrypoint == VAEntrypointVideoProc) {
        Object contextObj = allocateObject(drv, OBJECT_TYPE_CONTEXT, sizeof(NVContext));
        LOG("Creating VideoProc context id: %d", contextObj->id);

        NVContext *nvCtx = (NVContext*) contextObj->obj;
        nvCtx->drv = drv;
        nvCtx->decoder = NULL;
        nvCtx->profile = cfg->profile;
        nvCtx->entrypoint = cfg->entrypoint;
        nvCtx->width = picture_width;
        nvCtx->height = picture_height;
        nvCtx->codec = NULL;

        pthread_mutexattr_t attrib;
        pthread_mutexattr_init(&attrib);
        pthread_mutexattr_settype(&attrib, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&nvCtx->surfaceCreationMutex, &attrib);
        pthread_mutexattr_destroy(&attrib);

        pthread_mutex_init(&nvCtx->resolveMutex, NULL);
        pthread_cond_init(&nvCtx->resolveCondition, NULL);

        *context = contextObj->id;
        return VA_STATUS_SUCCESS;
    }

    LOG("Creating context with %d render targets, at %dx%d", num_render_targets, picture_width, picture_height);

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
    nvStatsIncrement(drv, NV_STAT_DECODER_CREATES);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    Object contextObj = allocateObject(drv, OBJECT_TYPE_CONTEXT, sizeof(NVContext));
    LOG("Creating decoder: %p for context id: %d", decoder, contextObj->id);

    NVContext *nvCtx = (NVContext*) contextObj->obj;
    nvCtx->drv = drv;
    nvCtx->decoder = decoder;
    nvCtx->profile = cfg->profile;
    nvCtx->entrypoint = cfg->entrypoint;
    nvCtx->width = picture_width;
    nvCtx->height = picture_height;
    nvCtx->codec = selectedCodec;
    nvCtx->cudaCodec = cfg->cudaCodec;
    nvCtx->decoderSurfaceFormat = cfg->surfaceFormat;
    nvCtx->decoderChromaFormat = cfg->chromaFormat;
    nvCtx->decoderBitDepth = cfg->bitDepth;
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

static VAStatus recreateDecoderForSurface(NVContext *nvCtx, NVSurface *surface) {
    if (nvCtx->decoderSurfaceFormat == surface->format &&
        nvCtx->decoderChromaFormat == surface->chromaFormat &&
        nvCtx->decoderBitDepth == surface->bitDepth) {
        return VA_STATUS_SUCCESS;
    }

    if (nvCtx->currentPictureId != 0) {
        LOG("Decoder/surface format mismatch after decode start: decoder format=%d chroma=%d bitDepth=%d, surface format=%d chroma=%d bitDepth=%d",
            nvCtx->decoderSurfaceFormat, nvCtx->decoderChromaFormat, nvCtx->decoderBitDepth,
            surface->format, surface->chromaFormat, surface->bitDepth);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    int display_area_width = nvCtx->width;
    int display_area_height = nvCtx->height;

    switch(surface->chromaFormat) {
        case cudaVideoChromaFormat_422:
            display_area_width = ROUND_UP(display_area_width, 2);
            break;
        case cudaVideoChromaFormat_420:
            display_area_width = ROUND_UP(display_area_width, 2);
            display_area_height = ROUND_UP(display_area_height, 2);
            break;
        default:
            break;
    }

    CUVIDDECODECREATEINFO vdci = {
        .ulWidth             = vdci.ulMaxWidth  = vdci.ulTargetWidth  = nvCtx->width,
        .ulHeight            = vdci.ulMaxHeight = vdci.ulTargetHeight = nvCtx->height,
        .CodecType           = nvCtx->cudaCodec,
        .ulCreationFlags     = cudaVideoCreate_PreferCUVID,
        .ulIntraDecodeOnly   = 0,
        .display_area.right  = display_area_width,
        .display_area.bottom = display_area_height,
        .ChromaFormat        = surface->chromaFormat,
        .OutputFormat        = surface->format,
        .bitDepthMinus8      = surface->bitDepth - 8,
        .DeinterlaceMode     = cudaVideoDeinterlaceMode_Weave,
        .ulNumOutputSurfaces = 1,
        .ulNumDecodeSurfaces = nvCtx->surfaceCount,
    };

    NVDriver *drv = nvCtx->drv;
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
    if (nvCtx->decoder != NULL) {
        CHECK_CUDA_RESULT(cv->cuvidDestroyDecoder(nvCtx->decoder));
        nvCtx->decoder = NULL;
    }

    CUvideodecoder decoder;
    CUresult result = cv->cuvidCreateDecoder(&decoder, &vdci);
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
    if (result != CUDA_SUCCESS) {
        LOG("cuvidCreateDecoder failed while matching first surface: %d", result);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    nvCtx->decoder = decoder;
    nvCtx->decoderSurfaceFormat = surface->format;
    nvCtx->decoderChromaFormat = surface->chromaFormat;
    nvCtx->decoderBitDepth = surface->bitDepth;
    nvStatsIncrement(drv, NV_STAT_DECODER_CREATES);
    return VA_STATUS_SUCCESS;
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
    *buf_id = bufferObject->id;

    NVBuffer *buf = (NVBuffer*) bufferObject->obj;
    buf->bufferType = type;
    buf->elements = num_elements;
    buf->size = num_elements * size;
    buf->ptr = memalign(16, buf->size);
    buf->offset = offset;

    if (buf->ptr == NULL) {
        LOG("Unable to allocate buffer of %zu bytes", buf->size);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    if (data != NULL)
    {
        memcpy(buf->ptr, data, buf->size);
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

    *pbuf = buf->ptr;

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

    if (buf->ptr != NULL) {
        free(buf->ptr);
    }

    deleteObject(drv, buffer_id);

    return VA_STATUS_SUCCESS;
}

static uint8_t clampU8(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t) value;
}

typedef struct {
    int vToR;
    int uToG;
    int vToG;
    int uToB;
} ColorMatrix;

typedef struct {
    int sampleShift;
    int yOffset;
    int uvOffset;
    int rounding;
    int valueShift;
} VideoProcSampleInfo;

static const ColorMatrix kColorMatrices[] = {
    { 409, 100, 208, 516 },
    { 459,  55, 136, 541 },
    { 430,  48, 167, 548 },
};

static const char *colorMatrixName(const ColorMatrix *matrix) {
    if (matrix == &kColorMatrices[0]) {
        return "BT.601";
    }
    if (matrix == &kColorMatrices[1]) {
        return "BT.709";
    }
    if (matrix == &kColorMatrices[2]) {
        return "BT.2020";
    }
    return "unknown";
}

VAProcColorStandardType nvColorStandardFromMatrixCoefficients(uint8_t matrixCoefficients) {
    switch (matrixCoefficients) {
    case 1:
        return VAProcColorStandardBT709;
    case 4:
        return VAProcColorStandardBT470M;
    case 5:
        return VAProcColorStandardBT470BG;
    case 6:
        return VAProcColorStandardSMPTE170M;
    case 7:
        return VAProcColorStandardSMPTE240M;
    case 9:
        return VAProcColorStandardBT2020;
    default:
        return VAProcColorStandardNone;
    }
}

void nvSurfaceResetColorMetadata(NVSurface *surface) {
    if (surface == NULL) {
        return;
    }

    surface->colorStandard = VAProcColorStandardNone;
    surface->colorRangeFull = false;
}

void nvSurfaceSetColorMetadata(NVSurface *surface, VAProcColorStandardType colorStandard, bool colorRangeFull) {
    if (surface == NULL) {
        return;
    }

    surface->colorStandard = colorStandard;
    surface->colorRangeFull = colorRangeFull;
}

void nvSurfaceCopyColorMetadata(NVSurface *dst, const NVSurface *src) {
    if (dst == NULL || src == NULL) {
        return;
    }

    dst->colorStandard = src->colorStandard;
    dst->colorRangeFull = src->colorRangeFull;
}

static BackingImage *metadataBackingImage(BackingImage *img) {
    if (img != NULL && img->borrowedBackingImage != NULL) {
        return img->borrowedBackingImage;
    }
    return img;
}

static const BackingImage *constMetadataBackingImage(const BackingImage *img) {
    if (img != NULL && img->borrowedBackingImage != NULL) {
        return img->borrowedBackingImage;
    }
    return img;
}

void nvSurfaceCopyColorMetadataFromBackingImage(NVSurface *surface, const BackingImage *img) {
    const BackingImage *metadataImg = constMetadataBackingImage(img);
    if (surface == NULL || metadataImg == NULL) {
        return;
    }

    surface->colorStandard = metadataImg->colorStandard;
    surface->colorRangeFull = metadataImg->colorRangeFull;
}

void nvBackingImageStoreSurfaceColorMetadata(BackingImage *img, const NVSurface *surface) {
    BackingImage *metadataImg = metadataBackingImage(img);
    if (metadataImg == NULL || surface == NULL) {
        return;
    }

    metadataImg->colorStandard = surface->colorStandard;
    metadataImg->colorRangeFull = surface->colorRangeFull;
}

void nvBackingImageCopyColorMetadata(BackingImage *dst, const BackingImage *src) {
    const BackingImage *metadataSrc = constMetadataBackingImage(src);
    if (dst == NULL || metadataSrc == NULL) {
        return;
    }

    dst->colorStandard = metadataSrc->colorStandard;
    dst->colorRangeFull = metadataSrc->colorRangeFull;
}

const char *nvColorStandardName(VAProcColorStandardType colorStandard) {
    switch (colorStandard) {
    case VAProcColorStandardNone:
        return "None";
    case VAProcColorStandardBT601:
        return "BT.601";
    case VAProcColorStandardBT709:
        return "BT.709";
    case VAProcColorStandardBT470M:
        return "BT.470M";
    case VAProcColorStandardBT470BG:
        return "BT.470BG";
    case VAProcColorStandardSMPTE170M:
        return "SMPTE170M";
    case VAProcColorStandardSMPTE240M:
        return "SMPTE240M";
    case VAProcColorStandardGenericFilm:
        return "GenericFilm";
    case VAProcColorStandardSRGB:
        return "sRGB";
    case VAProcColorStandardSTRGB:
        return "stRGB";
    case VAProcColorStandardXVYCC601:
        return "xvYCC601";
    case VAProcColorStandardXVYCC709:
        return "xvYCC709";
    case VAProcColorStandardBT2020:
        return "BT.2020";
    case VAProcColorStandardExplicit:
        return "Explicit";
    default:
        return "unknown";
    }
}

static const ColorMatrix *colorMatrixForStandard(VAProcColorStandardType colorStandard, uint32_t width) {
    switch (colorStandard) {
    case VAProcColorStandardBT601:
    case VAProcColorStandardSMPTE170M:
    case VAProcColorStandardBT470M:
    case VAProcColorStandardBT470BG:
        return &kColorMatrices[0];
    case VAProcColorStandardBT709:
    case VAProcColorStandardSMPTE240M:
        return &kColorMatrices[1];
    case VAProcColorStandardBT2020:
        return &kColorMatrices[2];
    case VAProcColorStandardNone:
    default:
        return width >= 1280 ? &kColorMatrices[1] : &kColorMatrices[0];
    }
}

static VAProcColorStandardType effectiveSurfaceColorStandard(const NVSurface *src, const VAProcPipelineParameterBuffer *pipeline) {
    if (pipeline->surface_color_standard != VAProcColorStandardNone) {
        return pipeline->surface_color_standard;
    }
    if (src != NULL && src->colorStandard != VAProcColorStandardNone) {
        return src->colorStandard;
    }
    return VAProcColorStandardNone;
}

static VideoProcSampleInfo videoProcSampleInfoForFormat(NVFormat format) {
    switch (format) {
    case NV_FORMAT_P010:
        return (VideoProcSampleInfo) { 6, 64, 512, 512, 10 };
    case NV_FORMAT_P012:
        return (VideoProcSampleInfo) { 4, 256, 2048, 2048, 12 };
    case NV_FORMAT_NV12:
    default:
        return (VideoProcSampleInfo) { 0, 16, 128, 128, 8 };
    }
}

static bool loadVideoProcKernel(NVDriver *drv, bool is16Bit) {
    if (is16Bit) {
        static bool loggedP010KernelFailure = false;

        if (drv->p010ToArgbKernel != NULL) {
            return true;
        }
        if (drv->videoProcKernelP010Failed) {
            return false;
        }

        if (CHECK_CUDA_RESULT(drv->cu->cuModuleLoadData(&drv->videoProcModuleP010, p010ToArgbPtx)) ||
            CHECK_CUDA_RESULT(drv->cu->cuModuleGetFunction(&drv->p010ToArgbKernel, drv->videoProcModuleP010, "p010_to_argb"))) {
            if (drv->videoProcModuleP010 != NULL) {
                CHECK_CUDA_RESULT(drv->cu->cuModuleUnload(drv->videoProcModuleP010));
                drv->videoProcModuleP010 = NULL;
            }
            drv->p010ToArgbKernel = NULL;
            drv->videoProcKernelP010Failed = true;
            if (!loggedP010KernelFailure) {
                LOG("CUDA P010 VideoProc kernel unavailable, using CPU fallback");
                loggedP010KernelFailure = true;
            }
            return false;
        }

        return true;
    } else {
        static bool loggedNV12KernelFailure = false;

        if (drv->nv12ToArgbKernel != NULL) {
            return true;
        }
        if (drv->videoProcKernelFailed) {
            return false;
        }

        if (CHECK_CUDA_RESULT(drv->cu->cuModuleLoadData(&drv->videoProcModule, nv12ToArgbPtx)) ||
            CHECK_CUDA_RESULT(drv->cu->cuModuleGetFunction(&drv->nv12ToArgbKernel, drv->videoProcModule, "nv12_to_argb"))) {
            if (drv->videoProcModule != NULL) {
                CHECK_CUDA_RESULT(drv->cu->cuModuleUnload(drv->videoProcModule));
                drv->videoProcModule = NULL;
            }
            drv->nv12ToArgbKernel = NULL;
            drv->videoProcKernelFailed = true;
            if (!loggedNV12KernelFailure) {
                LOG("CUDA NV12 VideoProc kernel unavailable, using CPU fallback");
                loggedNV12KernelFailure = true;
            }
            return false;
        }

        return true;
    }
}

static uint32_t rgbOrderForFourcc(uint32_t fourcc) {
    switch (fourcc) {
    case VA_FOURCC_RGBA:
    case VA_FOURCC_RGBX:
        return 1;
    case VA_FOURCC_ARGB:
    case VA_FOURCC_XRGB:
        return 2;
    case VA_FOURCC_ABGR:
    case VA_FOURCC_XBGR:
        return 3;
    case VA_FOURCC_BGRA:
    case VA_FOURCC_BGRX:
    default:
        return 0;
    }
}

static size_t roundVideoProcBufferSize(size_t requiredSize) {
    const size_t blockSize = 1024 * 1024;
    if (requiredSize > SIZE_MAX - (blockSize - 1)) {
        return requiredSize;
    }
    return (requiredSize + blockSize - 1) & ~(blockSize - 1);
}

static bool ensureVideoProcBuffer(NVDriver *drv, CUdeviceptr *buffer, size_t *bufferSize, size_t requiredSize) {
    if (*bufferSize >= requiredSize && *buffer != 0) {
        return true;
    }
    if (*buffer != 0 && CHECK_CUDA_RESULT(drv->cu->cuMemFree(*buffer))) {
        *buffer = 0;
        *bufferSize = 0;
        return false;
    }
    *buffer = 0;
    *bufferSize = 0;
    const size_t allocSize = roundVideoProcBufferSize(requiredSize);
    if (CHECK_CUDA_RESULT(drv->cu->cuMemAlloc(buffer, allocSize))) {
        return false;
    }
    *bufferSize = allocSize;
    return true;
}

static bool ensureCpuVideoProcBuffer(void **buffer, size_t *bufferSize, size_t requiredSize) {
    if (*bufferSize >= requiredSize && *buffer != NULL) {
        return true;
    }

    const size_t allocSize = roundVideoProcBufferSize(requiredSize);
    void *newBuffer = realloc(*buffer, allocSize);
    if (newBuffer == NULL) {
        return false;
    }

    *buffer = newBuffer;
    *bufferSize = allocSize;
    return true;
}

static void writeRgbPixel(uint8_t *dst, uint32_t fourcc, uint8_t r, uint8_t g, uint8_t b) {
    switch (fourcc) {
    case VA_FOURCC_RGBA:
    case VA_FOURCC_RGBX:
        dst[0] = r;
        dst[1] = g;
        dst[2] = b;
        dst[3] = 255;
        break;
    case VA_FOURCC_BGRA:
    case VA_FOURCC_BGRX:
        dst[0] = b;
        dst[1] = g;
        dst[2] = r;
        dst[3] = 255;
        break;
    case VA_FOURCC_ARGB:
    case VA_FOURCC_XRGB:
        dst[0] = 255;
        dst[1] = r;
        dst[2] = g;
        dst[3] = b;
        break;
    case VA_FOURCC_ABGR:
    case VA_FOURCC_XBGR:
        dst[0] = 255;
        dst[1] = b;
        dst[2] = g;
        dst[3] = r;
        break;
    default:
        dst[0] = b;
        dst[1] = g;
        dst[2] = r;
        dst[3] = 255;
        break;
    }
}

static bool convertNV12ToARGBCuda(NVDriver *drv, BackingImage *srcImg, BackingImage *dstImg, uint32_t width, uint32_t height, bool is16Bit, const ColorMatrix *matrix) {
    if (srcImg->arrays[0] == NULL || srcImg->arrays[1] == NULL) {
        return false;
    }

    const VideoProcSampleInfo sampleInfo = videoProcSampleInfoForFormat(srcImg->format);
    const size_t bpp = is16Bit ? 2 : 1;
    const size_t ySize = (size_t) width * height * bpp;
    const size_t uvHeight = (height + 1) / 2;
    const size_t uvSize = (size_t) width * uvHeight * bpp;
    const size_t argbSize = (size_t) width * height * 4;
    CUdeviceptr dstDevice = dstImg->externalDevicePtr != 0 ? dstImg->externalDevicePtr + (CUdeviceptr) dstImg->offsets[0] : drv->videoProcArgbBuffer;
    uint32_t dstPitch = dstImg->externalDevicePtr != 0 ? (uint32_t) dstImg->strides[0] : width * 4;

    pthread_mutex_lock(&drv->exportMutex);
    if (!loadVideoProcKernel(drv, is16Bit) ||
        !ensureVideoProcBuffer(drv, &drv->videoProcYBuffer, &drv->videoProcYBufferSize, ySize) ||
        !ensureVideoProcBuffer(drv, &drv->videoProcUVBuffer, &drv->videoProcUVBufferSize, uvSize)) {
        goto fail;
    }
    if (dstImg->externalDevicePtr == 0 &&
        !ensureVideoProcBuffer(drv, &drv->videoProcArgbBuffer, &drv->videoProcArgbBufferSize, argbSize)) {
        goto fail;
    }
    if (dstImg->externalDevicePtr == 0) {
        dstDevice = drv->videoProcArgbBuffer;
    }

    CUDA_MEMCPY2D yCpy = {
        .srcMemoryType = CU_MEMORYTYPE_ARRAY,
        .srcArray = srcImg->arrays[0],
        .dstMemoryType = CU_MEMORYTYPE_DEVICE,
        .dstDevice = drv->videoProcYBuffer,
        .dstPitch = width * bpp,
        .WidthInBytes = width * bpp,
        .Height = height
    };
    CUDA_MEMCPY2D uvCpy = {
        .srcMemoryType = CU_MEMORYTYPE_ARRAY,
        .srcArray = srcImg->arrays[1],
        .dstMemoryType = CU_MEMORYTYPE_DEVICE,
        .dstDevice = drv->videoProcUVBuffer,
        .dstPitch = width * bpp,
        .WidthInBytes = width * bpp,
        .Height = uvHeight
    };
    if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&yCpy)) ||
        CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&uvCpy))) {
        goto fail;
    }

    uint32_t yPitch = width * bpp;
    uint32_t uvPitch = width * bpp;
    uint32_t order = rgbOrderForFourcc((uint32_t) dstImg->fourcc);
    uint32_t vToR = (uint32_t) matrix->vToR;
    uint32_t uToG = (uint32_t) matrix->uToG;
    uint32_t vToG = (uint32_t) matrix->vToG;
    uint32_t uToB = (uint32_t) matrix->uToB;
    uint32_t sampleShift = (uint32_t) sampleInfo.sampleShift;
    uint32_t yOffset = (uint32_t) sampleInfo.yOffset;
    uint32_t uvOffset = (uint32_t) sampleInfo.uvOffset;
    uint32_t rounding = (uint32_t) sampleInfo.rounding;
    uint32_t valueShift = (uint32_t) sampleInfo.valueShift;
    void *nv12Args[] = {
        &drv->videoProcYBuffer,
        &drv->videoProcUVBuffer,
        &dstDevice,
        &width,
        &height,
        &yPitch,
        &uvPitch,
        &dstPitch,
        &order,
        &vToR,
        &uToG,
        &vToG,
        &uToB
    };
    void *p010Args[] = {
        &drv->videoProcYBuffer,
        &drv->videoProcUVBuffer,
        &dstDevice,
        &width,
        &height,
        &yPitch,
        &uvPitch,
        &dstPitch,
        &order,
        &vToR,
        &uToG,
        &vToG,
        &uToB,
        &sampleShift,
        &yOffset,
        &uvOffset,
        &rounding,
        &valueShift
    };
    void **args = is16Bit ? p010Args : nv12Args;
    CUfunction kernel = is16Bit ? drv->p010ToArgbKernel : drv->nv12ToArgbKernel;
    if (CHECK_CUDA_RESULT(drv->cu->cuLaunchKernel(kernel,
            (width + 15) / 16, (height + 15) / 16, 1,
            16, 16, 1, 0, 0, args, NULL))) {
        goto fail;
    }

    if (dstImg->externalDevicePtr == 0) {
        CUDA_MEMCPY2D argbCpy = {
            .srcMemoryType = CU_MEMORYTYPE_DEVICE,
            .srcDevice = drv->videoProcArgbBuffer,
            .srcPitch = width * 4,
            .dstMemoryType = CU_MEMORYTYPE_ARRAY,
            .dstArray = dstImg->arrays[0],
            .WidthInBytes = width * 4,
            .Height = height
        };
        if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&argbCpy))) {
            goto fail;
        }
    } else if (CHECK_CUDA_RESULT(drv->cu->cuStreamSynchronize(0))) {
        goto fail;
    }

    pthread_mutex_unlock(&drv->exportMutex);
    return true;

fail:
    pthread_mutex_unlock(&drv->exportMutex);
    return false;
}

static bool convertNV12ToARGB(NVDriver *drv, BackingImage *srcImg, BackingImage *dstImg, uint32_t width, uint32_t height, const ColorMatrix *matrix) {
    const bool is16Bit = srcImg->format == NV_FORMAT_P010 || srcImg->format == NV_FORMAT_P012;
    const char *formatName = is16Bit ? "P010/P012" : "NV12";
    const VideoProcSampleInfo sampleInfo = videoProcSampleInfoForFormat(srcImg->format);

    if (dstImg->externalMapping == NULL || dstImg->externalDevicePtr != 0) {
        if (convertNV12ToARGBCuda(drv, srcImg, dstImg, width, height, is16Bit, matrix)) {
            nvStatsIncrement(drv, NV_STAT_VIDEOPROC_CUDA);
            static bool loggedCudaVideoProc[2] = { false, false };
            const int logIndex = is16Bit ? 1 : 0;
            if (!loggedCudaVideoProc[logIndex]) {
                LOG("Using CUDA %s to RGB VideoProc conversion", formatName);
                loggedCudaVideoProc[logIndex] = true;
            }
            return true;
        }
        nvStatsIncrement(drv, NV_STAT_VIDEOPROC_CUDA_FAILURES);
        static bool loggedCudaFallback[2] = { false, false };
        const int logIndex = is16Bit ? 1 : 0;
        if (!loggedCudaFallback[logIndex]) {
            LOG("CUDA %s to RGB conversion failed, falling back to CPU", formatName);
            loggedCudaFallback[logIndex] = true;
        }
    }
    nvStatsIncrement(drv, NV_STAT_VIDEOPROC_CPU_FALLBACK);

    const size_t bpp = is16Bit ? 2 : 1;
    const size_t ySize = (size_t) width * height * bpp;
    const size_t uvSize = (size_t) width * ((height + 1) / 2) * bpp;
    const size_t argbSize = (size_t) width * height * 4;

    pthread_mutex_lock(&drv->exportMutex);

    if (!ensureCpuVideoProcBuffer(&drv->cpuVideoProcYBuffer, &drv->cpuVideoProcYBufferSize, ySize) ||
        !ensureCpuVideoProcBuffer(&drv->cpuVideoProcUVBuffer, &drv->cpuVideoProcUVBufferSize, uvSize)) {
        goto fail;
    }
    if (dstImg->externalMapping == NULL &&
        !ensureCpuVideoProcBuffer(&drv->cpuVideoProcArgbBuffer, &drv->cpuVideoProcArgbBufferSize, argbSize)) {
        goto fail;
    }

    uint8_t *yPlane = drv->cpuVideoProcYBuffer;
    uint8_t *uvPlane = drv->cpuVideoProcUVBuffer;
    uint8_t *argb = dstImg->externalMapping == NULL ? drv->cpuVideoProcArgbBuffer : NULL;

    if (srcImg->externalMapping != NULL) {
        const uint8_t *srcY = (const uint8_t*) srcImg->externalMapping + srcImg->offsets[0];
        const uint8_t *srcUV = (const uint8_t*) srcImg->externalMapping + srcImg->offsets[1];
        for (uint32_t y = 0; y < height; y++) {
            memcpy(yPlane + (size_t) y * width * bpp, srcY + (size_t) y * srcImg->strides[0], width * bpp);
        }
        for (uint32_t y = 0; y < (height + 1) / 2; y++) {
            memcpy(uvPlane + (size_t) y * width * bpp, srcUV + (size_t) y * srcImg->strides[1], width * bpp);
        }
    } else {
        CUDA_MEMCPY2D yCpy = {
            .srcMemoryType = CU_MEMORYTYPE_ARRAY,
            .srcArray = srcImg->arrays[0],
            .dstMemoryType = CU_MEMORYTYPE_HOST,
            .dstHost = yPlane,
            .dstPitch = width * bpp,
            .WidthInBytes = width * bpp,
            .Height = height
        };
        CUDA_MEMCPY2D uvCpy = {
            .srcMemoryType = CU_MEMORYTYPE_ARRAY,
            .srcArray = srcImg->arrays[1],
            .dstMemoryType = CU_MEMORYTYPE_HOST,
            .dstHost = uvPlane,
            .dstPitch = width * bpp,
            .WidthInBytes = width * bpp,
            .Height = (height + 1) / 2
        };

        if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&yCpy)) ||
            CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&uvCpy))) {
            goto fail;
        }
    }

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            int yy, u, v;
            if (is16Bit) {
                const uint16_t *yPlane16 = (const uint16_t*) yPlane;
                const uint16_t *uvPlane16 = (const uint16_t*) uvPlane;
                yy = yPlane16[(size_t) y * width + x] >> sampleInfo.sampleShift;
                const size_t uvIndex = (size_t) (y / 2) * width + (x & ~1u);
                u = uvPlane16[uvIndex] >> sampleInfo.sampleShift;
                v = uvPlane16[uvIndex + 1] >> sampleInfo.sampleShift;
            } else {
                yy = yPlane[(size_t) y * width + x];
                const size_t uvIndex = (size_t) (y / 2) * width + (x & ~1u);
                u = uvPlane[uvIndex];
                v = uvPlane[uvIndex + 1];
            }
            const int c = yy > sampleInfo.yOffset ? yy - sampleInfo.yOffset : 0;
            const int d = u - sampleInfo.uvOffset;
            const int e = v - sampleInfo.uvOffset;
            const uint8_t r = clampU8((298 * c + matrix->vToR * e + sampleInfo.rounding) >> sampleInfo.valueShift);
            const uint8_t g = clampU8((298 * c - matrix->uToG * d - matrix->vToG * e + sampleInfo.rounding) >> sampleInfo.valueShift);
            const uint8_t b = clampU8((298 * c + matrix->uToB * d + sampleInfo.rounding) >> sampleInfo.valueShift);
            if (dstImg->externalMapping != NULL) {
                uint8_t *row = (uint8_t*) dstImg->externalMapping + dstImg->offsets[0] + (size_t) y * dstImg->strides[0];
                writeRgbPixel(row + (size_t) x * 4, (uint32_t) dstImg->fourcc, r, g, b);
            } else {
                const size_t out = ((size_t) y * width + x) * 4;
                argb[out + 0] = b;
                argb[out + 1] = g;
                argb[out + 2] = r;
                argb[out + 3] = 255;
            }
        }
    }

    if (dstImg->externalMapping != NULL) {
        pthread_mutex_unlock(&drv->exportMutex);
        return true;
    }

    CUDA_MEMCPY2D argbCpy = {
        .srcMemoryType = CU_MEMORYTYPE_HOST,
        .srcHost = argb,
        .srcPitch = width * 4,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = dstImg->arrays[0],
        .WidthInBytes = width * 4,
        .Height = height
    };
    bool failed = CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&argbCpy));

    pthread_mutex_unlock(&drv->exportMutex);
    return !failed;

fail:
    pthread_mutex_unlock(&drv->exportMutex);
    return false;
}

static bool copySurfaceBackingImage(NVDriver *drv, NVSurface *src, NVSurface *dst, const VAProcPipelineParameterBuffer *pipeline) {
    if (src == NULL || dst == NULL || pipeline == NULL) {
        return false;
    }

    VARectangle srcRegion = { 0, 0, src->width, src->height };
    VARectangle dstRegion = { 0, 0, dst->width, dst->height };
    if (pipeline->surface_region != NULL) {
        srcRegion = *pipeline->surface_region;
    }
    if (pipeline->output_region != NULL) {
        dstRegion = *pipeline->output_region;
    }

    if (srcRegion.x != 0 || srcRegion.y != 0 || dstRegion.x != 0 || dstRegion.y != 0 ||
        srcRegion.width != dstRegion.width || srcRegion.height != dstRegion.height) {
        LOG("Unsupported VideoProc blit: src=%dx%d+%d+%d dst=%dx%d+%d+%d",
            srcRegion.width, srcRegion.height, srcRegion.x, srcRegion.y,
            dstRegion.width, dstRegion.height, dstRegion.x, dstRegion.y);
        return false;
    }

    pthread_mutex_lock(&src->mutex);
    if (src->resolving) {
        pthread_cond_wait(&src->cond, &src->mutex);
    }
    pthread_mutex_unlock(&src->mutex);
    if (src->backingImage != NULL && src->backingImage->borrowedBackingImage != NULL &&
        src->backingImage->borrowedBackingImage->syncInitialized) {
        BackingImage *borrowed = src->backingImage->borrowedBackingImage;
        pthread_mutex_lock(&borrowed->mutex);
        while (borrowed->resolving) {
            pthread_cond_wait(&borrowed->cond, &borrowed->mutex);
        }
        pthread_mutex_unlock(&borrowed->mutex);
    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), false);
    bool realised = drv->backend->realiseSurface(drv, src) && drv->backend->realiseSurface(drv, dst);
    if (!realised) {
        CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL));
        return false;
    }

    BackingImage *srcImg = src->backingImage;
    BackingImage *dstImg = dst->backingImage;
    nvSurfaceCopyColorMetadataFromBackingImage(src, srcImg);
    if (srcImg != NULL && dstImg != NULL && (srcImg->format == NV_FORMAT_NV12 || srcImg->format == NV_FORMAT_P010 || srcImg->format == NV_FORMAT_P012) && dstImg->format == NV_FORMAT_ARGB) {
        const VAProcColorStandardType colorStandard = effectiveSurfaceColorStandard(src, pipeline);
        const ColorMatrix *matrix = colorMatrixForStandard(colorStandard, srcRegion.width);
        const VideoProcSampleInfo sampleInfo = videoProcSampleInfoForFormat(srcImg->format);
        char srcFourcc[5];
        char dstFourcc[5];
        LOG_DEBUG("VideoProc color matrix: src=%s dst=%s size=%ux%u surface_color_standard=%s(%d) decoded_color_standard=%s(%d) decoded_full_range=%d output_color_standard=%s(%d) effective_color_standard=%s(%d) matrix=%s coeffs=%d,%d,%d,%d sample_shift=%d y_offset=%d uv_offset=%d value_shift=%d",
            fourccString(formatsInfo[srcImg->format].vaFormat.fourcc, srcFourcc),
            fourccString(formatsInfo[dstImg->format].vaFormat.fourcc, dstFourcc),
            srcRegion.width, srcRegion.height,
            nvColorStandardName(pipeline->surface_color_standard), pipeline->surface_color_standard,
            nvColorStandardName(src->colorStandard), src->colorStandard, src->colorRangeFull,
            nvColorStandardName(pipeline->output_color_standard), pipeline->output_color_standard,
            nvColorStandardName(colorStandard), colorStandard,
            colorMatrixName(matrix), matrix->vToR, matrix->uToG, matrix->vToG, matrix->uToB,
            sampleInfo.sampleShift, sampleInfo.yOffset, sampleInfo.uvOffset, sampleInfo.valueShift);
        bool ret = convertNV12ToARGB(drv, srcImg, dstImg, srcRegion.width, srcRegion.height, matrix);
        bool popFailed = CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL));
        if (!ret) {
            return false;
        }
        if (popFailed) {
            return false;
        }
        goto done;
    }

    if (srcImg == NULL || dstImg == NULL || srcImg->format != dstImg->format) {
        LOG("Unsupported VideoProc formats: %d -> %d",
            srcImg != NULL ? srcImg->format : NV_FORMAT_NONE,
            dstImg != NULL ? dstImg->format : NV_FORMAT_NONE);
        CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL));
        return false;
    }

    const NVFormatInfo *fmtInfo = &formatsInfo[srcImg->format];

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *p = &fmtInfo->plane[i];
        CUDA_MEMCPY2D cpy = {
            .srcMemoryType = CU_MEMORYTYPE_ARRAY,
            .srcArray = srcImg->arrays[i],
            .dstMemoryType = CU_MEMORYTYPE_ARRAY,
            .dstArray = dstImg->arrays[i],
            .WidthInBytes = ((uint32_t) srcRegion.width >> p->ss.x) * fmtInfo->bppc * p->channelCount,
            .Height = (uint32_t) srcRegion.height >> p->ss.y
        };

        if (i == fmtInfo->numPlanes - 1) {
            CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&cpy));
        } else {
            CHECK_CUDA_RESULT(drv->cu->cuMemcpy2DAsync(&cpy, 0));
        }
    }
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), false);

done:
    pthread_mutex_lock(&dst->mutex);
    dst->resolving = 0;
    dst->context = src->context;
    dst->progressiveFrame = src->progressiveFrame;
    dst->topFieldFirst = src->topFieldFirst;
    dst->secondField = src->secondField;
    dst->decodeFailed = src->decodeFailed;
    nvSurfaceCopyColorMetadata(dst, src);
    pthread_cond_signal(&dst->cond);
    pthread_mutex_unlock(&dst->mutex);

    return true;
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

    if (nvCtx->entrypoint == VAEntrypointVideoProc) {
        pthread_mutex_lock(&surface->mutex);
        surface->resolving = 1;
        pthread_mutex_unlock(&surface->mutex);
        nvCtx->renderTarget = surface;
        return VA_STATUS_SUCCESS;
    }

    if (surface->context != NULL && surface->context != nvCtx) {
        //this surface was last used on a different context, we need to free up the backing image (it might not be the correct size)
        if (surface->backingImage != NULL) {
            drv->backend->detachBackingImageFromSurface(drv, surface);
        }
        //...and reset the pictureIdx
        surface->pictureIdx = -1;
    }

    VAStatus decoderStatus = recreateDecoderForSurface(nvCtx, surface);
    if (decoderStatus != VA_STATUS_SUCCESS) {
        return decoderStatus;
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
    setSurfaceBackingImageResolving(surface, true);
    pthread_mutex_lock(&surface->mutex);
    surface->resolving = 1;
    pthread_mutex_unlock(&surface->mutex);

    memset(&nvCtx->pPicParams, 0, sizeof(CUVIDPICPARAMS));
    nvCtx->renderTarget = surface;
    nvCtx->displayTarget = surface;
    nvCtx->renderTarget->progressiveFrame = true; //assume we're producing progressive frame unless the codec says otherwise
    nvSurfaceResetColorMetadata(nvCtx->renderTarget);
    nvCtx->pPicParams.CurrPicIdx = nvCtx->renderTarget->pictureIdx;
    if (nvCtx->codec != NULL && nvCtx->codec->beginPicture != NULL) {
        nvCtx->codec->beginPicture(nvCtx);
    }

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

    if (nvCtx->entrypoint == VAEntrypointVideoProc) {
        for (int i = 0; i < num_buffers; i++) {
            NVBuffer *buf = (NVBuffer*) getObjectPtr(drv, OBJECT_TYPE_BUFFER, buffers[i]);
            if (buf == NULL || buf->ptr == NULL || buf->bufferType != VAProcPipelineParameterBufferType) {
                LOG("Invalid VideoProc buffer detected, skipping: %d", buffers[i]);
                continue;
            }

            nvStatsIncrement(drv, NV_STAT_VIDEOPROC_REQUESTS);
            VAProcPipelineParameterBuffer *pipeline = (VAProcPipelineParameterBuffer*) buf->ptr;
            NVSurface *src = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, pipeline->surface);
            if (!copySurfaceBackingImage(drv, src, nvCtx->renderTarget, pipeline)) {
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
        }

        return VA_STATUS_SUCCESS;
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

static VAStatus nvEndPicture(
        VADriverContextP ctx,
        VAContextID context
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);

    if (nvCtx != NULL && nvCtx->entrypoint == VAEntrypointVideoProc) {
        return VA_STATUS_SUCCESS;
    }

    if (nvCtx == NULL || nvCtx->decoder == NULL) {
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
    nvStatsIncrement(drv, NV_STAT_DECODE_PICTURES);

    VAStatus status = VA_STATUS_SUCCESS;

    if (result != CUDA_SUCCESS) {
        LOG("cuvidDecodePicture failed: %d", result);
        status = VA_STATUS_ERROR_DECODING_ERROR;
    }
    //LOG("Decoded frame successfully to idx: %d (%p)", picParams->CurrPicIdx, nvCtx->renderTarget);

    NVSurface *surface = nvCtx->displayTarget != NULL ? nvCtx->displayTarget : nvCtx->renderTarget;

    surface->context = nvCtx;
    surface->topFieldFirst = !picParams->bottom_field_flag;
    surface->secondField = picParams->second_field;
    surface->decodeFailed = status != VA_STATUS_SUCCESS;

    //TODO check we're not overflowing the queue
    pthread_mutex_lock(&nvCtx->resolveMutex);
    nvCtx->surfaceQueue[nvCtx->surfaceQueueWriteIdx++] = surface;
    if (nvCtx->surfaceQueueWriteIdx >= SURFACE_QUEUE_SIZE) {
        nvCtx->surfaceQueueWriteIdx = 0;
    }
    pthread_mutex_unlock(&nvCtx->resolveMutex);

    //Wake up the resolve thread
    pthread_cond_signal(&nvCtx->resolveCondition);

    return status;
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

    //LOG("Syncing on surface: %d (%p)", surface->pictureIdx, surface);

    //wait for resolve to occur before synchronising
    pthread_mutex_lock(&surface->mutex);
    if (surface->resolving) {
        //LOG("Surface %d not resolved, waiting", surface->pictureIdx);
        pthread_cond_wait(&surface->cond, &surface->mutex);
    }
    pthread_mutex_unlock(&surface->mutex);

    //LOG("Surface %d resolved (%p)", surface->pictureIdx, surface);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvQuerySurfaceStatus(
        VADriverContextP ctx,
        VASurfaceID render_target,
        VASurfaceStatus *status	/* out */
    )
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
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
    const NVFormatPlane *p = fmtInfo->plane;

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
        imageBuffer->size += ((width * height) >> (p[i].ss.x + p[i].ss.y)) * fmtInfo->bppc * p[i].channelCount;
    }
    imageBuffer->elements = 1;
    imageBuffer->ptr = memalign(16, imageBuffer->size);

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
    image->pitches[0] = width * fmtInfo->bppc;
    image->pitches[1] = width * fmtInfo->bppc;
    image->pitches[2] = width * fmtInfo->bppc;
    /*
     * An array indicating the byte offset from the beginning of the image data
     * to the start of each plane.
     */
    image->offsets[0] = 0;
    image->offsets[1] = image->offsets[0] + ((width * height) >> (p[0].ss.x + p[0].ss.y)) * fmtInfo->bppc * p[0].channelCount;
    image->offsets[2] = image->offsets[1] + ((width * height) >> (p[1].ss.x + p[1].ss.y)) * fmtInfo->bppc * p[1].channelCount;

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
        if (img->imageBuffer->ptr != NULL) {
            free(img->imageBuffer->ptr);
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

    NVContext *context = (NVContext*) surfaceObj->context;
    const NVFormatInfo *fmtInfo = &formatsInfo[imageObj->format];
    uint32_t offset = 0;

    if (context == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    //wait for the surface to be decoded
    nvSyncSurface(ctx, surface);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *p = &fmtInfo->plane[i];
        CUDA_MEMCPY2D memcpy2d = {
        .srcXInBytes = 0, .srcY = 0,
        .srcMemoryType = CU_MEMORYTYPE_ARRAY,
        .srcArray = surfaceObj->backingImage->arrays[i],

        .dstXInBytes = 0, .dstY = 0,
        .dstMemoryType = CU_MEMORYTYPE_HOST,
        .dstHost = (char *)imageObj->imageBuffer->ptr + offset,
        .dstPitch = width * fmtInfo->bppc,

        .WidthInBytes = (width >> p->ss.x) * fmtInfo->bppc * p->channelCount,
        .Height = height >> p->ss.y
        };

        CUresult result = cu->cuMemcpy2D(&memcpy2d);
        if (result != CUDA_SUCCESS) {
            LOG("cuMemcpy2D failed: %d", result);
            return VA_STATUS_ERROR_DECODING_ERROR;
        }
        offset += ((width * height) >> (p->ss.x + p->ss.y)) * fmtInfo->bppc * p->channelCount;
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

    if (cfg->entrypoint == VAEntrypointVideoProc) {
        if (num_attribs != NULL) {
            *num_attribs = drv->supports16BitSurface ? 14 : 13;
        }

        if (attrib_list != NULL) {
            attrib_list[0].type = VASurfaceAttribMinWidth;
            attrib_list[0].flags = 0;
            attrib_list[0].value.type = VAGenericValueTypeInteger;
            attrib_list[0].value.value.i = 1;

            attrib_list[1].type = VASurfaceAttribMinHeight;
            attrib_list[1].flags = 0;
            attrib_list[1].value.type = VAGenericValueTypeInteger;
            attrib_list[1].value.value.i = 1;

            attrib_list[2].type = VASurfaceAttribMaxWidth;
            attrib_list[2].flags = 0;
            attrib_list[2].value.type = VAGenericValueTypeInteger;
            attrib_list[2].value.value.i = 16384;

            attrib_list[3].type = VASurfaceAttribMaxHeight;
            attrib_list[3].flags = 0;
            attrib_list[3].value.type = VAGenericValueTypeInteger;
            attrib_list[3].value.value.i = 16384;

            int attrib_idx = 4;
            attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
            attrib_list[attrib_idx].flags = 0;
            attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
            attrib_list[attrib_idx].value.value.i = VA_FOURCC_NV12;
            attrib_idx++;

            if (drv->supports16BitSurface) {
                attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
                attrib_list[attrib_idx].flags = 0;
                attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
                attrib_list[attrib_idx].value.value.i = VA_FOURCC_P010;
                attrib_idx++;
            }

            const uint32_t rgbFormats[] = {
                VA_FOURCC_ARGB,
                VA_FOURCC_XRGB,
                VA_FOURCC_ABGR,
                VA_FOURCC_XBGR,
                VA_FOURCC_RGBA,
                VA_FOURCC_RGBX,
                VA_FOURCC_BGRA,
                VA_FOURCC_BGRX
            };
            for (uint32_t i = 0; i < ARRAY_SIZE(rgbFormats); i++) {
                attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
                attrib_list[attrib_idx].flags = 0;
                attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
                attrib_list[attrib_idx].value.value.i = (int) rgbFormats[i];
                attrib_idx++;
            }
        }

        return VA_STATUS_SUCCESS;
    }

    //LOG("with %d (%d) %p %d", cfg->cudaCodec, cfg->bitDepth, attrib_list, *num_attribs);

    if (cfg->chromaFormat != cudaVideoChromaFormat_420 && cfg->chromaFormat != cudaVideoChromaFormat_444) {
        //TODO not sure what pixel formats are needed for 422 formats
        LOG("Unknown chroma format: %d", cfg->chromaFormat);
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

    if (num_attribs != NULL) {
        int cnt = 4;
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
        *num_attribs = cnt;
    }

    if (attrib_list != NULL) {
        CUVIDDECODECAPS videoDecodeCaps = {
            .eCodecType      = cfg->cudaCodec,
            .eChromaFormat   = cfg->chromaFormat,
            .nBitDepthMinus8 = cfg->bitDepth - 8
        };

        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
        CHECK_CUDA_RESULT_RETURN(cv->cuvidGetDecoderCaps(&videoDecodeCaps), VA_STATUS_ERROR_OPERATION_FAILED);
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

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
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    VADRMPRIMESurfaceDescriptor *ptr = (VADRMPRIMESurfaceDescriptor*) descriptor;

    drv->backend->fillExportDescriptor(drv, surface, ptr);

    // LOG("Exporting with w:%d h:%d o:%d p:%d m:%" PRIx64 " o:%d p:%d m:%" PRIx64, ptr->width, ptr->height, ptr->layers[0].offset[0],
    //                                                             ptr->layers[0].pitch[0], ptr->objects[0].drm_format_modifier,
    //                                                             ptr->layers[1].offset[0], ptr->layers[1].pitch[0],
    //                                                             ptr->objects[1].drm_format_modifier);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvTerminate( VADriverContextP ctx )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("Terminating %p", ctx);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    drv->backend->destroyAllBackingImage(drv);

    deleteAllObjects(drv);

    if (drv->videoProcModule != NULL) {
        CHECK_CUDA_RESULT(cu->cuModuleUnload(drv->videoProcModule));
        drv->videoProcModule = NULL;
        drv->nv12ToArgbKernel = NULL;
    }
    if (drv->videoProcModuleP010 != NULL) {
        CHECK_CUDA_RESULT(cu->cuModuleUnload(drv->videoProcModuleP010));
        drv->videoProcModuleP010 = NULL;
        drv->p010ToArgbKernel = NULL;
    }
    if (drv->videoProcYBuffer != 0) {
        CHECK_CUDA_RESULT(cu->cuMemFree(drv->videoProcYBuffer));
        drv->videoProcYBuffer = 0;
    }
    if (drv->videoProcUVBuffer != 0) {
        CHECK_CUDA_RESULT(cu->cuMemFree(drv->videoProcUVBuffer));
        drv->videoProcUVBuffer = 0;
    }
    if (drv->videoProcArgbBuffer != 0) {
        CHECK_CUDA_RESULT(cu->cuMemFree(drv->videoProcArgbBuffer));
        drv->videoProcArgbBuffer = 0;
    }
    free(drv->cpuVideoProcYBuffer);
    free(drv->cpuVideoProcUVBuffer);
    free(drv->cpuVideoProcArgbBuffer);
    drv->cpuVideoProcYBuffer = NULL;
    drv->cpuVideoProcUVBuffer = NULL;
    drv->cpuVideoProcArgbBuffer = NULL;
    drv->cpuVideoProcYBufferSize = 0;
    drv->cpuVideoProcUVBufferSize = 0;
    drv->cpuVideoProcArgbBufferSize = 0;

    drv->backend->releaseExporter(drv);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    pthread_mutex_lock(&concurrency_mutex);
    instances--;
    LOG("Now have %d (%d max) instances", instances, max_instances);
    pthread_mutex_unlock(&concurrency_mutex);

    nvStatsLog(drv, "final");

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxDestroy(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
    drv->cudaContext = NULL;

    free(drv);

    return VA_STATUS_SUCCESS;
}

extern const NVBackend DIRECT_BACKEND;
extern const NVBackend EGL_BACKEND;

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
    drv->useCorrectNV12Format = true;
    drv->cudaGpuId = gpu;
    //make sure that we want the default GPU, and that a DRM fd that we care about is passed in
    drv->drmFd = drmFd;

    const char *modeEnv = getenv("NVD_DESCRIPTOR_MODE");
    if (modeEnv != NULL && strcmp(modeEnv, "single") == 0) {
        drv->descriptorMode = DESCRIPTOR_MODE_SINGLE;
    } else if (modeEnv != NULL && strcmp(modeEnv, "multi") == 0) {
        drv->descriptorMode = DESCRIPTOR_MODE_MULTI;
    } else if (modeEnv != NULL && strcmp(modeEnv, "auto") != 0) {
        LOG("Ignoring invalid NVD_DESCRIPTOR_MODE=%s", modeEnv);
        drv->descriptorMode = DESCRIPTOR_MODE_SINGLE;
    } else {
        drv->descriptorMode = DESCRIPTOR_MODE_SINGLE;
    }
    LOG("Descriptor mode: %s", drv->descriptorMode == DESCRIPTOR_MODE_SINGLE ? "single" : "multi")

    const char *statsEnv = getenv("NVD_STATS");
    if (statsEnv != NULL && strcmp(statsEnv, "0") != 0) {
        drv->statsEnabled = true;
        drv->statsLogInterval = 120;
        if (strcmp(statsEnv, "1") != 0) {
            char *end = NULL;
            unsigned long long interval = strtoull(statsEnv, &end, 10);
            if (end != statsEnv && interval > 0) {
                drv->statsLogInterval = interval;
            }
        }
        LOG("Stats enabled: interval=%llu decoded pictures", (unsigned long long) drv->statsLogInterval)
    }

    if (backend == EGL) {
        LOG("Selecting EGL backend");
        drv->backend = &EGL_BACKEND;
    } else if (backend == DIRECT) {
        LOG("Selecting Direct backend");
        drv->backend = &DIRECT_BACKEND;
    }

    ctx->max_profiles = MAX_PROFILES;
    ctx->max_entrypoints = 1;
    ctx->max_attributes = 1;
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
