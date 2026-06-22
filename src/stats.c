#include "stats.h"
#include "vabackend.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t backingImageStatsSize(const BackingImage *img) {
    if (img == NULL) {
        return 0;
    }
    if (img->totalSize != 0) {
        return img->totalSize;
    }

    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];
    uint64_t size = 0;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        size += img->size[i];
    }
    return size;
}

static void collectBackingImageStats(NVDriver *drv,
                                     uint32_t *activeCount,
                                     uint32_t *detachedCount,
                                     uint32_t *borrowedCount,
                                     uint32_t *externalCount,
                                     uint64_t *activeBytes,
                                     uint64_t *detachedBytes) {
    *activeCount = 0;
    *detachedCount = 0;
    *borrowedCount = 0;
    *externalCount = 0;
    *activeBytes = 0;
    *detachedBytes = 0;

    pthread_mutex_lock(&drv->imagesMutex);
    ARRAY_FOR_EACH(BackingImage*, img, &drv->images)
        const uint64_t imageBytes = backingImageStatsSize(img);
        if (img->surface != NULL) {
            (*activeCount)++;
            *activeBytes += imageBytes;
        } else {
            (*detachedCount)++;
            *detachedBytes += imageBytes;
        }
        if (img->borrowedCudaResources || img->borrowedBackingImage != NULL || atomic_load(&img->borrowCount) > 0) {
            (*borrowedCount)++;
        }
        if (img->isExternalBuffer) {
            (*externalCount)++;
        }
    END_FOR_EACH
    pthread_mutex_unlock(&drv->imagesMutex);
}

void nvStatsLog(NVDriver *drv, const char *reason) {
    if (drv == NULL || !drv->statsEnabled) {
        return;
    }

    FILE *out = nvStatsOutput();
    if (out == NULL) {
        return;
    }

    uint32_t activeBackingImages = 0;
    uint32_t detachedBackingImages = 0;
    uint32_t borrowedBackingImages = 0;
    uint32_t externalBackingImages = 0;
    uint64_t activeBackingBytes = 0;
    uint64_t detachedBackingBytes = 0;
    collectBackingImageStats(drv, &activeBackingImages, &detachedBackingImages,
                             &borrowedBackingImages, &externalBackingImages,
                             &activeBackingBytes, &detachedBackingBytes);

    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    fprintf(out,
        "%10ld.%09ld [%d-%d] Stats[%s]: decoder_creates=%llu decode_pictures=%llu resolve_frames=%llu export_copies=%llu export_host_copies=%llu export_descriptors=%llu single_descriptors=%llu multi_descriptors=%llu videoproc_requests=%llu videoproc_cuda=%llu videoproc_cuda_failures=%llu videoproc_cpu_fallback=%llu active_backing_images=%u detached_backing_images=%u borrowed_backing_images=%u external_backing_images=%u active_backing_bytes=%llu detached_backing_bytes=%llu detached_backing_limit_bytes=%llu detached_backing_limit_images=%u\n",
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
        (unsigned long long) atomic_load_explicit(&drv->stats[NV_STAT_VIDEOPROC_CPU_FALLBACK], memory_order_relaxed),
        activeBackingImages,
        detachedBackingImages,
        borrowedBackingImages,
        externalBackingImages,
        (unsigned long long) activeBackingBytes,
        (unsigned long long) detachedBackingBytes,
        (unsigned long long) drv->maxDetachedBackingImageBytes,
        drv->maxDetachedBackingImages);
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

void nvStatsInit(NVDriver *drv) {
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
}
