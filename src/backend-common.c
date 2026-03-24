#include "vabackend.h"
#include "backend-common.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int memFd;
} NvKmsExportMemoryParamsCompat;

typedef struct {
    pthread_mutex_t mu;
    unsigned char trackedFd[65536];
    uint16_t callerOwnedFdRefcount[65536];
    dev_t callerOwnedFdDev[65536];
    ino_t callerOwnedFdIno[65536];
} BackendFdState;

static BackendFdState gBackendFdState = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
};

static bool backendCallerOwnedFdMatchesLocked(int fd, const struct stat *fdStat) {
    return
        fd >= 0 &&
        fd < (int)ARRAY_SIZE(gBackendFdState.callerOwnedFdRefcount) &&
        gBackendFdState.callerOwnedFdRefcount[fd] != 0 &&
        fdStat != NULL &&
        gBackendFdState.callerOwnedFdDev[fd] == fdStat->st_dev &&
        gBackendFdState.callerOwnedFdIno[fd] == fdStat->st_ino;
}

void backendWatchCallerOwnedFd(int fd, const char *reason) {
    struct stat fdStat = {0};
    if (fd < 0 || fstat(fd, &fdStat) != 0) {
        return;
    }

    pthread_mutex_lock(&gBackendFdState.mu);
    if (fd < (int)ARRAY_SIZE(gBackendFdState.callerOwnedFdRefcount)) {
        if (gBackendFdState.callerOwnedFdRefcount[fd] == 0) {
            gBackendFdState.callerOwnedFdDev[fd] = fdStat.st_dev;
            gBackendFdState.callerOwnedFdIno[fd] = fdStat.st_ino;
        }
        gBackendFdState.callerOwnedFdRefcount[fd]++;
    }
    pthread_mutex_unlock(&gBackendFdState.mu);
    (void) reason;
}

void backendUnwatchCallerOwnedFd(int fd, const char *reason) {
    if (fd < 0) {
        return;
    }

    pthread_mutex_lock(&gBackendFdState.mu);
    if (fd < (int)ARRAY_SIZE(gBackendFdState.callerOwnedFdRefcount) &&
        gBackendFdState.callerOwnedFdRefcount[fd] > 0) {
        gBackendFdState.callerOwnedFdRefcount[fd]--;
        if (gBackendFdState.callerOwnedFdRefcount[fd] == 0) {
            gBackendFdState.callerOwnedFdDev[fd] = 0;
            gBackendFdState.callerOwnedFdIno[fd] = 0;
        }
    }
    pthread_mutex_unlock(&gBackendFdState.mu);
    (void) reason;
}

static void backendTrackNvKmsFdLocked(int fd) {
    if (fd < 0 || fd >= (int)ARRAY_SIZE(gBackendFdState.trackedFd)) {
        return;
    }
    if (gBackendFdState.trackedFd[fd] != 0) {
        return;
    }
    gBackendFdState.trackedFd[fd] = 1;
}

static void backendUntrackNvKmsFdLocked(int fd) {
    if (fd < 0 || fd >= (int)ARRAY_SIZE(gBackendFdState.trackedFd)) {
        return;
    }
    if (gBackendFdState.trackedFd[fd] == 0) {
        return;
    }
    gBackendFdState.trackedFd[fd] = 0;
}

void backendTrackNvKmsFdDup(int srcFd, int dupFd, const char *reason) {
    if (dupFd < 0) {
        return;
    }
    pthread_mutex_lock(&gBackendFdState.mu);
    if (srcFd >= 0 &&
        srcFd < (int)ARRAY_SIZE(gBackendFdState.trackedFd) &&
        gBackendFdState.trackedFd[srcFd] != 0) {
        backendTrackNvKmsFdLocked(dupFd);
    }
    pthread_mutex_unlock(&gBackendFdState.mu);
    (void) reason;
}

void backendCloseTrackedNvKmsFd(int fd, const char *reason) {
    if (fd < 0) {
        return;
    }
    struct stat fdStat = {0};
    const bool statOk = fstat(fd, &fdStat) == 0;
    bool preventClose = false;
    pthread_mutex_lock(&gBackendFdState.mu);
    if (statOk && backendCallerOwnedFdMatchesLocked(fd, &fdStat)) {
        preventClose = true;
    } else if (fd < (int)ARRAY_SIZE(gBackendFdState.trackedFd) &&
        gBackendFdState.trackedFd[fd] != 0) {
        backendUntrackNvKmsFdLocked(fd);
    }
    pthread_mutex_unlock(&gBackendFdState.mu);

    if (preventClose) {
        return;
    }

    (void) reason;
    close(fd);
}

void backendReleaseTrackedNvKmsFd(int fd, const char *reason) {
    if (fd < 0) {
        return;
    }

    pthread_mutex_lock(&gBackendFdState.mu);
    if (fd < (int)ARRAY_SIZE(gBackendFdState.trackedFd) &&
        gBackendFdState.trackedFd[fd] != 0) {
        backendUntrackNvKmsFdLocked(fd);
    }
    pthread_mutex_unlock(&gBackendFdState.mu);
    (void) reason;
}

void backendCloseFd(int fd, const char *reason) {
    backendCloseTrackedNvKmsFd(fd, reason);
}

bool checkModesetParameterFromFd(int fd) {
    if (fd > 0) {
        //this ioctl should fail if modeset=0
        struct drm_get_cap caps = { .capability = DRM_CAP_DUMB_BUFFER };
        int ret = ioctl(fd, DRM_IOCTL_GET_CAP, &caps);
        if (ret != 0) {
            //the modeset parameter is set to 0
            LOG("ERROR: This driver requires the nvidia_drm.modeset kernel module parameter set to 1");
            return false;
        }
        return true;
    }
    return true;
}

bool isNvidiaDrmFd(int fd, bool log) {
    if (fd > 0) {
        char name[16] = {0};
        struct drm_version ver = {
            .name = name,
            .name_len = 15
        };
        int ret = ioctl(fd, DRM_IOCTL_VERSION, &ver);
        if (ret || strncmp(name, "nvidia-drm", 10)) {
            if (log) {
                LOG("Invalid driver for DRM device: %s", ver.name);
            }
            return false;
        }
        return true;
    }
    return false;
}

static int exportGemHandleToNvFd(
    int drmFd,
    int nvctlFallbackFd,
    uint32_t driverMajorVersion,
    uint32_t handle
) {
    static const uint64_t kLegacyParamsSize = 0x20;
    const uint64_t preferredSize =
        driverMajorVersion == 470
            ? kLegacyParamsSize
            : sizeof(NvKmsExportMemoryParamsCompat);
    const uint64_t candidateSizes[] = {
        preferredSize,
        sizeof(NvKmsExportMemoryParamsCompat),
        kLegacyParamsSize
    };
    uint64_t triedSizes[ARRAY_SIZE(candidateSizes)] = {0};
    size_t triedSizeCount = 0;

    if (drmFd < 0) {
        return -1;
    }

    for (size_t sizeIdx = 0; sizeIdx < ARRAY_SIZE(candidateSizes); sizeIdx++) {
        const uint64_t paramsSize = candidateSizes[sizeIdx];
        bool alreadyTried = false;
        for (size_t i = 0; i < triedSizeCount; i++) {
            if (triedSizes[i] == paramsSize) {
                alreadyTried = true;
                break;
            }
        }
        if (alreadyTried) {
            continue;
        }
        triedSizes[triedSizeCount++] = paramsSize;

        // Newer drivers: input memFd must be a valid nvctl fd.
        int inputNvctlFd = open(
            "/dev/nvidiactl",
            O_RDWR
#ifdef O_CLOEXEC
                | O_CLOEXEC
#endif
        );
        if (inputNvctlFd < 0 && nvctlFallbackFd >= 0) {
            inputNvctlFd = dup(nvctlFallbackFd);
        }
        if (inputNvctlFd >= 0) {
            NvKmsExportMemoryParamsCompat exportParams = {
                .memFd = inputNvctlFd
            };
            struct drm_nvidia_gem_export_nvkms_memory_params exportIoctl = {
                .handle = handle,
                .nvkms_params_ptr = (uint64_t)(uintptr_t)&exportParams,
                .nvkms_params_size = paramsSize
            };

            errno = 0;
            int ret = ioctl(
                drmFd,
                DRM_IOCTL_NVIDIA_GEM_EXPORT_NVKMS_MEMORY,
                &exportIoctl
            );
            int savedErrno = errno;
            if (ret == 0 && exportParams.memFd >= 0) {
                if (exportParams.memFd != inputNvctlFd) {
                    backendCloseFd(inputNvctlFd, "gem_export_replace_input_nvctl");
                }
                pthread_mutex_lock(&gBackendFdState.mu);
                backendTrackNvKmsFdLocked(exportParams.memFd);
                pthread_mutex_unlock(&gBackendFdState.mu);
                LOG(
                    "GEM export succeeded handle=%u params_size=%llu memFd=%d mode=input_nvctl",
                    handle,
                    (unsigned long long)paramsSize,
                    exportParams.memFd
                );
                return exportParams.memFd;
            }

            LOG(
                "GEM export failed handle=%u errno=%d ret=%d memFd=%d params_size=%llu mode=input_nvctl",
                handle,
                savedErrno,
                ret,
                exportParams.memFd,
                (unsigned long long)paramsSize
            );
            backendCloseFd(inputNvctlFd, "gem_export_failed_input_nvctl");
        }

        // Older drivers: memFd can be returned as output when set to -1.
        NvKmsExportMemoryParamsCompat exportParams = {
            .memFd = -1
        };
        struct drm_nvidia_gem_export_nvkms_memory_params exportIoctl = {
            .handle = handle,
            .nvkms_params_ptr = (uint64_t)(uintptr_t)&exportParams,
            .nvkms_params_size = paramsSize
        };

        errno = 0;
        int ret = ioctl(
            drmFd,
            DRM_IOCTL_NVIDIA_GEM_EXPORT_NVKMS_MEMORY,
            &exportIoctl
        );
        int savedErrno = errno;
        if (ret == 0 && exportParams.memFd >= 0) {
            pthread_mutex_lock(&gBackendFdState.mu);
            backendTrackNvKmsFdLocked(exportParams.memFd);
            pthread_mutex_unlock(&gBackendFdState.mu);
            LOG(
                "GEM export succeeded handle=%u params_size=%llu memFd=%d mode=legacy_output_fd",
                handle,
                (unsigned long long)paramsSize,
                exportParams.memFd
            );
            return exportParams.memFd;
        }

        LOG(
            "GEM export failed handle=%u errno=%d ret=%d memFd=%d params_size=%llu mode=legacy_output_fd",
            handle,
            savedErrno,
            ret,
            exportParams.memFd,
            (unsigned long long)paramsSize
        );
    }

    return -1;
}

int convertDmabufFdToNvFd(
    int dmaBufFd,
    int drmFd,
    int nvctlFallbackFd,
    uint32_t driverMajorVersion
) {
    if (dmaBufFd < 0 || drmFd < 0) {
        LOG(
            "convert dmabuf->nvfd skipped: dmaBufFd=%d drmFd=%d",
            dmaBufFd,
            drmFd
        );
        return -1;
    }

    struct drm_prime_handle primeHandle = {
        .fd = dmaBufFd,
        .flags = 0
    };
    if (ioctl(drmFd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &primeHandle) != 0) {
        LOG(
            "DRM_IOCTL_PRIME_FD_TO_HANDLE failed fd=%d errno=%d",
            dmaBufFd,
            errno
        );
        return -1;
    }

    int exportFd = exportGemHandleToNvFd(
        drmFd,
        nvctlFallbackFd,
        driverMajorVersion,
        primeHandle.handle
    );

    struct drm_gem_close gemClose = {
        .handle = primeHandle.handle
    };
    if (ioctl(drmFd, DRM_IOCTL_GEM_CLOSE, &gemClose) != 0) {
        LOG(
            "DRM_IOCTL_GEM_CLOSE failed handle=%u errno=%d",
            primeHandle.handle,
            errno
        );
    }

    if (exportFd < 0) {
        return -1;
    }

    LOG(
        "converted dmabuf fd=%d to nvkms memfd=%d (handle=%u)",
        dmaBufFd,
        exportFd,
        primeHandle.handle
    );
    return exportFd;
}

bool copy_external_plane_to_cuda_array(
    struct _NVDriver *drv,
    int importFd,
    bool useDmaBufHandle,
    uint32_t objectSize,
    uint32_t planeOffset,
    uint32_t planePitch,
    uint32_t copyWidthBytes,
    uint32_t planeHeight,
    CUarray dstArray
) {
    if (drv == NULL || importFd < 0 || dstArray == NULL || objectSize == 0) {
        if (importFd >= 0) {
            backendCloseTrackedNvKmsFd(importFd, "copy_external_invalid_input");
        }
        return false;
    }

    if (drv->cu->cuExternalMemoryGetMappedBuffer == NULL) {
        LOG("cuExternalMemoryGetMappedBuffer is unavailable");
        backendCloseTrackedNvKmsFd(importFd, "copy_external_no_mapped_buffer");
        return false;
    }

    CUexternalMemoryHandleType handleTypes[2] = {0};
    size_t handleTypeCount = 0;
    if (useDmaBufHandle) {
#ifdef CU_EXTERNAL_MEMORY_HANDLE_TYPE_DMABUF_FD
        handleTypes[handleTypeCount++] = CU_EXTERNAL_MEMORY_HANDLE_TYPE_DMABUF_FD;
#else
        const CUexternalMemoryHandleType kDmaBufHandleType = (CUexternalMemoryHandleType)9;
        handleTypes[handleTypeCount++] = kDmaBufHandleType;
        LOG(
            "DMABUF_FD handle type is missing from this CUDA SDK; trying numeric handleType=%d",
            (int)kDmaBufHandleType
        );
#endif
        handleTypes[handleTypeCount++] = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    } else {
        handleTypes[handleTypeCount++] = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    }

    bool copied = false;
    for (size_t attempt = 0; attempt < handleTypeCount; attempt++) {
        const CUexternalMemoryHandleType handleType = handleTypes[attempt];
        CUexternalMemory extMem = NULL;
        CUdeviceptr mappedBuffer = 0;
        int importAttemptFd = dup(importFd);
        if (importAttemptFd < 0) {
            LOG(
                "GPU-copy plane dup failed handleType=%d attempt=%zu/%zu errno=%d",
                handleType,
                attempt + 1,
                handleTypeCount,
                errno
            );
            continue;
        }
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
            .type = handleType,
            .handle.fd = importAttemptFd,
            .flags = 0,
            .size = objectSize,
        };

        CUresult importResult = drv->cu->cuImportExternalMemory(&extMem, &extMemDesc);
        if (importResult != CUDA_SUCCESS) {
            const char *errStr = "unknown";
            drv->cu->cuGetErrorString(importResult, &errStr);
            LOG(
                "GPU-copy plane import failed handleType=%d attempt=%zu/%zu cuerr=%d (%s)",
                handleType,
                attempt + 1,
                handleTypeCount,
                importResult,
                errStr
            );
            backendCloseTrackedNvKmsFd(importAttemptFd, "copy_external_import_failed");
            continue;
        }
        LOG(
            "GPU-copy plane import succeeded fd=%d handleType=%d attempt=%zu/%zu",
            importAttemptFd,
            handleType,
            attempt + 1,
            handleTypeCount
        );
        // Successful cuImportExternalMemory() transfers FD ownership to CUDA.
        // Do not close |importAttemptFd| locally after this point.

        CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufferDesc = {
            .offset = 0,
            .size = objectSize,
            .flags = 0,
        };
        CUresult mapResult =
            drv->cu->cuExternalMemoryGetMappedBuffer(&mappedBuffer, extMem, &bufferDesc);
        if (mapResult != CUDA_SUCCESS) {
            const char *errStr = "unknown";
            drv->cu->cuGetErrorString(mapResult, &errStr);
            LOG(
                "GPU-copy plane map buffer failed handleType=%d attempt=%zu/%zu cuerr=%d (%s)",
                handleType,
                attempt + 1,
                handleTypeCount,
                mapResult,
                errStr
            );
            drv->cu->cuDestroyExternalMemory(extMem);
            backendReleaseTrackedNvKmsFd(
                importAttemptFd,
                "copy_external_map_failed_destroyed_owned_by_cuda"
            );
            continue;
        }

        CUDA_MEMCPY2D copy = {
            .srcMemoryType = CU_MEMORYTYPE_DEVICE,
            .srcDevice = mappedBuffer + planeOffset,
            .srcPitch = planePitch,
            .dstMemoryType = CU_MEMORYTYPE_ARRAY,
            .dstArray = dstArray,
            .WidthInBytes = copyWidthBytes,
            .Height = planeHeight,
        };
        CUresult copyResult = drv->cu->cuMemcpy2D(&copy);
        if (copyResult != CUDA_SUCCESS) {
            const char *errStr = "unknown";
            drv->cu->cuGetErrorString(copyResult, &errStr);
            LOG(
                "GPU-copy plane cuMemcpy2D failed handleType=%d attempt=%zu/%zu cuerr=%d (%s)",
                handleType,
                attempt + 1,
                handleTypeCount,
                copyResult,
                errStr
            );
        } else {
            copied = true;
        }

        if (mappedBuffer != 0) {
            CUresult freeResult = drv->cu->cuMemFree(mappedBuffer);
            if (freeResult != CUDA_SUCCESS) {
                const char *errStr = "unknown";
                drv->cu->cuGetErrorString(freeResult, &errStr);
                LOG("GPU-copy plane mapped buffer free failed cuerr=%d (%s)", freeResult, errStr);
            }
        }

        drv->cu->cuDestroyExternalMemory(extMem);
        backendReleaseTrackedNvKmsFd(
            importAttemptFd,
            "copy_external_destroyed_owned_by_cuda"
        );

        if (copied) {
            break;
        }
    }

    backendCloseTrackedNvKmsFd(importFd, "copy_external_complete");
    return copied;
}

bool isCudaDmabufSupported(struct _NVDriver *drv, int gpuId) {
    if (drv == NULL || drv->cu == NULL || gpuId < 0) {
        return false;
    }

    int value = 0;
    CUresult attrResult =
        drv->cu->cuDeviceGetAttribute(
            &value,
#ifdef CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED
            CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED,
#else
            124,
#endif
            gpuId
        );
    if (attrResult != CUDA_SUCCESS) {
        const char *errStr = "unknown";
        drv->cu->cuGetErrorString(attrResult, &errStr);
        LOG(
            "cuDeviceGetAttribute(CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED) failed gpu_id=%d cuerr=%d (%s)",
            gpuId,
            attrResult,
            errStr
        );
        return false;
    }

    return value != 0;
}
