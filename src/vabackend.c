#define _GNU_SOURCE

#include "vabackend.h"
#include "backend-common.h"

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/param.h>
#include <sys/stat.h>

#include <va/va_backend.h>
#include <va/va_backend_vpp.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>
#include <va/va_enc_av1.h>

#include <drm_fourcc.h>

#include <unistd.h>
#include <sys/types.h>
#include <stdarg.h>
#include <dlfcn.h>

#include <time.h>

#ifndef VA_FOURCC_NV16
#define VA_FOURCC_NV16 0x3631564e
#endif

#ifndef VA_FOURCC_P210
#define VA_FOURCC_P210 0x30313250
#endif

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

static bool isHEVCEncodeProfile(VAProfile profile);

static unsigned long long nv_getmonotonic_us(void)
{
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    return ((unsigned long long)tp.tv_sec * 1000000ull) +
           ((unsigned long long)tp.tv_nsec / 1000ull);
}

static bool nvAssumeLinearForDrmPrimeImport(void);

static bool nvRequireDrmPrime2ForExternalImport(void)
{
    return !nvAssumeLinearForDrmPrimeImport();
}

static bool nvAssumeLinearForDrmPrimeImport(void)
{
    return isTruthyEnv(getenv("NVD_DRM_PRIME_ASSUME_LINEAR"));
}

static pthread_mutex_t concurrency_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t instances;
static uint32_t max_instances;
static pthread_mutex_t cuda_init_backoff_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t cuda_init_retry_not_before_ns;
static uint64_t cuda_init_backoff_last_log_ns;
static bool cuda_init_backoff_force_hard;
static pthread_mutex_t cuda_init_recovery_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t cuda_init_recovery_last_attempt_ns;
static pthread_mutex_t encode_probe_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t vaImageCreateCount;
static uint64_t vaImageDestroyCount;
static uint64_t vaImageLiveBytes;
static uint64_t vaImagePeakBytes;
static uint64_t encodeInputAllocCount;
static uint64_t encodeInputFreeCount;
static uint64_t encodeInputLiveBytes;
static uint64_t encodeInputPeakBytes;
static uint64_t nvencSessionOpenCount;
static uint64_t nvencSessionDestroyCount;
static uint64_t nvencSessionPeakLive;
static uint64_t nvencBitstreamCreateCount;
static uint64_t nvencBitstreamDestroyCount;
static uint64_t nvencBitstreamPeakLive;
static uint64_t nvencRegisteredCreateCount;
static uint64_t nvencRegisteredDestroyCount;
static uint64_t nvencRegisteredPeakLive;
static uint64_t nvencMappedCreateCount;
static uint64_t nvencMappedDestroyCount;
static uint64_t nvencMappedPeakLive;
static pthread_mutex_t nvencVisibleMemMutex = PTHREAD_MUTEX_INITIALIZER;
static long long nvencVisibleGlobalDeltaUsed;
static long long nvencVisibleGlobalPeakDeltaUsed;
static pthread_once_t cuda_ctx_sync_once = PTHREAD_ONCE_INIT;
typedef CUresult (CUDAAPI *tcuCtxSynchronize_local)(void);
static tcuCtxSynchronize_local optionalCuCtxSynchronize;

static CudaFunctions *cu;
static CuvidFunctions *cv;
static NvencFunctions *nv;
static pthread_mutex_t global_codec_loader_mutex = PTHREAD_MUTEX_INITIALIZER;

extern const NVCodec __start_nvd_codecs[];
extern const NVCodec __stop_nvd_codecs[];

static FILE *LOG_OUTPUT;
static const char *LOG_SIGNATURE = "NVD_DRIVER_TRACE";

static int gpu = -1;
static enum {
    EGL, DIRECT
} backend = DIRECT;

static void* getObjectPtr(NVDriver *drv, ObjectType type, VAGenericID id);
static VAStatus nvSyncSurface(VADriverContextP ctx, VASurfaceID render_target);
static void releaseGlobalCodecFunctions(void);
static bool ensureGlobalCodecFunctionsLoaded(void);

typedef struct {
    bool valid;
    int gpuId;
    bool supportsEncodeH264;
    bool supportsEncodeH26410Bit;
    bool supportsEncodeH264444;
    bool supportsEncodeHEVC;
    bool supportsEncodeAV1;
    bool supportsEncodeAV110Bit;
    bool supportsEncodeHEVC10Bit;
    bool supportsEncodeHEVC422;
    bool supportsEncodeHEVC444;
} EncodeProbeCache;

static EncodeProbeCache encodeProbeCache = {
    .valid = false
};

#define ENCODE_PROBE_CACHE_VERSION 1

static bool writeAllToFile(int fd, const void *ptr, size_t size);
static uint64_t monotonicNowNs(void);

static uint64_t counterLoad(const uint64_t *value) {
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}

static uint64_t counterAdd(uint64_t *value, uint64_t delta) {
    return __atomic_add_fetch(value, delta, __ATOMIC_RELAXED);
}

static uint64_t counterSub(uint64_t *value, uint64_t delta) {
    return __atomic_sub_fetch(value, delta, __ATOMIC_RELAXED);
}

static void counterUpdatePeak(uint64_t *peak, uint64_t candidate) {
    uint64_t current = counterLoad(peak);
    while (candidate > current) {
        if (__atomic_compare_exchange_n(
                peak,
                &current,
                candidate,
                false,
                __ATOMIC_RELAXED,
                __ATOMIC_RELAXED)) {
            break;
        }
    }
}

static void noteVaImageCreate(size_t bytes, VAImageID imageId) {
    (void)imageId;
    (void)counterAdd(&vaImageCreateCount, 1);
    const uint64_t liveBytes = counterAdd(&vaImageLiveBytes, bytes);
    counterUpdatePeak(&vaImagePeakBytes, liveBytes);
}

static void noteVaImageDestroy(size_t bytes, VAImageID imageId) {
    (void)imageId;
    (void)counterAdd(&vaImageDestroyCount, 1);
    const uint64_t liveBytes = counterLoad(&vaImageLiveBytes);
    if (bytes <= liveBytes) {
        (void) counterSub(&vaImageLiveBytes, bytes);
    } else {
        __atomic_store_n(&vaImageLiveBytes, 0, __ATOMIC_RELAXED);
    }
}

static void noteEncodeInputAlloc(size_t bytes) {
    const uint64_t alloc = counterAdd(&encodeInputAllocCount, 1);
    const uint64_t liveBytes = counterAdd(&encodeInputLiveBytes, bytes);
    counterUpdatePeak(&encodeInputPeakBytes, liveBytes);
    (void) alloc;
}

static void noteEncodeInputFree(size_t bytes) {
    const uint64_t freeCount = counterAdd(&encodeInputFreeCount, 1);
    const uint64_t liveBytes = counterLoad(&encodeInputLiveBytes);
    if (bytes <= liveBytes) {
        (void) counterSub(&encodeInputLiveBytes, bytes);
    } else {
        __atomic_store_n(&encodeInputLiveBytes, 0, __ATOMIC_RELAXED);
    }
    (void) freeCount;
}

static const char *nvencStatusToString(NVENCSTATUS status) {
    switch (status) {
    case NV_ENC_SUCCESS:
        return "NV_ENC_SUCCESS";
    case NV_ENC_ERR_NO_ENCODE_DEVICE:
        return "NV_ENC_ERR_NO_ENCODE_DEVICE";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE:
        return "NV_ENC_ERR_UNSUPPORTED_DEVICE";
    case NV_ENC_ERR_INVALID_ENCODERDEVICE:
        return "NV_ENC_ERR_INVALID_ENCODERDEVICE";
    case NV_ENC_ERR_INVALID_DEVICE:
        return "NV_ENC_ERR_INVALID_DEVICE";
    case NV_ENC_ERR_DEVICE_NOT_EXIST:
        return "NV_ENC_ERR_DEVICE_NOT_EXIST";
    case NV_ENC_ERR_INVALID_PTR:
        return "NV_ENC_ERR_INVALID_PTR";
    case NV_ENC_ERR_INVALID_EVENT:
        return "NV_ENC_ERR_INVALID_EVENT";
    case NV_ENC_ERR_INVALID_PARAM:
        return "NV_ENC_ERR_INVALID_PARAM";
    case NV_ENC_ERR_INVALID_CALL:
        return "NV_ENC_ERR_INVALID_CALL";
    case NV_ENC_ERR_OUT_OF_MEMORY:
        return "NV_ENC_ERR_OUT_OF_MEMORY";
    case NV_ENC_ERR_ENCODER_NOT_INITIALIZED:
        return "NV_ENC_ERR_ENCODER_NOT_INITIALIZED";
    case NV_ENC_ERR_UNSUPPORTED_PARAM:
        return "NV_ENC_ERR_UNSUPPORTED_PARAM";
    case NV_ENC_ERR_LOCK_BUSY:
        return "NV_ENC_ERR_LOCK_BUSY";
    case NV_ENC_ERR_NOT_ENOUGH_BUFFER:
        return "NV_ENC_ERR_NOT_ENOUGH_BUFFER";
    case NV_ENC_ERR_INVALID_VERSION:
        return "NV_ENC_ERR_INVALID_VERSION";
    case NV_ENC_ERR_MAP_FAILED:
        return "NV_ENC_ERR_MAP_FAILED";
    case NV_ENC_ERR_NEED_MORE_INPUT:
        return "NV_ENC_ERR_NEED_MORE_INPUT";
    case NV_ENC_ERR_ENCODER_BUSY:
        return "NV_ENC_ERR_ENCODER_BUSY";
    case NV_ENC_ERR_EVENT_NOT_REGISTERD:
        return "NV_ENC_ERR_EVENT_NOT_REGISTERD";
    case NV_ENC_ERR_GENERIC:
        return "NV_ENC_ERR_GENERIC";
    case NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY:
        return "NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY";
    case NV_ENC_ERR_UNIMPLEMENTED:
        return "NV_ENC_ERR_UNIMPLEMENTED";
    case NV_ENC_ERR_RESOURCE_REGISTER_FAILED:
        return "NV_ENC_ERR_RESOURCE_REGISTER_FAILED";
    case NV_ENC_ERR_RESOURCE_NOT_REGISTERED:
        return "NV_ENC_ERR_RESOURCE_NOT_REGISTERED";
    case NV_ENC_ERR_RESOURCE_NOT_MAPPED:
        return "NV_ENC_ERR_RESOURCE_NOT_MAPPED";
    case NV_ENC_ERR_NEED_MORE_OUTPUT:
        return "NV_ENC_ERR_NEED_MORE_OUTPUT";
    default:
        return "NV_ENC_STATUS_UNKNOWN";
    }
}

static void initCudaCtxSynchronizeSymbol(void) {
    void *sym = dlsym(RTLD_DEFAULT, "cuCtxSynchronize");
    if (sym == NULL) {
        void *handle = dlopen("libcuda.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (handle != NULL) {
            sym = dlsym(handle, "cuCtxSynchronize");
        }
    }
    optionalCuCtxSynchronize = (tcuCtxSynchronize_local) sym;
}

static void resetProcessTransientState(void);
static void probeEncodeSupport(NVDriver *drv);

static void noteNvencSessionOpen(void *encoder) {
    (void) encoder;
    const uint64_t create = counterAdd(&nvencSessionOpenCount, 1);
    const uint64_t destroy = counterLoad(&nvencSessionDestroyCount);
    const uint64_t live = create >= destroy ? create - destroy : 0;
    counterUpdatePeak(&nvencSessionPeakLive, live);
}

static void noteNvencSessionDestroy(void *encoder) {
    (void) encoder;
    const uint64_t destroy = counterAdd(&nvencSessionDestroyCount, 1);
    (void) destroy;
}

static void noteNvencBitstreamCreate(void *bitstream) {
    (void) bitstream;
    const uint64_t create = counterAdd(&nvencBitstreamCreateCount, 1);
    const uint64_t destroy = counterLoad(&nvencBitstreamDestroyCount);
    const uint64_t live = create >= destroy ? create - destroy : 0;
    counterUpdatePeak(&nvencBitstreamPeakLive, live);
}

static void noteNvencBitstreamDestroy(void *bitstream) {
    (void) bitstream;
    const uint64_t destroy = counterAdd(&nvencBitstreamDestroyCount, 1);
    (void) destroy;
}

static void noteNvencRegisteredCreate(void *registered) {
    (void) registered;
    const uint64_t create = counterAdd(&nvencRegisteredCreateCount, 1);
    const uint64_t destroy = counterLoad(&nvencRegisteredDestroyCount);
    const uint64_t live = create >= destroy ? create - destroy : 0;
    counterUpdatePeak(&nvencRegisteredPeakLive, live);
}

static void noteNvencRegisteredDestroy(void *registered) {
    (void) registered;
    const uint64_t destroy = counterAdd(&nvencRegisteredDestroyCount, 1);
    (void) destroy;
}

static void noteNvencMappedCreate(void *mapped) {
    (void) mapped;
    const uint64_t create = counterAdd(&nvencMappedCreateCount, 1);
    const uint64_t destroy = counterLoad(&nvencMappedDestroyCount);
    const uint64_t live = create >= destroy ? create - destroy : 0;
    counterUpdatePeak(&nvencMappedPeakLive, live);
}

static void noteNvencMappedDestroy(void *mapped) {
    (void) mapped;
    const uint64_t destroy = counterAdd(&nvencMappedDestroyCount, 1);
    (void) destroy;
}

static bool trackedNvencObjectsIdle(void) {
    const uint64_t sessionsOpen = counterLoad(&nvencSessionOpenCount);
    const uint64_t sessionsDestroy = counterLoad(&nvencSessionDestroyCount);
    const uint64_t bitstreamsCreate = counterLoad(&nvencBitstreamCreateCount);
    const uint64_t bitstreamsDestroy = counterLoad(&nvencBitstreamDestroyCount);
    const uint64_t registeredCreate = counterLoad(&nvencRegisteredCreateCount);
    const uint64_t registeredDestroy = counterLoad(&nvencRegisteredDestroyCount);
    const uint64_t mappedCreate = counterLoad(&nvencMappedCreateCount);
    const uint64_t mappedDestroy = counterLoad(&nvencMappedDestroyCount);

    return sessionsOpen == sessionsDestroy &&
           bitstreamsCreate == bitstreamsDestroy &&
           registeredCreate == registeredDestroy &&
           mappedCreate == mappedDestroy;
}

static bool trackedCudaInitResourcesIdle(void) {
    const uint64_t vaCreate = counterLoad(&vaImageCreateCount);
    const uint64_t vaDestroy = counterLoad(&vaImageDestroyCount);

    return vaCreate == vaDestroy &&
           counterLoad(&vaImageLiveBytes) == 0 &&
           counterLoad(&encodeInputLiveBytes) == 0 &&
           trackedNvencObjectsIdle() &&
           directResourcesIdle();
}

static unsigned long long hashString64(const char *value) {
    unsigned long long hash = 1469598103934665603ull;
    if (value == NULL) {
        return hash;
    }
    while (*value != '\0') {
        hash ^= (unsigned long long) (unsigned char) *value;
        hash *= 1099511628211ull;
        value++;
    }
    return hash;
}

static bool bytesToHexString(const uint8_t *bytes, size_t byteCount, char *out, size_t outSize) {
    static const char hexDigits[] = "0123456789abcdef";
    if (bytes == NULL || out == NULL) {
        return false;
    }
    if ((byteCount * 2) + 1 > outSize) {
        return false;
    }
    for (size_t i = 0; i < byteCount; i++) {
        out[(i * 2) + 0] = hexDigits[bytes[i] >> 4];
        out[(i * 2) + 1] = hexDigits[bytes[i] & 0x0f];
    }
    out[byteCount * 2] = '\0';
    return true;
}

static void sanitizeToken(char *token) {
    if (token == NULL) {
        return;
    }
    for (char *cursor = token; *cursor != '\0'; cursor++) {
        if (*cursor == '-' || *cursor == '_') {
            continue;
        }
        if (isalnum((unsigned char) *cursor)) {
            continue;
        }
        *cursor = '_';
    }
}

static bool buildCudaDeviceCacheToken(const NVDriver *drv, char *token, size_t tokenSize) {
    if (drv == NULL || token == NULL || tokenSize == 0) {
        return false;
    }
    token[0] = '\0';

    if (drv->cu != NULL && drv->cu->cuDeviceGet != NULL) {
        CUdevice device = 0;
        if (drv->cu->cuDeviceGet(&device, drv->cudaGpuId) == CUDA_SUCCESS) {
            CUuuid uuid = {0};
            if (drv->cu->cuDeviceGetUuid_v2 != NULL &&
                drv->cu->cuDeviceGetUuid_v2(&uuid, device) == CUDA_SUCCESS) {
                char uuidHex[(16 * 2) + 1];
                if (bytesToHexString((const uint8_t *) uuid.bytes, sizeof(uuid.bytes), uuidHex, sizeof(uuidHex))) {
                    const int written = snprintf(token, tokenSize, "uuid%s", uuidHex);
                    if (written > 0 && (size_t) written < tokenSize) {
                        return true;
                    }
                }
            }

            if (drv->cu->cuDeviceGetUuid != NULL &&
                drv->cu->cuDeviceGetUuid(&uuid, device) == CUDA_SUCCESS) {
                char uuidHex[(16 * 2) + 1];
                if (bytesToHexString((const uint8_t *) uuid.bytes, sizeof(uuid.bytes), uuidHex, sizeof(uuidHex))) {
                    const int written = snprintf(token, tokenSize, "uuid%s", uuidHex);
                    if (written > 0 && (size_t) written < tokenSize) {
                        return true;
                    }
                }
            }

            if (drv->cu->cuDeviceGetPCIBusId != NULL) {
                char pciBusId[32] = {0};
                if (drv->cu->cuDeviceGetPCIBusId(pciBusId, sizeof(pciBusId), device) == CUDA_SUCCESS) {
                    sanitizeToken(pciBusId);
                    const int written = snprintf(token, tokenSize, "pci%s", pciBusId);
                    if (written > 0 && (size_t) written < tokenSize) {
                        return true;
                    }
                }
            }
        }
    }

    const int written = snprintf(token, tokenSize, "idx%d", drv->cudaGpuId);
    return written > 0 && (size_t) written < tokenSize;
}

static bool ensureDirectoryRecursive(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return false;
    }

    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int) sizeof(tmp)) {
        return false;
    }

    const size_t len = strlen(tmp);
    if (len == 0) {
        return false;
    }
    if (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (char *cursor = tmp + 1; *cursor != '\0'; cursor++) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            return false;
        }
        *cursor = '/';
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

static bool buildEncodeProbePersistentCachePath(
    const NVDriver *drv,
    uint32_t negotiatedApiVersion,
    char *path,
    size_t pathSize
) {
    if (drv == NULL || path == NULL || pathSize == 0 || drv->nv == NULL ||
        drv->nv->NvEncodeAPICreateInstance == NULL) {
        return false;
    }

    const char *xdgCacheHome = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    char cacheDir[PATH_MAX];
    if (xdgCacheHome != NULL && xdgCacheHome[0] != '\0') {
        if (snprintf(cacheDir, sizeof(cacheDir), "%s/nvidia-vaapi-driver", xdgCacheHome) >= (int) sizeof(cacheDir)) {
            return false;
        }
    } else if (home != NULL && home[0] != '\0') {
        if (snprintf(cacheDir, sizeof(cacheDir), "%s/.cache/nvidia-vaapi-driver", home) >= (int) sizeof(cacheDir)) {
            return false;
        }
    } else {
        return false;
    }

    if (!ensureDirectoryRecursive(cacheDir)) {
        return false;
    }

    Dl_info info;
    if (dladdr((void *) drv->nv->NvEncodeAPICreateInstance, &info) == 0 || info.dli_fname == NULL) {
        return false;
    }

    struct stat libStat;
    if (stat(info.dli_fname, &libStat) != 0) {
        return false;
    }

    const unsigned long long pathHash = hashString64(info.dli_fname);
    const unsigned long long libSize = (unsigned long long) libStat.st_size;
    const unsigned long long libMtime = (unsigned long long) libStat.st_mtime;

    char deviceToken[64] = {0};
    if (!buildCudaDeviceCacheToken(drv, deviceToken, sizeof(deviceToken))) {
        return false;
    }

    const int written = snprintf(
        path,
        pathSize,
        "%s/encode-probe-v%d-dev%s-gpu%d-api%u-lib%llx-%llu-%llu.cache",
        cacheDir,
        ENCODE_PROBE_CACHE_VERSION,
        deviceToken,
        drv->cudaGpuId,
        negotiatedApiVersion,
        pathHash,
        libSize,
        libMtime
    );
    return written > 0 && (size_t) written < pathSize;
}

static bool loadEncodeProbePersistentCache(const char *path, NVDriver *drv) {
    if (path == NULL || path[0] == '\0' || drv == NULL) {
        return false;
    }

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return false;
    }

    unsigned int version = 0;
    int supportsEncodeH264 = 0;
    int supportsEncodeH26410Bit = 0;
    int supportsEncodeH264444 = 0;
    int supportsEncodeHEVC = 0;
    int supportsEncodeAV1 = 0;
    int supportsEncodeAV110Bit = 0;
    int supportsEncodeHEVC10Bit = 0;
    int supportsEncodeHEVC422 = 0;
    int supportsEncodeHEVC444 = 0;
    const int scanned = fscanf(
        fp,
        "version=%u h264=%d h26410=%d h264444=%d hevc=%d av1=%d av110=%d hevc10=%d hevc422=%d hevc444=%d",
        &version,
        &supportsEncodeH264,
        &supportsEncodeH26410Bit,
        &supportsEncodeH264444,
        &supportsEncodeHEVC,
        &supportsEncodeAV1,
        &supportsEncodeAV110Bit,
        &supportsEncodeHEVC10Bit,
        &supportsEncodeHEVC422,
        &supportsEncodeHEVC444
    );
    fclose(fp);

    if (scanned != 10 || version != ENCODE_PROBE_CACHE_VERSION) {
        return false;
    }

    drv->supportsEncodeH264 = supportsEncodeH264 != 0;
    drv->supportsEncodeH26410Bit = supportsEncodeH26410Bit != 0;
    drv->supportsEncodeH264444 = supportsEncodeH264444 != 0;
    drv->supportsEncodeHEVC = supportsEncodeHEVC != 0;
    drv->supportsEncodeAV1 = supportsEncodeAV1 != 0;
    drv->supportsEncodeAV110Bit = supportsEncodeAV110Bit != 0;
    drv->supportsEncodeHEVC10Bit = supportsEncodeHEVC10Bit != 0;
    drv->supportsEncodeHEVC422 = supportsEncodeHEVC422 != 0;
    drv->supportsEncodeHEVC444 = supportsEncodeHEVC444 != 0;
    return true;
}

static void storeEncodeProbePersistentCache(const char *path, const NVDriver *drv) {
    if (path == NULL || path[0] == '\0' || drv == NULL) {
        return;
    }

    char tmpPath[PATH_MAX];
    const int written = snprintf(tmpPath, sizeof(tmpPath), "%s.tmp.%d", path, (int) getpid());
    if (written <= 0 || (size_t) written >= sizeof(tmpPath)) {
        return;
    }

    int fd = open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return;
    }

    char buffer[512];
    const int payloadSize = snprintf(
        buffer,
        sizeof(buffer),
        "version=%d h264=%d h26410=%d h264444=%d hevc=%d av1=%d av110=%d hevc10=%d hevc422=%d hevc444=%d\n",
        ENCODE_PROBE_CACHE_VERSION,
        drv->supportsEncodeH264 ? 1 : 0,
        drv->supportsEncodeH26410Bit ? 1 : 0,
        drv->supportsEncodeH264444 ? 1 : 0,
        drv->supportsEncodeHEVC ? 1 : 0,
        drv->supportsEncodeAV1 ? 1 : 0,
        drv->supportsEncodeAV110Bit ? 1 : 0,
        drv->supportsEncodeHEVC10Bit ? 1 : 0,
        drv->supportsEncodeHEVC422 ? 1 : 0,
        drv->supportsEncodeHEVC444 ? 1 : 0
    );
    if (payloadSize <= 0 || (size_t) payloadSize >= sizeof(buffer) ||
        !writeAllToFile(fd, buffer, (size_t) payloadSize)) {
        close(fd);
        unlink(tmpPath);
        return;
    }

    close(fd);
    if (rename(tmpPath, path) != 0) {
        unlink(tmpPath);
    }
}

static bool isEncodeProbeCacheEnabled(void) {
    const char *value = getenv("NVD_ENCODE_PROBE_CACHE");
    if (value == NULL) {
        return true;
    }
    return isTruthyEnv(value);
}

static bool writeAllToFile(int fd, const void *ptr, size_t size) {
    const uint8_t *cursor = (const uint8_t *) ptr;
    size_t remaining = size;
    while (remaining > 0) {
        ssize_t wrote = write(fd, cursor, remaining);
        if (wrote < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        cursor += (size_t) wrote;
        remaining -= (size_t) wrote;
    }
    return true;
}

const NVFormatInfo formatsInfo[] =
{
    [NV_FORMAT_NONE] = {0},
    [NV_FORMAT_NV12] = {1, 2, DRM_FORMAT_NV12,     false, false, {{1, DRM_FORMAT_R8,       {0,0}}, {2, DRM_FORMAT_RG88,   {1,1}}},                            {VA_FOURCC_NV12, VA_LSB_FIRST,   12, 0,0,0,0,0}},
#if NVENCAPI_MAJOR_VERSION >= 13
    [NV_FORMAT_NV16] = {1, 2, DRM_FORMAT_NV16,     false, false, {{1, DRM_FORMAT_R8,       {0,0}}, {2, DRM_FORMAT_RG88,   {1,0}}},                            {VA_FOURCC_NV16, VA_LSB_FIRST,   16, 0,0,0,0,0}},
#endif
    [NV_FORMAT_P010] = {2, 2, DRM_FORMAT_P010,     true,  false, {{1, DRM_FORMAT_R16,      {0,0}}, {2, DRM_FORMAT_RG1616, {1,1}}},                            {VA_FOURCC_P010, VA_LSB_FIRST,   24, 0,0,0,0,0}},
#if NVENCAPI_MAJOR_VERSION >= 13
    [NV_FORMAT_P210] = {2, 2, DRM_FORMAT_P210,     true,  false, {{1, DRM_FORMAT_R16,      {0,0}}, {2, DRM_FORMAT_RG1616, {1,0}}},                            {VA_FOURCC_P210, VA_LSB_FIRST,   32, 0,0,0,0,0}},
#endif
    [NV_FORMAT_P012] = {2, 2, DRM_FORMAT_P012,     true,  false, {{1, DRM_FORMAT_R16,      {0,0}}, {2, DRM_FORMAT_RG1616, {1,1}}},                            {VA_FOURCC_P012, VA_LSB_FIRST,   24, 0,0,0,0,0}},
    [NV_FORMAT_P016] = {2, 2, DRM_FORMAT_P016,     true,  false, {{1, DRM_FORMAT_R16,      {0,0}}, {2, DRM_FORMAT_RG1616, {1,1}}},                            {VA_FOURCC_P016, VA_LSB_FIRST,   24, 0,0,0,0,0}},
    [NV_FORMAT_ARGB] = {1, 1, DRM_FORMAT_ARGB8888, false, false, {{4, DRM_FORMAT_ARGB8888, {0,0}}},                                                      {VA_FOURCC_ARGB, VA_LSB_FIRST,   32, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000}},
    [NV_FORMAT_444P] = {1, 3, DRM_FORMAT_YUV444,   false, true,  {{1, DRM_FORMAT_R8,       {0,0}}, {1, DRM_FORMAT_R8,     {0,0}}, {1, DRM_FORMAT_R8, {0,0}}}, {VA_FOURCC_444P, VA_LSB_FIRST,   24, 0,0,0,0,0}},
#if VA_CHECK_VERSION(1, 20, 0)
    [NV_FORMAT_Q416] = {2, 3, DRM_FORMAT_INVALID,  true,  true,  {{1, DRM_FORMAT_R16,      {0,0}}, {1, DRM_FORMAT_R16,    {0,0}}, {1, DRM_FORMAT_R16,{0,0}}}, {VA_FOURCC_Q416, VA_LSB_FIRST,   48, 0,0,0,0,0}},
#endif
};

static NVFormat nvFormatFromVaFormat(uint32_t fourcc) {
    for (uint32_t i = NV_FORMAT_NONE + 1; i < ARRAY_SIZE(formatsInfo); i++) {
        if (formatsInfo[i].numPlanes == 0) {
            continue;
        }
        if (formatsInfo[i].vaFormat.fourcc == fourcc) {
            return i;
        }
    }
    return NV_FORMAT_NONE;
}

static NVFormat nvFormatFromSurface(const NVSurface *surface) {
    if (surface == NULL) {
        return NV_FORMAT_NONE;
    }

    if (surface->rtFormat == VA_RT_FORMAT_RGB32) {
        return NV_FORMAT_ARGB;
    }

    switch (surface->format) {
    case cudaVideoSurfaceFormat_NV12:
        return NV_FORMAT_NV12;
#if NVENCAPI_MAJOR_VERSION >= 13
    case cudaVideoSurfaceFormat_NV16:
        return NV_FORMAT_NV16;
    case cudaVideoSurfaceFormat_P216:
        return NV_FORMAT_P210;
#endif
    case cudaVideoSurfaceFormat_P016:
        if (surface->chromaFormat == cudaVideoChromaFormat_422) {
            return NV_FORMAT_P210;
        }
        if (surface->bitDepth <= 10) {
            return NV_FORMAT_P010;
        }
        if (surface->bitDepth <= 12) {
            return NV_FORMAT_P012;
        }
        return NV_FORMAT_P016;
    case cudaVideoSurfaceFormat_YUV444:
        return NV_FORMAT_444P;
    case cudaVideoSurfaceFormat_YUV444_16Bit:
#if VA_CHECK_VERSION(1, 20, 0)
        return NV_FORMAT_Q416;
#else
        return NV_FORMAT_NONE;
#endif
    default:
        return NV_FORMAT_NONE;
    }
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

    if (!ensureGlobalCodecFunctionsLoaded()) {
        return;
    }
}

__attribute__ ((destructor))
static void cleanup() {
    releaseGlobalCodecFunctions();
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

    fprintf(
        LOG_OUTPUT,
        "[%s] %10ld.%09ld [%d-%d] %s:%4d %24s %s\n",
        LOG_SIGNATURE,
        (long)tp.tv_sec,
        tp.tv_nsec,
        getpid(),
        nv_gettid(),
        filename,
        line,
        function,
        formattedMessage
    );
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

static uint64_t monotonicNowNs(void) {
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    return ((uint64_t) tp.tv_sec * 1000000000ull) + (uint64_t) tp.tv_nsec;
}

static bool isCudaFatalContextInitError(CUresult err) {
    return err == (CUresult) 719 || err == (CUresult) 999;
}

static void noteCudaInitBackoff(uint64_t backoffNs, const char *reason, CUresult err) {
    const uint64_t nowNs = monotonicNowNs();
    const uint64_t candidateRetryNs = nowNs + backoffNs;
    bool shouldLog = false;
    const bool forceHard = isCudaFatalContextInitError(err);

    pthread_mutex_lock(&cuda_init_backoff_mutex);
    if (candidateRetryNs > cuda_init_retry_not_before_ns) {
        cuda_init_retry_not_before_ns = candidateRetryNs;
    }
    if (forceHard) {
        cuda_init_backoff_force_hard = true;
    }
    if (nowNs - cuda_init_backoff_last_log_ns > 500000000ull) {
        cuda_init_backoff_last_log_ns = nowNs;
        shouldLog = true;
    }
    pthread_mutex_unlock(&cuda_init_backoff_mutex);

    if (shouldLog) {
        LOG(
            "CUDA init backoff armed reason=%s err=%d backoff_ms=%llu",
            reason,
            (int) err,
            (unsigned long long) (backoffNs / 1000000ull)
        );
    }
}

static bool cudaInitBackoffActive(uint64_t *remainingNs) {
    const uint64_t nowNs = monotonicNowNs();
    bool active = false;
    uint64_t remaining = 0;

    pthread_mutex_lock(&cuda_init_backoff_mutex);
    if (nowNs < cuda_init_retry_not_before_ns) {
        active = true;
        remaining = cuda_init_retry_not_before_ns - nowNs;
    } else {
        cuda_init_retry_not_before_ns = 0;
        cuda_init_backoff_force_hard = false;
    }
    pthread_mutex_unlock(&cuda_init_backoff_mutex);

    if (remainingNs != NULL) {
        *remainingNs = remaining;
    }

    return active;
}

static bool cudaInitBackoffForced(void) {
    return __atomic_load_n(&cuda_init_backoff_force_hard, __ATOMIC_RELAXED);
}


static bool isCudaOutOfMemory(CUresult err) {
    return err == (CUresult) 2;
}

static bool tryCudaInitRecoveryReset(NVDriver *drv, CUresult err, const char *reason) {
    const uint64_t nowNs = monotonicNowNs();
    CUresult resetResult;

    if (!isCudaFatalContextInitError(err)) {
        return false;
    }
    if (drv == NULL || drv->cu == NULL || drv->cu->cuDevicePrimaryCtxReset == NULL) {
        return false;
    }
    if (!trackedCudaInitResourcesIdle()) {
        LOG(
            "Skipping CUDA init recovery reset reason=%s err=%d because tracked resources are still live",
            reason,
            (int) err
        );
        return false;
    }

    pthread_mutex_lock(&cuda_init_recovery_mutex);
    if (nowNs - cuda_init_recovery_last_attempt_ns < 500000000ull) {
        pthread_mutex_unlock(&cuda_init_recovery_mutex);
        LOG(
            "Skipping CUDA init recovery reset reason=%s err=%d because reset retry window is active",
            reason,
            (int) err
        );
        return false;
    }
    cuda_init_recovery_last_attempt_ns = nowNs;
    pthread_mutex_unlock(&cuda_init_recovery_mutex);

    LOG(
        "Attempting CUDA primary context reset reason=%s err=%d gpu_id=%d",
        reason,
        (int) err,
        drv->cudaGpuId
    );

    resetResult = drv->cu->cuDevicePrimaryCtxReset(drv->cudaGpuId);
    if (CHECK_CUDA_RESULT(resetResult)) {
        LOG(
            "CUDA primary context reset failed reason=%s err=%d gpu_id=%d",
            reason,
            (int) err,
            drv->cudaGpuId
        );
        noteCudaInitBackoff(300000000ull, "cuDevicePrimaryCtxReset", resetResult);
        return false;
    }

    LOG(
        "CUDA primary context reset succeeded reason=%s err=%d gpu_id=%d",
        reason,
        (int) err,
        drv->cudaGpuId
    );
    return true;
}

static bool retainPrimaryCudaContext(NVDriver *drv, const char *reason) {
    drv->cudaContext = NULL;
    CUresult retainResult =
        drv->cu->cuDevicePrimaryCtxRetain(&drv->cudaContext, drv->cudaGpuId);
    if (CHECK_CUDA_RESULT(retainResult)) {
        drv->cudaContext = NULL;
        noteCudaInitBackoff(
            isCudaOutOfMemory(retainResult) ? 2000000000ull : 300000000ull,
            reason,
            retainResult
        );
        if (tryCudaInitRecoveryReset(drv, retainResult, reason)) {
            retainResult = drv->cu->cuDevicePrimaryCtxRetain(&drv->cudaContext, drv->cudaGpuId);
            if (retainResult == CUDA_SUCCESS) {
                drv->usesPrimaryCudaContext = true;
                __atomic_store_n(&cuda_init_backoff_force_hard, false, __ATOMIC_RELAXED);
                LOG(
                    "Using CUDA primary context after reset (gpu_id=%d reason=%s)",
                    drv->cudaGpuId,
                    reason
                );
                return true;
            }
            CHECK_CUDA_RESULT(retainResult);
            drv->cudaContext = NULL;
            noteCudaInitBackoff(
                isCudaOutOfMemory(retainResult) ? 2000000000ull : 300000000ull,
                "cuDevicePrimaryCtxRetain_after_reset",
                retainResult
            );
        }
        LOG("Unable to retain CUDA primary context (reason=%s)", reason);
        return false;
    }

    drv->usesPrimaryCudaContext = true;
    __atomic_store_n(&cuda_init_backoff_force_hard, false, __ATOMIC_RELAXED);
    LOG("Using CUDA primary context (gpu_id=%d reason=%s)", drv->cudaGpuId, reason);
    return true;
}

static bool initCudaContext(NVDriver *drv) {
    const bool preferPrimaryContext = isTruthyEnv(getenv("NVD_USE_PRIMARY_CUDA_CONTEXT"));
    drv->usesPrimaryCudaContext = false;

    if (preferPrimaryContext) {
        if (retainPrimaryCudaContext(drv, "NVD_USE_PRIMARY_CUDA_CONTEXT")) {
            return true;
        }
        LOG("Primary context requested but unavailable; falling back to cuCtxCreate");
    }

    drv->cudaContext = NULL;
    CUresult createResult =
        drv->cu->cuCtxCreate(
            &drv->cudaContext,
            CU_CTX_SCHED_BLOCKING_SYNC,
            drv->cudaGpuId
        );
    if (createResult == CUDA_SUCCESS) {
        __atomic_store_n(&cuda_init_backoff_force_hard, false, __ATOMIC_RELAXED);
        return true;
    }
    CHECK_CUDA_RESULT(createResult);
    noteCudaInitBackoff(
        isCudaOutOfMemory(createResult) ? 2000000000ull : 300000000ull,
        "cuCtxCreate",
        createResult
    );
    if (drv->cudaContext != NULL) {
        LOG("cuCtxCreate returned error but left a non-null context; destroying partial context");
        CHECK_CUDA_RESULT(drv->cu->cuCtxDestroy(drv->cudaContext));
        drv->cudaContext = NULL;
    }

    if (tryCudaInitRecoveryReset(drv, createResult, "cuCtxCreate")) {
        createResult =
            drv->cu->cuCtxCreate(
                &drv->cudaContext,
                CU_CTX_SCHED_BLOCKING_SYNC,
                drv->cudaGpuId
            );
        if (createResult == CUDA_SUCCESS) {
            __atomic_store_n(&cuda_init_backoff_force_hard, false, __ATOMIC_RELAXED);
            LOG("cuCtxCreate succeeded after CUDA primary context reset (gpu_id=%d)", drv->cudaGpuId);
            return true;
        }
        CHECK_CUDA_RESULT(createResult);
        noteCudaInitBackoff(
            isCudaOutOfMemory(createResult) ? 2000000000ull : 300000000ull,
            "cuCtxCreate_after_reset",
            createResult
        );
        if (drv->cudaContext != NULL) {
            LOG("cuCtxCreate after reset returned error but left a non-null context; destroying partial context");
            CHECK_CUDA_RESULT(drv->cu->cuCtxDestroy(drv->cudaContext));
            drv->cudaContext = NULL;
        }
    }

    if (preferPrimaryContext) {
        return false;
    }

    LOG("cuCtxCreate failed for gpu_id=%d; trying CUDA primary context fallback", drv->cudaGpuId);
    return retainPrimaryCudaContext(drv, "cuCtxCreate_failed");
}

static bool recycleCudaContextAfterLastSession(NVDriver *drv, const char *reason) {
    if (drv == NULL || drv->cu == NULL) {
        return false;
    }
    if (!isTruthyEnv(getenv("NVD_EXPERIMENTAL_RECYCLE_CUDA_ON_LAST_SESSION_DESTROY"))) {
        return false;
    }
    if (!directResourcesIdle()) {
        LOG(
            "Skipping CUDA recycle after last session reason=%s because tracked resources are still live",
            reason != NULL ? reason : "(null)"
        );
        return false;
    }

    LOG(
        "Recycling CUDA context after last session reason=%s uses_primary=%d ctx=%p",
        reason != NULL ? reason : "(null)",
        drv->usesPrimaryCudaContext ? 1 : 0,
        drv->cudaContext
    );

    if (drv->cudaContext != NULL) {
        if (drv->usesPrimaryCudaContext) {
            if (drv->cu->cuDevicePrimaryCtxReset == NULL) {
                LOG("Skipping CUDA recycle after last session because cuDevicePrimaryCtxReset is unavailable");
                return false;
            }
            if (CHECK_CUDA_RESULT(drv->cu->cuDevicePrimaryCtxReset(drv->cudaGpuId))) {
                LOG("CUDA recycle after last session failed during primary context reset");
                return false;
            }
        } else {
            if (CHECK_CUDA_RESULT(drv->cu->cuCtxDestroy(drv->cudaContext))) {
                LOG("CUDA recycle after last session failed during cuCtxDestroy");
                return false;
            }
        }
    }

    drv->cudaContext = NULL;
    drv->usesPrimaryCudaContext = false;

    if (!initCudaContext(drv)) {
        LOG("CUDA recycle after last session failed to reinitialize context");
        return false;
    }

    LOG(
        "CUDA recycle after last session succeeded uses_primary=%d ctx=%p",
        drv->usesPrimaryCudaContext ? 1 : 0,
        drv->cudaContext
    );
    return true;
}

static bool reloadGlobalCodecFunctionsAfterLastSession(NVDriver *drv, const char *reason) {
    if (drv == NULL) {
        return false;
    }

    LOG(
        "Reloading global codec function tables after last session reason=%s",
        reason != NULL ? reason : "(null)"
    );

    resetProcessTransientState();
    releaseGlobalCodecFunctions();
    if (!ensureGlobalCodecFunctionsLoaded()) {
        LOG(
            "Global codec function reload failed after last session reason=%s",
            reason != NULL ? reason : "(null)"
        );
        return false;
    }

    drv->cu = cu;
    drv->cv = cv;
    drv->nv = nv;
    probeEncodeSupport(drv);

    LOG(
        "Reloaded global codec function tables after last session reason=%s cu=%p cv=%p nv=%p",
        reason != NULL ? reason : "(null)",
        drv->cu,
        drv->cv,
        drv->nv
    );
    return true;
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

static void appendHevcPackedHeaderBuffer(
    AppendableBuffer *ab,
    const uint8_t *buf,
    uint64_t size,
    bool hasEmulationBytes
) {
    if (size == 0 || buf == NULL) {
        return;
    }

    if (hasEmulationBytes) {
        appendBuffer(ab, buf, size);
        return;
    }

    size_t prefixBytes = 0;
    if (size >= 4 &&
        buf[0] == 0x00 &&
        buf[1] == 0x00 &&
        buf[2] == 0x00 &&
        buf[3] == 0x01) {
        prefixBytes = 4;
    } else if (size >= 3 &&
               buf[0] == 0x00 &&
               buf[1] == 0x00 &&
               buf[2] == 0x01) {
        prefixBytes = 3;
    }

    if (prefixBytes > 0) {
        appendBuffer(ab, buf, prefixBytes);
    }

    const size_t nalHeaderBytes =
        size > prefixBytes ? ((size - prefixBytes) >= 2 ? 2 : (size - prefixBytes))
                           : 0;
    if (nalHeaderBytes > 0) {
        appendBuffer(ab, buf + prefixBytes, nalHeaderBytes);
    }

    unsigned consecutiveZeros = 0;
    static const uint8_t kEmulationPreventionByte = 0x03;
    for (size_t i = prefixBytes + nalHeaderBytes; i < size; ++i) {
        const uint8_t byte = buf[i];
        if (consecutiveZeros >= 2 && byte <= 0x03) {
            appendBuffer(ab, &kEmulationPreventionByte, 1);
            consecutiveZeros = 0;
        }
        appendBuffer(ab, &byte, 1);
        consecutiveZeros = (byte == 0x00) ? (consecutiveZeros + 1) : 0;
    }
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

static void destroyBufferObject(NVBuffer *buf) {
    if (buf == NULL) {
        return;
    }

    if (buf->ptr != NULL) {
        free(buf->ptr);
        buf->ptr = NULL;
    }

    if (buf->codedData != NULL) {
        free(buf->codedData);
        buf->codedData = NULL;
    }
}

static void destroyImageObject(NVDriver *drv, VAGenericID imageId, NVImage *img) {
    if (img == NULL) {
        return;
    }

    Object imageBufferObj = getObjectByPtr(drv, OBJECT_TYPE_BUFFER, img->imageBuffer);
    if (imageBufferObj != NULL) {
        const size_t imageBufferSize = img->imageBuffer != NULL ? img->imageBuffer->size : 0;
        noteVaImageDestroy(imageBufferSize, imageId);
        destroyBufferObject((NVBuffer *) imageBufferObj->obj);
        deleteObject(drv, imageBufferObj->id);
    }
}

static void destroySurfaceObject(NVDriver *drv, VAGenericID surfaceId, NVSurface *surface) {
    if (surface == NULL) {
        return;
    }

    LOG(
        "deleteAllObjects destroying surface %d (%p) context=%p size=%ux%u rt_format=0x%x resolving=%d decode_failed=%d",
        surface->pictureIdx,
        surface,
        surface->context,
        surface->width,
        surface->height,
        surface->rtFormat,
        surface->resolving,
        surface->decodeFailed
    );
    drv->backend->detachBackingImageFromSurface(drv, surface);
    deleteObject(drv, surfaceId);
}

static bool guidEqual(const GUID *a, const GUID *b) {
    return a->Data1 == b->Data1 &&
           a->Data2 == b->Data2 &&
           a->Data3 == b->Data3 &&
           memcmp(a->Data4, b->Data4, sizeof(a->Data4)) == 0;
}

static bool isH264EncodeProfile(VAProfile profile) {
    return profile == VAProfileH264ConstrainedBaseline ||
           profile == VAProfileH264Main ||
           profile == VAProfileH264High ||
           profile == VAProfileH264High10;
}

static bool isHEVCEncodeProfile(VAProfile profile) {
    return profile == VAProfileHEVCMain ||
           profile == VAProfileHEVCMain10 ||
           profile == VAProfileHEVCMain422_10 ||
           profile == VAProfileHEVCMain12 ||
           profile == VAProfileHEVCMain444 ||
           profile == VAProfileHEVCMain444_10 ||
           profile == VAProfileHEVCMain444_12;
}

static bool isHEVC422EncodeProfile(VAProfile profile) {
    return profile == VAProfileHEVCMain422_10;
}

static bool isHEVC444EncodeProfile(VAProfile profile) {
    return profile == VAProfileHEVCMain444 ||
           profile == VAProfileHEVCMain444_10 ||
           profile == VAProfileHEVCMain444_12;
}

static bool isAV1EncodeProfile(VAProfile profile) {
    return profile == VAProfileAV1Profile0;
}

static bool isEncodeProfile(VAProfile profile) {
    return isH264EncodeProfile(profile) ||
           isHEVCEncodeProfile(profile) ||
           isAV1EncodeProfile(profile);
}

static bool isEncodeProfileSupportedByDriver(const NVDriver *drv, VAProfile profile) {
    if (profile == VAProfileH264High10) {
#if NVENCAPI_MAJOR_VERSION >= 13
        return drv->supportsEncodeH264 && drv->supportsEncodeH26410Bit;
#else
        return false;
#endif
    }

    if (isH264EncodeProfile(profile)) {
        return drv->supportsEncodeH264;
    }
    if (isHEVCEncodeProfile(profile)) {
        if (!drv->supportsEncodeHEVC) {
            return false;
        }

        switch (profile) {
        case VAProfileHEVCMain:
            return true;
        case VAProfileHEVCMain10:
            return drv->supportsEncodeHEVC10Bit;
        case VAProfileHEVCMain422_10:
            return drv->supportsEncodeHEVC10Bit && drv->supportsEncodeHEVC422;
        case VAProfileHEVCMain12:
            // NVENC does not expose HEVC 12-bit encode in the public API.
            return false;
        case VAProfileHEVCMain444:
            return drv->supportsEncodeHEVC444;
        case VAProfileHEVCMain444_10:
            return drv->supportsEncodeHEVC444 && drv->supportsEncodeHEVC10Bit;
        case VAProfileHEVCMain444_12:
            return false;
        default:
            return false;
        }
    }
    if (isAV1EncodeProfile(profile)) {
        return drv->supportsEncodeAV1;
    }

    return false;
}

static bool isEncodeEntrypoint(VAEntrypoint entrypoint) {
    return entrypoint == VAEntrypointEncSlice;
}

static bool isVideoProcEntrypoint(VAEntrypoint entrypoint) {
    return entrypoint == VAEntrypointVideoProc;
}

static size_t countOtherLiveCodecContextsLocked(NVDriver *drv, VAGenericID excludeContextId) {
    size_t count = 0;

    ARRAY_FOR_EACH(Object, o, &drv->objects)
        if (o->type != OBJECT_TYPE_CONTEXT || o->id == excludeContextId || o->obj == NULL) {
            continue;
        }

        NVContext *otherCtx = (NVContext *) o->obj;
        if (isVideoProcEntrypoint(otherCtx->entrypoint)) {
            continue;
        }

        count++;
    END_FOR_EACH

    return count;
}

static uint32_t getVideoProcRtFormatMask(NVDriver *drv) {
    uint32_t mask = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32;

#if NVENCAPI_MAJOR_VERSION >= 13
    mask |= VA_RT_FORMAT_YUV422;
#endif

    if (drv->supports16BitSurface) {
        mask |= VA_RT_FORMAT_YUV420_10 | VA_RT_FORMAT_YUV420_12;
#if NVENCAPI_MAJOR_VERSION >= 13
        mask |= VA_RT_FORMAT_YUV422_10 | VA_RT_FORMAT_YUV422_12;
#endif
    }

    if (drv->supports444Surface) {
        mask |= VA_RT_FORMAT_YUV444;
        if (drv->supports16BitSurface) {
            mask |= VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV444_12;
        }
    }

    return mask;
}

static const char *getEncodeCodecName(VAProfile profile) {
    if (isH264EncodeProfile(profile)) {
        return "H.264";
    }
    if (isHEVCEncodeProfile(profile)) {
        return "HEVC";
    }
    if (isAV1EncodeProfile(profile)) {
        return "AV1";
    }

    return "unknown";
}

static GUID vaProfileToEncodeGuid(VAProfile profile) {
    if (isHEVCEncodeProfile(profile)) {
        return NV_ENC_CODEC_HEVC_GUID;
    }
    if (isAV1EncodeProfile(profile)) {
        return NV_ENC_CODEC_AV1_GUID;
    }

    return NV_ENC_CODEC_H264_GUID;
}

static GUID vaProfileToEncodeProfileGuid(VAProfile profile, NV_ENC_BUFFER_FORMAT inputFmt) {
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
        return NV_ENC_H264_PROFILE_BASELINE_GUID;
    case VAProfileH264Main:
        return NV_ENC_H264_PROFILE_MAIN_GUID;
    case VAProfileH264High:
        if (inputFmt == NV_ENC_BUFFER_FORMAT_YUV444 ||
            inputFmt == NV_ENC_BUFFER_FORMAT_YUV444_10BIT) {
            return NV_ENC_H264_PROFILE_HIGH_444_GUID;
        }
        return NV_ENC_H264_PROFILE_HIGH_GUID;
    case VAProfileH264High10:
#if NVENCAPI_MAJOR_VERSION >= 13
        return NV_ENC_H264_PROFILE_HIGH_10_GUID;
#else
        return NV_ENC_CODEC_PROFILE_AUTOSELECT_GUID;
#endif
    case VAProfileHEVCMain:
        return NV_ENC_HEVC_PROFILE_MAIN_GUID;
    case VAProfileHEVCMain10:
        return NV_ENC_HEVC_PROFILE_MAIN10_GUID;
    case VAProfileHEVCMain422_10:
    case VAProfileHEVCMain12:
    case VAProfileHEVCMain444:
    case VAProfileHEVCMain444_10:
    case VAProfileHEVCMain444_12:
        return NV_ENC_HEVC_PROFILE_FREXT_GUID;
    case VAProfileAV1Profile0:
        return NV_ENC_AV1_PROFILE_MAIN_GUID;
    default:
        return NV_ENC_CODEC_PROFILE_AUTOSELECT_GUID;
    }
}

static NV_ENC_BUFFER_FORMAT encodeProfileToInputBufferFormat(VAProfile profile) {
    switch (profile) {
    case VAProfileH264High10:
        return NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
    case VAProfileHEVCMain422_10:
#if NVENCAPI_MAJOR_VERSION >= 13
        return NV_ENC_BUFFER_FORMAT_P210;
#else
        return NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
#endif
    case VAProfileHEVCMain10:
    case VAProfileHEVCMain12:
        return NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
    case VAProfileHEVCMain444:
        return NV_ENC_BUFFER_FORMAT_YUV444;
    case VAProfileHEVCMain444_10:
    case VAProfileHEVCMain444_12:
        return NV_ENC_BUFFER_FORMAT_YUV444_10BIT;
    default:
        return NV_ENC_BUFFER_FORMAT_NV12;
    }
}

static NV_ENC_BUFFER_FORMAT surfaceFormatToEncodeInputBufferFormat(
    cudaVideoSurfaceFormat surfaceFormat,
    cudaVideoChromaFormat chromaFormat,
    int bitDepth
) {
    (void)chromaFormat;
    switch (surfaceFormat) {
    case cudaVideoSurfaceFormat_NV12:
        return NV_ENC_BUFFER_FORMAT_NV12;
#if NVENCAPI_MAJOR_VERSION >= 13
    case cudaVideoSurfaceFormat_NV16:
        return NV_ENC_BUFFER_FORMAT_NV16;
#endif
    case cudaVideoSurfaceFormat_P016:
        return bitDepth > 8 ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
#if NVENCAPI_MAJOR_VERSION >= 13
    case cudaVideoSurfaceFormat_P216:
        return bitDepth > 8 ? NV_ENC_BUFFER_FORMAT_P210 : NV_ENC_BUFFER_FORMAT_NV16;
#endif
    case cudaVideoSurfaceFormat_YUV444:
        return NV_ENC_BUFFER_FORMAT_YUV444;
    case cudaVideoSurfaceFormat_YUV444_16Bit:
        return NV_ENC_BUFFER_FORMAT_YUV444_10BIT;
    default:
        return NV_ENC_BUFFER_FORMAT_NV12;
    }
}

static uint32_t encodeProfileToRtFormatMask(const NVDriver *drv, VAProfile profile) {
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
    case VAProfileH264Main:
        return VA_RT_FORMAT_YUV420;
    case VAProfileH264High:
        return VA_RT_FORMAT_YUV420 |
               (drv->supportsEncodeH264444 ? VA_RT_FORMAT_YUV444 : 0);
    case VAProfileH264High10:
        return drv->supportsEncodeH26410Bit ? VA_RT_FORMAT_YUV420_10 : 0;
    case VAProfileHEVCMain10:
        return VA_RT_FORMAT_YUV420_10;
    case VAProfileHEVCMain422_10:
        return drv->supportsEncodeHEVC422 ? VA_RT_FORMAT_YUV422_10 : 0;
    case VAProfileHEVCMain12:
        return VA_RT_FORMAT_YUV420_12;
    case VAProfileHEVCMain444:
        return VA_RT_FORMAT_YUV444;
    case VAProfileHEVCMain444_10:
        return VA_RT_FORMAT_YUV444_10;
    case VAProfileHEVCMain444_12:
        return VA_RT_FORMAT_YUV444_12;
    case VAProfileAV1Profile0:
        return VA_RT_FORMAT_YUV420 |
               (drv->supportsEncodeAV110Bit ? VA_RT_FORMAT_YUV420_10 : 0);
    default:
        return VA_RT_FORMAT_YUV420;
    }
}

static uint32_t encodeProfileToDefaultRtFormat(VAProfile profile) {
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
    case VAProfileH264Main:
    case VAProfileH264High:
    case VAProfileHEVCMain:
    case VAProfileAV1Profile0:
        return VA_RT_FORMAT_YUV420;
    case VAProfileH264High10:
        return VA_RT_FORMAT_YUV420_10;
    case VAProfileHEVCMain10:
        return VA_RT_FORMAT_YUV420_10;
    case VAProfileHEVCMain422_10:
        return VA_RT_FORMAT_YUV422_10;
    case VAProfileHEVCMain12:
        return VA_RT_FORMAT_YUV420_12;
    case VAProfileHEVCMain444:
        return VA_RT_FORMAT_YUV444;
    case VAProfileHEVCMain444_10:
        return VA_RT_FORMAT_YUV444_10;
    case VAProfileHEVCMain444_12:
        return VA_RT_FORMAT_YUV444_12;
    default:
        return VA_RT_FORMAT_YUV420;
    }
}

static uint32_t pickEncodeRtFormatFromMask(VAProfile profile, uint32_t requestedMask) {
    const uint32_t defaultRtFormat = encodeProfileToDefaultRtFormat(profile);
    if ((requestedMask & defaultRtFormat) != 0) {
        return defaultRtFormat;
    }

    static const uint32_t kFallbackOrder[] = {
        VA_RT_FORMAT_YUV444_12,
        VA_RT_FORMAT_YUV444_10,
        VA_RT_FORMAT_YUV444,
        VA_RT_FORMAT_YUV422_12,
        VA_RT_FORMAT_YUV422_10,
        VA_RT_FORMAT_YUV422,
        VA_RT_FORMAT_YUV420_12,
        VA_RT_FORMAT_YUV420_10,
        VA_RT_FORMAT_YUV420
    };
    for (uint32_t i = 0; i < ARRAY_SIZE(kFallbackOrder); i++) {
        if ((requestedMask & kFallbackOrder[i]) != 0) {
            return kFallbackOrder[i];
        }
    }

    return 0;
}

static bool configureEncodeConfigForRtFormat(
    NVConfig *cfg,
    VAProfile profile,
    uint32_t requestedRtFormat
) {
    cfg->chromaFormat = cudaVideoChromaFormat_420;
    cfg->surfaceFormat = cudaVideoSurfaceFormat_NV12;
    cfg->bitDepth = 8;

    if (isHEVCEncodeProfile(profile)) {
        if ((requestedRtFormat & VA_RT_FORMAT_YUV422_10) != 0) {
#if NVENCAPI_MAJOR_VERSION >= 13
            cfg->surfaceFormat = cudaVideoSurfaceFormat_P216;
            cfg->chromaFormat = cudaVideoChromaFormat_422;
            cfg->bitDepth = 10;
            return true;
#else
            return false;
#endif
        }
        if ((requestedRtFormat & VA_RT_FORMAT_YUV422) != 0) {
#if NVENCAPI_MAJOR_VERSION >= 13
            cfg->surfaceFormat = cudaVideoSurfaceFormat_NV16;
            cfg->chromaFormat = cudaVideoChromaFormat_422;
            cfg->bitDepth = 8;
            return true;
#else
            return false;
#endif
        }
        if ((requestedRtFormat & VA_RT_FORMAT_YUV444_12) != 0) {
            cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
            cfg->chromaFormat = cudaVideoChromaFormat_444;
            cfg->bitDepth = 12;
            return true;
        }
        if ((requestedRtFormat & VA_RT_FORMAT_YUV444_10) != 0) {
            cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
            cfg->chromaFormat = cudaVideoChromaFormat_444;
            cfg->bitDepth = 10;
            return true;
        }
        if ((requestedRtFormat & VA_RT_FORMAT_YUV444) != 0) {
            cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444;
            cfg->chromaFormat = cudaVideoChromaFormat_444;
            cfg->bitDepth = 8;
            return true;
        }
        if ((requestedRtFormat & VA_RT_FORMAT_YUV420_12) != 0) {
            cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
            cfg->chromaFormat = cudaVideoChromaFormat_420;
            cfg->bitDepth = 12;
            return true;
        }
        if ((requestedRtFormat & VA_RT_FORMAT_YUV420_10) != 0) {
            cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
            cfg->chromaFormat = cudaVideoChromaFormat_420;
            cfg->bitDepth = 10;
            return true;
        }
        cfg->surfaceFormat = cudaVideoSurfaceFormat_NV12;
        cfg->chromaFormat = cudaVideoChromaFormat_420;
        cfg->bitDepth = 8;
        return true;
    }

    if (profile == VAProfileAV1Profile0) {
        if ((requestedRtFormat & VA_RT_FORMAT_YUV420_10) != 0) {
            cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
            cfg->chromaFormat = cudaVideoChromaFormat_420;
            cfg->bitDepth = 10;
        } else {
            cfg->surfaceFormat = cudaVideoSurfaceFormat_NV12;
            cfg->chromaFormat = cudaVideoChromaFormat_420;
            cfg->bitDepth = 8;
        }
        return true;
    }

    if (profile == VAProfileH264High10) {
        if ((requestedRtFormat & VA_RT_FORMAT_YUV420_10) == 0) {
            return false;
        }
        cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
        cfg->chromaFormat = cudaVideoChromaFormat_420;
        cfg->bitDepth = 10;
        return true;
    }

    if (profile == VAProfileH264High) {
        if ((requestedRtFormat & VA_RT_FORMAT_YUV444) != 0) {
            cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444;
            cfg->chromaFormat = cudaVideoChromaFormat_444;
            cfg->bitDepth = 8;
        } else {
            cfg->surfaceFormat = cudaVideoSurfaceFormat_NV12;
            cfg->chromaFormat = cudaVideoChromaFormat_420;
            cfg->bitDepth = 8;
        }
        return true;
    }

    // H.264 baseline/main are limited to NV12 8-bit.
    cfg->surfaceFormat = cudaVideoSurfaceFormat_NV12;
    cfg->chromaFormat = cudaVideoChromaFormat_420;
    cfg->bitDepth = 8;
    return true;
}

static uint32_t encodeProfileToPackedHeaderMask(VAProfile profile) {
    if (isH264EncodeProfile(profile) || isHEVCEncodeProfile(profile)) {
        return VA_ENC_PACKED_HEADER_SEQUENCE |
               VA_ENC_PACKED_HEADER_PICTURE |
               VA_ENC_PACKED_HEADER_SLICE |
               VA_ENC_PACKED_HEADER_MISC;
    }
    return VA_ENC_PACKED_HEADER_NONE;
}

static uint32_t encodeHevcFeaturesAttribValue(void) {
    VAConfigAttribValEncHEVCFeatures features = {0};

    // Conservative feature advertising: expose only capabilities known to be
    // consumed by the current NVENC path without extra parameter handling.
    features.bits.sao = VA_FEATURE_SUPPORTED;
    features.bits.temporal_mvp = VA_FEATURE_SUPPORTED;
    features.bits.strong_intra_smoothing = VA_FEATURE_SUPPORTED;
    features.bits.dependent_slices = VA_FEATURE_SUPPORTED;
    features.bits.sign_data_hiding = VA_FEATURE_SUPPORTED;
    features.bits.constrained_intra_pred = VA_FEATURE_SUPPORTED;
    features.bits.weighted_prediction = VA_FEATURE_SUPPORTED;
    features.bits.deblocking_filter_disable = VA_FEATURE_SUPPORTED;

    return features.value;
}

static uint32_t encodeHevcBlockSizesAttribValue(void) {
    VAConfigAttribValEncHEVCBlockSizes blockSizes = {0};

    // Match ffmpeg's previous guessed defaults used with this driver:
    // CTU 32x32 and min CB 16x16.
    blockSizes.bits.log2_max_coding_tree_block_size_minus3 = 2;    // 32x32
    blockSizes.bits.log2_min_coding_tree_block_size_minus3 = 2;    // 32x32
    blockSizes.bits.log2_min_luma_coding_block_size_minus3 = 1;    // 16x16
    blockSizes.bits.log2_max_luma_transform_block_size_minus2 = 3; // 32x32
    blockSizes.bits.log2_min_luma_transform_block_size_minus2 = 0; // 4x4
    blockSizes.bits.max_max_transform_hierarchy_depth_inter = 0;
    blockSizes.bits.min_max_transform_hierarchy_depth_inter = 0;
    blockSizes.bits.max_max_transform_hierarchy_depth_intra = 0;
    blockSizes.bits.min_max_transform_hierarchy_depth_intra = 0;
    blockSizes.bits.log2_max_pcm_coding_block_size_minus3 = 2;     // 32x32
    blockSizes.bits.log2_min_pcm_coding_block_size_minus3 = 1;     // 16x16

    return blockSizes.value;
}

static bool checkNvencStatus(NVENCSTATUS status, const char *func) {
    if (status == NV_ENC_SUCCESS) {
        return true;
    }

    LOG("NVENC call failed: %s -> %d (%s)", func, status, nvencStatusToString(status));
    return false;
}

static uint32_t compactNvencVersionFromApiVersion(uint32_t apiVersion) {
    const uint32_t major = apiVersion & 0xffu;
    const uint32_t minor = (apiVersion >> 24) & 0xffu;
    return (major << 4) | (minor & 0xfu);
}

static uint32_t apiVersionFromCompactNvencVersion(uint32_t compactVersion) {
    const uint32_t major = (compactVersion >> 4) & 0xffu;
    const uint32_t minor = compactVersion & 0xfu;
    return (major & 0xffu) | ((minor & 0xffu) << 24);
}

static uint32_t nvencStructVersionForApi(uint32_t structVersion, uint32_t apiVersion) {
    const uint32_t clearMask = 0xffu | 0x0f000000u;
    return (structVersion & ~clearMask) | (apiVersion & clearMask);
}

static uint32_t negotiateNvencApiVersion(NVDriver *drv, const char *reason) {
    const uint32_t headerApiVersion = NVENCAPI_VERSION;
    const uint32_t headerCompactVersion = compactNvencVersionFromApiVersion(headerApiVersion);
    if (drv == NULL || drv->nv == NULL || drv->nv->NvEncodeAPIGetMaxSupportedVersion == NULL) {
        LOG(
            "NVENC API negotiation (%s): max-version query unavailable, using header version=%u.%u",
            reason,
            NVENCAPI_MAJOR_VERSION,
            NVENCAPI_MINOR_VERSION
        );
        return headerApiVersion;
    }

    uint32_t maxSupportedCompactVersion = 0;
    if (!checkNvencStatus(
            drv->nv->NvEncodeAPIGetMaxSupportedVersion(&maxSupportedCompactVersion),
            "NvEncodeAPIGetMaxSupportedVersion")) {
        LOG(
            "NVENC API negotiation (%s): max-version query failed, using header version=%u.%u",
            reason,
            NVENCAPI_MAJOR_VERSION,
            NVENCAPI_MINOR_VERSION
        );
        return headerApiVersion;
    }

    const uint32_t selectedCompactVersion =
        headerCompactVersion <= maxSupportedCompactVersion
            ? headerCompactVersion
            : maxSupportedCompactVersion;
    const uint32_t selectedApiVersion = apiVersionFromCompactNvencVersion(selectedCompactVersion);

    LOG(
        "NVENC API negotiation (%s): header=%u.%u max=%u.%u selected=%u.%u",
        reason,
        (unsigned int) ((headerCompactVersion >> 4) & 0xffu),
        (unsigned int) (headerCompactVersion & 0xfu),
        (unsigned int) ((maxSupportedCompactVersion >> 4) & 0xffu),
        (unsigned int) (maxSupportedCompactVersion & 0xfu),
        (unsigned int) ((selectedCompactVersion >> 4) & 0xffu),
        (unsigned int) (selectedCompactVersion & 0xfu)
    );

    return selectedApiVersion;
}

static NV_ENC_PARAMS_RC_MODE vaRateControlToNvencRateControl(uint32_t rateControl) {
    if (rateControl & VA_RC_CBR) {
        return NV_ENC_PARAMS_RC_CBR;
    }
    if (rateControl & VA_RC_VBR) {
        return NV_ENC_PARAMS_RC_VBR;
    }
    if (rateControl & VA_RC_CQP) {
        return NV_ENC_PARAMS_RC_CONSTQP;
    }

    return NV_ENC_PARAMS_RC_CBR;
}

static uint32_t sanitizeQp(uint32_t qp) {
    if (qp == 0) {
        return 0;
    }
    if (qp > 51) {
        return 51;
    }
    return qp;
}

static uint32_t clampQpToBounds(uint32_t qp, uint32_t minQp, uint32_t maxQp) {
    if (minQp != 0 && qp < minQp) {
        qp = minQp;
    }
    if (maxQp != 0 && qp > maxQp) {
        qp = maxQp;
    }
    return qp;
}

static void applyQpBoundsToRcParams(
    NV_ENC_RC_PARAMS *rcParams,
    uint32_t minQp,
    uint32_t maxQp,
    uint32_t initialQp,
    bool setInitialQp
) {
    if (minQp != 0) {
        rcParams->enableMinQP = 1;
        rcParams->minQP.qpInterP = minQp;
        rcParams->minQP.qpInterB = minQp;
        rcParams->minQP.qpIntra = minQp;
    }
    if (maxQp != 0) {
        rcParams->enableMaxQP = 1;
        rcParams->maxQP.qpInterP = maxQp;
        rcParams->maxQP.qpInterB = maxQp;
        rcParams->maxQP.qpIntra = maxQp;
    }
    if (setInitialQp) {
        rcParams->enableInitialRCQP = 1;
        rcParams->initialRCQP.qpInterP = initialQp;
        rcParams->initialRCQP.qpInterB = initialQp;
        rcParams->initialRCQP.qpIntra = initialQp;
    }
}

static void maybeSetEncodeInitialQpFromHint(NVContext *nvCtx, int32_t qpHint, const char *source) {
    if (nvCtx->encodeSessionInitialized) {
        return;
    }
    if (nvCtx->encodeInitialQpFromMisc) {
        return;
    }
    if (qpHint <= 0) {
        return;
    }

    const uint32_t qp = sanitizeQp((uint32_t) qpHint);
    if (qp == 0) {
        return;
    }
    if (nvCtx->encodeInitialQp == qp) {
        return;
    }

    nvCtx->encodeInitialQp = qp;
    LOG("updated encode initial QP from %s: hint=%d applied=%u", source, qpHint, qp);
}

static void configureConstQpForEncode(NVContext *nvCtx, NV_ENC_RC_PARAMS *rcParams) {
    const uint32_t defaultQp = 26;
    uint32_t qp = sanitizeQp(nvCtx->encodeInitialQp);
    uint32_t minQp = sanitizeQp(nvCtx->encodeMinQp);
    uint32_t maxQp = sanitizeQp(nvCtx->encodeMaxQp);

    if (qp == 0) {
        qp = defaultQp;
    }
    if (minQp != 0 && maxQp != 0 && minQp > maxQp) {
        maxQp = minQp;
    }
    qp = clampQpToBounds(qp, minQp, maxQp);

    rcParams->constQP.qpInterP = qp;
    rcParams->constQP.qpInterB = qp;
    rcParams->constQP.qpIntra = qp;

    applyQpBoundsToRcParams(rcParams, minQp, maxQp, qp, nvCtx->encodeInitialQp != 0);

    LOG(
        "configured CONSTQP: qp=%u initial_qp=%u min_qp=%u max_qp=%u",
        qp,
        nvCtx->encodeInitialQp,
        nvCtx->encodeMinQp,
        nvCtx->encodeMaxQp
    );
}

static void configureQpBoundsForRateControl(NVContext *nvCtx, NV_ENC_RC_PARAMS *rcParams) {
    uint32_t minQp = sanitizeQp(nvCtx->encodeMinQp);
    uint32_t maxQp = sanitizeQp(nvCtx->encodeMaxQp);
    uint32_t initialQp = sanitizeQp(nvCtx->encodeInitialQp);

    if (minQp != 0 && maxQp != 0 && minQp > maxQp) {
        maxQp = minQp;
    }
    if (initialQp != 0) {
        initialQp = clampQpToBounds(initialQp, minQp, maxQp);
    }

    applyQpBoundsToRcParams(rcParams, minQp, maxQp, initialQp, initialQp != 0);
    LOG(
        "configured RC QP bounds: initial_qp=%u min_qp=%u max_qp=%u",
        initialQp,
        minQp,
        maxQp
    );
}

static void setH264ConfigBitDepth(NV_ENC_CONFIG_H264 *h264Cfg, uint32_t bitDepthMinus8) {
#if NVENCAPI_MAJOR_VERSION >= 13
    const NV_ENC_BIT_DEPTH bitDepth =
        bitDepthMinus8 >= 2 ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
    h264Cfg->inputBitDepth = bitDepth;
    h264Cfg->outputBitDepth = bitDepth;
#else
    (void) h264Cfg;
    (void) bitDepthMinus8;
#endif
}

static void setHevcConfigBitDepth(NV_ENC_CONFIG_HEVC *hevcCfg, uint32_t bitDepthMinus8) {
#if NVENCAPI_MAJOR_VERSION >= 13
    const NV_ENC_BIT_DEPTH bitDepth =
        bitDepthMinus8 >= 2 ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
    hevcCfg->inputBitDepth = bitDepth;
    hevcCfg->outputBitDepth = bitDepth;
#else
    hevcCfg->pixelBitDepthMinus8 = bitDepthMinus8;
#endif
}

static void setAv1ConfigBitDepth(NV_ENC_CONFIG_AV1 *av1Cfg, uint32_t bitDepthMinus8) {
#if NVENCAPI_MAJOR_VERSION >= 13
    const NV_ENC_BIT_DEPTH bitDepth =
        bitDepthMinus8 >= 2 ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
    av1Cfg->inputBitDepth = bitDepth;
    av1Cfg->outputBitDepth = bitDepth;
#else
    av1Cfg->inputPixelBitDepthMinus8 = bitDepthMinus8;
    av1Cfg->pixelBitDepthMinus8 = bitDepthMinus8;
#endif
}

static void clearCodedBuffer(NVBuffer *codedBuffer) {
    codedBuffer->codedSegment.size = 0;
    codedBuffer->codedSegment.bit_offset = 0;
    codedBuffer->codedSegment.status = 0;
    codedBuffer->codedSegment.reserved = 0;
    codedBuffer->codedSegment.buf = codedBuffer->codedData;
    codedBuffer->codedSegment.next = NULL;
}

static bool ensureCodedDataBuffer(NVBuffer *codedBuffer, size_t required) {
    if (required <= codedBuffer->codedDataAllocated) {
        return true;
    }

    size_t allocateSize = required;
    if (allocateSize < 4096) {
        allocateSize = 4096;
    }

    LOG("resizing coded buffer from %zu to %zu bytes", codedBuffer->codedDataAllocated, allocateSize);

    void *newBuffer = memalign(16, allocateSize);
    if (newBuffer == NULL) {
        LOG("Unable to allocate coded buffer of %zu bytes", allocateSize);
        LOG("failed to allocate coded buffer of %zu bytes", allocateSize);
        return false;
    }

    if (codedBuffer->codedData != NULL && codedBuffer->codedSegment.size > 0) {
        memcpy(newBuffer, codedBuffer->codedData, codedBuffer->codedSegment.size);
    }
    free(codedBuffer->codedData);

    codedBuffer->codedData = newBuffer;
    codedBuffer->codedDataAllocated = allocateSize;
    codedBuffer->codedSegment.buf = codedBuffer->codedData;

    return true;
}

static bool clearP010EncodeInputPadding(
    NVContext *nvCtx,
    NVSurface *surface,
    uint32_t copyWidthBytes,
    uint32_t chromaCopyHeight,
    size_t lumaPlaneBytes,
    bool is422
) {
    NVDriver *drv = nvCtx->drv;
    CudaFunctions *cu = drv->cu;
    const uint16_t lumaValue = (uint16_t)(940u << 6);
    const uint16_t chromaValue = (uint16_t)(512u << 6);
    const uint32_t lumaPlaneHeight = nvCtx->height;
    const uint32_t chromaPlaneHeight = is422 ? nvCtx->height : (nvCtx->height >> 1);
    const size_t pitch = nvCtx->encodeInputPitch;
    const size_t tailBytes = pitch > copyWidthBytes ? (pitch - copyWidthBytes) : 0u;

    if ((pitch & 1u) != 0u) {
        LOG("unexpected odd encode input pitch for P010 clear: %zu", pitch);
        return false;
    }

    uint16_t *lumaRow = malloc(pitch);
    uint16_t *chromaRow = malloc(pitch);
    if (lumaRow == NULL || chromaRow == NULL) {
        free(lumaRow);
        free(chromaRow);
        LOG("failed to allocate temporary host rows for P010 padding clear");
        return false;
    }

    for (size_t i = 0; i < pitch / sizeof(uint16_t); i++) {
        lumaRow[i] = lumaValue;
        chromaRow[i] = chromaValue;
    }

    CUdeviceptr lumaBase = nvCtx->encodeInputBuffer;
    CUdeviceptr chromaBase = nvCtx->encodeInputBuffer + lumaPlaneBytes;

    for (uint32_t row = 0; row < surface->height && tailBytes > 0; row++) {
        CUdeviceptr dst = lumaBase + (CUdeviceptr)(row * pitch + copyWidthBytes);
        if (cu->cuMemcpyHtoDAsync != NULL) {
            CHECK_CUDA_RESULT_RETURN(
                cu->cuMemcpyHtoDAsync(dst, (const uint8_t *)lumaRow + copyWidthBytes, tailBytes, 0),
                false);
        } else {
            CHECK_CUDA_RESULT_RETURN(
                cu->cuMemcpyHtoD(dst, (const uint8_t *)lumaRow + copyWidthBytes, tailBytes),
                false);
        }
    }

    for (uint32_t row = surface->height; row < lumaPlaneHeight; row++) {
        CUdeviceptr dst = lumaBase + (CUdeviceptr)(row * pitch);
        if (cu->cuMemcpyHtoDAsync != NULL) {
            CHECK_CUDA_RESULT_RETURN(cu->cuMemcpyHtoDAsync(dst, lumaRow, pitch, 0), false);
        } else {
            CHECK_CUDA_RESULT_RETURN(cu->cuMemcpyHtoD(dst, lumaRow, pitch), false);
        }
    }

    for (uint32_t row = 0; row < chromaCopyHeight && tailBytes > 0; row++) {
        CUdeviceptr dst = chromaBase + (CUdeviceptr)(row * pitch + copyWidthBytes);
        if (cu->cuMemcpyHtoDAsync != NULL) {
            CHECK_CUDA_RESULT_RETURN(
                cu->cuMemcpyHtoDAsync(dst, (const uint8_t *)chromaRow + copyWidthBytes, tailBytes, 0),
                false);
        } else {
            CHECK_CUDA_RESULT_RETURN(
                cu->cuMemcpyHtoD(dst, (const uint8_t *)chromaRow + copyWidthBytes, tailBytes),
                false);
        }
    }

    for (uint32_t row = chromaCopyHeight; row < chromaPlaneHeight; row++) {
        CUdeviceptr dst = chromaBase + (CUdeviceptr)(row * pitch);
        if (cu->cuMemcpyHtoDAsync != NULL) {
            CHECK_CUDA_RESULT_RETURN(cu->cuMemcpyHtoDAsync(dst, chromaRow, pitch, 0), false);
        } else {
            CHECK_CUDA_RESULT_RETURN(cu->cuMemcpyHtoD(dst, chromaRow, pitch), false);
        }
    }

    free(lumaRow);
    free(chromaRow);
    return true;
}

static bool copySurfaceToEncodeInputBuffer(NVContext *nvCtx, NVSurface *surface) {
    const unsigned long long startUs = nv_getmonotonic_us();
    if (surface->backingImage == NULL) {
        LOG("surface %p has no backing image", (void *) surface);
        return false;
    }

    const NV_ENC_BUFFER_FORMAT inputFmt =
        nvCtx->encodeInputFormat != NV_ENC_BUFFER_FORMAT_UNDEFINED
            ? nvCtx->encodeInputFormat
            : encodeProfileToInputBufferFormat(nvCtx->profile);
    nvCtx->encodeInputFormat = inputFmt;

    bool is444 = false;
    bool is422 = false;
    uint32_t bytesPerSample = 1;
    uint32_t planeCount = 2;
    cudaVideoSurfaceFormat requiredSurfaceFormat = cudaVideoSurfaceFormat_NV12;

    switch (inputFmt) {
    case NV_ENC_BUFFER_FORMAT_NV12:
        requiredSurfaceFormat = cudaVideoSurfaceFormat_NV12;
        break;
    case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
        requiredSurfaceFormat = cudaVideoSurfaceFormat_P016;
        bytesPerSample = 2;
        break;
#if NVENCAPI_MAJOR_VERSION >= 13
    case NV_ENC_BUFFER_FORMAT_NV16:
        requiredSurfaceFormat = cudaVideoSurfaceFormat_NV16;
        is422 = true;
        break;
    case NV_ENC_BUFFER_FORMAT_P210:
        requiredSurfaceFormat = cudaVideoSurfaceFormat_P216;
        is422 = true;
        bytesPerSample = 2;
        break;
#endif
    case NV_ENC_BUFFER_FORMAT_YUV444:
        requiredSurfaceFormat = cudaVideoSurfaceFormat_YUV444;
        is444 = true;
        planeCount = 3;
        break;
    case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:
        requiredSurfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
        is444 = true;
        bytesPerSample = 2;
        planeCount = 3;
        break;
    default:
        LOG("unsupported encode input buffer format=0x%x", inputFmt);
        return false;
    }

    const uint32_t srcOffsetX = 0;
    const uint32_t srcOffsetY = 0;
    const uint32_t copyWidthPixels = surface->width;
    const uint32_t copyHeightPixels = surface->height;

    if (surface->format != requiredSurfaceFormat) {
        LOG(
            "unsupported input surface format=%d for inputFmt=0x%x (required=%d)",
            surface->format,
            inputFmt,
            requiredSurfaceFormat
        );
        return false;
    }
    if (copyWidthPixels == 0 || copyHeightPixels == 0 ||
        srcOffsetX + copyWidthPixels > surface->width ||
        srcOffsetY + copyHeightPixels > surface->height) {
        LOG(
            "invalid visible copy window: surface=%ux%u crop=(%u,%u %ux%u)",
            surface->width,
            surface->height,
            srcOffsetX,
            srcOffsetY,
            copyWidthPixels,
            copyHeightPixels
        );
        return false;
    }
    if (copyWidthPixels > nvCtx->width || copyHeightPixels > nvCtx->height) {
        LOG(
            "input copy area (%ux%u) is larger than encoder session size (%ux%u)",
            copyWidthPixels,
            copyHeightPixels,
            nvCtx->width,
            nvCtx->height
        );
        return false;
    }
    if (!is444) {
        if ((srcOffsetX & 1u) != 0 || (copyWidthPixels & 1u) != 0) {
            LOG("4:2:x encode requires even X/width, got offset=%u width=%u", srcOffsetX, copyWidthPixels);
            return false;
        }
        if (!is422 && ((srcOffsetY & 1u) != 0 || (copyHeightPixels & 1u) != 0)) {
            LOG("4:2:0 encode requires even Y/height, got offset=%u height=%u", srcOffsetY, copyHeightPixels);
            return false;
        }
    }

    const uint32_t copyWidthBytes = copyWidthPixels * bytesPerSample;
    if (nvCtx->encodeInputPitch < copyWidthBytes) {
        LOG(
            "encode input pitch %zu is smaller than copy width bytes %u",
            nvCtx->encodeInputPitch,
            copyWidthBytes
        );
        return false;
    }

    for (uint32_t i = 0; i < planeCount; i++) {
        if (surface->backingImage->arrays[i] == NULL) {
            LOG("surface backing plane %u is NULL", i);
            return false;
        }
        if (surface->backingImage->strides[i] > 0 &&
            (uint32_t)surface->backingImage->strides[i] < copyWidthBytes) {
            LOG(
                "plane %u stride %d is smaller than copy width bytes %u",
                i,
                surface->backingImage->strides[i],
                copyWidthBytes
            );
            return false;
        }
    }

    LOG(
        "copying surface=%p backing=%p surface=%ux%u session=%ux%u fmt=0x%x into inputBuffer=%p pitch=%zu",
        (void *) surface,
        (void *) surface->backingImage,
        surface->width,
        surface->height,
        nvCtx->width,
        nvCtx->height,
        inputFmt,
        (void *) nvCtx->encodeInputBuffer,
        nvCtx->encodeInputPitch
    );

    const size_t planeBytes = nvCtx->encodeInputPitch * nvCtx->height;
    const size_t lumaPlaneBytes = planeBytes;
    const size_t chromaPlaneBytes = nvCtx->encodeInputPitch * (is422 ? nvCtx->height : (nvCtx->height >> 1));
    const bool fullFrameCopy = copyWidthPixels == nvCtx->width &&
                               copyHeightPixels == nvCtx->height;
    const bool hasStridePadding = nvCtx->encodeInputPitch > copyWidthBytes;
    const bool clearPaddingNv12 = inputFmt == NV_ENC_BUFFER_FORMAT_NV12 &&
                                  (!fullFrameCopy || hasStridePadding);
    const bool clearPaddingP010 = inputFmt == NV_ENC_BUFFER_FORMAT_YUV420_10BIT &&
                                  (!fullFrameCopy || hasStridePadding);
    const bool clearPadding444 = inputFmt == NV_ENC_BUFFER_FORMAT_YUV444 &&
                                 (!fullFrameCopy || hasStridePadding);
    const bool clearPadding = clearPaddingNv12 || clearPaddingP010 || clearPadding444;

    const uint32_t chromaCopyHeight = is444 ? copyHeightPixels
                                            : (is422 ? copyHeightPixels : (copyHeightPixels >> 1));
    const uint32_t chromaSrcY = is444 ? srcOffsetY
                                      : (is422 ? srcOffsetY : (srcOffsetY >> 1));
    const size_t srcXInBytes = (size_t) srcOffsetX * bytesPerSample;
    if (!is444 && chromaCopyHeight == 0) {
        LOG("invalid 4:2:x chroma height=0 for copy height=%u", copyHeightPixels);
        return false;
    }

    if (clearPaddingNv12 && cu->cuMemsetD8Async != NULL) {
        CHECK_CUDA_RESULT_RETURN(cu->cuMemsetD8Async(nvCtx->encodeInputBuffer, 0, lumaPlaneBytes, 0), false);
        CHECK_CUDA_RESULT_RETURN(cu->cuMemsetD8Async(nvCtx->encodeInputBuffer + lumaPlaneBytes, 0x80, chromaPlaneBytes, 0), false);
    } else if (clearPaddingP010) {
        if (!clearP010EncodeInputPadding(
                nvCtx,
                surface,
                copyWidthBytes,
                chromaCopyHeight,
                lumaPlaneBytes,
                is422)) {
            return false;
        }
    } else if (clearPadding444 && cu->cuMemsetD8Async != NULL) {
        CHECK_CUDA_RESULT_RETURN(cu->cuMemsetD8Async(nvCtx->encodeInputBuffer, 0, lumaPlaneBytes, 0), false);
        CHECK_CUDA_RESULT_RETURN(cu->cuMemsetD8Async(nvCtx->encodeInputBuffer + planeBytes, 0x80, planeBytes, 0), false);
        CHECK_CUDA_RESULT_RETURN(cu->cuMemsetD8Async(nvCtx->encodeInputBuffer + (planeBytes * 2), 0x80, planeBytes, 0), false);
    }

    CUDA_MEMCPY2D plane0Copy = {
        .srcMemoryType = CU_MEMORYTYPE_ARRAY,
        .srcArray = surface->backingImage->arrays[0],
        .srcXInBytes = srcXInBytes,
        .srcY = srcOffsetY,
        .dstMemoryType = CU_MEMORYTYPE_DEVICE,
        .dstDevice = nvCtx->encodeInputBuffer,
        .dstPitch = nvCtx->encodeInputPitch,
        .WidthInBytes = copyWidthBytes,
        .Height = copyHeightPixels,
    };

    if (cu->cuMemcpy2DAsync != NULL) {
        CHECK_CUDA_RESULT_RETURN(cu->cuMemcpy2DAsync(&plane0Copy, 0), false);
    } else {
        CHECK_CUDA_RESULT_RETURN(cu->cuMemcpy2D(&plane0Copy), false);
    }

    if (is444) {
        CUDA_MEMCPY2D plane1Copy = {
            .srcMemoryType = CU_MEMORYTYPE_ARRAY,
            .srcArray = surface->backingImage->arrays[1],
            .srcXInBytes = srcXInBytes,
            .srcY = srcOffsetY,
            .dstMemoryType = CU_MEMORYTYPE_DEVICE,
            .dstDevice = nvCtx->encodeInputBuffer + planeBytes,
            .dstPitch = nvCtx->encodeInputPitch,
            .WidthInBytes = copyWidthBytes,
            .Height = copyHeightPixels,
        };
        CUDA_MEMCPY2D plane2Copy = {
            .srcMemoryType = CU_MEMORYTYPE_ARRAY,
            .srcArray = surface->backingImage->arrays[2],
            .srcXInBytes = srcXInBytes,
            .srcY = srcOffsetY,
            .dstMemoryType = CU_MEMORYTYPE_DEVICE,
            .dstDevice = nvCtx->encodeInputBuffer + (planeBytes * 2),
            .dstPitch = nvCtx->encodeInputPitch,
            .WidthInBytes = copyWidthBytes,
            .Height = copyHeightPixels,
        };

        if (cu->cuMemcpy2DAsync != NULL) {
            CHECK_CUDA_RESULT_RETURN(cu->cuMemcpy2DAsync(&plane1Copy, 0), false);
        } else {
            CHECK_CUDA_RESULT_RETURN(cu->cuMemcpy2D(&plane1Copy), false);
        }
        CHECK_CUDA_RESULT_RETURN(cu->cuMemcpy2D(&plane2Copy), false);
    } else {
        CUDA_MEMCPY2D chromaCopy = {
            .srcMemoryType = CU_MEMORYTYPE_ARRAY,
            .srcArray = surface->backingImage->arrays[1],
            .srcXInBytes = srcXInBytes,
            .srcY = chromaSrcY,
            .dstMemoryType = CU_MEMORYTYPE_DEVICE,
            .dstDevice = nvCtx->encodeInputBuffer + lumaPlaneBytes,
            .dstPitch = nvCtx->encodeInputPitch,
            .WidthInBytes = copyWidthBytes,
            .Height = chromaCopyHeight,
        };
        CHECK_CUDA_RESULT_RETURN(cu->cuMemcpy2D(&chromaCopy), false);
    }

    LOG(
        "surface copy completed elapsed_us=%llu clear_padding=%s",
        nv_getmonotonic_us() - startUs,
        clearPadding ? "yes" : "no"
    );
    return true;
}

static bool destroyEncodeSession(NVContext *nvCtx) {
    NVDriver *drv = nvCtx->drv;
    bool hadError = false;

    LOG(
        "destroying encode session context_id=%d initialized=%d encoder=%p registered=%p bitstream=%p inputBuffer=%p",
        (int) nvCtx->contextId,
        nvCtx->encodeSessionInitialized,
        nvCtx->encoder,
        nvCtx->encodeRegisteredInput,
        nvCtx->encodeBitstream,
        (void *) nvCtx->encodeInputBuffer
    );
    freeBuffer(&nvCtx->encodePackedHeaders);
    nvCtx->encodePackedHeaderType = 0;
    nvCtx->encodePackedHeaderBitLength = 0;
    nvCtx->encodePackedHeaderHasEmulationBytes = false;

    if (nvCtx->encoder != NULL) {
        bool destroyPushed = false;
        LOG(
            "destroyEncodeSession context_id=%d begin pre-destroy cuCtxPushCurrent encoder=%p",
            (int) nvCtx->contextId,
            nvCtx->encoder
        );
        if (CHECK_CUDA_RESULT(cu->cuCtxPushCurrent(drv->cudaContext))) {
            hadError = true;
            LOG("destroyEncodeSession context_id=%d failed pre-destroy cuCtxPushCurrent", (int) nvCtx->contextId);
        } else {
            destroyPushed = true;
            LOG("destroyEncodeSession context_id=%d done pre-destroy cuCtxPushCurrent", (int) nvCtx->contextId);
            pthread_once(&cuda_ctx_sync_once, initCudaCtxSynchronizeSymbol);
            if (optionalCuCtxSynchronize != NULL) {
                LOG("destroyEncodeSession context_id=%d begin pre-destroy cuCtxSynchronize", (int) nvCtx->contextId);
                if (CHECK_CUDA_RESULT(optionalCuCtxSynchronize())) {
                    hadError = true;
                    LOG("destroyEncodeSession context_id=%d failed pre-destroy cuCtxSynchronize", (int) nvCtx->contextId);
                }
                LOG("destroyEncodeSession context_id=%d done pre-destroy cuCtxSynchronize", (int) nvCtx->contextId);
            }
        }
        if (nvCtx->encodeRegisteredInput != NULL && nvCtx->encodeApi.nvEncUnregisterResource != NULL) {
            LOG(
                "destroyEncodeSession context_id=%d begin nvEncUnregisterResource registered=%p encoder=%p",
                (int) nvCtx->contextId,
                nvCtx->encodeRegisteredInput,
                nvCtx->encoder
            );
            noteNvencRegisteredDestroy(nvCtx->encodeRegisteredInput);
            checkNvencStatus(nvCtx->encodeApi.nvEncUnregisterResource(nvCtx->encoder, nvCtx->encodeRegisteredInput), "nvEncUnregisterResource");
            LOG("destroyEncodeSession context_id=%d done nvEncUnregisterResource", (int) nvCtx->contextId);
            nvCtx->encodeRegisteredInput = NULL;
        }
        if (nvCtx->encodeBitstream != NULL && nvCtx->encodeApi.nvEncDestroyBitstreamBuffer != NULL) {
            LOG(
                "destroyEncodeSession context_id=%d begin nvEncDestroyBitstreamBuffer bitstream=%p encoder=%p",
                (int) nvCtx->contextId,
                nvCtx->encodeBitstream,
                nvCtx->encoder
            );
            noteNvencBitstreamDestroy(nvCtx->encodeBitstream);
            checkNvencStatus(nvCtx->encodeApi.nvEncDestroyBitstreamBuffer(nvCtx->encoder, nvCtx->encodeBitstream), "nvEncDestroyBitstreamBuffer");
            LOG("destroyEncodeSession context_id=%d done nvEncDestroyBitstreamBuffer", (int) nvCtx->contextId);
            nvCtx->encodeBitstream = NULL;
        }
        if (nvCtx->encodeApi.nvEncDestroyEncoder != NULL) {
            LOG(
                "destroyEncodeSession context_id=%d begin nvEncDestroyEncoder encoder=%p",
                (int) nvCtx->contextId,
                nvCtx->encoder
            );
            noteNvencSessionDestroy(nvCtx->encoder);
            checkNvencStatus(nvCtx->encodeApi.nvEncDestroyEncoder(nvCtx->encoder), "nvEncDestroyEncoder");
            LOG("destroyEncodeSession context_id=%d done nvEncDestroyEncoder", (int) nvCtx->contextId);
        }
        if (destroyPushed) {
            LOG("destroyEncodeSession context_id=%d begin pre-destroy cuCtxPopCurrent", (int) nvCtx->contextId);
            if (CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL))) {
                hadError = true;
                LOG("destroyEncodeSession context_id=%d failed pre-destroy cuCtxPopCurrent", (int) nvCtx->contextId);
            }
            LOG("destroyEncodeSession context_id=%d done pre-destroy cuCtxPopCurrent", (int) nvCtx->contextId);
        }
        nvCtx->encoder = NULL;
    }

    if (nvCtx->encodeInputBuffer != 0) {
        bool pushed = false;
        LOG(
            "destroyEncodeSession context_id=%d begin cuCtxPushCurrent inputBuffer=%p",
            (int) nvCtx->contextId,
            (void *) nvCtx->encodeInputBuffer
        );
        if (CHECK_CUDA_RESULT(cu->cuCtxPushCurrent(drv->cudaContext))) {
            hadError = true;
            LOG("destroyEncodeSession context_id=%d failed to push CUDA context before input free", (int) nvCtx->contextId);
        } else {
            pushed = true;
            LOG("destroyEncodeSession context_id=%d done cuCtxPushCurrent", (int) nvCtx->contextId);
            pthread_once(&cuda_ctx_sync_once, initCudaCtxSynchronizeSymbol);
            if (optionalCuCtxSynchronize != NULL) {
                LOG("destroyEncodeSession context_id=%d begin cuCtxSynchronize", (int) nvCtx->contextId);
                if (CHECK_CUDA_RESULT(optionalCuCtxSynchronize())) {
                    hadError = true;
                    LOG("destroyEncodeSession context_id=%d cuCtxSynchronize failed before input free", (int) nvCtx->contextId);
                }
                LOG("destroyEncodeSession context_id=%d done cuCtxSynchronize", (int) nvCtx->contextId);
            }
            LOG(
                "destroyEncodeSession context_id=%d begin cuMemFree inputBuffer=%p bytes=%zu",
                (int) nvCtx->contextId,
                (void *) nvCtx->encodeInputBuffer,
                nvCtx->encodeInputBytes
            );
            if (CHECK_CUDA_RESULT(cu->cuMemFree(nvCtx->encodeInputBuffer))) {
                hadError = true;
                LOG(
                    "destroyEncodeSession context_id=%d cuMemFree failed for inputBuffer=%p bytes=%zu pitch=%zu",
                    (int) nvCtx->contextId,
                    (void *) nvCtx->encodeInputBuffer,
                    nvCtx->encodeInputBytes,
                    nvCtx->encodeInputPitch
                );
            } else if (nvCtx->encodeInputBytes > 0) {
                noteEncodeInputFree(nvCtx->encodeInputBytes);
            }
            LOG("destroyEncodeSession context_id=%d done cuMemFree", (int) nvCtx->contextId);
        }

        if (pushed) {
            LOG("destroyEncodeSession context_id=%d begin cuCtxPopCurrent", (int) nvCtx->contextId);
            if (CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL))) {
                hadError = true;
                LOG("destroyEncodeSession context_id=%d failed to pop CUDA context after input free", (int) nvCtx->contextId);
            }
            LOG("destroyEncodeSession context_id=%d done cuCtxPopCurrent", (int) nvCtx->contextId);
        }

        nvCtx->encodeInputBuffer = 0;
        nvCtx->encodeInputBytes = 0;
        nvCtx->encodeInputPitch = 0;
    }

    memset(&nvCtx->encodeApi, 0, sizeof(nvCtx->encodeApi));
    nvCtx->encodeNvencApiVersion = 0;
    nvCtx->encodeSessionInitialized = false;

    LOG("encode session destroyed context_id=%d", (int) nvCtx->contextId);
    const uint64_t liveEncodeSessions =
        counterLoad(&nvencSessionOpenCount) -
        counterLoad(&nvencSessionDestroyCount);
    if (liveEncodeSessions == 0) {
        size_t otherLiveCodecContexts = 0;
        uint32_t activeInstances = 0;
        bool canRunDriverGlobalLastSessionCleanup = false;

        pthread_mutex_lock(&drv->objectCreationMutex);
        otherLiveCodecContexts = countOtherLiveCodecContextsLocked(drv, nvCtx->contextId);
        pthread_mutex_lock(&concurrency_mutex);
        activeInstances = instances;
        pthread_mutex_unlock(&concurrency_mutex);
        canRunDriverGlobalLastSessionCleanup =
            otherLiveCodecContexts == 0 && activeInstances == 1;

        if (!canRunDriverGlobalLastSessionCleanup) {
            pthread_mutex_unlock(&drv->objectCreationMutex);
            LOG(
                "Deferring driver-global last-session cleanup context_id=%d live_encode_sessions=%llu other_codec_contexts=%zu driver_instances=%u",
                (int) nvCtx->contextId,
                (unsigned long long) liveEncodeSessions,
                otherLiveCodecContexts,
                activeInstances
            );
        } else {
            directAdvanceResourceEpoch();
            bool releasePushed = false;
            LOG(
                "destroyEncodeSession context_id=%d begin releaseOldImportGpuCopyBackingCudaViewsAfterLastSession",
                (int) nvCtx->contextId
            );
            if (CHECK_CUDA_RESULT(cu->cuCtxPushCurrent(drv->cudaContext))) {
                hadError = true;
                LOG(
                    "destroyEncodeSession context_id=%d failed cuCtxPushCurrent before releaseOldImportGpuCopyBackingCudaViewsAfterLastSession",
                    (int) nvCtx->contextId
                );
            } else {
                releasePushed = true;
                directReleaseOldImportGpuCopyBackingCudaViews(drv);
            }
            if (releasePushed) {
                if (CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL))) {
                    hadError = true;
                    LOG(
                        "destroyEncodeSession context_id=%d failed cuCtxPopCurrent after releaseOldImportGpuCopyBackingCudaViewsAfterLastSession",
                        (int) nvCtx->contextId
                    );
                }
            }
            LOG(
                "destroyEncodeSession context_id=%d done releaseOldImportGpuCopyBackingCudaViewsAfterLastSession",
                (int) nvCtx->contextId
            );
            if (isTruthyEnv(getenv("NVD_EXPERIMENTAL_PURGE_OLD_IMPORT_GPU_COPY_BACKINGS_ON_LAST_SESSION_DESTROY"))) {
                bool purgePushed = false;
                LOG(
                    "destroyEncodeSession context_id=%d begin purgeOldImportGpuCopyBackingsAfterLastSession",
                    (int) nvCtx->contextId
                );
                if (CHECK_CUDA_RESULT(cu->cuCtxPushCurrent(drv->cudaContext))) {
                    hadError = true;
                    LOG(
                        "destroyEncodeSession context_id=%d failed cuCtxPushCurrent before purgeOldImportGpuCopyBackingsAfterLastSession",
                        (int) nvCtx->contextId
                    );
                } else {
                    purgePushed = true;
                    directPurgeOldImportGpuCopyBackings(drv);
                }
                if (purgePushed) {
                    if (CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL))) {
                        hadError = true;
                        LOG(
                            "destroyEncodeSession context_id=%d failed cuCtxPopCurrent after purgeOldImportGpuCopyBackingsAfterLastSession",
                            (int) nvCtx->contextId
                        );
                    }
                }
                LOG(
                    "destroyEncodeSession context_id=%d done purgeOldImportGpuCopyBackingsAfterLastSession",
                    (int) nvCtx->contextId
                );
            }
            LOG("destroyEncodeSession context_id=%d begin recycleCudaContextAfterLastSession", (int) nvCtx->contextId);
            recycleCudaContextAfterLastSession(drv, "all_sessions_destroyed");
            LOG("destroyEncodeSession context_id=%d done recycleCudaContextAfterLastSession", (int) nvCtx->contextId);
            LOG(
                "destroyEncodeSession context_id=%d begin reloadGlobalCodecFunctionsAfterLastSession",
                (int) nvCtx->contextId
            );
            reloadGlobalCodecFunctionsAfterLastSession(drv, "all_sessions_destroyed");
            LOG(
                "destroyEncodeSession context_id=%d done reloadGlobalCodecFunctionsAfterLastSession",
                (int) nvCtx->contextId
            );
            pthread_mutex_unlock(&drv->objectCreationMutex);
        }
    }
    return !hadError;
}

static bool initEncodeSession(NVContext *nvCtx) {
    if (nvCtx->encodeSessionInitialized) {
        LOG("encode session already initialized");
        return true;
    }

    NVDriver *drv = nvCtx->drv;
    if (drv->nv == NULL) {
        LOG("NVENC loader is unavailable");
        LOG("cannot initialize session: NVENC loader is unavailable");
        return false;
    }
    if (!isEncodeProfileSupportedByDriver(drv, nvCtx->profile)) {
        LOG("%s NVENC is not supported on this system", getEncodeCodecName(nvCtx->profile));
        LOG("cannot initialize session: %s NVENC is unavailable", getEncodeCodecName(nvCtx->profile));
        return false;
    }

    NV_ENC_BUFFER_FORMAT inputBufferFormat = nvCtx->encodeInputFormat;
    if (inputBufferFormat == NV_ENC_BUFFER_FORMAT_UNDEFINED && nvCtx->renderTarget != NULL) {
        inputBufferFormat = surfaceFormatToEncodeInputBufferFormat(
            nvCtx->renderTarget->format,
            nvCtx->renderTarget->chromaFormat,
            nvCtx->renderTarget->bitDepth
        );
    }
    if (inputBufferFormat == NV_ENC_BUFFER_FORMAT_UNDEFINED) {
        inputBufferFormat = encodeProfileToInputBufferFormat(nvCtx->profile);
    }
    GUID encodeGuid = vaProfileToEncodeGuid(nvCtx->profile);
    GUID profileGuid = vaProfileToEncodeProfileGuid(nvCtx->profile, inputBufferFormat);
    nvCtx->encodeInputFormat = inputBufferFormat;

    const int rtSurfaceFormat = nvCtx->renderTarget ? (int) nvCtx->renderTarget->format : -1;
    const int rtChromaFormat = nvCtx->renderTarget ? (int) nvCtx->renderTarget->chromaFormat : -1;
    const int rtBitDepth = nvCtx->renderTarget ? nvCtx->renderTarget->bitDepth : -1;

    LOG(
        "initializing encode session profile=%d size=%ux%u rc=0x%x bitrate=%u target=%u fps=%u/%u intra=%u idr=%u ip=%u inputFmt=0x%x rtSurface=%d rtChroma=%d rtBitDepth=%d initialQp=%u minQp=%u maxQp=%u",
        nvCtx->profile,
        nvCtx->width,
        nvCtx->height,
        nvCtx->encodeRateControl,
        nvCtx->encodeBitrate,
        nvCtx->encodeTargetPercentage,
        nvCtx->encodeFrameRateNum,
        nvCtx->encodeFrameRateDen,
        nvCtx->encodeIntraPeriod,
        nvCtx->encodeIntraIDRPeriod,
        nvCtx->encodeIPPeriod,
        inputBufferFormat,
        rtSurfaceFormat,
        rtChromaFormat,
        rtBitDepth,
        nvCtx->encodeInitialQp,
        nvCtx->encodeMinQp,
        nvCtx->encodeMaxQp
    );

    const uint32_t negotiatedApiVersion = negotiateNvencApiVersion(drv, "encode session");
    nvCtx->encodeNvencApiVersion = negotiatedApiVersion;

    memset(&nvCtx->encodeApi, 0, sizeof(nvCtx->encodeApi));
    nvCtx->encodeApi.version = nvencStructVersionForApi(
        NV_ENCODE_API_FUNCTION_LIST_VER,
        negotiatedApiVersion
    );
    if (!checkNvencStatus(drv->nv->NvEncodeAPICreateInstance(&nvCtx->encodeApi), "NvEncodeAPICreateInstance")) {
        return false;
    }

    if (nvCtx->encodeApi.nvEncOpenEncodeSessionEx == NULL ||
        nvCtx->encodeApi.nvEncInitializeEncoder == NULL ||
        nvCtx->encodeApi.nvEncRegisterResource == NULL ||
        nvCtx->encodeApi.nvEncMapInputResource == NULL ||
        nvCtx->encodeApi.nvEncUnmapInputResource == NULL ||
        nvCtx->encodeApi.nvEncEncodePicture == NULL ||
        nvCtx->encodeApi.nvEncCreateBitstreamBuffer == NULL ||
        nvCtx->encodeApi.nvEncLockBitstream == NULL ||
        nvCtx->encodeApi.nvEncUnlockBitstream == NULL ||
        nvCtx->encodeApi.nvEncDestroyEncoder == NULL) {
        LOG("Required NVENC functions are not available");
        LOG("cannot initialize session: required NVENC symbols are missing");
        return false;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams = {0};
    openParams.version = nvencStructVersionForApi(
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER,
        negotiatedApiVersion
    );
    openParams.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    openParams.device = drv->cudaContext;
    openParams.apiVersion = negotiatedApiVersion;

    NVENCSTATUS status = NV_ENC_SUCCESS;

    status = nvCtx->encodeApi.nvEncOpenEncodeSessionEx(&openParams, &nvCtx->encoder);
    if (!checkNvencStatus(status, "nvEncOpenEncodeSessionEx")) {
        LOG(
            "nvEncOpenEncodeSessionEx failed context_id=%d profile=%d entrypoint=%d size=%ux%u inputFmt=0x%x api=0x%x cuda_ctx=%p",
            (int) nvCtx->contextId,
            (int) nvCtx->profile,
            (int) nvCtx->entrypoint,
            nvCtx->width,
            nvCtx->height,
            (unsigned int) nvCtx->encodeInputFormat,
            negotiatedApiVersion,
            drv->cudaContext
        );
        return false;
    }
    noteNvencSessionOpen(nvCtx->encoder);
    LOG("nvEncOpenEncodeSessionEx succeeded context_id=%d encoder=%p", (int) nvCtx->contextId, nvCtx->encoder);

    NV_ENC_PRESET_CONFIG preset = {0};
    preset.version = nvencStructVersionForApi(NV_ENC_PRESET_CONFIG_VER, negotiatedApiVersion);
    preset.presetCfg.version = nvencStructVersionForApi(NV_ENC_CONFIG_VER, negotiatedApiVersion);
    if (nvCtx->encodeApi.nvEncGetEncodePresetConfigEx != NULL) {
        checkNvencStatus(
            nvCtx->encodeApi.nvEncGetEncodePresetConfigEx(
                nvCtx->encoder,
                encodeGuid,
                NV_ENC_PRESET_P4_GUID,
                NV_ENC_TUNING_INFO_LOW_LATENCY,
                &preset
            ),
            "nvEncGetEncodePresetConfigEx");
    } else if (nvCtx->encodeApi.nvEncGetEncodePresetConfig != NULL) {
        checkNvencStatus(
            nvCtx->encodeApi.nvEncGetEncodePresetConfig(
                nvCtx->encoder,
                encodeGuid,
                NV_ENC_PRESET_P4_GUID,
                &preset
            ),
            "nvEncGetEncodePresetConfig");
    }

    NV_ENC_CONFIG encodeConfig = preset.presetCfg;
    if (encodeConfig.version == 0) {
        memset(&encodeConfig, 0, sizeof(encodeConfig));
        encodeConfig.version = nvencStructVersionForApi(NV_ENC_CONFIG_VER, negotiatedApiVersion);
    } else {
        encodeConfig.version = nvencStructVersionForApi(NV_ENC_CONFIG_VER, negotiatedApiVersion);
    }

    encodeConfig.profileGUID = profileGuid;
    encodeConfig.gopLength = nvCtx->encodeIntraPeriod > 0 ? nvCtx->encodeIntraPeriod : NVENC_INFINITE_GOPLENGTH;
    encodeConfig.frameIntervalP = 1;
    encodeConfig.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    encodeConfig.rcParams.rateControlMode = vaRateControlToNvencRateControl(nvCtx->encodeRateControl);
    uint32_t bitrate = nvCtx->encodeBitrate;
    uint32_t targetPercentage = nvCtx->encodeTargetPercentage;
    if (targetPercentage == 0 || targetPercentage > 100) {
        targetPercentage = 100;
    }
    if (encodeConfig.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CONSTQP) {
        configureConstQpForEncode(nvCtx, &encodeConfig.rcParams);
    } else {
        if (bitrate == 0) {
            bitrate = 4 * 1000 * 1000;
        }
        if (encodeConfig.rcParams.rateControlMode == NV_ENC_PARAMS_RC_VBR) {
            encodeConfig.rcParams.maxBitRate = bitrate;
            uint64_t averageBitrate = ((uint64_t) bitrate * targetPercentage) / 100;
            if (averageBitrate == 0) {
                averageBitrate = bitrate;
            }
            encodeConfig.rcParams.averageBitRate = (uint32_t) averageBitrate;
        } else {
            encodeConfig.rcParams.averageBitRate = bitrate;
            encodeConfig.rcParams.maxBitRate = bitrate;
        }
        configureQpBoundsForRateControl(nvCtx, &encodeConfig.rcParams);
        LOG(
            "configured bitrate RC: mode=%d maxBitrate=%u averageBitrate=%u targetPercentage=%u",
            encodeConfig.rcParams.rateControlMode,
            encodeConfig.rcParams.maxBitRate,
            encodeConfig.rcParams.averageBitRate,
            targetPercentage
        );
    }

    uint32_t codecIdrPeriod = nvCtx->encodeIntraIDRPeriod > 0 ? nvCtx->encodeIntraIDRPeriod : encodeConfig.gopLength;
    if (isH264EncodeProfile(nvCtx->profile)) {
        const uint32_t h264ChromaFormatIDC =
            (inputBufferFormat == NV_ENC_BUFFER_FORMAT_YUV444 ||
             inputBufferFormat == NV_ENC_BUFFER_FORMAT_YUV444_10BIT) ? 3u : 1u;
        const uint32_t h264BitDepthMinus8 =
            (inputBufferFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT ||
             inputBufferFormat == NV_ENC_BUFFER_FORMAT_YUV444_10BIT) ? 2u : 0u;
        encodeConfig.encodeCodecConfig.h264Config.idrPeriod = codecIdrPeriod;
        encodeConfig.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
        encodeConfig.encodeCodecConfig.h264Config.chromaFormatIDC = h264ChromaFormatIDC;
        setH264ConfigBitDepth(&encodeConfig.encodeCodecConfig.h264Config, h264BitDepthMinus8);
        encodeConfig.encodeCodecConfig.h264Config.maxNumRefFrames = 2;
    } else if (isHEVCEncodeProfile(nvCtx->profile)) {
        uint32_t hevcChromaFormatIDC = 1;
        if (isHEVC444EncodeProfile(nvCtx->profile)) {
            hevcChromaFormatIDC = 3;
        } else if (isHEVC422EncodeProfile(nvCtx->profile)) {
            hevcChromaFormatIDC = 2;
        }
        uint32_t hevcBitDepthMinus8 = 0;
        if (inputBufferFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT ||
            inputBufferFormat == NV_ENC_BUFFER_FORMAT_YUV444_10BIT
#if NVENCAPI_MAJOR_VERSION >= 13
            || inputBufferFormat == NV_ENC_BUFFER_FORMAT_P210
#endif
           ) {
            hevcBitDepthMinus8 = 2;
        }
        encodeConfig.encodeCodecConfig.hevcConfig.idrPeriod = codecIdrPeriod;
        encodeConfig.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
        encodeConfig.encodeCodecConfig.hevcConfig.chromaFormatIDC = hevcChromaFormatIDC;
        setHevcConfigBitDepth(&encodeConfig.encodeCodecConfig.hevcConfig, hevcBitDepthMinus8);
        encodeConfig.encodeCodecConfig.hevcConfig.maxNumRefFramesInDPB = 2;
    } else if (isAV1EncodeProfile(nvCtx->profile)) {
        const uint32_t av1BitDepthMinus8 =
            inputBufferFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT ? 2 : 0;
        encodeConfig.encodeCodecConfig.av1Config.idrPeriod = codecIdrPeriod;
        encodeConfig.encodeCodecConfig.av1Config.repeatSeqHdr = 1;
        encodeConfig.encodeCodecConfig.av1Config.chromaFormatIDC = 1;
        setAv1ConfigBitDepth(&encodeConfig.encodeCodecConfig.av1Config, av1BitDepthMinus8);
        encodeConfig.encodeCodecConfig.av1Config.maxNumRefFramesInDPB = 2;
    } else {
        LOG("unsupported encode profile=%d during session initialization", nvCtx->profile);
        destroyEncodeSession(nvCtx);
        return false;
    }

    NV_ENC_INITIALIZE_PARAMS initParams = {0};
    initParams.version = nvencStructVersionForApi(NV_ENC_INITIALIZE_PARAMS_VER, negotiatedApiVersion);
    initParams.encodeGUID = encodeGuid;
    initParams.presetGUID = NV_ENC_PRESET_P4_GUID;
    initParams.encodeWidth = nvCtx->width;
    initParams.encodeHeight = nvCtx->height;
    initParams.darWidth = nvCtx->width;
    initParams.darHeight = nvCtx->height;
    initParams.frameRateNum = nvCtx->encodeFrameRateNum > 0 ? nvCtx->encodeFrameRateNum : 30;
    initParams.frameRateDen = nvCtx->encodeFrameRateDen > 0 ? nvCtx->encodeFrameRateDen : 1;
    initParams.enableEncodeAsync = 0;
    initParams.enablePTD = 1;
    initParams.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    initParams.encodeConfig = &encodeConfig;

    LOG(
        "encoder config prepared: codec=%s gop=%u idr=%u frameIntervalP=%u bitrate=%u rcMode=%d fps=%u/%u inputFmt=0x%x",
        getEncodeCodecName(nvCtx->profile),
        encodeConfig.gopLength,
        codecIdrPeriod,
        encodeConfig.frameIntervalP,
        encodeConfig.rcParams.averageBitRate,
        encodeConfig.rcParams.rateControlMode,
        initParams.frameRateNum,
        initParams.frameRateDen,
        inputBufferFormat
    );

    status = nvCtx->encodeApi.nvEncInitializeEncoder(nvCtx->encoder, &initParams);
    if (!checkNvencStatus(status, "nvEncInitializeEncoder")) {
        destroyEncodeSession(nvCtx);
        return false;
    }
    LOG("nvEncInitializeEncoder succeeded");

    NV_ENC_CREATE_BITSTREAM_BUFFER bitstreamParams = {0};
    bitstreamParams.version = nvencStructVersionForApi(
        NV_ENC_CREATE_BITSTREAM_BUFFER_VER,
        negotiatedApiVersion
    );
    status = nvCtx->encodeApi.nvEncCreateBitstreamBuffer(nvCtx->encoder, &bitstreamParams);
    if (!checkNvencStatus(status, "nvEncCreateBitstreamBuffer")) {
        destroyEncodeSession(nvCtx);
        return false;
    }
    nvCtx->encodeBitstream = bitstreamParams.bitstreamBuffer;
    noteNvencBitstreamCreate(nvCtx->encodeBitstream);
    LOG("bitstream buffer created context_id=%d handle=%p", (int) nvCtx->contextId, nvCtx->encodeBitstream);

    if (CHECK_CUDA_RESULT(cu->cuCtxPushCurrent(drv->cudaContext))) {
        destroyEncodeSession(nvCtx);
        return false;
    }
    const bool inputIs444 =
        inputBufferFormat == NV_ENC_BUFFER_FORMAT_YUV444 ||
        inputBufferFormat == NV_ENC_BUFFER_FORMAT_YUV444_10BIT;
    const bool inputIs422 =
#if NVENCAPI_MAJOR_VERSION >= 13
        inputBufferFormat == NV_ENC_BUFFER_FORMAT_NV16 ||
        inputBufferFormat == NV_ENC_BUFFER_FORMAT_P210;
#else
        false;
#endif
    const uint32_t inputBytesPerSample =
        (inputBufferFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT ||
         inputBufferFormat == NV_ENC_BUFFER_FORMAT_YUV444_10BIT
#if NVENCAPI_MAJOR_VERSION >= 13
         || inputBufferFormat == NV_ENC_BUFFER_FORMAT_P210
#endif
        ) ? 2u : 1u;
    const size_t inputWidthBytes = (size_t) nvCtx->width * inputBytesPerSample;
    const size_t inputTotalHeight = inputIs444
        ? (size_t) nvCtx->height * 3u
        : (inputIs422
            ? (size_t) nvCtx->height * 2u
            : (size_t) nvCtx->height + ((size_t) nvCtx->height >> 1));

    CUresult inputAllocResult = cu->cuMemAllocPitch(&nvCtx->encodeInputBuffer, &nvCtx->encodeInputPitch, inputWidthBytes, inputTotalHeight, 16);
    if (CHECK_CUDA_RESULT(inputAllocResult)) {
        cu->cuCtxPopCurrent(NULL);
        destroyEncodeSession(nvCtx);
        return false;
    }
    nvCtx->encodeInputBytes = nvCtx->encodeInputPitch * inputTotalHeight;
    noteEncodeInputAlloc(nvCtx->encodeInputBytes);
    if (CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL))) {
        destroyEncodeSession(nvCtx);
        return false;
    }
    LOG(
        "CUDA encode input allocated context_id=%d ptr=%p pitch=%zu widthBytes=%zu totalHeight=%zu fmt=0x%x",
        (int) nvCtx->contextId,
        (void *) nvCtx->encodeInputBuffer,
        nvCtx->encodeInputPitch,
        inputWidthBytes,
        inputTotalHeight,
        inputBufferFormat
    );

    NV_ENC_REGISTER_RESOURCE registerParams = {0};
    registerParams.version = nvencStructVersionForApi(NV_ENC_REGISTER_RESOURCE_VER, negotiatedApiVersion);
    registerParams.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    registerParams.width = nvCtx->width;
    registerParams.height = nvCtx->height;
    registerParams.pitch = (uint32_t)nvCtx->encodeInputPitch;
    registerParams.resourceToRegister = (void *)nvCtx->encodeInputBuffer;
    registerParams.bufferFormat = inputBufferFormat;
    registerParams.bufferUsage = NV_ENC_INPUT_IMAGE;

    status = nvCtx->encodeApi.nvEncRegisterResource(nvCtx->encoder, &registerParams);
    if (!checkNvencStatus(status, "nvEncRegisterResource")) {
        destroyEncodeSession(nvCtx);
        return false;
    }
    nvCtx->encodeRegisteredInput = registerParams.registeredResource;
    noteNvencRegisteredCreate(nvCtx->encodeRegisteredInput);
    LOG("registered encode input context_id=%d resource=%p", (int) nvCtx->contextId, nvCtx->encodeRegisteredInput);

    nvCtx->encodeSessionInitialized = true;
    LOG("encode session initialized successfully context_id=%d", (int) nvCtx->contextId);
    return true;
}

static bool destroyContext(NVDriver *drv, NVContext *nvCtx) {
    if (isEncodeEntrypoint(nvCtx->entrypoint)) {
        return destroyEncodeSession(nvCtx);
    }

    if (isVideoProcEntrypoint(nvCtx->entrypoint)) {
        return true;
    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), false);

    if (nvCtx->resolveThreadStarted) {
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
    for (;;) {
        VAGenericID id = VA_INVALID_ID;
        ObjectType type = OBJECT_TYPE_CONFIG;

        pthread_mutex_lock(&drv->objectCreationMutex);
        if (drv->objects.size > 0) {
            Object obj = (Object) drv->objects.buf[0];
            id = obj->id;
            type = obj->type;
        }
        pthread_mutex_unlock(&drv->objectCreationMutex);

        if (id == VA_INVALID_ID) {
            break;
        }

        LOG("deleteAllObjects handling object %d type %d", id, type);

        switch (type) {
            case OBJECT_TYPE_CONFIG:
                deleteObject(drv, id);
                break;
            case OBJECT_TYPE_CONTEXT: {
                NVContext *nvCtx = (NVContext *) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, id);
                if (nvCtx != NULL) {
                    destroyContext(drv, nvCtx);
                }
                deleteObject(drv, id);
                break;
            }
            case OBJECT_TYPE_SURFACE: {
                NVSurface *surface = (NVSurface *) getObjectPtr(drv, OBJECT_TYPE_SURFACE, id);
                destroySurfaceObject(drv, id, surface);
                break;
            }
            case OBJECT_TYPE_BUFFER: {
                NVBuffer *buf = (NVBuffer *) getObjectPtr(drv, OBJECT_TYPE_BUFFER, id);
                destroyBufferObject(buf);
                deleteObject(drv, id);
                break;
            }
            case OBJECT_TYPE_IMAGE: {
                NVImage *img = (NVImage *) getObjectPtr(drv, OBJECT_TYPE_IMAGE, id);
                destroyImageObject(drv, id, img);
                deleteObject(drv, id);
                break;
            }
        }
    }
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

static bool encodeGuidListContains(const GUID *guidList, uint32_t guidCount, const GUID *targetGuid) {
    if (guidList == NULL || targetGuid == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < guidCount; i++) {
        if (guidEqual(&guidList[i], targetGuid)) {
            return true;
        }
    }
    return false;
}

static bool queryEncodeCapabilityFromSession(
    NV_ENCODE_API_FUNCTION_LIST *api,
    void *encoder,
    uint32_t negotiatedApiVersion,
    const GUID *encodeGuid,
    NV_ENC_CAPS capsToQuery,
    const char *capName
) {
    NV_ENC_CAPS_PARAM capsParam = {0};
    capsParam.version = nvencStructVersionForApi(NV_ENC_CAPS_PARAM_VER, negotiatedApiVersion);
    capsParam.capsToQuery = capsToQuery;

    int capsValue = 0;
    const bool ok = checkNvencStatus(
        api->nvEncGetEncodeCaps(encoder, *encodeGuid, &capsParam, &capsValue),
        "nvEncGetEncodeCaps"
    );
    if (!ok) {
        return false;
    }

    LOG(
        "encode caps probe result: %s capsToQuery=%d value=%d",
        capName,
        (int) capsToQuery,
        capsValue
    );
    return capsValue > 0;
}

static void resetEncodeSupport(NVDriver *drv) {
    drv->supportsEncodeH264 = false;
    drv->supportsEncodeHEVC = false;
    drv->supportsEncodeAV1 = false;
    drv->supportsEncodeH26410Bit = false;
    drv->supportsEncodeH264444 = false;
    drv->supportsEncodeAV110Bit = false;
    drv->supportsEncodeHEVC10Bit = false;
    drv->supportsEncodeHEVC422 = false;
    drv->supportsEncodeHEVC444 = false;
}

static void applyEncodeProbeCacheToDriver(NVDriver *drv, const EncodeProbeCache *cache) {
    drv->supportsEncodeH264 = cache->supportsEncodeH264;
    drv->supportsEncodeH26410Bit = cache->supportsEncodeH26410Bit;
    drv->supportsEncodeH264444 = cache->supportsEncodeH264444;
    drv->supportsEncodeHEVC = cache->supportsEncodeHEVC;
    drv->supportsEncodeAV1 = cache->supportsEncodeAV1;
    drv->supportsEncodeAV110Bit = cache->supportsEncodeAV110Bit;
    drv->supportsEncodeHEVC10Bit = cache->supportsEncodeHEVC10Bit;
    drv->supportsEncodeHEVC422 = cache->supportsEncodeHEVC422;
    drv->supportsEncodeHEVC444 = cache->supportsEncodeHEVC444;
}

static void updateEncodeProbeCacheFromDriver(const NVDriver *drv, EncodeProbeCache *cache) {
    cache->supportsEncodeH264 = drv->supportsEncodeH264;
    cache->supportsEncodeH26410Bit = drv->supportsEncodeH26410Bit;
    cache->supportsEncodeH264444 = drv->supportsEncodeH264444;
    cache->supportsEncodeHEVC = drv->supportsEncodeHEVC;
    cache->supportsEncodeAV1 = drv->supportsEncodeAV1;
    cache->supportsEncodeAV110Bit = drv->supportsEncodeAV110Bit;
    cache->supportsEncodeHEVC10Bit = drv->supportsEncodeHEVC10Bit;
    cache->supportsEncodeHEVC422 = drv->supportsEncodeHEVC422;
    cache->supportsEncodeHEVC444 = drv->supportsEncodeHEVC444;
}

static void resetProcessTransientState(void) {
    pthread_mutex_lock(&encode_probe_cache_mutex);
    encodeProbeCache.valid = false;
    encodeProbeCache.gpuId = 0;
    encodeProbeCache.supportsEncodeH264 = false;
    encodeProbeCache.supportsEncodeH26410Bit = false;
    encodeProbeCache.supportsEncodeH264444 = false;
    encodeProbeCache.supportsEncodeHEVC = false;
    encodeProbeCache.supportsEncodeAV1 = false;
    encodeProbeCache.supportsEncodeAV110Bit = false;
    encodeProbeCache.supportsEncodeHEVC10Bit = false;
    encodeProbeCache.supportsEncodeHEVC422 = false;
    encodeProbeCache.supportsEncodeHEVC444 = false;
    pthread_mutex_unlock(&encode_probe_cache_mutex);

    pthread_mutex_lock(&cuda_init_backoff_mutex);
    cuda_init_retry_not_before_ns = 0;
    cuda_init_backoff_last_log_ns = 0;
    cuda_init_backoff_force_hard = false;
    pthread_mutex_unlock(&cuda_init_backoff_mutex);

    pthread_mutex_lock(&cuda_init_recovery_mutex);
    cuda_init_recovery_last_attempt_ns = 0;
    pthread_mutex_unlock(&cuda_init_recovery_mutex);

    __atomic_store_n(&vaImageCreateCount, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&vaImageDestroyCount, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&vaImageLiveBytes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&vaImagePeakBytes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&encodeInputAllocCount, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&encodeInputFreeCount, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&encodeInputLiveBytes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&encodeInputPeakBytes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&nvencSessionOpenCount, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&nvencSessionDestroyCount, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&nvencSessionPeakLive, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&nvencBitstreamCreateCount, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&nvencBitstreamDestroyCount, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&nvencBitstreamPeakLive, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&nvencRegisteredCreateCount, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&nvencRegisteredDestroyCount, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&nvencRegisteredPeakLive, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&nvencMappedCreateCount, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&nvencMappedDestroyCount, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&nvencMappedPeakLive, 0, __ATOMIC_RELAXED);

    pthread_mutex_lock(&nvencVisibleMemMutex);
    nvencVisibleGlobalDeltaUsed = 0;
    nvencVisibleGlobalPeakDeltaUsed = 0;
    pthread_mutex_unlock(&nvencVisibleMemMutex);

    eglResetProcessTransientState();
}

static void releaseGlobalCodecFunctionsLocked(void) {
    if (nv != NULL) {
        nvenc_free_functions(&nv);
        nv = NULL;
    }
    if (cv != NULL) {
        cuvid_free_functions(&cv);
        cv = NULL;
    }
    if (cu != NULL) {
        cuda_free_functions(&cu);
        cu = NULL;
    }
}

static void releaseGlobalCodecFunctions(void) {
    pthread_mutex_lock(&global_codec_loader_mutex);
    releaseGlobalCodecFunctionsLocked();
    pthread_mutex_unlock(&global_codec_loader_mutex);
}

static bool ensureGlobalCodecFunctionsLoaded(void) {
    pthread_mutex_lock(&global_codec_loader_mutex);

    if (cu != NULL && cv != NULL) {
        pthread_mutex_unlock(&global_codec_loader_mutex);
        return true;
    }

    if (cu != NULL || cv != NULL || nv != NULL) {
        LOG("Releasing partially loaded global codec function tables before reload");
        releaseGlobalCodecFunctionsLocked();
    }

    int ret = cuda_load_functions(&cu, NULL);
    if (ret != 0) {
        cu = NULL;
        LOG("Failed to load CUDA functions");
        pthread_mutex_unlock(&global_codec_loader_mutex);
        return false;
    }

    ret = cuvid_load_functions(&cv, NULL);
    if (ret != 0) {
        cv = NULL;
        LOG("Failed to load NVDEC functions");
        releaseGlobalCodecFunctionsLocked();
        pthread_mutex_unlock(&global_codec_loader_mutex);
        return false;
    }

    ret = nvenc_load_functions(&nv, NULL);
    if (ret != 0) {
        nv = NULL;
        LOG("NVENC library not available, encode entrypoints will be disabled");
    }

    CUresult cuInitRet = cu->cuInit(0);
    if (cuInitRet != CUDA_SUCCESS) {
        LOG("cuInit failed while loading global codec function tables: %d", cuInitRet);
        releaseGlobalCodecFunctionsLocked();
        pthread_mutex_unlock(&global_codec_loader_mutex);
        return false;
    }

    LOG("Global codec functions ready (cu=%p cv=%p nv=%p)", cu, cv, nv);
    pthread_mutex_unlock(&global_codec_loader_mutex);
    return true;
}

static void probeEncodeSupport(NVDriver *drv) {
    resetEncodeSupport(drv);
    const bool cacheEnabled = isEncodeProbeCacheEnabled();

    if (cacheEnabled) {
        pthread_mutex_lock(&encode_probe_cache_mutex);
        if (encodeProbeCache.valid && encodeProbeCache.gpuId == drv->cudaGpuId) {
            applyEncodeProbeCacheToDriver(drv, &encodeProbeCache);
            pthread_mutex_unlock(&encode_probe_cache_mutex);
            LOG("encode capability probe cache hit: gpu=%d", drv->cudaGpuId);
            return;
        }
        pthread_mutex_unlock(&encode_probe_cache_mutex);
    } else {
        LOG("encode capability probe cache disabled via NVD_ENCODE_PROBE_CACHE=0");
    }

    if (drv->nv == NULL) {
        LOG("encode capability probe skipped: NVENC loader is unavailable");
        return;
    }

    const uint32_t negotiatedApiVersion = negotiateNvencApiVersion(drv, "encode probe");
    char persistentCachePath[PATH_MAX] = {0};
    if (cacheEnabled) {
        if (buildEncodeProbePersistentCachePath(drv, negotiatedApiVersion, persistentCachePath, sizeof(persistentCachePath)) &&
            loadEncodeProbePersistentCache(persistentCachePath, drv)) {
            pthread_mutex_lock(&encode_probe_cache_mutex);
            encodeProbeCache.valid = true;
            encodeProbeCache.gpuId = drv->cudaGpuId;
            updateEncodeProbeCacheFromDriver(drv, &encodeProbeCache);
            pthread_mutex_unlock(&encode_probe_cache_mutex);
            LOG("encode capability probe persistent cache hit: %s", persistentCachePath);
            return;
        }
    }

    NV_ENCODE_API_FUNCTION_LIST api = {0};
    api.version = nvencStructVersionForApi(NV_ENCODE_API_FUNCTION_LIST_VER, negotiatedApiVersion);
    if (!checkNvencStatus(drv->nv->NvEncodeAPICreateInstance(&api), "NvEncodeAPICreateInstance")) {
        return;
    }

    if (api.nvEncOpenEncodeSessionEx == NULL ||
        api.nvEncGetEncodeGUIDCount == NULL ||
        api.nvEncGetEncodeGUIDs == NULL ||
        api.nvEncDestroyEncoder == NULL) {
        LOG(
            "encode capability probe skipped: required symbols missing open=%p getGuidCount=%p getGuids=%p destroy=%p",
            (void *) api.nvEncOpenEncodeSessionEx,
            (void *) api.nvEncGetEncodeGUIDCount,
            (void *) api.nvEncGetEncodeGUIDs,
            (void *) api.nvEncDestroyEncoder
        );
        return;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams = {0};
    openParams.version = nvencStructVersionForApi(
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER,
        negotiatedApiVersion
    );
    openParams.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    openParams.device = drv->cudaContext;
    openParams.apiVersion = negotiatedApiVersion;

    void *encoder = NULL;
    GUID *encodeGuids = NULL;
    uint32_t encodeGuidCount = 0;
    bool cacheable = false;

    if (!checkNvencStatus(api.nvEncOpenEncodeSessionEx(&openParams, &encoder), "nvEncOpenEncodeSessionEx")) {
        LOG("encode capability probe: nvEncOpenEncodeSessionEx failed");
        return;
    }
    cacheable = true;

    if (checkNvencStatus(api.nvEncGetEncodeGUIDCount(encoder, &encodeGuidCount), "nvEncGetEncodeGUIDCount") &&
        encodeGuidCount > 0) {
        encodeGuids = calloc(encodeGuidCount, sizeof(GUID));
        if (encodeGuids != NULL &&
            !checkNvencStatus(
                api.nvEncGetEncodeGUIDs(encoder, encodeGuids, encodeGuidCount, &encodeGuidCount),
                "nvEncGetEncodeGUIDs"
            )) {
            free(encodeGuids);
            encodeGuids = NULL;
            encodeGuidCount = 0;
        }
    }

    drv->supportsEncodeH264 =
        encodeGuidListContains(encodeGuids, encodeGuidCount, &NV_ENC_CODEC_H264_GUID);
    drv->supportsEncodeHEVC =
        encodeGuidListContains(encodeGuids, encodeGuidCount, &NV_ENC_CODEC_HEVC_GUID);
    drv->supportsEncodeAV1 =
        encodeGuidListContains(encodeGuids, encodeGuidCount, &NV_ENC_CODEC_AV1_GUID);

    LOG(
        "encode capability probe result: codec=H.264 supported=%d guidCount=%u",
        drv->supportsEncodeH264,
        encodeGuidCount
    );
    LOG(
        "encode capability probe result: codec=HEVC supported=%d guidCount=%u",
        drv->supportsEncodeHEVC,
        encodeGuidCount
    );
    LOG(
        "encode capability probe result: codec=AV1 supported=%d guidCount=%u",
        drv->supportsEncodeAV1,
        encodeGuidCount
    );

    if (api.nvEncGetEncodeCaps == NULL) {
        LOG(
            "encode caps probe skipped: open=%p getCaps=%p destroy=%p",
            (void *) api.nvEncOpenEncodeSessionEx,
            (void *) api.nvEncGetEncodeCaps,
            (void *) api.nvEncDestroyEncoder
        );
        goto cleanup;
    }

    if (drv->supportsEncodeH264) {
#if NVENCAPI_MAJOR_VERSION >= 13
        drv->supportsEncodeH26410Bit = queryEncodeCapabilityFromSession(
            &api,
            encoder,
            negotiatedApiVersion,
            &NV_ENC_CODEC_H264_GUID,
            NV_ENC_CAPS_SUPPORT_10BIT_ENCODE,
            "H.264 10-bit encode"
        );
#else
        LOG("H.264 10-bit encode probe skipped: ffnvcodec headers are older than 13.x");
#endif
        drv->supportsEncodeH264444 = queryEncodeCapabilityFromSession(
            &api,
            encoder,
            negotiatedApiVersion,
            &NV_ENC_CODEC_H264_GUID,
            NV_ENC_CAPS_SUPPORT_YUV444_ENCODE,
            "H.264 YUV444 encode"
        );
    }
    if (drv->supportsEncodeAV1) {
        drv->supportsEncodeAV110Bit = queryEncodeCapabilityFromSession(
            &api,
            encoder,
            negotiatedApiVersion,
            &NV_ENC_CODEC_AV1_GUID,
            NV_ENC_CAPS_SUPPORT_10BIT_ENCODE,
            "AV1 10-bit encode"
        );
    }
    if (drv->supportsEncodeHEVC) {
        drv->supportsEncodeHEVC10Bit = queryEncodeCapabilityFromSession(
            &api,
            encoder,
            negotiatedApiVersion,
            &NV_ENC_CODEC_HEVC_GUID,
            NV_ENC_CAPS_SUPPORT_10BIT_ENCODE,
            "HEVC 10-bit encode"
        );
#if NVENCAPI_MAJOR_VERSION >= 13
        drv->supportsEncodeHEVC422 = queryEncodeCapabilityFromSession(
            &api,
            encoder,
            negotiatedApiVersion,
            &NV_ENC_CODEC_HEVC_GUID,
            NV_ENC_CAPS_SUPPORT_YUV422_ENCODE,
            "HEVC YUV422 encode"
        );
#else
        LOG("HEVC YUV422 encode probe skipped: ffnvcodec headers are older than 13.x");
#endif
        drv->supportsEncodeHEVC444 = queryEncodeCapabilityFromSession(
            &api,
            encoder,
            negotiatedApiVersion,
            &NV_ENC_CODEC_HEVC_GUID,
            NV_ENC_CAPS_SUPPORT_YUV444_ENCODE,
            "HEVC YUV444 encode"
        );
    }

cleanup:
    if (cacheable && cacheEnabled) {
        pthread_mutex_lock(&encode_probe_cache_mutex);
        encodeProbeCache.valid = true;
        encodeProbeCache.gpuId = drv->cudaGpuId;
        updateEncodeProbeCacheFromDriver(drv, &encodeProbeCache);
        pthread_mutex_unlock(&encode_probe_cache_mutex);
        if (persistentCachePath[0] != '\0') {
            storeEncodeProbePersistentCache(persistentCachePath, drv);
        }
    }
    free(encodeGuids);
    if (encoder != NULL) {
        checkNvencStatus(api.nvEncDestroyEncoder(encoder), "nvEncDestroyEncoder");
    }
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
        LOG(
            "[DECODE][RT] dequeued surface=%p pictureIdx=%d read_idx=%d write_idx=%d decode_failed=%d",
            (void*)surface,
            surface != NULL ? surface->pictureIdx : -1,
            ctx->surfaceQueueReadIdx,
            ctx->surfaceQueueWriteIdx,
            surface != NULL ? surface->decodeFailed : 1
        );

        if (surface == NULL) {
            LOG("[DECODE][RT] dequeued NULL surface, skipping");
            continue;
        }

        CUdeviceptr deviceMemory = (CUdeviceptr) NULL;
        unsigned int pitch = 0;

        //map frame
        CUVIDPROCPARAMS procParams = {
            .progressive_frame = surface->progressiveFrame,
            .top_field_first = surface->topFieldFirst,
            .second_field = surface->secondField
        };

        const unsigned long long mapStartUs = nv_getmonotonic_us();
        CUresult mapResult = CUDA_SUCCESS;
        if (!surface->decodeFailed) {
            mapResult = cv->cuvidMapVideoFrame(
                ctx->decoder, surface->pictureIdx, &deviceMemory, &pitch, &procParams);
        }

        if (surface->decodeFailed || mapResult != CUDA_SUCCESS) {
            if (mapResult != CUDA_SUCCESS) {
                CHECK_CUDA_RESULT(mapResult);
            }
            LOG(
                "[DECODE][RT] map failed pictureIdx=%d decode_failed=%d cuerr=%d elapsed_us=%llu",
                surface->pictureIdx,
                surface->decodeFailed,
                mapResult,
                nv_getmonotonic_us() - mapStartUs
            );
            pthread_mutex_lock(&surface->mutex);
            surface->resolving = 0;
            pthread_cond_signal(&surface->cond);
            pthread_mutex_unlock(&surface->mutex);
            continue;
        }
        LOG(
            "[DECODE][RT] map success pictureIdx=%d device_ptr=%p pitch=%u elapsed_us=%llu",
            surface->pictureIdx,
            (void*)deviceMemory,
            pitch,
            nv_getmonotonic_us() - mapStartUs
        );

        //update cuarray
        const bool exportOk = drv->backend->exportCudaPtr(drv, deviceMemory, surface, pitch);
        LOG(
            "[DECODE][RT] exportCudaPtr result=%d pictureIdx=%d",
            exportOk,
            surface->pictureIdx
        );
        //unmap frame

        CUresult unmapResult = cv->cuvidUnmapVideoFrame(ctx->decoder, deviceMemory);
        if (unmapResult != CUDA_SUCCESS) {
            CHECK_CUDA_RESULT(unmapResult);
            LOG("[DECODE][RT] unmap failed pictureIdx=%d cuerr=%d", surface->pictureIdx, unmapResult);
        } else {
            LOG("[DECODE][RT] unmap success pictureIdx=%d", surface->pictureIdx);
        }
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

    // resolve thread owns one pushed CUDA context for its lifetime.
    // Pop it on thread exit to avoid leaking context references.
    CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL));

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

    if (drv->supportsEncodeH264) {
        bool haveBaseline = false;
        bool haveMain = false;
        bool haveHigh = false;
        bool haveHigh10 = false;
        for (int i = 0; i < profiles; i++) {
            haveBaseline |= profile_list[i] == VAProfileH264ConstrainedBaseline;
            haveMain |= profile_list[i] == VAProfileH264Main;
            haveHigh |= profile_list[i] == VAProfileH264High;
            haveHigh10 |= profile_list[i] == VAProfileH264High10;
        }
        if (!haveBaseline) {
            profile_list[profiles++] = VAProfileH264ConstrainedBaseline;
        }
        if (!haveMain) {
            profile_list[profiles++] = VAProfileH264Main;
        }
        if (!haveHigh) {
            profile_list[profiles++] = VAProfileH264High;
        }
        if (drv->supportsEncodeH26410Bit && !haveHigh10) {
            profile_list[profiles++] = VAProfileH264High10;
        }
    }
    if (drv->supportsEncodeHEVC) {
        bool haveMain = false;
        bool haveMain10 = false;
        bool haveMain422_10 = false;
        bool haveMain444 = false;
        bool haveMain444_10 = false;
        for (int i = 0; i < profiles; i++) {
            haveMain |= profile_list[i] == VAProfileHEVCMain;
            haveMain10 |= profile_list[i] == VAProfileHEVCMain10;
            haveMain422_10 |= profile_list[i] == VAProfileHEVCMain422_10;
            haveMain444 |= profile_list[i] == VAProfileHEVCMain444;
            haveMain444_10 |= profile_list[i] == VAProfileHEVCMain444_10;
        }
        if (!haveMain) {
            profile_list[profiles++] = VAProfileHEVCMain;
        }
        if (drv->supportsEncodeHEVC10Bit && !haveMain10) {
            profile_list[profiles++] = VAProfileHEVCMain10;
        }
        if (drv->supportsEncodeHEVC10Bit && drv->supportsEncodeHEVC422 && !haveMain422_10) {
            profile_list[profiles++] = VAProfileHEVCMain422_10;
        }
        if (drv->supportsEncodeHEVC444 && !haveMain444) {
            profile_list[profiles++] = VAProfileHEVCMain444;
        }
        if (drv->supportsEncodeHEVC444 && drv->supportsEncodeHEVC10Bit && !haveMain444_10) {
            profile_list[profiles++] = VAProfileHEVCMain444_10;
        }
    }
    if (drv->supportsEncodeAV1) {
        bool haveProfile0 = false;
        for (int i = 0; i < profiles; i++) {
            haveProfile0 |= profile_list[i] == VAProfileAV1Profile0;
        }
        if (!haveProfile0) {
            profile_list[profiles++] = VAProfileAV1Profile0;
        }
    }

    profile_list[profiles++] = VAProfileNone;

    //now filter out the codecs we don't support
    for (int i = 0; i < profiles; i++) {
        bool supportedByDecode = vaToCuCodec(profile_list[i]) != cudaVideoCodec_NONE;
        bool supportedByEncode = isEncodeProfileSupportedByDriver(drv, profile_list[i]);
        bool supportedByVpp = profile_list[i] == VAProfileNone;
        if (!supportedByDecode && !supportedByEncode && !supportedByVpp) {
            for (int x = i; x < profiles-1; x++) {
                profile_list[x] = profile_list[x+1];
            }
            profiles--;
            i--;
        }
    }

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
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    int count = 0;

    if (vaToCuCodec(profile) != cudaVideoCodec_NONE) {
        entrypoint_list[count++] = VAEntrypointVLD;
    }

    if (isEncodeProfileSupportedByDriver(drv, profile)) {
        entrypoint_list[count++] = VAEntrypointEncSlice;
    }

    if (count == 0) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
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
    if (entrypoint != VAEntrypointVLD) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }

    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    if (isVideoProcEntrypoint(entrypoint)) {
        if (profile != VAProfileNone) {
            return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        }

        for (int i = 0; i < num_attribs; i++) {
            switch (attrib_list[i].type) {
            case VAConfigAttribRTFormat:
                attrib_list[i].value = getVideoProcRtFormatMask(drv);
                break;
            case VAConfigAttribMaxPictureWidth:
            case VAConfigAttribMaxPictureHeight:
                attrib_list[i].value = 16384;
                break;
            default:
                attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
                break;
            }
        }

        return VA_STATUS_SUCCESS;
    }

    if (isEncodeEntrypoint(entrypoint)) {
        if (!isEncodeProfileSupportedByDriver(drv, profile)) {
            return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        }

        for (int i = 0; i < num_attribs; i++) {
            switch (attrib_list[i].type) {
            case VAConfigAttribRTFormat:
                attrib_list[i].value = encodeProfileToRtFormatMask(drv, profile);
                break;
            case VAConfigAttribRateControl:
                attrib_list[i].value = VA_RC_CBR | VA_RC_VBR | VA_RC_CQP;
                break;
            case VAConfigAttribEncMaxRefFrames:
                // At least one L0 reference frame is required by many VAAPI clients (ffmpeg).
                attrib_list[i].value = 1;
                break;
            case VAConfigAttribPredictionDirection:
                attrib_list[i].value = VA_PREDICTION_DIRECTION_PREVIOUS;
                break;
            case VAConfigAttribEncMaxSlices:
                attrib_list[i].value = 1;
                break;
            case VAConfigAttribEncPackedHeaders:
                attrib_list[i].value = encodeProfileToPackedHeaderMask(profile);
                break;
            case VAConfigAttribEncHEVCFeatures:
                attrib_list[i].value = isHEVCEncodeProfile(profile)
                    ? encodeHevcFeaturesAttribValue()
                    : VA_ATTRIB_NOT_SUPPORTED;
                break;
            case VAConfigAttribEncHEVCBlockSizes:
                attrib_list[i].value = isHEVCEncodeProfile(profile)
                    ? encodeHevcBlockSizesAttribValue()
                    : VA_ATTRIB_NOT_SUPPORTED;
                break;
            case VAConfigAttribMaxPictureWidth:
                attrib_list[i].value = 8192;
                break;
            case VAConfigAttribMaxPictureHeight:
                attrib_list[i].value = 8192;
                break;
            default:
                attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
                break;
            }
        }

        return VA_STATUS_SUCCESS;
    }

    if (entrypoint != VAEntrypointVLD || vaToCuCodec(profile) == cudaVideoCodec_NONE) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

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
            attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
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

    if (isVideoProcEntrypoint(entrypoint)) {
        if (profile != VAProfileNone) {
            LOG("VideoProc requires VAProfileNone, got: %d", profile);
            return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        }

        uint32_t selectedRtFormat = VA_RT_FORMAT_YUV420;
        const uint32_t supportedRtMask = getVideoProcRtFormatMask(drv);
        for (int i = 0; i < num_attribs; i++) {
            if (attrib_list[i].type != VAConfigAttribRTFormat) {
                continue;
            }
            const uint32_t requestedMask = attrib_list[i].value & supportedRtMask;
            if (requestedMask == 0) {
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
            }
            if (requestedMask & VA_RT_FORMAT_YUV420) {
                selectedRtFormat = VA_RT_FORMAT_YUV420;
#if NVENCAPI_MAJOR_VERSION >= 13
            } else if (requestedMask & VA_RT_FORMAT_YUV422) {
                selectedRtFormat = VA_RT_FORMAT_YUV422;
#endif
            } else if (requestedMask & VA_RT_FORMAT_YUV444) {
                selectedRtFormat = VA_RT_FORMAT_YUV444;
            } else if (requestedMask & VA_RT_FORMAT_YUV420_10) {
                selectedRtFormat = VA_RT_FORMAT_YUV420_10;
            } else if (requestedMask & VA_RT_FORMAT_YUV420_12) {
                selectedRtFormat = VA_RT_FORMAT_YUV420_12;
#if NVENCAPI_MAJOR_VERSION >= 13
            } else if (requestedMask & VA_RT_FORMAT_YUV422_10) {
                selectedRtFormat = VA_RT_FORMAT_YUV422_10;
            } else if (requestedMask & VA_RT_FORMAT_YUV422_12) {
                selectedRtFormat = VA_RT_FORMAT_YUV422_12;
#endif
            } else if (requestedMask & VA_RT_FORMAT_YUV444_10) {
                selectedRtFormat = VA_RT_FORMAT_YUV444_10;
            } else if (requestedMask & VA_RT_FORMAT_YUV444_12) {
                selectedRtFormat = VA_RT_FORMAT_YUV444_12;
            } else if (requestedMask & VA_RT_FORMAT_RGB32) {
                selectedRtFormat = VA_RT_FORMAT_RGB32;
            }
            break;
        }

        Object obj = allocateObject(drv, OBJECT_TYPE_CONFIG, sizeof(NVConfig));
        NVConfig *cfg = (NVConfig*) obj->obj;
        cfg->profile = profile;
        cfg->entrypoint = entrypoint;
        cfg->cudaCodec = cudaVideoCodec_NONE;
        cfg->rateControl = 0;

        switch (selectedRtFormat) {
        case VA_RT_FORMAT_YUV420:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_NV12;
            cfg->chromaFormat = cudaVideoChromaFormat_420;
            cfg->bitDepth = 8;
            break;
#if NVENCAPI_MAJOR_VERSION >= 13
        case VA_RT_FORMAT_YUV422:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_NV16;
            cfg->chromaFormat = cudaVideoChromaFormat_422;
            cfg->bitDepth = 8;
            break;
#endif
        case VA_RT_FORMAT_YUV420_10:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
            cfg->chromaFormat = cudaVideoChromaFormat_420;
            cfg->bitDepth = 10;
            break;
        case VA_RT_FORMAT_YUV420_12:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
            cfg->chromaFormat = cudaVideoChromaFormat_420;
            cfg->bitDepth = 12;
            break;
#if NVENCAPI_MAJOR_VERSION >= 13
        case VA_RT_FORMAT_YUV422_10:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_P216;
            cfg->chromaFormat = cudaVideoChromaFormat_422;
            cfg->bitDepth = 10;
            break;
        case VA_RT_FORMAT_YUV422_12:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_P216;
            cfg->chromaFormat = cudaVideoChromaFormat_422;
            cfg->bitDepth = 12;
            break;
#endif
        case VA_RT_FORMAT_YUV444:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444;
            cfg->chromaFormat = cudaVideoChromaFormat_444;
            cfg->bitDepth = 8;
            break;
        case VA_RT_FORMAT_YUV444_10:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
            cfg->chromaFormat = cudaVideoChromaFormat_444;
            cfg->bitDepth = 10;
            break;
        case VA_RT_FORMAT_YUV444_12:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
            cfg->chromaFormat = cudaVideoChromaFormat_444;
            cfg->bitDepth = 12;
            break;
        case VA_RT_FORMAT_RGB32:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444;
            cfg->chromaFormat = cudaVideoChromaFormat_444;
            cfg->bitDepth = 8;
            break;
        default:
            deleteObject(drv, obj->id);
            return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        }

        *config_id = obj->id;
        return VA_STATUS_SUCCESS;
    }

    if (isEncodeEntrypoint(entrypoint)) {
        if (!drv->supportsEncodeH264 && !drv->supportsEncodeHEVC && !drv->supportsEncodeAV1) {
            LOG("Encode entrypoint requested but NVENC is not available");
            return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
        }
        if (!isEncodeProfile(profile) || !isEncodeProfileSupportedByDriver(drv, profile)) {
            LOG("Unsupported encode profile: %d", profile);
            return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        }

        Object obj = allocateObject(drv, OBJECT_TYPE_CONFIG, sizeof(NVConfig));
        NVConfig *cfg = (NVConfig*) obj->obj;
        cfg->profile = profile;
        cfg->entrypoint = entrypoint;
        cfg->cudaCodec = isH264EncodeProfile(profile) ? cudaVideoCodec_H264 :
                         (isHEVCEncodeProfile(profile) ? cudaVideoCodec_HEVC : cudaVideoCodec_AV1);
        cfg->chromaFormat = cudaVideoChromaFormat_420;
        cfg->surfaceFormat = cudaVideoSurfaceFormat_NV12;
        cfg->bitDepth = 8;
        cfg->rateControl = VA_RC_CBR;
        const uint32_t supportedRtFormatMask = encodeProfileToRtFormatMask(drv, profile);
        uint32_t requestedRtMask = supportedRtFormatMask;
        uint32_t selectedRtFormat = pickEncodeRtFormatFromMask(
            profile,
            supportedRtFormatMask
        );
        if (selectedRtFormat == 0 ||
            !configureEncodeConfigForRtFormat(cfg, profile, selectedRtFormat)) {
            deleteObject(drv, obj->id);
            return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        }

        for (int i = 0; i < num_attribs; i++) {
            if (attrib_list[i].type == VAConfigAttribRateControl) {
                uint32_t requestedRC = attrib_list[i].value;
                if ((requestedRC & (VA_RC_CBR | VA_RC_VBR | VA_RC_CQP)) == 0) {
                    deleteObject(drv, obj->id);
                    return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                }
                cfg->rateControl = requestedRC;
            } else if (attrib_list[i].type == VAConfigAttribRTFormat) {
                requestedRtMask = attrib_list[i].value & supportedRtFormatMask;
                selectedRtFormat = pickEncodeRtFormatFromMask(profile, requestedRtMask);
                if (selectedRtFormat == 0 ||
                    !configureEncodeConfigForRtFormat(cfg, profile, selectedRtFormat)) {
                    deleteObject(drv, obj->id);
                    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
                }
            }
        }

        LOG(
            "created encode config id=%d profile=%d supportedRtMask=0x%x requestedRtMask=0x%x selectedRt=0x%x surfaceFormat=%d chroma=%d bitDepth=%d rc=0x%x",
            obj->id,
            profile,
            supportedRtFormatMask,
            requestedRtMask,
            selectedRtFormat,
            cfg->surfaceFormat,
            cfg->chromaFormat,
            cfg->bitDepth,
            cfg->rateControl
        );

        *config_id = obj->id;
        return VA_STATUS_SUCCESS;
    }

    cudaVideoCodec cudaCodec = vaToCuCodec(profile);
    if (cudaCodec == cudaVideoCodec_NONE) {
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
    cfg->rateControl = 0;

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

    *profile = cfg->profile;
    *entrypoint = cfg->entrypoint;
    int i = 0;

    if (isVideoProcEntrypoint(cfg->entrypoint)) {
        attrib_list[i].value = getVideoProcRtFormatMask(drv);
        attrib_list[i].type = VAConfigAttribRTFormat;
        i++;
        attrib_list[i].value = 16384;
        attrib_list[i].type = VAConfigAttribMaxPictureWidth;
        i++;
        attrib_list[i].value = 16384;
        attrib_list[i].type = VAConfigAttribMaxPictureHeight;
        i++;
        *num_attribs = i;
        return VA_STATUS_SUCCESS;
    }

    if (isEncodeEntrypoint(cfg->entrypoint)) {
        attrib_list[i].value = encodeProfileToRtFormatMask(drv, cfg->profile);
        attrib_list[i].type = VAConfigAttribRTFormat;
        i++;
        attrib_list[i].value = cfg->rateControl ? cfg->rateControl : (VA_RC_CBR | VA_RC_VBR | VA_RC_CQP);
        attrib_list[i].type = VAConfigAttribRateControl;
        i++;
        attrib_list[i].value = encodeProfileToPackedHeaderMask(cfg->profile);
        attrib_list[i].type = VAConfigAttribEncPackedHeaders;
        i++;
        if (isHEVCEncodeProfile(cfg->profile)) {
            attrib_list[i].value = encodeHevcFeaturesAttribValue();
            attrib_list[i].type = VAConfigAttribEncHEVCFeatures;
            i++;
            attrib_list[i].value = encodeHevcBlockSizesAttribValue();
            attrib_list[i].type = VAConfigAttribEncHEVCBlockSizes;
            i++;
        }
        attrib_list[i].value = 1;
        attrib_list[i].type = VAConfigAttribEncMaxRefFrames;
        i++;
        attrib_list[i].value = VA_PREDICTION_DIRECTION_PREVIOUS;
        attrib_list[i].type = VAConfigAttribPredictionDirection;
        i++;
        attrib_list[i].value = 1;
        attrib_list[i].type = VAConfigAttribEncMaxSlices;
        i++;
        *num_attribs = i;
        return VA_STATUS_SUCCESS;
    }

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
    const VADRMPRIMESurfaceDescriptor *externalSurfaceDesc = NULL;
    const VASurfaceAttribExternalBuffers *externalBufferDesc = NULL;
    uint32_t externalMemType = 0;
    void *externalBufferPointer = NULL;
    bool importExternalPrime2 = false;
    bool importExternalPrime = false;

    cudaVideoSurfaceFormat nvFormat;
    cudaVideoChromaFormat chromaFormat;
    int bitdepth;

    if (attrib_list != NULL) {
        for (uint32_t i = 0; i < num_attribs; i++) {
            if (attrib_list[i].type == VASurfaceAttribMemoryType &&
                attrib_list[i].value.type == VAGenericValueTypeInteger) {
                externalMemType = (uint32_t) attrib_list[i].value.value.i;
            } else if (attrib_list[i].type == VASurfaceAttribExternalBufferDescriptor &&
                       attrib_list[i].value.type == VAGenericValueTypePointer) {
                externalBufferPointer = attrib_list[i].value.value.p;
            }
        }
    }

    importExternalPrime2 = externalBufferPointer != NULL &&
                           (externalMemType & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) != 0;
    importExternalPrime = externalBufferPointer != NULL &&
                          (externalMemType & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME) != 0 &&
                          !importExternalPrime2;
    const bool allocateMode = !importExternalPrime2 && !importExternalPrime;

    if (importExternalPrime2) {
        externalSurfaceDesc = (const VADRMPRIMESurfaceDescriptor *) externalBufferPointer;
        if (num_surfaces != 1) {
            LOG("External DRM PRIME surface import currently supports one surface at a time (got %u)", num_surfaces);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if (drv->backend->importExternalSurface == NULL) {
            LOG("Backend does not support external DRM PRIME surface import");
            return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        }
        if (externalSurfaceDesc->width > 0) {
            width = externalSurfaceDesc->width;
        }
        if (externalSurfaceDesc->height > 0) {
            height = externalSurfaceDesc->height;
        }
        LOG("using DRM_PRIME_2 import path (w=%u h=%u)", width, height);
    } else if (importExternalPrime) {
        if (nvRequireDrmPrime2ForExternalImport()) {
            LOG(
                "Rejecting DRM_PRIME external import: DRM_PRIME_2 is required to preserve explicit modifiers. "
                "Set NVD_DRM_PRIME_ASSUME_LINEAR=1 only for known-linear callers."
            );
            return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        }
        externalBufferDesc = (const VASurfaceAttribExternalBuffers *) externalBufferPointer;
        if (num_surfaces != 1) {
            LOG("External DRM PRIME surface import currently supports one surface at a time (got %u)", num_surfaces);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if (drv->backend->importExternalSurface == NULL) {
            LOG("Backend does not support external DRM PRIME surface import");
            return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        }
        if (externalBufferDesc->buffers == NULL || externalBufferDesc->num_buffers == 0) {
            LOG("External DRM PRIME surface import missing dma-buf handles");
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if (externalBufferDesc->num_planes == 0) {
            LOG("External DRM PRIME surface import has no planes");
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if (externalBufferDesc->width > 0) {
            width = externalBufferDesc->width;
        }
        if (externalBufferDesc->height > 0) {
            height = externalBufferDesc->height;
        }
        LOG(
            "using DRM_PRIME import path: fourcc=0x%x size=%ux%u planes=%u buffers=%u data_size=%u",
            externalBufferDesc->pixel_format,
            width,
            height,
            externalBufferDesc->num_planes,
            externalBufferDesc->num_buffers,
            externalBufferDesc->data_size
        );
        if (nvAssumeLinearForDrmPrimeImport()) {
            LOG(
                "DRM_PRIME import will assume DRM_FORMAT_MOD_LINEAR for all objects "
                "(NVD_DRM_PRIME_ASSUME_LINEAR=1). This is only safe for truly linear callers."
            );
        } else {
            LOG(
                "DRM_PRIME import has no explicit modifier metadata; reconstructed descriptor will use "
                "DRM_FORMAT_MOD_INVALID and may fail for non-linear layouts."
            );
        }
    }

    switch (format)
    {
    case VA_RT_FORMAT_YUV420:
        nvFormat = cudaVideoSurfaceFormat_NV12;
        chromaFormat = cudaVideoChromaFormat_420;
        bitdepth = 8;
        break;
#if NVENCAPI_MAJOR_VERSION >= 13
    case VA_RT_FORMAT_YUV422:
        nvFormat = cudaVideoSurfaceFormat_NV16;
        chromaFormat = cudaVideoChromaFormat_422;
        bitdepth = 8;
        break;
#endif
    case VA_RT_FORMAT_YUV420_10:
        nvFormat = cudaVideoSurfaceFormat_P016;
        chromaFormat = cudaVideoChromaFormat_420;
        bitdepth = 10;
        break;
#if NVENCAPI_MAJOR_VERSION >= 13
    case VA_RT_FORMAT_YUV422_10:
        nvFormat = cudaVideoSurfaceFormat_P216;
        chromaFormat = cudaVideoChromaFormat_422;
        bitdepth = 10;
        break;
#endif
    case VA_RT_FORMAT_YUV420_12:
        nvFormat = cudaVideoSurfaceFormat_P016;
        chromaFormat = cudaVideoChromaFormat_420;
        bitdepth = 12;
        break;
#if NVENCAPI_MAJOR_VERSION >= 13
    case VA_RT_FORMAT_YUV422_12:
        nvFormat = cudaVideoSurfaceFormat_P216;
        chromaFormat = cudaVideoChromaFormat_422;
        bitdepth = 12;
        break;
#endif
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
        // Keep CUDA-facing format in a known YUV enum; backing allocation
        // uses rtFormat to choose ARGB storage when needed.
        nvFormat = cudaVideoSurfaceFormat_YUV444;
        chromaFormat = cudaVideoChromaFormat_444;
        bitdepth = 8;
        break;
    
    default:
        LOG(
            "unsupported RT format=0x%x size=%ux%u surfaces=%u allocate_mode=%s external_mem_type=0x%x",
            format,
            width,
            height,
            num_surfaces,
            allocateMode ? "yes" : "no",
            externalMemType
        );
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

    VAStatus status = VA_STATUS_SUCCESS;
    uint32_t createdSurfaces = 0;
    for (uint32_t i = 0; i < num_surfaces; i++) {
        Object surfaceObject = allocateObject(drv, OBJECT_TYPE_SURFACE, sizeof(NVSurface));
        surfaces[i] = surfaceObject->id;
        NVSurface *suf = (NVSurface*) surfaceObject->obj;
        suf->width = width;
        suf->height = height;
        suf->format = nvFormat;
        suf->pictureIdx = -1;
        suf->bitDepth = bitdepth;
        suf->rtFormat = format;
        suf->context = NULL;
        suf->chromaFormat = chromaFormat;
        pthread_mutex_init(&suf->mutex, NULL);
        pthread_cond_init(&suf->cond, NULL);

        LOG("Creating surface %ux%u, format %X (%p)", width, height, format, suf);
        createdSurfaces++;

        if (importExternalPrime2 || importExternalPrime) {
            VADRMPRIMESurfaceDescriptor primeDesc;
            memset(&primeDesc, 0, sizeof(primeDesc));

            if (importExternalPrime2) {
                primeDesc = *externalSurfaceDesc;
            } else {
                primeDesc.fourcc = externalBufferDesc->pixel_format;
                primeDesc.width = externalBufferDesc->width;
                primeDesc.height = externalBufferDesc->height;

                primeDesc.num_objects = externalBufferDesc->num_buffers;
                if (primeDesc.num_objects > ARRAY_SIZE(primeDesc.objects)) {
                    primeDesc.num_objects = ARRAY_SIZE(primeDesc.objects);
                }
                if (primeDesc.num_objects == 0) {
                    LOG("External DRM PRIME descriptor has no objects");
                    status = VA_STATUS_ERROR_INVALID_PARAMETER;
                    break;
                }

                for (uint32_t j = 0; j < primeDesc.num_objects; j++) {
                    primeDesc.objects[j].fd = (int) externalBufferDesc->buffers[j];
                    primeDesc.objects[j].size = externalBufferDesc->data_size;
                    primeDesc.objects[j].drm_format_modifier =
                        nvAssumeLinearForDrmPrimeImport()
                            ? DRM_FORMAT_MOD_LINEAR
                            : DRM_FORMAT_MOD_INVALID;
                }

                primeDesc.num_layers = externalBufferDesc->num_planes;
                if (primeDesc.num_layers > ARRAY_SIZE(primeDesc.layers)) {
                    primeDesc.num_layers = ARRAY_SIZE(primeDesc.layers);
                }

                for (uint32_t j = 0; j < primeDesc.num_layers; j++) {
                    const uint32_t objectIndex =
                        (primeDesc.num_objects == 1) ? 0 : j;
                    primeDesc.layers[j].drm_format =
                        (j == 0) ? DRM_FORMAT_R8 : DRM_FORMAT_RG88;
                    primeDesc.layers[j].num_planes = 1;
                    primeDesc.layers[j].object_index[0] = objectIndex;
                    primeDesc.layers[j].offset[0] = externalBufferDesc->offsets[j];
                    primeDesc.layers[j].pitch[0] = externalBufferDesc->pitches[j];
                }
            }

            drv->importSurfaceAttemptCount++;
            drv->importLastMemType = externalMemType;

            if (!drv->backend->importExternalSurface(drv, suf, &primeDesc)) {
                drv->importSurfaceFailCount++;
                drv->importLastFailWidth = width;
                drv->importLastFailHeight = height;
                drv->importLastFailStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
                LOG("Failed to import external DRM PRIME surface into CUDA");
                if (importExternalPrime2) {
                    status = VA_STATUS_ERROR_ALLOCATION_FAILED;
                    break;
                }
                LOG("External DRM_PRIME import failed; falling back to internal surface allocation path");
            } else {
                drv->importSurfaceSuccessCount++;
                drv->importLastSuccessWidth = width;
                drv->importLastSuccessHeight = height;
            }
        }
    }

    if (status != VA_STATUS_SUCCESS) {
        for (uint32_t i = 0; i < createdSurfaces; i++) {
            NVSurface *surface = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surfaces[i]);
            if (surface != NULL && surface->backingImage != NULL) {
                drv->backend->detachBackingImageFromSurface(drv, surface);
            }
            deleteObject(drv, surfaces[i]);
            surfaces[i] = VA_INVALID_ID;
        }
    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    return status;
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
    VAStatus status = VA_STATUS_SUCCESS;

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    for (int i = 0; i < num_surfaces; i++) {
        NVSurface *surface = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surface_list[i]);
        if (!surface) {
            status = VA_STATUS_ERROR_INVALID_SURFACE;
            break;
        }

        LOG(
            "Destroying surface %d (%p) context=%p size=%ux%u rt_format=0x%x resolving=%d decode_failed=%d",
            surface->pictureIdx,
            surface,
            surface->context,
            surface->width,
            surface->height,
            surface->rtFormat,
            surface->resolving,
            surface->decodeFailed
        );

        drv->backend->detachBackingImageFromSurface(drv, surface);

        deleteObject(drv, surface_list[i]);
    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    return status;
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

    if (isEncodeEntrypoint(cfg->entrypoint)) {
        if (num_render_targets > 0) {
            NVSurface *surface = (NVSurface *) getObjectPtr(drv, OBJECT_TYPE_SURFACE, render_targets[0]);
            if (!surface) {
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
            picture_width = surface->width;
            picture_height = surface->height;
        }

        Object contextObj = allocateObject(drv, OBJECT_TYPE_CONTEXT, sizeof(NVContext));
        NVContext *nvCtx = (NVContext*) contextObj->obj;
        nvCtx->drv = drv;
        nvCtx->contextId = contextObj->id;
        nvCtx->profile = cfg->profile;
        nvCtx->entrypoint = cfg->entrypoint;
        nvCtx->width = picture_width;
        nvCtx->height = picture_height;
        nvCtx->encodeFrameRateNum = 30;
        nvCtx->encodeFrameRateDen = 1;
        nvCtx->encodeBitrate = 0;
        nvCtx->encodeRateControl = cfg->rateControl ? cfg->rateControl : VA_RC_CBR;
        nvCtx->encodeTargetPercentage = 100;
        nvCtx->encodeInitialQp = 0;
        nvCtx->encodeMinQp = 0;
        nvCtx->encodeMaxQp = 0;
        nvCtx->encodePicInitQp = 0;
        nvCtx->encodeInitialQpFromMisc = false;
        nvCtx->encodeCodedBuffer = VA_INVALID_ID;
        nvCtx->encodeForceIDR = false;
        nvCtx->encodeSessionInitialized = false;
        nvCtx->encodeInputFormat = surfaceFormatToEncodeInputBufferFormat(
            cfg->surfaceFormat,
            cfg->chromaFormat,
            cfg->bitDepth
        );
        nvCtx->resolveThreadStarted = false;

        LOG(
            "created encode context id=%d ptr=%p profile=%d entrypoint=%d size=%ux%u rc=0x%x renderTargets=%d",
            contextObj->id,
            nvCtx,
            nvCtx->profile,
            nvCtx->entrypoint,
            nvCtx->width,
            nvCtx->height,
            nvCtx->encodeRateControl,
            num_render_targets
        );

        *context = contextObj->id;
        return VA_STATUS_SUCCESS;
    }

    if (isVideoProcEntrypoint(cfg->entrypoint)) {
        Object contextObj = allocateObject(drv, OBJECT_TYPE_CONTEXT, sizeof(NVContext));
        NVContext *nvCtx = (NVContext*) contextObj->obj;
        nvCtx->drv = drv;
        nvCtx->contextId = contextObj->id;
        nvCtx->profile = cfg->profile;
        nvCtx->entrypoint = cfg->entrypoint;
        nvCtx->width = picture_width;
        nvCtx->height = picture_height;
        nvCtx->resolveThreadStarted = false;
        nvCtx->vppPipelineSet = false;
        nvCtx->renderTarget = NULL;
        *context = contextObj->id;
        return VA_STATUS_SUCCESS;
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

    CUvideodecoder decoder = NULL;
    if (CHECK_CUDA_RESULT(cv->cuvidCreateDecoder(&decoder, &vdci))) {
        // Pop the context even when decoder creation fails to avoid
        // accumulating pushed-context references across retries.
        CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL));
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    Object contextObj = allocateObject(drv, OBJECT_TYPE_CONTEXT, sizeof(NVContext));
    LOG("Creating decoder: %p for context id: %d", decoder, contextObj->id);

    NVContext *nvCtx = (NVContext*) contextObj->obj;
    nvCtx->drv = drv;
    nvCtx->contextId = contextObj->id;
    nvCtx->decoder = decoder;
    nvCtx->profile = cfg->profile;
    nvCtx->entrypoint = cfg->entrypoint;
    nvCtx->width = picture_width;
    nvCtx->height = picture_height;
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
    nvCtx->resolveThreadStarted = true;

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

    LOG(
        "Destroying context detail id=%d ptr=%p entrypoint=%d profile=%d size=%ux%u encodeSessionInitialized=%d surfaceCount=%d encoder=%p bitstream=%p registered=%p inputBuffer=%p",
        context,
        nvCtx,
        nvCtx->entrypoint,
        nvCtx->profile,
        nvCtx->width,
        nvCtx->height,
        nvCtx->encodeSessionInitialized,
        nvCtx->surfaceCount,
        nvCtx->encoder,
        nvCtx->encodeBitstream,
        nvCtx->encodeRegisteredInput,
        (void *) nvCtx->encodeInputBuffer
    );

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
    *buf_id = bufferObject->id;

    NVBuffer *buf = (NVBuffer*) bufferObject->obj;
    buf->bufferType = type;
    buf->elements = num_elements;
    buf->size = num_elements * size;
    buf->offset = offset;

    if (type == VAEncCodedBufferType) {
        if (!ensureCodedDataBuffer(buf, buf->size)) {
            deleteObject(drv, bufferObject->id);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        clearCodedBuffer(buf);
        LOG(
            "created coded buffer id=%u requestedSize=%zu elements=%u",
            bufferObject->id,
            buf->size,
            buf->elements
        );
    } else {
        buf->ptr = memalign(16, buf->size);
        if (buf->ptr == NULL) {
            LOG("Unable to allocate buffer of %zu bytes", buf->size);
            deleteObject(drv, bufferObject->id);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }

        if (data != NULL) {
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
        *pbuf = &buf->codedSegment;
        LOG(
            "mapped coded buffer id=%u size=%u status=0x%x",
            buf_id,
            buf->codedSegment.size,
            buf->codedSegment.status
        );
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

    if (buf->ptr != NULL) {
        free(buf->ptr);
    }

    if (buf->codedData != NULL) {
        free(buf->codedData);
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

    if (isVideoProcEntrypoint(nvCtx->entrypoint)) {
        nvCtx->renderTarget = surface;
        nvCtx->vppPipelineSet = false;
        nvCtx->vppSurfaceRegionSet = false;
        nvCtx->vppOutputRegionSet = false;
        memset(&nvCtx->vppPipeline, 0, sizeof(nvCtx->vppPipeline));
        return VA_STATUS_SUCCESS;
    }

    if (isEncodeEntrypoint(nvCtx->entrypoint)) {
        nvCtx->renderTarget = surface;
        nvCtx->encodeCodedBuffer = VA_INVALID_ID;
        nvCtx->encodeForceIDR = false;
        nvCtx->encodePackedHeaders.size = 0;
        nvCtx->encodePackedHeaderType = 0;
        nvCtx->encodePackedHeaderBitLength = 0;
        nvCtx->encodePackedHeaderHasEmulationBytes = false;
        LOG(
            "encode context=%u surface=%u size=%ux%u format=%d backing=%p",
            context,
            render_target,
            surface->width,
            surface->height,
            surface->format,
            (void *) surface->backingImage
        );
        return VA_STATUS_SUCCESS;
    }

    if (surface->context != NULL && surface->context != nvCtx) {
        //this surface was last used on a different context, we need to free up the backing image (it might not be the correct size)
        if (surface->backingImage != NULL) {
            CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
            drv->backend->detachBackingImageFromSurface(drv, surface);
            CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
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

static VAStatus nvStoreVideoProcPipelineParameters(
        NVDriver *drv,
        NVContext *nvCtx,
        VABufferID *buffers,
        int num_buffers
    )
{
    if (num_buffers <= 0) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    for (int i = 0; i < num_buffers; i++) {
        NVBuffer *buf = (NVBuffer*) getObjectPtr(drv, OBJECT_TYPE_BUFFER, buffers[i]);
        if (buf == NULL || buf->ptr == NULL) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        if (buf->bufferType != VAProcPipelineParameterBufferType ||
            buf->size < sizeof(VAProcPipelineParameterBuffer)) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        if (nvCtx->vppPipelineSet) {
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        }

        const VAProcPipelineParameterBuffer *pipeline =
            (const VAProcPipelineParameterBuffer *) buf->ptr;
        if (pipeline->num_filters != 0 ||
            pipeline->num_forward_references != 0 ||
            pipeline->num_backward_references != 0 ||
            pipeline->num_additional_outputs != 0 ||
            pipeline->rotation_state != VA_ROTATION_NONE ||
            pipeline->mirror_state != VA_MIRROR_NONE ||
            pipeline->blend_state != NULL ||
            pipeline->processing_mode != VAProcDefaultMode ||
            pipeline->output_hdr_metadata != NULL) {
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        }

        memcpy(&nvCtx->vppPipeline, pipeline, sizeof(VAProcPipelineParameterBuffer));
        nvCtx->vppSurfaceRegionSet = false;
        nvCtx->vppOutputRegionSet = false;
        if (pipeline->surface_region != NULL) {
            nvCtx->vppSurfaceRegion = *pipeline->surface_region;
            nvCtx->vppPipeline.surface_region = &nvCtx->vppSurfaceRegion;
            nvCtx->vppSurfaceRegionSet = true;
        } else {
            nvCtx->vppPipeline.surface_region = NULL;
        }
        if (pipeline->output_region != NULL) {
            nvCtx->vppOutputRegion = *pipeline->output_region;
            nvCtx->vppPipeline.output_region = &nvCtx->vppOutputRegion;
            nvCtx->vppOutputRegionSet = true;
        } else {
            nvCtx->vppPipeline.output_region = NULL;
        }

        LOG(
            "stored vpp pipeline: src_surface=%u src_rect=%s[%d,%d %ux%u] dst_rect=%s[%d,%d %ux%u] flags=0x%x filter_flags=0x%x",
            nvCtx->vppPipeline.surface,
            nvCtx->vppSurfaceRegionSet ? "" : "null",
            nvCtx->vppSurfaceRegionSet ? nvCtx->vppSurfaceRegion.x : 0,
            nvCtx->vppSurfaceRegionSet ? nvCtx->vppSurfaceRegion.y : 0,
            nvCtx->vppSurfaceRegionSet ? nvCtx->vppSurfaceRegion.width : 0,
            nvCtx->vppSurfaceRegionSet ? nvCtx->vppSurfaceRegion.height : 0,
            nvCtx->vppOutputRegionSet ? "" : "null",
            nvCtx->vppOutputRegionSet ? nvCtx->vppOutputRegion.x : 0,
            nvCtx->vppOutputRegionSet ? nvCtx->vppOutputRegion.y : 0,
            nvCtx->vppOutputRegionSet ? nvCtx->vppOutputRegion.width : 0,
            nvCtx->vppOutputRegionSet ? nvCtx->vppOutputRegion.height : 0,
            nvCtx->vppPipeline.pipeline_flags,
            nvCtx->vppPipeline.filter_flags
        );
        nvCtx->vppPipelineSet = true;
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

    if (isVideoProcEntrypoint(nvCtx->entrypoint)) {
        return nvStoreVideoProcPipelineParameters(drv, nvCtx, buffers, num_buffers);
    }

    if (isEncodeEntrypoint(nvCtx->entrypoint)) {
        LOG("encode context=%u bufferCount=%d", context, num_buffers);
        for (int i = 0; i < num_buffers; i++) {
            NVBuffer *buf = (NVBuffer*) getObjectPtr(drv, OBJECT_TYPE_BUFFER, buffers[i]);
            if (buf == NULL || buf->ptr == NULL) {
                LOG("buffer[%d] id=%u is invalid (buf=%p ptr=%p)", i, buffers[i], (void *) buf, buf == NULL ? NULL : buf->ptr);
                continue;
            }

            LOG(
                "buffer[%d] id=%u type=%d size=%zu elements=%u",
                i,
                buffers[i],
                buf->bufferType,
                buf->size,
                buf->elements
            );

            if (buf->bufferType == VAEncSequenceParameterBufferType) {
                if (isH264EncodeProfile(nvCtx->profile) &&
                    buf->size >= sizeof(VAEncSequenceParameterBufferH264)) {
                    VAEncSequenceParameterBufferH264 *seq = (VAEncSequenceParameterBufferH264 *) buf->ptr;
                    nvCtx->encodeIntraPeriod = seq->intra_period;
                    nvCtx->encodeIntraIDRPeriod = seq->intra_idr_period;
                    nvCtx->encodeIPPeriod = seq->ip_period;
                    if (seq->bits_per_second > 0) {
                        nvCtx->encodeBitrate = seq->bits_per_second;
                    }
                    if (seq->vui_fields.bits.timing_info_present_flag && seq->time_scale > 0) {
                        uint32_t den = seq->num_units_in_tick > 0 ? seq->num_units_in_tick * 2 : 1;
                        nvCtx->encodeFrameRateNum = seq->time_scale;
                        nvCtx->encodeFrameRateDen = den;
                    }
                } else if (isHEVCEncodeProfile(nvCtx->profile) &&
                           buf->size >= sizeof(VAEncSequenceParameterBufferHEVC)) {
                    VAEncSequenceParameterBufferHEVC *seq = (VAEncSequenceParameterBufferHEVC *) buf->ptr;
                    nvCtx->encodeIntraPeriod = seq->intra_period;
                    nvCtx->encodeIntraIDRPeriod = seq->intra_idr_period;
                    nvCtx->encodeIPPeriod = seq->ip_period;
                    if (seq->bits_per_second > 0) {
                        nvCtx->encodeBitrate = seq->bits_per_second;
                    }
                    if (seq->vui_parameters_present_flag &&
                        seq->vui_fields.bits.vui_timing_info_present_flag &&
                        seq->vui_time_scale > 0) {
                        uint32_t den = seq->vui_num_units_in_tick > 0 ? seq->vui_num_units_in_tick : 1;
                        nvCtx->encodeFrameRateNum = seq->vui_time_scale;
                        nvCtx->encodeFrameRateDen = den;
                    }
                } else if (isAV1EncodeProfile(nvCtx->profile) &&
                           buf->size >= sizeof(VAEncSequenceParameterBufferAV1)) {
                    VAEncSequenceParameterBufferAV1 *seq = (VAEncSequenceParameterBufferAV1 *) buf->ptr;
                    nvCtx->encodeIntraPeriod = seq->intra_period;
                    nvCtx->encodeIntraIDRPeriod = seq->intra_period;
                    nvCtx->encodeIPPeriod = seq->ip_period;
                    if (seq->bits_per_second > 0) {
                        nvCtx->encodeBitrate = seq->bits_per_second;
                    }
                } else {
                    LOG("sequence params ignored for profile=%d (size=%zu)", nvCtx->profile, buf->size);
                }
                LOG(
                    "updated sequence params: profile=%d intra=%u intraIDR=%u ip=%u bitrate=%u fps=%u/%u",
                    nvCtx->profile,
                    nvCtx->encodeIntraPeriod,
                    nvCtx->encodeIntraIDRPeriod,
                    nvCtx->encodeIPPeriod,
                    nvCtx->encodeBitrate,
                    nvCtx->encodeFrameRateNum,
                    nvCtx->encodeFrameRateDen
                );
            } else if (buf->bufferType == VAEncPictureParameterBufferType) {
                if (isH264EncodeProfile(nvCtx->profile) &&
                    buf->size >= sizeof(VAEncPictureParameterBufferH264)) {
                    VAEncPictureParameterBufferH264 *pic = (VAEncPictureParameterBufferH264 *) buf->ptr;
                    nvCtx->encodeCodedBuffer = pic->coded_buf;
                    nvCtx->encodeForceIDR = pic->pic_fields.bits.idr_pic_flag != 0;
                    if (nvCtx->encodeRateControl & VA_RC_CQP) {
                        nvCtx->encodePicInitQp = pic->pic_init_qp;
                        maybeSetEncodeInitialQpFromHint(
                            nvCtx,
                            (int32_t) pic->pic_init_qp,
                            "h264 pic_init_qp"
                        );
                    }
                    LOG(
                        "updated picture params: profile=%d codedBuffer=%u forceIDR=%d frameNum=%u picInitQp=%u",
                        nvCtx->profile,
                        nvCtx->encodeCodedBuffer,
                        nvCtx->encodeForceIDR,
                        pic->frame_num,
                        pic->pic_init_qp
                    );
                } else if (isHEVCEncodeProfile(nvCtx->profile) &&
                           buf->size >= sizeof(VAEncPictureParameterBufferHEVC)) {
                    VAEncPictureParameterBufferHEVC *pic = (VAEncPictureParameterBufferHEVC *) buf->ptr;
                    nvCtx->encodeCodedBuffer = pic->coded_buf;
                    nvCtx->encodeForceIDR = pic->pic_fields.bits.idr_pic_flag != 0;
                    if (nvCtx->encodeRateControl & VA_RC_CQP) {
                        nvCtx->encodePicInitQp = pic->pic_init_qp;
                        maybeSetEncodeInitialQpFromHint(
                            nvCtx,
                            (int32_t) pic->pic_init_qp,
                            "hevc pic_init_qp"
                        );
                    }
                    LOG(
                        "updated picture params: profile=%d codedBuffer=%u forceIDR=%d codingType=%u picInitQp=%u",
                        nvCtx->profile,
                        nvCtx->encodeCodedBuffer,
                        nvCtx->encodeForceIDR,
                        pic->pic_fields.bits.coding_type,
                        pic->pic_init_qp
                    );
                } else if (isAV1EncodeProfile(nvCtx->profile) &&
                           buf->size >= sizeof(VAEncPictureParameterBufferAV1)) {
                    VAEncPictureParameterBufferAV1 *pic = (VAEncPictureParameterBufferAV1 *) buf->ptr;
                    nvCtx->encodeCodedBuffer = pic->coded_buf;
                    nvCtx->encodeForceIDR = pic->picture_flags.bits.frame_type == 0;
                    if (nvCtx->encodeRateControl & VA_RC_CQP) {
                        maybeSetEncodeInitialQpFromHint(
                            nvCtx,
                            (int32_t) pic->base_qindex,
                            "av1 base_qindex"
                        );
                    }
                    LOG(
                        "updated picture params: profile=%d codedBuffer=%u forceIDR=%d frameType=%u baseQindex=%u",
                        nvCtx->profile,
                        nvCtx->encodeCodedBuffer,
                        nvCtx->encodeForceIDR,
                        pic->picture_flags.bits.frame_type,
                        pic->base_qindex
                    );
                } else {
                    LOG("picture params ignored for profile=%d (size=%zu)", nvCtx->profile, buf->size);
                }
            } else if (buf->bufferType == VAEncSliceParameterBufferType) {
                if ((nvCtx->encodeRateControl & VA_RC_CQP) &&
                    isH264EncodeProfile(nvCtx->profile) &&
                    buf->size >= sizeof(VAEncSliceParameterBufferH264)) {
                    VAEncSliceParameterBufferH264 *slice = (VAEncSliceParameterBufferH264 *) buf->ptr;
                    const int32_t baseQp =
                        nvCtx->encodePicInitQp > 0 ? (int32_t) nvCtx->encodePicInitQp
                                                   : (int32_t) nvCtx->encodeInitialQp;
                    maybeSetEncodeInitialQpFromHint(
                        nvCtx,
                        baseQp + (int32_t) slice->slice_qp_delta,
                        "h264 slice_qp_delta"
                    );
                    if (!nvCtx->encodeSessionInitialized) {
                        LOG(
                            "updated h264 slice params: firstMacroblock=%u numMacroblocks=%u sliceQpDelta=%d",
                            slice->macroblock_address,
                            slice->num_macroblocks,
                            slice->slice_qp_delta
                        );
                    }
                } else if ((nvCtx->encodeRateControl & VA_RC_CQP) &&
                           isHEVCEncodeProfile(nvCtx->profile) &&
                           buf->size >= sizeof(VAEncSliceParameterBufferHEVC)) {
                    VAEncSliceParameterBufferHEVC *slice = (VAEncSliceParameterBufferHEVC *) buf->ptr;
                    const int32_t baseQp =
                        nvCtx->encodePicInitQp > 0 ? (int32_t) nvCtx->encodePicInitQp
                                                   : (int32_t) nvCtx->encodeInitialQp;
                    maybeSetEncodeInitialQpFromHint(
                        nvCtx,
                        baseQp + (int32_t) slice->slice_qp_delta,
                        "hevc slice_qp_delta"
                    );
                    if (!nvCtx->encodeSessionInitialized) {
                        LOG(
                            "updated hevc slice params: sliceSegmentAddress=%u numCtus=%u sliceQpDelta=%d",
                            slice->slice_segment_address,
                            slice->num_ctu_in_slice,
                            slice->slice_qp_delta
                        );
                    }
                }
            } else if (buf->bufferType == VAEncPackedHeaderParameterBufferType &&
                       buf->size >= sizeof(VAEncPackedHeaderParameterBuffer)) {
                VAEncPackedHeaderParameterBuffer *packed =
                    (VAEncPackedHeaderParameterBuffer *) buf->ptr;
                if (isHEVCEncodeProfile(nvCtx->profile)) {
                    nvCtx->encodePackedHeaderType = packed->type;
                    nvCtx->encodePackedHeaderBitLength = packed->bit_length;
                    nvCtx->encodePackedHeaderHasEmulationBytes =
                        packed->has_emulation_bytes != 0;
                }
                LOG(
                    "packed header param: type=%u bits=%u has_emulation=%u",
                    packed->type,
                    packed->bit_length,
                    packed->has_emulation_bytes
                );
            } else if (buf->bufferType == VAEncPackedHeaderDataBufferType) {
                size_t appendedBytes = 0;
                if (isHEVCEncodeProfile(nvCtx->profile) &&
                    (nvCtx->encodePackedHeaderType == VAEncPackedHeaderSequence ||
                     nvCtx->encodePackedHeaderType == VAEncPackedHeaderPicture ||
                     nvCtx->encodePackedHeaderType == VAEncPackedHeaderRawData)) {
                    size_t packedBytes = buf->size;
                    if (nvCtx->encodePackedHeaderBitLength != 0) {
                        const size_t announcedBytes =
                            (nvCtx->encodePackedHeaderBitLength + 7u) / 8u;
                        if (announcedBytes < packedBytes) {
                            packedBytes = announcedBytes;
                        }
                    }
                    if (packedBytes > 0) {
                        const uint64_t beforeSize = nvCtx->encodePackedHeaders.size;
                        appendHevcPackedHeaderBuffer(
                            &nvCtx->encodePackedHeaders,
                            (const uint8_t *) buf->ptr,
                            packedBytes,
                            nvCtx->encodePackedHeaderHasEmulationBytes
                        );
                        appendedBytes = nvCtx->encodePackedHeaders.size - beforeSize;
                    }
                }
                LOG(
                    "packed header data: bytes=%zu appended=%zu type=%u has_emulation=%u total=%llu",
                    buf->size,
                    appendedBytes,
                    nvCtx->encodePackedHeaderType,
                    nvCtx->encodePackedHeaderHasEmulationBytes ? 1 : 0,
                    (unsigned long long) nvCtx->encodePackedHeaders.size
                );
                nvCtx->encodePackedHeaderType = 0;
                nvCtx->encodePackedHeaderBitLength = 0;
                nvCtx->encodePackedHeaderHasEmulationBytes = false;
            } else if (buf->bufferType == VAEncMiscParameterBufferType &&
                       buf->size >= sizeof(VAEncMiscParameterBuffer)) {
                VAEncMiscParameterBuffer *misc = (VAEncMiscParameterBuffer *) buf->ptr;
                if (misc->type == VAEncMiscParameterTypeRateControl &&
                    buf->size >= sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl)) {
                    VAEncMiscParameterRateControl *rc = (VAEncMiscParameterRateControl *) misc->data;
                    if (rc->bits_per_second > 0) {
                        nvCtx->encodeBitrate = rc->bits_per_second;
                    }
                    if (rc->target_percentage > 0) {
                        nvCtx->encodeTargetPercentage =
                            rc->target_percentage > 100 ? 100 : rc->target_percentage;
                    } else {
                        nvCtx->encodeTargetPercentage = 100;
                    }
                    if (rc->initial_qp > 0) {
                        nvCtx->encodeInitialQp = rc->initial_qp;
                        nvCtx->encodeInitialQpFromMisc = true;
                    }
                    if (rc->min_qp > 0) {
                        nvCtx->encodeMinQp = rc->min_qp;
                    }
                    if (rc->max_qp > 0) {
                        nvCtx->encodeMaxQp = rc->max_qp;
                    }
                    LOG(
                        "updated misc rate control: bitsPerSecond=%u targetPercentage=%u initialQp=%u minQp=%u maxQp=%u",
                        rc->bits_per_second,
                        rc->target_percentage,
                        rc->initial_qp,
                        rc->min_qp,
                        rc->max_qp
                    );
                } else if (misc->type == VAEncMiscParameterTypeFrameRate &&
                           buf->size >= sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterFrameRate)) {
                    VAEncMiscParameterFrameRate *fr = (VAEncMiscParameterFrameRate *) misc->data;
                    uint32_t den = (fr->framerate >> 16) & 0xffff;
                    uint32_t num = fr->framerate & 0xffff;
                    if (den == 0) {
                        den = 1;
                    }
                    if (num > 0) {
                        nvCtx->encodeFrameRateNum = num;
                        nvCtx->encodeFrameRateDen = den;
                    }
                    LOG(
                        "updated misc frame rate: frRaw=0x%x fps=%u/%u",
                        fr->framerate,
                        nvCtx->encodeFrameRateNum,
                        nvCtx->encodeFrameRateDen
                    );
                } else {
                    LOG("misc buffer type=%u not parsed (size=%zu)", misc->type, buf->size);
                }
            } else {
                LOG("buffer type=%d ignored (size=%zu)", buf->bufferType, buf->size);
            }
        }

        LOG(
            "done: codedBuffer=%u forceIDR=%d bitrate=%u target=%u fps=%u/%u initialQp=%u minQp=%u maxQp=%u",
            nvCtx->encodeCodedBuffer,
            nvCtx->encodeForceIDR,
            nvCtx->encodeBitrate,
            nvCtx->encodeTargetPercentage,
            nvCtx->encodeFrameRateNum,
            nvCtx->encodeFrameRateDen,
            nvCtx->encodeInitialQp,
            nvCtx->encodeMinQp,
            nvCtx->encodeMaxQp
        );
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

static VAStatus nvEncodeEndPicture(VADriverContextP ctx, NVContext *nvCtx) {
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVSurface *surface = nvCtx->renderTarget;
    LOG(
        "begin: surface=%p codedBuffer=%u forceIDR=%d frameIdx=%llu sessionInitialized=%d",
        (void *) surface,
        nvCtx->encodeCodedBuffer,
        nvCtx->encodeForceIDR,
        (unsigned long long) nvCtx->encodeFrameIdx,
        nvCtx->encodeSessionInitialized
    );

    if (surface == NULL) {
        LOG("failed: render target surface is NULL");
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (nvCtx->encodeCodedBuffer == VA_INVALID_ID) {
        LOG("failed: coded buffer id is invalid");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    NVBuffer *codedBuffer = (NVBuffer *) getObjectPtr(drv, OBJECT_TYPE_BUFFER, nvCtx->encodeCodedBuffer);
    if (codedBuffer == NULL || codedBuffer->bufferType != VAEncCodedBufferType) {
        LOG(
            "failed: coded buffer lookup failed id=%u buffer=%p type=%d",
            nvCtx->encodeCodedBuffer,
            (void *) codedBuffer,
            codedBuffer == NULL ? -1 : (int) codedBuffer->bufferType
        );
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    clearCodedBuffer(codedBuffer);
    LOG("coded buffer cleared id=%u allocated=%zu", nvCtx->encodeCodedBuffer, codedBuffer->codedDataAllocated);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
    if (!drv->backend->realiseSurface(drv, surface)) {
        LOG("realiseSurface failed for surface=%p", (void *) surface);
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    LOG("surface realized: size=%ux%u format=%d backing=%p", surface->width, surface->height, surface->format, (void *) surface->backingImage);
    if (!nvCtx->encodeSessionInitialized &&
        (nvCtx->width != surface->width || nvCtx->height != surface->height)) {
        LOG(
            "adjusting encode coded size from %ux%u to first surface size %ux%u",
            nvCtx->width,
            nvCtx->height,
            surface->width,
            surface->height
        );
        nvCtx->width = surface->width;
        nvCtx->height = surface->height;
    }

    if (!initEncodeSession(nvCtx)) {
        LOG("initEncodeSession failed");
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!copySurfaceToEncodeInputBuffer(nvCtx, surface)) {
        LOG("copySurfaceToEncodeInputBuffer failed");
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
    LOG("surface copy finished, entering NVENC map/encode");

    const uint32_t negotiatedApiVersion =
        nvCtx->encodeNvencApiVersion != 0 ? nvCtx->encodeNvencApiVersion : NVENCAPI_VERSION;

    NV_ENC_MAP_INPUT_RESOURCE mapInput = {0};
    mapInput.version = nvencStructVersionForApi(NV_ENC_MAP_INPUT_RESOURCE_VER, negotiatedApiVersion);
    mapInput.registeredResource = nvCtx->encodeRegisteredInput;
    if (!checkNvencStatus(nvCtx->encodeApi.nvEncMapInputResource(nvCtx->encoder, &mapInput), "nvEncMapInputResource")) {
        LOG("nvEncMapInputResource failed");
        return VA_STATUS_ERROR_ENCODING_ERROR;
    }
    noteNvencMappedCreate(mapInput.mappedResource);
    LOG("mapped input resource mappedResource=%p", mapInput.mappedResource);

    NV_ENC_PIC_PARAMS picParams = {0};
    picParams.version = nvencStructVersionForApi(NV_ENC_PIC_PARAMS_VER, negotiatedApiVersion);
    picParams.inputWidth = surface->width;
    picParams.inputHeight = surface->height;
    picParams.inputPitch = (uint32_t)nvCtx->encodeInputPitch;
    picParams.inputBuffer = mapInput.mappedResource;
    picParams.outputBitstream = nvCtx->encodeBitstream;
    picParams.inputTimeStamp = nvCtx->encodeFrameIdx++;
    picParams.bufferFmt = nvCtx->encodeInputFormat != NV_ENC_BUFFER_FORMAT_UNDEFINED
        ? nvCtx->encodeInputFormat
        : NV_ENC_BUFFER_FORMAT_NV12;
    picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    const bool useHevcPackedHeaders =
        isHEVCEncodeProfile(nvCtx->profile) &&
        nvCtx->encodePackedHeaders.size > 0;
    if (nvCtx->encodeForceIDR) {
        if (isAV1EncodeProfile(nvCtx->profile)) {
            picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEINTRA | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        } else if (isHEVCEncodeProfile(nvCtx->profile)) {
            picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
            if (!useHevcPackedHeaders) {
                picParams.encodePicFlags |= NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
            }
        } else {
            picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        }
    }
    LOG(
        "submitting encode picture: width=%u height=%u pitch=%u flags=0x%x timestamp=%llu",
        picParams.inputWidth,
        picParams.inputHeight,
        picParams.inputPitch,
        picParams.encodePicFlags,
        (unsigned long long) picParams.inputTimeStamp
    );

    VAStatus status = VA_STATUS_SUCCESS;
    if (!checkNvencStatus(nvCtx->encodeApi.nvEncEncodePicture(nvCtx->encoder, &picParams), "nvEncEncodePicture")) {
        LOG("nvEncEncodePicture failed");
        status = VA_STATUS_ERROR_ENCODING_ERROR;
        goto out_unmap;
    }
    LOG("nvEncEncodePicture succeeded");

    NV_ENC_LOCK_BITSTREAM lockBitstream = {0};
    lockBitstream.version = nvencStructVersionForApi(NV_ENC_LOCK_BITSTREAM_VER, negotiatedApiVersion);
    lockBitstream.outputBitstream = nvCtx->encodeBitstream;
    lockBitstream.doNotWait = 0;

    if (!checkNvencStatus(nvCtx->encodeApi.nvEncLockBitstream(nvCtx->encoder, &lockBitstream), "nvEncLockBitstream")) {
        LOG("nvEncLockBitstream failed");
        status = VA_STATUS_ERROR_ENCODING_ERROR;
        goto out_unmap;
    }
    LOG(
        "bitstream locked: size=%u bufferPtr=%p frameIdx=%u picType=%u",
        lockBitstream.bitstreamSizeInBytes,
        lockBitstream.bitstreamBufferPtr,
        lockBitstream.frameIdx,
        lockBitstream.pictureType
    );
    if (lockBitstream.bitstreamSizeInBytes == 0) {
        LOG(
            "zero-sized bitstream output detected context_id=%d timestamp=%llu frameIdx=%u picType=%u",
            (int) nvCtx->contextId,
            (unsigned long long) picParams.inputTimeStamp,
            lockBitstream.frameIdx,
            lockBitstream.pictureType
        );
    }

    const size_t packedHeaderBytes =
        useHevcPackedHeaders ? nvCtx->encodePackedHeaders.size : 0;
    const size_t totalBitstreamBytes =
        packedHeaderBytes + lockBitstream.bitstreamSizeInBytes;

    if (!ensureCodedDataBuffer(codedBuffer, totalBitstreamBytes)) {
        checkNvencStatus(nvCtx->encodeApi.nvEncUnlockBitstream(nvCtx->encoder, nvCtx->encodeBitstream), "nvEncUnlockBitstream");
        LOG("ensureCodedDataBuffer failed for %zu bytes", totalBitstreamBytes);
        status = VA_STATUS_ERROR_ALLOCATION_FAILED;
        goto out_unmap;
    }

    if (packedHeaderBytes > 0) {
        memcpy(codedBuffer->codedData, nvCtx->encodePackedHeaders.buf, packedHeaderBytes);
    }
    if (lockBitstream.bitstreamSizeInBytes > 0) {
        memcpy(PTROFF(codedBuffer->codedData, packedHeaderBytes),
               lockBitstream.bitstreamBufferPtr,
               lockBitstream.bitstreamSizeInBytes);
    }
    codedBuffer->codedSegment.size = totalBitstreamBytes;
    codedBuffer->codedSegment.bit_offset = 0;
    codedBuffer->codedSegment.status = VA_CODED_BUF_STATUS_SINGLE_NALU;
    codedBuffer->codedSegment.reserved = 0;
    codedBuffer->codedSegment.buf = codedBuffer->codedData;
    codedBuffer->codedSegment.next = NULL;
    LOG(
        "coded segment prepared: size=%u status=0x%x buffer=%p packedHeaders=%zu",
        codedBuffer->codedSegment.size,
        codedBuffer->codedSegment.status,
        codedBuffer->codedSegment.buf,
        packedHeaderBytes
    );

    checkNvencStatus(nvCtx->encodeApi.nvEncUnlockBitstream(nvCtx->encoder, nvCtx->encodeBitstream), "nvEncUnlockBitstream");
    LOG("bitstream unlocked");

out_unmap:
    if (mapInput.mappedResource != NULL) {
        noteNvencMappedDestroy(mapInput.mappedResource);
    }
    checkNvencStatus(nvCtx->encodeApi.nvEncUnmapInputResource(nvCtx->encoder, mapInput.mappedResource), "nvEncUnmapInputResource");
    LOG("input resource unmapped status=%d", status);
    return status;
}

static void scalePlaneNearest(
        const uint8_t *src,
        uint32_t srcWidthPixels,
        uint32_t srcHeightPixels,
        size_t srcStrideBytes,
        uint8_t *dst,
        uint32_t dstWidthPixels,
        uint32_t dstHeightPixels,
        size_t dstStrideBytes,
        uint32_t bytesPerPixel
    )
{
    for (uint32_t y = 0; y < dstHeightPixels; y++) {
        const uint32_t srcY = (uint32_t) (((uint64_t) y * srcHeightPixels) / dstHeightPixels);
        const uint8_t *srcRow = src + ((size_t) srcY * srcStrideBytes);
        uint8_t *dstRow = dst + ((size_t) y * dstStrideBytes);
        for (uint32_t x = 0; x < dstWidthPixels; x++) {
            const uint32_t srcX = (uint32_t) (((uint64_t) x * srcWidthPixels) / dstWidthPixels);
            memcpy(
                dstRow + ((size_t) x * bytesPerPixel),
                srcRow + ((size_t) srcX * bytesPerPixel),
                bytesPerPixel
            );
        }
    }
}

static uint8_t clampToU8(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t) value;
}

static VAStatus nvConvertNv12ToArgbRegion(
        const NVSurface *sourceSurface,
        NVSurface *targetSurface,
        uint32_t srcX,
        uint32_t srcY,
        uint32_t srcWidth,
        uint32_t srcHeight,
        uint32_t dstX,
        uint32_t dstY,
        uint32_t dstWidth,
        uint32_t dstHeight
    )
{
    if ((srcX & 1u) != 0 ||
        (srcY & 1u) != 0 ||
        (srcWidth & 1u) != 0 ||
        (srcHeight & 1u) != 0) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (sourceSurface->backingImage == NULL ||
        targetSurface->backingImage == NULL ||
        sourceSurface->backingImage->arrays[0] == NULL ||
        sourceSurface->backingImage->arrays[1] == NULL ||
        targetSurface->backingImage->arrays[0] == NULL) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    const size_t srcYRowBytes = srcWidth;
    const size_t srcYBytes = srcYRowBytes * srcHeight;
    const size_t srcUVRowBytes = srcWidth;
    const uint32_t srcUVHeight = srcHeight >> 1;
    const size_t srcUVBytes = srcUVRowBytes * srcUVHeight;
    const size_t dstRowBytes = (size_t) dstWidth * 4u;
    const size_t dstBytes = dstRowBytes * dstHeight;

    if ((srcHeight > 0 && srcYBytes / srcHeight != srcYRowBytes) ||
        (srcUVHeight > 0 && srcUVBytes / srcUVHeight != srcUVRowBytes) ||
        (dstHeight > 0 && dstBytes / dstHeight != dstRowBytes)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    uint8_t *srcYHost = (uint8_t *) malloc(srcYBytes);
    uint8_t *srcUVHost = (uint8_t *) malloc(srcUVBytes);
    uint32_t *dstArgbHost = (uint32_t *) malloc(dstBytes);
    if (srcYHost == NULL || srcUVHost == NULL || dstArgbHost == NULL) {
        free(srcYHost);
        free(srcUVHost);
        free(dstArgbHost);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    CUDA_MEMCPY2D copyY = {
        .srcXInBytes = srcX,
        .srcY = srcY,
        .srcMemoryType = CU_MEMORYTYPE_ARRAY,
        .srcArray = sourceSurface->backingImage->arrays[0],
        .dstXInBytes = 0,
        .dstY = 0,
        .dstMemoryType = CU_MEMORYTYPE_HOST,
        .dstHost = srcYHost,
        .dstPitch = srcYRowBytes,
        .WidthInBytes = srcYRowBytes,
        .Height = srcHeight
    };
    if (CHECK_CUDA_RESULT(cu->cuMemcpy2D(&copyY))) {
        free(srcYHost);
        free(srcUVHost);
        free(dstArgbHost);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    CUDA_MEMCPY2D copyUV = {
        .srcXInBytes = srcX,
        .srcY = srcY >> 1,
        .srcMemoryType = CU_MEMORYTYPE_ARRAY,
        .srcArray = sourceSurface->backingImage->arrays[1],
        .dstXInBytes = 0,
        .dstY = 0,
        .dstMemoryType = CU_MEMORYTYPE_HOST,
        .dstHost = srcUVHost,
        .dstPitch = srcUVRowBytes,
        .WidthInBytes = srcUVRowBytes,
        .Height = srcUVHeight
    };
    if (CHECK_CUDA_RESULT(cu->cuMemcpy2D(&copyUV))) {
        free(srcYHost);
        free(srcUVHost);
        free(dstArgbHost);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    for (uint32_t y = 0; y < dstHeight; y++) {
        const uint32_t srcSampleY = (uint32_t) (((uint64_t) y * srcHeight) / dstHeight);
        const uint8_t *srcYRow = srcYHost + ((size_t) srcSampleY * srcYRowBytes);
        const uint8_t *srcUVRow = srcUVHost + ((size_t) (srcSampleY >> 1) * srcUVRowBytes);
        uint32_t *dstRow = dstArgbHost + ((size_t) y * dstWidth);

        for (uint32_t x = 0; x < dstWidth; x++) {
            const uint32_t srcSampleX = (uint32_t) (((uint64_t) x * srcWidth) / dstWidth);
            const uint8_t ySample = srcYRow[srcSampleX];
            const uint32_t uvIndex = srcSampleX & ~1u;
            const uint8_t uSample = srcUVRow[uvIndex];
            const uint8_t vSample = srcUVRow[uvIndex + 1];

            const int c = ySample > 16 ? (int) ySample - 16 : 0;
            const int d = (int) uSample - 128;
            const int e = (int) vSample - 128;
            const uint8_t r = clampToU8((298 * c + 409 * e + 128) >> 8);
            const uint8_t g = clampToU8((298 * c - 100 * d - 208 * e + 128) >> 8);
            const uint8_t b = clampToU8((298 * c + 516 * d + 128) >> 8);

            dstRow[x] = 0xFF000000u | ((uint32_t) r << 16) | ((uint32_t) g << 8) | (uint32_t) b;
        }
    }

    CUDA_MEMCPY2D upload = {
        .srcXInBytes = 0,
        .srcY = 0,
        .srcMemoryType = CU_MEMORYTYPE_HOST,
        .srcHost = dstArgbHost,
        .srcPitch = dstRowBytes,
        .dstXInBytes = dstX * 4u,
        .dstY = dstY,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = targetSurface->backingImage->arrays[0],
        .WidthInBytes = dstRowBytes,
        .Height = dstHeight
    };
    if (CHECK_CUDA_RESULT(cu->cuMemcpy2D(&upload))) {
        free(srcYHost);
        free(srcUVHost);
        free(dstArgbHost);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    free(srcYHost);
    free(srcUVHost);
    free(dstArgbHost);
    return VA_STATUS_SUCCESS;
}

static VAStatus nvCopyVideoProcSurfaceRegion(
        NVDriver *drv,
        const NVSurface *sourceSurface,
        NVSurface *targetSurface,
        const VARectangle *sourceRegion,
        const VARectangle *targetRegion
    )
{
    if (sourceSurface == NULL || targetSurface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    uint32_t srcX = 0;
    uint32_t srcY = 0;
    uint32_t srcWidth = sourceSurface->width;
    uint32_t srcHeight = sourceSurface->height;
    uint32_t dstX = 0;
    uint32_t dstY = 0;
    uint32_t dstWidth = targetSurface->width;
    uint32_t dstHeight = targetSurface->height;

    if (sourceRegion != NULL) {
        srcX = (uint32_t) sourceRegion->x;
        srcY = (uint32_t) sourceRegion->y;
        srcWidth = sourceRegion->width;
        srcHeight = sourceRegion->height;
    }
    if (targetRegion != NULL) {
        dstX = (uint32_t) targetRegion->x;
        dstY = (uint32_t) targetRegion->y;
        dstWidth = targetRegion->width;
        dstHeight = targetRegion->height;
    }

    if (srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0) {
        return VA_STATUS_SUCCESS;
    }
    if (((uint64_t) srcX + srcWidth) > sourceSurface->width ||
        ((uint64_t) srcY + srcHeight) > sourceSurface->height ||
        ((uint64_t) dstX + dstWidth) > targetSurface->width ||
        ((uint64_t) dstY + dstHeight) > targetSurface->height) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    const NVFormat sourceFormat = nvFormatFromSurface(sourceSurface);
    const NVFormat targetFormat = nvFormatFromSurface(targetSurface);
    if (sourceFormat == NV_FORMAT_NV12 && targetFormat == NV_FORMAT_ARGB) {
        LOG(
            "vpp copy format convert NV12->ARGB src=[%u,%u %ux%u] dst=[%u,%u %ux%u]",
            srcX,
            srcY,
            srcWidth,
            srcHeight,
            dstX,
            dstY,
            dstWidth,
            dstHeight
        );
        return nvConvertNv12ToArgbRegion(
            sourceSurface,
            targetSurface,
            srcX,
            srcY,
            srcWidth,
            srcHeight,
            dstX,
            dstY,
            dstWidth,
            dstHeight
        );
    }
    if (sourceFormat == NV_FORMAT_NONE ||
        targetFormat == NV_FORMAT_NONE ||
        sourceFormat != targetFormat) {
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }

    const bool scalingRequired = srcWidth != dstWidth || srcHeight != dstHeight;
    const NVFormatInfo *fmtInfo = &formatsInfo[sourceFormat];
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (sourceSurface->backingImage->arrays[i] == NULL ||
            targetSurface->backingImage->arrays[i] == NULL) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }

        const NVFormatPlane *plane = &fmtInfo->plane[i];
        const uint32_t xAlign = 1u << plane->ss.x;
        const uint32_t yAlign = 1u << plane->ss.y;
        if ((srcX % xAlign) != 0 ||
            (srcY % yAlign) != 0 ||
            (dstX % xAlign) != 0 ||
            (dstY % yAlign) != 0 ||
            (srcWidth % xAlign) != 0 ||
            (srcHeight % yAlign) != 0 ||
            (dstWidth % xAlign) != 0 ||
            (dstHeight % yAlign) != 0) {
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }

        const uint32_t bytesPerPixel = fmtInfo->bppc * plane->channelCount;
        const uint32_t srcPlaneWidthPixels = sourceSurface->width >> plane->ss.x;
        const uint32_t srcPlaneHeightPixels = sourceSurface->height >> plane->ss.y;
        const uint32_t dstPlaneWidthPixels = targetSurface->width >> plane->ss.x;
        const uint32_t dstPlaneHeightPixels = targetSurface->height >> plane->ss.y;
        const uint32_t srcRegionWidthPixels = srcWidth >> plane->ss.x;
        const uint32_t srcRegionHeightPixels = srcHeight >> plane->ss.y;
        const uint32_t dstRegionWidthPixels = dstWidth >> plane->ss.x;
        const uint32_t dstRegionHeightPixels = dstHeight >> plane->ss.y;

        const uint32_t srcPlaneXBytes = (srcX >> plane->ss.x) * bytesPerPixel;
        const uint32_t srcPlaneY = srcY >> plane->ss.y;
        const uint32_t dstPlaneXBytes = (dstX >> plane->ss.x) * bytesPerPixel;
        const uint32_t dstPlaneY = dstY >> plane->ss.y;
        const size_t srcRowBytes = (size_t) srcRegionWidthPixels * bytesPerPixel;
        const size_t dstRowBytes = (size_t) dstRegionWidthPixels * bytesPerPixel;

        if (srcPlaneXBytes + srcRowBytes > ((size_t) srcPlaneWidthPixels * bytesPerPixel) ||
            srcPlaneY + srcRegionHeightPixels > srcPlaneHeightPixels ||
            dstPlaneXBytes + dstRowBytes > ((size_t) dstPlaneWidthPixels * bytesPerPixel) ||
            dstPlaneY + dstRegionHeightPixels > dstPlaneHeightPixels) {
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }

        if (!scalingRequired) {
            CUDA_MEMCPY2D copy = {
                .srcXInBytes = srcPlaneXBytes,
                .srcY = srcPlaneY,
                .srcMemoryType = CU_MEMORYTYPE_ARRAY,
                .srcArray = sourceSurface->backingImage->arrays[i],
                .dstXInBytes = dstPlaneXBytes,
                .dstY = dstPlaneY,
                .dstMemoryType = CU_MEMORYTYPE_ARRAY,
                .dstArray = targetSurface->backingImage->arrays[i],
                .WidthInBytes = srcRowBytes,
                .Height = srcRegionHeightPixels
            };
            if (CHECK_CUDA_RESULT(cu->cuMemcpy2D(&copy))) {
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            continue;
        }

        const size_t srcBufferSize = srcRowBytes * srcRegionHeightPixels;
        const size_t dstBufferSize = dstRowBytes * dstRegionHeightPixels;
        if ((srcRegionHeightPixels > 0 && srcBufferSize / srcRegionHeightPixels != srcRowBytes) ||
            (dstRegionHeightPixels > 0 && dstBufferSize / dstRegionHeightPixels != dstRowBytes)) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }

        uint8_t *srcHost = (uint8_t *) malloc(srcBufferSize);
        uint8_t *dstHost = (uint8_t *) malloc(dstBufferSize);
        if (srcHost == NULL || dstHost == NULL) {
            free(srcHost);
            free(dstHost);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }

        CUDA_MEMCPY2D download = {
            .srcXInBytes = srcPlaneXBytes,
            .srcY = srcPlaneY,
            .srcMemoryType = CU_MEMORYTYPE_ARRAY,
            .srcArray = sourceSurface->backingImage->arrays[i],
            .dstXInBytes = 0,
            .dstY = 0,
            .dstMemoryType = CU_MEMORYTYPE_HOST,
            .dstHost = srcHost,
            .dstPitch = srcRowBytes,
            .WidthInBytes = srcRowBytes,
            .Height = srcRegionHeightPixels
        };
        if (CHECK_CUDA_RESULT(cu->cuMemcpy2D(&download))) {
            free(srcHost);
            free(dstHost);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        scalePlaneNearest(
            srcHost,
            srcRegionWidthPixels,
            srcRegionHeightPixels,
            srcRowBytes,
            dstHost,
            dstRegionWidthPixels,
            dstRegionHeightPixels,
            dstRowBytes,
            bytesPerPixel
        );

        CUDA_MEMCPY2D upload = {
            .srcXInBytes = 0,
            .srcY = 0,
            .srcMemoryType = CU_MEMORYTYPE_HOST,
            .srcHost = dstHost,
            .srcPitch = dstRowBytes,
            .dstXInBytes = dstPlaneXBytes,
            .dstY = dstPlaneY,
            .dstMemoryType = CU_MEMORYTYPE_ARRAY,
            .dstArray = targetSurface->backingImage->arrays[i],
            .WidthInBytes = dstRowBytes,
            .Height = dstRegionHeightPixels
        };
        if (CHECK_CUDA_RESULT(cu->cuMemcpy2D(&upload))) {
            free(srcHost);
            free(dstHost);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        free(srcHost);
        free(dstHost);
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvVideoProcEndPicture(VADriverContextP ctx, NVContext *nvCtx) {
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVSurface *targetSurface = nvCtx->renderTarget;
    if (targetSurface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (!nvCtx->vppPipelineSet) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    VAProcPipelineParameterBuffer *pipeline = &nvCtx->vppPipeline;
    if (pipeline->surface == VA_INVALID_ID) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (pipeline->num_filters != 0 ||
        pipeline->num_forward_references != 0 ||
        pipeline->num_backward_references != 0 ||
        pipeline->num_additional_outputs != 0 ||
        pipeline->rotation_state != VA_ROTATION_NONE ||
        pipeline->mirror_state != VA_MIRROR_NONE ||
        pipeline->blend_state != NULL ||
        pipeline->processing_mode != VAProcDefaultMode ||
        pipeline->output_hdr_metadata != NULL) {
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }

    NVSurface *sourceSurface = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, pipeline->surface);
    if (sourceSurface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    LOG(
        "vpp end picture: src_surface=%u dst_surface=%p src_size=%ux%u dst_size=%ux%u src_rect=%s[%d,%d %ux%u] dst_rect=%s[%d,%d %ux%u]",
        pipeline->surface,
        (void *) targetSurface,
        sourceSurface->width,
        sourceSurface->height,
        targetSurface->width,
        targetSurface->height,
        pipeline->surface_region != NULL ? "" : "null",
        pipeline->surface_region != NULL ? pipeline->surface_region->x : 0,
        pipeline->surface_region != NULL ? pipeline->surface_region->y : 0,
        pipeline->surface_region != NULL ? pipeline->surface_region->width : 0,
        pipeline->surface_region != NULL ? pipeline->surface_region->height : 0,
        pipeline->output_region != NULL ? "" : "null",
        pipeline->output_region != NULL ? pipeline->output_region->x : 0,
        pipeline->output_region != NULL ? pipeline->output_region->y : 0,
        pipeline->output_region != NULL ? pipeline->output_region->width : 0,
        pipeline->output_region != NULL ? pipeline->output_region->height : 0
    );

    VAStatus syncStatus = nvSyncSurface(ctx, pipeline->surface);
    if (syncStatus != VA_STATUS_SUCCESS) {
        return syncStatus;
    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    VAStatus status = VA_STATUS_SUCCESS;
    if (!drv->backend->realiseSurface(drv, sourceSurface) ||
        !drv->backend->realiseSurface(drv, targetSurface)) {
        status = VA_STATUS_ERROR_ALLOCATION_FAILED;
        goto out;
    }

    status = nvCopyVideoProcSurfaceRegion(
        drv,
        sourceSurface,
        targetSurface,
        pipeline->surface_region,
        pipeline->output_region
    );
    if (status == VA_STATUS_SUCCESS) {
        targetSurface->context = nvCtx;
        targetSurface->decodeFailed = false;
    }
    nvCtx->vppPipelineSet = false;

out:
    if (CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL))) {
        status = VA_STATUS_ERROR_OPERATION_FAILED;
    }
    LOG("vpp end picture status=%d", status);
    return status;
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

    if (isVideoProcEntrypoint(nvCtx->entrypoint)) {
        return nvVideoProcEndPicture(ctx, nvCtx);
    }

    if (isEncodeEntrypoint(nvCtx->entrypoint)) {
        VAStatus status = nvEncodeEndPicture(ctx, nvCtx);
        LOG("encode completed with status=%d", status);
        return status;
    }

    if (nvCtx->decoder == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    CUVIDPICPARAMS *picParams = &nvCtx->pPicParams;
    const unsigned long long decodeSubmitStartUs = nv_getmonotonic_us();

    LOG(
        "[DECODE] EndPicture submit ctx=%u currPicIdx=%d bitstream_size=%llu slice_offsets_size=%llu",
        context,
        picParams->CurrPicIdx,
        (unsigned long long)nvCtx->bitstreamBuffer.size,
        (unsigned long long)nvCtx->sliceOffsets.size
    );

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
        LOG(
            "[DECODE] cuvidDecodePicture failed currPicIdx=%d cuerr=%d elapsed_us=%llu",
            picParams->CurrPicIdx,
            result,
            nv_getmonotonic_us() - decodeSubmitStartUs
        );
        status = VA_STATUS_ERROR_DECODING_ERROR;
    } else {
        LOG(
            "[DECODE] cuvidDecodePicture success currPicIdx=%d elapsed_us=%llu",
            picParams->CurrPicIdx,
            nv_getmonotonic_us() - decodeSubmitStartUs
        );
    }
    //LOG("Decoded frame successfully to idx: %d (%p)", picParams->CurrPicIdx, nvCtx->renderTarget);

    NVSurface *surface = nvCtx->renderTarget;

    surface->context = nvCtx;
    surface->topFieldFirst = !picParams->bottom_field_flag;
    surface->secondField = picParams->second_field;
    surface->decodeFailed = status != VA_STATUS_SUCCESS;

    //TODO check we're not overflowing the queue
    pthread_mutex_lock(&nvCtx->resolveMutex);
    const int queueWriteIdxBefore = nvCtx->surfaceQueueWriteIdx;
    const int queueReadIdxSnapshot = nvCtx->surfaceQueueReadIdx;
    nvCtx->surfaceQueue[nvCtx->surfaceQueueWriteIdx++] = nvCtx->renderTarget;
    if (nvCtx->surfaceQueueWriteIdx >= SURFACE_QUEUE_SIZE) {
        nvCtx->surfaceQueueWriteIdx = 0;
    }
    const int queueWriteIdxAfter = nvCtx->surfaceQueueWriteIdx;
    pthread_mutex_unlock(&nvCtx->resolveMutex);
    LOG(
        "[DECODE] queue push pictureIdx=%d decode_failed=%d read_idx=%d write_idx_before=%d write_idx_after=%d",
        surface != NULL ? surface->pictureIdx : -1,
        surface != NULL ? surface->decodeFailed : 1,
        queueReadIdxSnapshot,
        queueWriteIdxBefore,
        queueWriteIdxAfter
    );

    //Wake up the resolve thread
    pthread_cond_signal(&nvCtx->resolveCondition);
    LOG(
        "[DECODE] signaled resolve condition pictureIdx=%d",
        surface != NULL ? surface->pictureIdx : -1
    );

    return status;
}

static VAStatus nvSyncSurface(
        VADriverContextP ctx,
        VASurfaceID render_target
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVSurface *surface = getObjectPtr(drv, OBJECT_TYPE_SURFACE, render_target);
    const unsigned long long syncStartUs = nv_getmonotonic_us();

    if (surface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    LOG(
        "[DECODE] SyncSurface begin surface_id=%u pictureIdx=%d resolving=%d decode_failed=%d",
        render_target,
        surface->pictureIdx,
        surface->resolving,
        surface->decodeFailed
    );

    //wait for resolve to occur before synchronising
    pthread_mutex_lock(&surface->mutex);
    if (surface->resolving) {
        LOG("[DECODE] SyncSurface waiting pictureIdx=%d", surface->pictureIdx);
        pthread_cond_wait(&surface->cond, &surface->mutex);
        LOG("[DECODE] SyncSurface wake pictureIdx=%d", surface->pictureIdx);
    }
    pthread_mutex_unlock(&surface->mutex);

    LOG(
        "[DECODE] SyncSurface done pictureIdx=%d elapsed_us=%llu resolving=%d decode_failed=%d",
        surface->pictureIdx,
        nv_getmonotonic_us() - syncStartUs,
        surface->resolving,
        surface->decodeFailed
    );

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
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQueryImageFormats(
        VADriverContextP ctx,
        VAImageFormat *format_list,        /* out */
        int *num_formats           /* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    *num_formats = 0;
    for (unsigned int i = NV_FORMAT_NONE + 1; i < ARRAY_SIZE(formatsInfo); i++) {
        if (formatsInfo[i].numPlanes == 0) {
            continue;
        }
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
    noteVaImageCreate(imageBuffer->size, image->image_id);

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
    for (uint32_t i = 0; i < ARRAY_SIZE(image->pitches); i++) {
        image->pitches[i] = 0;
    }
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        image->pitches[i] =
            (width >> p[i].ss.x) * fmtInfo->bppc * p[i].channelCount;
    }
    /*
     * An array indicating the byte offset from the beginning of the image data
     * to the start of each plane.
     */
    for (uint32_t i = 0; i < ARRAY_SIZE(image->offsets); i++) {
        image->offsets[i] = 0;
    }
    size_t runningOffset = 0;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        image->offsets[i] = runningOffset;
        runningOffset +=
            ((width * height) >> (p[i].ss.x + p[i].ss.y)) *
            fmtInfo->bppc * p[i].channelCount;
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvDeriveImage(
        VADriverContextP ctx,
        VASurfaceID surface,
        VAImage *image     /* out */
    )
{
    // FAILED because we don't support it.
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
        const size_t imageBufferSize = img->imageBuffer != NULL ? img->imageBuffer->size : 0;
        noteVaImageDestroy(imageBufferSize, image);
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

    //wait for the surface to be decoded when bound to decode context
    if (context != NULL) {
        nvSyncSurface(ctx, surface);
    }
    if (surfaceObj->backingImage == NULL && !drv->backend->realiseSurface(drv, surfaceObj)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (surfaceObj->backingImage == NULL) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

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
        .dstPitch = (width >> p->ss.x) * fmtInfo->bppc * p->channelCount,

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

static bool canPutImageToSurfaceFormat(const NVSurface *surfaceObj, NVFormat imageFormat) {
    if (surfaceObj == NULL) {
        return false;
    }

    switch (imageFormat) {
    case NV_FORMAT_NV12:
        return surfaceObj->format == cudaVideoSurfaceFormat_NV12;
#if NVENCAPI_MAJOR_VERSION >= 13
    case NV_FORMAT_NV16:
        return surfaceObj->format == cudaVideoSurfaceFormat_NV16;
#endif
    case NV_FORMAT_P010:
    case NV_FORMAT_P210:
    case NV_FORMAT_P012:
    case NV_FORMAT_P016:
#if NVENCAPI_MAJOR_VERSION >= 13
        return surfaceObj->format == cudaVideoSurfaceFormat_P016 ||
               surfaceObj->format == cudaVideoSurfaceFormat_P216;
#else
        return surfaceObj->format == cudaVideoSurfaceFormat_P016;
#endif
    case NV_FORMAT_444P:
        return surfaceObj->format == cudaVideoSurfaceFormat_YUV444;
    case NV_FORMAT_Q416:
        return surfaceObj->format == cudaVideoSurfaceFormat_YUV444_16Bit;
    case NV_FORMAT_ARGB:
        return surfaceObj->rtFormat == VA_RT_FORMAT_RGB32;
    default:
        return false;
    }
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
    const unsigned long long startUs = nv_getmonotonic_us();
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVSurface *surfaceObj = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surface);
    NVImage *imageObj = (NVImage*) getObjectPtr(drv, OBJECT_TYPE_IMAGE, image);

    if (surfaceObj == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (imageObj == NULL) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }
    if (imageObj->imageBuffer == NULL || imageObj->imageBuffer->ptr == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (src_width == 0 || src_height == 0 || dest_width == 0 || dest_height == 0) {
        return VA_STATUS_SUCCESS;
    }
    if (src_width != dest_width || src_height != dest_height) {
        LOG(
            "nvPutImage scaling is unsupported (src=%ux%u dst=%ux%u)",
            src_width,
            src_height,
            dest_width,
            dest_height
        );
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    if (src_x < 0 || src_y < 0 || dest_x < 0 || dest_y < 0) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if ((uint32_t)src_x + src_width > imageObj->width ||
        (uint32_t)src_y + src_height > imageObj->height) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if ((uint32_t)dest_x + dest_width > surfaceObj->width ||
        (uint32_t)dest_y + dest_height > surfaceObj->height) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (!canPutImageToSurfaceFormat(surfaceObj, imageObj->format)) {
        LOG(
            "nvPutImage format mismatch (image_format=%d surface_format=%d)",
            imageObj->format,
            surfaceObj->format
        );
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    LOG(
        "nvPutImage copy begin surface=%u image=%u src=%d,%d %ux%u dst=%d,%d %ux%u",
        surface,
        image,
        src_x,
        src_y,
        src_width,
        src_height,
        dest_x,
        dest_y,
        dest_width,
        dest_height
    );

    const NVFormatInfo *fmtInfo = &formatsInfo[imageObj->format];
    const NVFormatPlane *planes = fmtInfo->plane;
    uint32_t imagePlaneOffsets[3] = {0};
    size_t runningOffset = 0;

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        imagePlaneOffsets[i] = (uint32_t)runningOffset;
        runningOffset +=
            ((imageObj->width * imageObj->height) >> (planes[i].ss.x + planes[i].ss.y)) *
            fmtInfo->bppc * planes[i].channelCount;
    }
    if (runningOffset > imageObj->imageBuffer->size) {
        LOG(
            "nvPutImage image buffer too small (required=%zu allocated=%zu)",
            runningOffset,
            imageObj->imageBuffer->size
        );
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    CUcontext currentCtx = NULL;
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxGetCurrent(&currentCtx), VA_STATUS_ERROR_OPERATION_FAILED);
    bool pushedCtx = false;
    if (currentCtx != drv->cudaContext) {
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
        pushedCtx = true;
    }

    VAStatus status = VA_STATUS_SUCCESS;
    if (!drv->backend->realiseSurface(drv, surfaceObj)) {
        status = VA_STATUS_ERROR_ALLOCATION_FAILED;
        goto out;
    }
    if (surfaceObj->backingImage == NULL) {
        status = VA_STATUS_ERROR_ALLOCATION_FAILED;
        goto out;
    }

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *plane = &planes[i];
        const uint32_t xAlign = 1u << plane->ss.x;
        const uint32_t yAlign = 1u << plane->ss.y;

        if (((uint32_t)src_x % xAlign) != 0 ||
            ((uint32_t)src_y % yAlign) != 0 ||
            ((uint32_t)dest_x % xAlign) != 0 ||
            ((uint32_t)dest_y % yAlign) != 0 ||
            (src_width % xAlign) != 0 ||
            (src_height % yAlign) != 0) {
            status = VA_STATUS_ERROR_INVALID_PARAMETER;
            goto out;
        }

        const uint32_t srcPlanePitchBytes =
            (imageObj->width >> plane->ss.x) * fmtInfo->bppc * plane->channelCount;
        const uint32_t srcPlaneWidthBytes =
            (imageObj->width >> plane->ss.x) * fmtInfo->bppc * plane->channelCount;
        const uint32_t srcPlaneHeight = imageObj->height >> plane->ss.y;

        const uint32_t dstPlaneWidthBytes =
            (surfaceObj->width >> plane->ss.x) * fmtInfo->bppc * plane->channelCount;
        const uint32_t dstPlaneHeight = surfaceObj->height >> plane->ss.y;

        const uint32_t srcPlaneXBytes =
            ((uint32_t)src_x >> plane->ss.x) * fmtInfo->bppc * plane->channelCount;
        const uint32_t srcPlaneY = (uint32_t)src_y >> plane->ss.y;
        const uint32_t dstPlaneXBytes =
            ((uint32_t)dest_x >> plane->ss.x) * fmtInfo->bppc * plane->channelCount;
        const uint32_t dstPlaneY = (uint32_t)dest_y >> plane->ss.y;

        const uint32_t copyWidthBytes =
            (src_width >> plane->ss.x) * fmtInfo->bppc * plane->channelCount;
        const uint32_t copyHeight = src_height >> plane->ss.y;

        if (srcPlaneXBytes + copyWidthBytes > srcPlaneWidthBytes ||
            srcPlaneY + copyHeight > srcPlaneHeight ||
            dstPlaneXBytes + copyWidthBytes > dstPlaneWidthBytes ||
            dstPlaneY + copyHeight > dstPlaneHeight) {
            status = VA_STATUS_ERROR_INVALID_PARAMETER;
            goto out;
        }

        CUDA_MEMCPY2D copy = {
            .srcXInBytes = 0,
            .srcY = 0,
            .srcMemoryType = CU_MEMORYTYPE_HOST,
            .srcHost = PTROFF(
                imageObj->imageBuffer->ptr,
                imagePlaneOffsets[i] + ((size_t)srcPlaneY * srcPlanePitchBytes) + srcPlaneXBytes
            ),
            .srcPitch = srcPlanePitchBytes,
            .dstXInBytes = dstPlaneXBytes,
            .dstY = dstPlaneY,
            .dstMemoryType = CU_MEMORYTYPE_ARRAY,
            .dstArray = surfaceObj->backingImage->arrays[i],
            .WidthInBytes = copyWidthBytes,
            .Height = copyHeight
        };

        if (CHECK_CUDA_RESULT(cu->cuMemcpy2D(&copy))) {
            status = VA_STATUS_ERROR_OPERATION_FAILED;
            goto out;
        }
    }
    LOG(
        "nvPutImage copy success surface=%u image=%u copied=%ux%u",
        surface,
        image,
        src_width,
        src_height
    );

out:
    if (pushedCtx && CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL))) {
        status = VA_STATUS_ERROR_OPERATION_FAILED;
    }
    LOG(
        "nvPutImage done status=%d elapsed_us=%llu pushed_ctx=%s",
        status,
        nv_getmonotonic_us() - startUs,
        pushedCtx ? "yes" : "no"
    );
    return status;
}

static VAStatus nvQuerySubpictureFormats(
        VADriverContextP ctx,
        VAImageFormat *format_list,        /* out */
        unsigned int *flags,       /* out */
        unsigned int *num_formats  /* out */
    )
{
    *num_formats = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateSubpicture(
        VADriverContextP ctx,
        VAImageID image,
        VASubpictureID *subpicture   /* out */
    )
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvDestroySubpicture(
        VADriverContextP ctx,
        VASubpictureID subpicture
    )
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetSubpictureImage(
                VADriverContextP ctx,
                VASubpictureID subpicture,
                VAImageID image
        )
{
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
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetSubpictureGlobalAlpha(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        float global_alpha
    )
{
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
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvDeassociateSubpicture(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        VASurfaceID *target_surfaces,
        int num_surfaces
    )
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQueryDisplayAttributes(
        VADriverContextP ctx,
        VADisplayAttribute *attr_list,	/* out */
        int *num_attributes		/* out */
        )
{
    *num_attributes = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus nvGetDisplayAttributes(
        VADriverContextP ctx,
        VADisplayAttribute *attr_list,	/* in/out */
        int num_attributes
        )
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetDisplayAttributes(
        VADriverContextP ctx,
                VADisplayAttribute *attr_list,
                int num_attributes
        )
{
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

    if (isVideoProcEntrypoint(cfg->entrypoint)) {
        unsigned int pixelFormatCount = 1; // NV12
#if NVENCAPI_MAJOR_VERSION >= 13
        pixelFormatCount += 1; // NV16
#endif
        if (drv->supports16BitSurface) {
            pixelFormatCount += 3; // P010, P012, P016
#if NVENCAPI_MAJOR_VERSION >= 13
            pixelFormatCount += 1; // P210
#endif
        }
        if (drv->supports444Surface) {
            pixelFormatCount += 1; // 444P
#if VA_CHECK_VERSION(1, 20, 0)
            if (drv->supports16BitSurface) {
                pixelFormatCount += 1; // Q416
            }
#endif
        }
        pixelFormatCount += 1; // ARGB

        if (num_attribs != NULL) {
            *num_attribs = 4 + pixelFormatCount;
        }
        if (attrib_list == NULL) {
            return VA_STATUS_SUCCESS;
        }

        unsigned int i = 0;
        attrib_list[i].type = VASurfaceAttribMinWidth;
        attrib_list[i].flags = 0;
        attrib_list[i].value.type = VAGenericValueTypeInteger;
        attrib_list[i].value.value.i = 1;
        i++;

        attrib_list[i].type = VASurfaceAttribMinHeight;
        attrib_list[i].flags = 0;
        attrib_list[i].value.type = VAGenericValueTypeInteger;
        attrib_list[i].value.value.i = 1;
        i++;

        attrib_list[i].type = VASurfaceAttribMaxWidth;
        attrib_list[i].flags = 0;
        attrib_list[i].value.type = VAGenericValueTypeInteger;
        attrib_list[i].value.value.i = 16384;
        i++;

        attrib_list[i].type = VASurfaceAttribMaxHeight;
        attrib_list[i].flags = 0;
        attrib_list[i].value.type = VAGenericValueTypeInteger;
        attrib_list[i].value.value.i = 16384;
        i++;

        attrib_list[i].type = VASurfaceAttribPixelFormat;
        attrib_list[i].flags = 0;
        attrib_list[i].value.type = VAGenericValueTypeInteger;
        attrib_list[i].value.value.i = VA_FOURCC_NV12;
        i++;
#if NVENCAPI_MAJOR_VERSION >= 13
        attrib_list[i].type = VASurfaceAttribPixelFormat;
        attrib_list[i].flags = 0;
        attrib_list[i].value.type = VAGenericValueTypeInteger;
        attrib_list[i].value.value.i = VA_FOURCC_NV16;
        i++;
#endif
        if (drv->supports16BitSurface) {
            attrib_list[i].type = VASurfaceAttribPixelFormat;
            attrib_list[i].flags = 0;
            attrib_list[i].value.type = VAGenericValueTypeInteger;
            attrib_list[i].value.value.i = VA_FOURCC_P010;
            i++;
            attrib_list[i].type = VASurfaceAttribPixelFormat;
            attrib_list[i].flags = 0;
            attrib_list[i].value.type = VAGenericValueTypeInteger;
            attrib_list[i].value.value.i = VA_FOURCC_P012;
            i++;
            attrib_list[i].type = VASurfaceAttribPixelFormat;
            attrib_list[i].flags = 0;
            attrib_list[i].value.type = VAGenericValueTypeInteger;
            attrib_list[i].value.value.i = VA_FOURCC_P016;
            i++;
#if NVENCAPI_MAJOR_VERSION >= 13
            attrib_list[i].type = VASurfaceAttribPixelFormat;
            attrib_list[i].flags = 0;
            attrib_list[i].value.type = VAGenericValueTypeInteger;
            attrib_list[i].value.value.i = VA_FOURCC_P210;
            i++;
#endif
        }
        if (drv->supports444Surface) {
            attrib_list[i].type = VASurfaceAttribPixelFormat;
            attrib_list[i].flags = 0;
            attrib_list[i].value.type = VAGenericValueTypeInteger;
            attrib_list[i].value.value.i = VA_FOURCC_444P;
            i++;
#if VA_CHECK_VERSION(1, 20, 0)
            if (drv->supports16BitSurface) {
                attrib_list[i].type = VASurfaceAttribPixelFormat;
                attrib_list[i].flags = 0;
                attrib_list[i].value.type = VAGenericValueTypeInteger;
                attrib_list[i].value.value.i = VA_FOURCC_Q416;
                i++;
            }
#endif
        }
        attrib_list[i].type = VASurfaceAttribPixelFormat;
        attrib_list[i].flags = 0;
        attrib_list[i].value.type = VAGenericValueTypeInteger;
        attrib_list[i].value.value.i = VA_FOURCC_ARGB;

        return VA_STATUS_SUCCESS;
    }

    //LOG("with %d (%d) %p %d", cfg->cudaCodec, cfg->bitDepth, attrib_list, *num_attribs);

    if (cfg->chromaFormat != cudaVideoChromaFormat_420 &&
        cfg->chromaFormat != cudaVideoChromaFormat_422 &&
        cfg->chromaFormat != cudaVideoChromaFormat_444) {
        LOG("Unknown chrome format: %d", cfg->chromaFormat);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    if ((cfg->chromaFormat == cudaVideoChromaFormat_444 || cfg->surfaceFormat == cudaVideoSurfaceFormat_YUV444_16Bit) && !drv->supports444Surface) {
        //TODO not sure what pixel formats are needed for 422 and 444 formats
        LOG("YUV444 surfaces not supported: %d", cfg->chromaFormat);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    if ((cfg->surfaceFormat == cudaVideoSurfaceFormat_P016
#if NVENCAPI_MAJOR_VERSION >= 13
         || cfg->surfaceFormat == cudaVideoSurfaceFormat_P216
#endif
        ) && !drv->supports16BitSurface) {
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
        } else if (cfg->chromaFormat == cudaVideoChromaFormat_422) {
            cnt += 1;
            if (drv->supports16BitSurface) {
                cnt += 1;
            }
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
        } else if (cfg->chromaFormat == cudaVideoChromaFormat_422) {
            attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
            attrib_list[attrib_idx].flags = 0;
            attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
            attrib_list[attrib_idx].value.value.i = VA_FOURCC_NV16;
            attrib_idx += 1;
            if (drv->supports16BitSurface) {
                attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
                attrib_list[attrib_idx].flags = 0;
                attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
                attrib_list[attrib_idx].value.value.i = VA_FOURCC_P210;
                attrib_idx += 1;
            }
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
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVBuffer *buf = getObjectPtr(drv, OBJECT_TYPE_BUFFER, buf_id);

    if (buf == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (type == NULL || size == NULL || num_elements == NULL) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    *type = buf->bufferType;
    *num_elements = buf->elements > 0 ? buf->elements : 1;
    if (buf->elements > 0) {
        *size = (unsigned int)(buf->size / buf->elements);
    } else {
        *size = (unsigned int)buf->size;
    }

    LOG(
        "BufferInfo id=%u type=%d size=%u num_elements=%u total=%zu",
        buf_id,
        *type,
        *size,
        *num_elements,
        buf->size
    );

    return VA_STATUS_SUCCESS;
}

static VAStatus nvAcquireBufferHandle(
            VADriverContextP    ctx,
            VABufferID          buf_id,         /* in */
            VABufferInfo *      buf_info        /* in/out */
        )
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvReleaseBufferHandle(
            VADriverContextP    ctx,
            VABufferID          buf_id          /* in */
        )
{
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
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvUnlockSurface(
        VADriverContextP ctx,
                VASurfaceID surface
        )
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvCreateMFContext(
            VADriverContextP ctx,
            VAMFContextID *mfe_context    /* out */
        )
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvMFAddContext(
            VADriverContextP ctx,
            VAMFContextID mf_context,
            VAContextID context
        )
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvMFReleaseContext(
            VADriverContextP ctx,
            VAMFContextID mf_context,
            VAContextID context
        )
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvMFSubmit(
            VADriverContextP ctx,
            VAMFContextID mf_context,
            VAContextID *contexts,
            int num_contexts
        )
{
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
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQueryProcessingRate(
            VADriverContextP ctx,               /* in */
            VAConfigID config_id,               /* in */
            VAProcessingRateParameter *proc_buf,/* in */
            unsigned int *processing_rate	/* out */
        )
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static bool nvIsVideoProcContext(const NVContext *nvCtx) {
    return nvCtx != NULL && isVideoProcEntrypoint(nvCtx->entrypoint);
}

static VAStatus nvQueryVideoProcFilters(
            VADriverContextP    ctx,
            VAContextID         context,
            VAProcFilterType   *filters,
            unsigned int       *num_filters
        )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);
    if (!nvIsVideoProcContext(nvCtx)) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (num_filters == NULL) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (filters != NULL && *num_filters > 0) {
        // No optional filters are supported for now.
    }
    *num_filters = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus nvQueryVideoProcFilterCaps(
            VADriverContextP    ctx,
            VAContextID         context,
            VAProcFilterType    type,
            void               *filter_caps,
            unsigned int       *num_filter_caps
        )
{
    (void) type;
    (void) filter_caps;
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);
    if (!nvIsVideoProcContext(nvCtx)) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (num_filter_caps == NULL) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    *num_filter_caps = 0;
    return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
}

static VAStatus nvQueryVideoProcPipelineCaps(
            VADriverContextP    ctx,
            VAContextID         context,
            VABufferID         *filters,
            unsigned int        num_filters,
            VAProcPipelineCaps *pipeline_caps
        )
{
    (void) filters;
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);
    if (!nvIsVideoProcContext(nvCtx)) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (pipeline_caps == NULL) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (num_filters != 0) {
        return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
    }

    pipeline_caps->pipeline_flags = 0;
    pipeline_caps->filter_flags = 0;
    pipeline_caps->num_forward_references = 0;
    pipeline_caps->num_backward_references = 0;
    pipeline_caps->rotation_flags = 1u << VA_ROTATION_NONE;
    pipeline_caps->blend_flags = 0;
    pipeline_caps->mirror_flags = 1u << VA_MIRROR_NONE;
    pipeline_caps->num_additional_outputs = 0;
    pipeline_caps->max_input_width = 16384;
    pipeline_caps->max_input_height = 16384;
    pipeline_caps->min_input_width = 1;
    pipeline_caps->min_input_height = 1;
    pipeline_caps->max_output_width = 16384;
    pipeline_caps->max_output_height = 16384;
    pipeline_caps->min_output_width = 1;
    pipeline_caps->min_output_height = 1;

    pipeline_caps->input_color_standards = NULL;
    pipeline_caps->num_input_color_standards = 0;
    pipeline_caps->output_color_standards = NULL;
    pipeline_caps->num_output_color_standards = 0;
    pipeline_caps->input_pixel_format = NULL;
    pipeline_caps->num_input_pixel_formats = 0;
    pipeline_caps->output_pixel_format = NULL;
    pipeline_caps->num_output_pixel_formats = 0;

    return VA_STATUS_SUCCESS;
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

    LOG(
        "[DECODE] ExportSurfaceHandle begin surface_id=%u pictureIdx=%d mem_type=0x%x flags=0x%x",
        surface_id,
        surface->pictureIdx,
        mem_type,
        flags
    );

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    if (!drv->backend->realiseSurface(drv, surface)) {
        LOG("Unable to export surface");
        LOG("[DECODE] ExportSurfaceHandle failed to realise surface pictureIdx=%d", surface->pictureIdx);
        status = VA_STATUS_ERROR_ALLOCATION_FAILED;
        goto out;
    }

    VADRMPRIMESurfaceDescriptor *ptr = (VADRMPRIMESurfaceDescriptor*) descriptor;

    if (!drv->backend->fillExportDescriptor(drv, surface, ptr)) {
        LOG("[DECODE] ExportSurfaceHandle failed to fill export descriptor pictureIdx=%d", surface->pictureIdx);
        status = VA_STATUS_ERROR_ALLOCATION_FAILED;
        goto out;
    }
    LOG(
        "[DECODE] ExportSurfaceHandle descriptor fourcc=0x%x size=%ux%u objects=%u layers=%u",
        ptr->fourcc,
        ptr->width,
        ptr->height,
        ptr->num_objects,
        ptr->num_layers
    );
    for (uint32_t i = 0; i < ptr->num_layers; i++) {
        const uint32_t objectIndex =
            ptr->layers[i].num_planes > 0 ? ptr->layers[i].object_index[0] : 0;
        const uint64_t modifier =
            (ptr->layers[i].num_planes > 0 && objectIndex < ptr->num_objects)
                ? ptr->objects[objectIndex].drm_format_modifier
                : 0;
        LOG(
            "[DECODE] ExportSurfaceHandle layer[%u] drm_format=0x%x planes=%u obj0=%u pitch0=%u offset0=%u modifier0=0x%llx",
            i,
            ptr->layers[i].drm_format,
            ptr->layers[i].num_planes,
            objectIndex,
            ptr->layers[i].num_planes > 0 ? ptr->layers[i].pitch[0] : 0,
            ptr->layers[i].num_planes > 0 ? ptr->layers[i].offset[0] : 0,
            (unsigned long long)modifier
        );
    }

    // LOG("Exporting with w:%d h:%d o:%d p:%d m:%" PRIx64 " o:%d p:%d m:%" PRIx64, ptr->width, ptr->height, ptr->layers[0].offset[0],
    //                                                             ptr->layers[0].pitch[0], ptr->objects[0].drm_format_modifier,
    //                                                             ptr->layers[1].offset[0], ptr->layers[1].pitch[0],
    //                                                             ptr->objects[1].drm_format_modifier);
out:
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
    return status;
}

static VAStatus nvTerminate( VADriverContextP ctx )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    bool lastInstance = false;
    LOG("Terminating %p", ctx);
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    // Let object teardown detach and destroy owned backings first. Any leftover
    // images are cleaned up by the backend afterwards.
    deleteAllObjects(drv);

    drv->backend->destroyAllBackingImage(drv);

    drv->backend->releaseExporter(drv);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    pthread_mutex_lock(&concurrency_mutex);
    instances--;
    lastInstance = instances == 0;
    LOG("Now have %d (%d max) instances", instances, max_instances);
    pthread_mutex_unlock(&concurrency_mutex);

    if (drv->usesPrimaryCudaContext) {
        CHECK_CUDA_RESULT_RETURN(cu->cuDevicePrimaryCtxRelease(drv->cudaGpuId), VA_STATUS_ERROR_OPERATION_FAILED);
    } else {
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxDestroy(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
    }
    drv->cudaContext = NULL;

    free(drv);

    if (lastInstance) {
        resetProcessTransientState();
        releaseGlobalCodecFunctions();
        LOG("Released global CUDA/NVDEC/NVENC function tables after last instance shutdown");
    }

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

static const struct VADriverVTableVPP vtable_vpp = {
    .version = VA_DRIVER_VTABLE_VPP_VERSION,
    .vaQueryVideoProcFilters = &nvQueryVideoProcFilters,
    .vaQueryVideoProcFilterCaps = &nvQueryVideoProcFilterCaps,
    .vaQueryVideoProcPipelineCaps = &nvQueryVideoProcPipelineCaps,
};

__attribute__((visibility("default")))
VAStatus __vaDriverInit_1_0(VADriverContextP ctx);

__attribute__((visibility("default")))
VAStatus __vaDriverInit_1_0(VADriverContextP ctx) {
    uint64_t backoffRemainingNs = 0;
    const bool backoffActive = cudaInitBackoffActive(&backoffRemainingNs);
    const bool backoffForced = cudaInitBackoffForced();
    const bool disableCudaInitBackoff =
        isTruthyEnv(getenv("NVD_DISABLE_CUDA_INIT_BACKOFF"));
    if (backoffActive && (!disableCudaInitBackoff || backoffForced)) {
        LOG(
            backoffForced
                ? "Skipping VA-API init during forced CUDA init backoff after fatal error (remaining_ms=%llu)"
                : "Skipping VA-API init during CUDA init backoff (remaining_ms=%llu)",
            (unsigned long long) (backoffRemainingNs / 1000000ull)
        );
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (backoffActive && disableCudaInitBackoff) {
        LOG(
            "CUDA init backoff active but disabled by env (remaining_ms=%llu)",
            (unsigned long long) (backoffRemainingNs / 1000000ull)
        );
    }

    LOG("Initialising NVIDIA VA-API Driver");
    bool instance_acquired = false;

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
    instance_acquired = true;
    pthread_mutex_unlock(&concurrency_mutex);

    if (!ensureGlobalCodecFunctionsLoaded()) {
        pthread_mutex_lock(&concurrency_mutex);
        if (instance_acquired) {
            instances--;
            LOG("Init failed before driver creation, rolled back instances to %d (%d max)", instances, max_instances);
        }
        pthread_mutex_unlock(&concurrency_mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    NVDriver *drv = (NVDriver*) calloc(1, sizeof(NVDriver));
    if (drv == NULL) {
        pthread_mutex_lock(&concurrency_mutex);
        if (instance_acquired) {
            instances--;
            LOG("Init failed on calloc, rolled back instances to %d (%d max)", instances, max_instances);
        }
        pthread_mutex_unlock(&concurrency_mutex);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    ctx->pDriverData = drv;

    drv->cu = cu;
    drv->cv = cv;
    drv->nv = nv;
    drv->useCorrectNV12Format = true;
    drv->allowDirectDmabufCudaImport = false;
    drv->allowDmabufExportIoctl = true;
    drv->cudaGpuId = gpu;
    //make sure that we want the default GPU, and that a DRM fd that we care about is passed in
    drv->drmFd = drmFd;

    if (backend == EGL) {
        LOG("Selecting EGL backend");
        drv->backend = &EGL_BACKEND;
    } else if (backend == DIRECT) {
        LOG("Selecting Direct backend");
        drv->backend = &DIRECT_BACKEND;
    }

    ctx->max_profiles = MAX_PROFILES;
    ctx->max_entrypoints = 3;
    ctx->max_attributes = 8;
    ctx->max_display_attributes = 1;
    ctx->max_image_formats = ARRAY_SIZE(formatsInfo) - 1;
    ctx->max_subpic_formats = 1;

    if (backend == DIRECT) {
        ctx->str_vendor = "VA-API NVDEC/NVENC driver [direct backend]";
    } else if (backend == EGL) {
        ctx->str_vendor = "VA-API NVDEC/NVENC driver [egl backend]";
    }

    pthread_mutexattr_t attrib;
    pthread_mutexattr_init(&attrib);
    pthread_mutexattr_settype(&attrib, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&drv->objectCreationMutex, &attrib);
    pthread_mutex_init(&drv->imagesMutex, &attrib);
    pthread_mutex_init(&drv->exportMutex, NULL);

    if (!drv->backend->initExporter(drv)) {
        LOG("Exporter failed");
        pthread_mutex_lock(&concurrency_mutex);
        if (instance_acquired) {
            instances--;
            LOG("Init failed in initExporter, rolled back instances to %d (%d max)", instances, max_instances);
        }
        pthread_mutex_unlock(&concurrency_mutex);
        free(drv);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (!initCudaContext(drv)) {
        drv->backend->releaseExporter(drv);
        pthread_mutex_lock(&concurrency_mutex);
        if (instance_acquired) {
            instances--;
            LOG("Init failed in CUDA context setup, rolled back instances to %d (%d max)", instances, max_instances);
        }
        pthread_mutex_unlock(&concurrency_mutex);
        free(drv);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    probeEncodeSupport(drv);
    LOG(
        "driver init encode support: h264=%d h26410=%d h264444=%d hevc=%d hevc10=%d hevc422=%d hevc444=%d av1=%d av110=%d",
        drv->supportsEncodeH264,
        drv->supportsEncodeH26410Bit,
        drv->supportsEncodeH264444,
        drv->supportsEncodeHEVC,
        drv->supportsEncodeHEVC10Bit,
        drv->supportsEncodeHEVC422,
        drv->supportsEncodeHEVC444,
        drv->supportsEncodeAV1,
        drv->supportsEncodeAV110Bit
    );

    //CHECK_CUDA_RESULT_RETURN(cv->cuvidCtxLockCreate(&drv->vidLock, drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    nvQueryConfigProfiles2(ctx, drv->profiles, &drv->profileCount);

    *ctx->vtable = vtable;
    if (ctx->vtable_vpp != NULL) {
        *ctx->vtable_vpp = vtable_vpp;
    }
    return VA_STATUS_SUCCESS;
}
