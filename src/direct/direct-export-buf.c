#define _GNU_SOURCE 1

#include "../vabackend.h"
#include <stdio.h>
#include <ffnvcodec/dynlink_loader.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include <string.h>
#include "../backend-common.h"

#include <drm.h>
#include <drm_fourcc.h>

static void destroyBackingImage(NVDriver *drv, BackingImage *img);
static void directReleaseCudaImportedFd(NVCudaImage *cudaImage, const char *reason);
static bool directRestoreBackingCudaViews(NVDriver *drv, BackingImage *img, const char *reason);
static void directReleaseBackingCudaViews(NVDriver *drv, BackingImage *img, const char *reason);
static bool direct_isLinearModifier(uint64_t modifier);

static void directCloseImportedPlaneFd(int fd, const char *reason) {
    if (fd < 0) {
        return;
    }
    backendCloseFd(fd, reason);
}

static void directWatchCallerOwnedDescriptorFds(
    const VADRMPRIMESurfaceDescriptor *desc,
    const char *reason
) {
    if (desc == NULL) {
        return;
    }

    const uint32_t objectCount =
        desc->num_objects <= ARRAY_SIZE(desc->objects)
            ? desc->num_objects
            : ARRAY_SIZE(desc->objects);
    for (uint32_t i = 0; i < objectCount; i++) {
        backendWatchCallerOwnedFd(desc->objects[i].fd, reason);
    }
}

static void directUnwatchCallerOwnedDescriptorFds(
    const VADRMPRIMESurfaceDescriptor *desc,
    const char *reason
) {
    if (desc == NULL) {
        return;
    }

    const uint32_t objectCount =
        desc->num_objects <= ARRAY_SIZE(desc->objects)
            ? desc->num_objects
            : ARRAY_SIZE(desc->objects);
    for (uint32_t i = 0; i < objectCount; i++) {
        backendUnwatchCallerOwnedFd(desc->objects[i].fd, reason);
    }
}

static uint64_t directCurrentResourceEpoch;
static uint64_t directCurrentEpochLiveBackings;
static uint64_t directOlderEpochLiveBackings;
static pthread_mutex_t directResourceEpochMutex = PTHREAD_MUTEX_INITIALIZER;

static bool directIsImportGpuCopyBacking(const BackingImage *img) {
    return img != NULL && img->importedGpuCopy;
}

static uint64_t directCurrentResourceEpochValue(void) {
    pthread_mutex_lock(&directResourceEpochMutex);
    const uint64_t epoch = directCurrentResourceEpoch;
    pthread_mutex_unlock(&directResourceEpochMutex);
    return epoch;
}

static void directNoteBackingCreate(BackingImage *img) {
    if (img == NULL) {
        return;
    }

    pthread_mutex_lock(&directResourceEpochMutex);
    img->resourceEpoch = directCurrentResourceEpoch;
    directCurrentEpochLiveBackings++;
    pthread_mutex_unlock(&directResourceEpochMutex);
}

static void directNoteBackingDestroy(BackingImage *img) {
    if (img == NULL) {
        return;
    }

    pthread_mutex_lock(&directResourceEpochMutex);
    uint64_t *liveBackings =
        img->resourceEpoch == directCurrentResourceEpoch
            ? &directCurrentEpochLiveBackings
            : &directOlderEpochLiveBackings;
    if (*liveBackings > 0) {
        (*liveBackings)--;
    }
    pthread_mutex_unlock(&directResourceEpochMutex);
}

void directAdvanceResourceEpoch(void) {
    pthread_mutex_lock(&directResourceEpochMutex);
    directOlderEpochLiveBackings += directCurrentEpochLiveBackings;
    directCurrentEpochLiveBackings = 0;
    directCurrentResourceEpoch++;
    pthread_mutex_unlock(&directResourceEpochMutex);
}

bool directResourcesIdle(void) {
    pthread_mutex_lock(&directResourceEpochMutex);
    const bool idle =
        directCurrentEpochLiveBackings == 0 &&
        directOlderEpochLiveBackings == 0;
    pthread_mutex_unlock(&directResourceEpochMutex);
    return idle;
}

static void directInitBackingImage(BackingImage *img, bool importedGpuCopy) {
    if (img == NULL) {
        return;
    }

    img->importedGpuCopy = importedGpuCopy;
}

static void directInitCudaImage(NVCudaImage *cudaImage, int importedFd) {
    if (cudaImage == NULL) {
        return;
    }

    cudaImage->importedFd = importedFd;
}

static NVFormat directSurfaceBackingFormat(const NVSurface *surface) {
    if (surface == NULL) {
        return NV_FORMAT_NONE;
    }

    if (surface->rtFormat == VA_RT_FORMAT_RGB32) {
        return NV_FORMAT_ARGB;
    }

    switch (surface->format) {
    case cudaVideoSurfaceFormat_P016:
        switch (surface->bitDepth) {
        case 10:
            return NV_FORMAT_P010;
        case 12:
            return NV_FORMAT_P012;
        default:
            return NV_FORMAT_P016;
        }

#if NVENCAPI_MAJOR_VERSION >= 13
    case cudaVideoSurfaceFormat_P216:
        return NV_FORMAT_P210;

    case cudaVideoSurfaceFormat_NV16:
        return NV_FORMAT_NV16;
#endif

    case cudaVideoSurfaceFormat_YUV444_16Bit:
        return NV_FORMAT_Q416;

    case cudaVideoSurfaceFormat_YUV444:
        return NV_FORMAT_444P;

    default:
        return NV_FORMAT_NV12;
    }
}

static void direct_initBackingImageFds(BackingImage *img) {
    if (img == NULL) {
        return;
    }
    for (uint32_t i = 0; i < ARRAY_SIZE(img->fds); i++) {
        img->fds[i] = -1;
    }
}

static void findGPUIndexFromFd(NVDriver *drv) {
    //find the CUDA device id
    uint8_t drmUuid[16];
    get_device_uuid(&drv->driverContext, drmUuid);

    int gpuCount = 0;
    if (CHECK_CUDA_RESULT(drv->cu->cuDeviceGetCount(&gpuCount))) {
        return;
    }

    for (int i = 0; i < gpuCount; i++) {
        CUuuid uuid;
        if (!CHECK_CUDA_RESULT(drv->cu->cuDeviceGetUuid(&uuid, i))) {
            if (memcmp(drmUuid, uuid.bytes, 16) == 0) {
                drv->cudaGpuId = i;
                return;
            }
        }
    }

    //default to index 0
    drv->cudaGpuId = 0;
}

static bool map_external_memory_to_cuda_array(
    NVDriver *drv,
    CUexternalMemory extMem,
    const NVDriverImage *image,
    int bpc,
    int channels,
    CUmipmappedArray *outMipmapArray,
    CUarray *outArray,
    CUexternalMemoryHandleType handleTypeForLog,
    const NVCudaImage *cudaImageForLog
) {
    if (drv == NULL || extMem == NULL || image == NULL ||
        outMipmapArray == NULL || outArray == NULL) {
        return false;
    }

    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC mipmapArrayDesc = {
        .arrayDesc = {
            .Width = image->width,
            .Height = image->height,
            .Depth = 0,
            .Format = bpc == 8 ? CU_AD_FORMAT_UNSIGNED_INT8 : CU_AD_FORMAT_UNSIGNED_INT16,
            .NumChannels = channels,
            .Flags = 0
        },
        .numLevels = 1,
        .offset = image->offset
    };

    *outMipmapArray = NULL;
    *outArray = NULL;

    CUresult mapResult = drv->cu->cuExternalMemoryGetMappedMipmappedArray(
        outMipmapArray,
        extMem,
        &mipmapArrayDesc);
    if (mapResult != CUDA_SUCCESS) {
        const char *errStr = "unknown";
        drv->cu->cuGetErrorString(mapResult, &errStr);
        LOG(
            "import_to_cuda map mipmapped array failed handleType=%d cuerr=%d (%s) size=%u width=%u height=%u pitch=%u offset=%u channels=%d bpc=%d fourcc=0x%x modifier=0x%llx",
            handleTypeForLog,
            mapResult,
            errStr,
            image->memorySize,
            image->width,
            image->height,
            image->pitch,
            image->offset,
            channels,
            bpc,
            image->fourcc,
            (unsigned long long)image->mods
        );
        return false;
    }

    CUresult levelResult =
        drv->cu->cuMipmappedArrayGetLevel(outArray, *outMipmapArray, 0);
    if (levelResult != CUDA_SUCCESS) {
        const char *errStr = "unknown";
        drv->cu->cuGetErrorString(levelResult, &errStr);
        LOG(
            "import_to_cuda map level failed handleType=%d cuerr=%d (%s) size=%u width=%u height=%u pitch=%u offset=%u channels=%d bpc=%d fourcc=0x%x modifier=0x%llx",
            handleTypeForLog,
            levelResult,
            errStr,
            image->memorySize,
            image->width,
            image->height,
            image->pitch,
            image->offset,
            channels,
            bpc,
            image->fourcc,
            (unsigned long long)image->mods
        );
        drv->cu->cuMipmappedArrayDestroy(*outMipmapArray);
        *outMipmapArray = NULL;
        *outArray = NULL;
        return false;
    }

    return true;
}

static bool map_external_memory_to_cuda_buffer(
    NVDriver *drv,
    CUexternalMemory extMem,
    const NVDriverImage *image,
    CUdeviceptr *outMappedBuffer,
    CUexternalMemoryHandleType handleTypeForLog
) {
    if (drv == NULL || extMem == NULL || image == NULL || outMappedBuffer == NULL) {
        return false;
    }

    CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufferDesc = {
        .offset = 0,
        .size = image->memorySize,
        .flags = 0,
    };

    *outMappedBuffer = 0;

    CUresult mapResult =
        drv->cu->cuExternalMemoryGetMappedBuffer(outMappedBuffer, extMem, &bufferDesc);
    if (mapResult != CUDA_SUCCESS) {
        const char *errStr = "unknown";
        drv->cu->cuGetErrorString(mapResult, &errStr);
        LOG(
            "import_to_cuda map buffer failed handleType=%d cuerr=%d (%s) size=%u width=%u height=%u pitch=%u offset=%u fourcc=0x%x modifier=0x%llx",
            handleTypeForLog,
            mapResult,
            errStr,
            image->memorySize,
            image->width,
            image->height,
            image->pitch,
            image->offset,
            image->fourcc,
            (unsigned long long)image->mods
        );
        return false;
    }

    return true;
}

static bool import_to_cuda(NVDriver *drv, NVDriverImage *image, int bpc, int channels, NVCudaImage *cudaImage, CUarray *array) {
    if (image->useDmaBufHandle) {
        LOG(
            "import_to_cuda skipped: DMABUF_FD cannot be mapped with cuExternalMemoryGetMappedMipmappedArray"
        );
        return false;
    }
    const int importedFd = image->nvFd;
    const CUexternalMemoryHandleType handleType =
        CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    uint32_t extMemFlags = 0;
    const bool preferDedicatedHandle =
        isTruthyEnv(getenv("NVD_DIRECT_IMPORT_OPAQUE_FD_DEDICATED"));
#ifdef CUDA_EXTERNAL_MEMORY_DEDICATED
    if (preferDedicatedHandle) {
        extMemFlags |= CUDA_EXTERNAL_MEMORY_DEDICATED;
    }
#else
    if (preferDedicatedHandle) {
        LOG(
            "import_to_cuda: NVD_DIRECT_IMPORT_OPAQUE_FD_DEDICATED requested but CUDA_EXTERNAL_MEMORY_DEDICATED is unavailable in this build"
        );
    }
#endif

    LOG(
        "import_to_cuda begin fd=%d size=%u width=%u height=%u pitch=%u offset=%u channels=%d bpc=%d fourcc=0x%x modifier=0x%llx handleType=%d flags=0x%x",
        image->nvFd,
        image->memorySize,
        image->width,
        image->height,
        image->pitch,
        image->offset,
        channels,
        bpc,
        image->fourcc,
        (unsigned long long)image->mods,
        handleType,
        extMemFlags
    );
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
        .type      = handleType,
        .handle.fd = image->nvFd,
        .flags     = extMemFlags,
        .size      = image->memorySize
    };

    CUresult importResult = drv->cu->cuImportExternalMemory(&cudaImage->extMem, &extMemDesc);
    CHECK_CUDA_RESULT_RETURN(importResult, false);
    directInitCudaImage(cudaImage, importedFd);

    // Successful cuImportExternalMemory() transfers OPAQUE_FD ownership to
    // CUDA. Do not close |importedFd| locally after cuDestroyExternalMemory(),
    // or Chromium's FD ownership checks can observe a stale/double close.
    image->nvFd = 0;
    if (image->nvFd2 == importedFd) {
        image->nvFd2 = 0;
    }

    if (!map_external_memory_to_cuda_array(
            drv,
            cudaImage->extMem,
            image,
            bpc,
            channels,
            &cudaImage->mipmapArray,
            array,
            handleType,
            cudaImage)) {
        CHECK_CUDA_RESULT(drv->cu->cuDestroyExternalMemory(cudaImage->extMem));
        cudaImage->extMem = NULL;
        directReleaseCudaImportedFd(cudaImage, "import_to_cuda_map_failed");
        return false;
    }

    LOG(
        "import_to_cuda success array=%p handleType=%d",
        (void *)*array,
        handleType
    );
    return true;
}

static bool import_to_cuda_buffer(
    NVDriver *drv,
    NVDriverImage *image,
    NVCudaImage *cudaImage
) {
    if (drv == NULL || image == NULL || cudaImage == NULL) {
        return false;
    }

    if (drv->cu->cuExternalMemoryGetMappedBuffer == NULL) {
        LOG("import_to_cuda_buffer skipped: cuExternalMemoryGetMappedBuffer is unavailable");
        return false;
    }

    const int sourceFd = image->nvFd;
    CUexternalMemoryHandleType handleTypes[2] = {0};
    size_t handleTypeCount = 0;
    if (image->useDmaBufHandle) {
#ifdef CU_EXTERNAL_MEMORY_HANDLE_TYPE_DMABUF_FD
        handleTypes[handleTypeCount++] = CU_EXTERNAL_MEMORY_HANDLE_TYPE_DMABUF_FD;
#else
        const CUexternalMemoryHandleType kDmaBufHandleType = (CUexternalMemoryHandleType)9;
        handleTypes[handleTypeCount++] = kDmaBufHandleType;
        LOG(
            "DMABUF_FD handle type is missing from this CUDA SDK; trying numeric handleType=%d for buffer import",
            (int)kDmaBufHandleType
        );
#endif
        handleTypes[handleTypeCount++] = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    } else {
        handleTypes[handleTypeCount++] = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    }

    for (size_t attempt = 0; attempt < handleTypeCount; attempt++) {
        const CUexternalMemoryHandleType handleType = handleTypes[attempt];
        int importAttemptFd = dup(sourceFd);
        if (importAttemptFd < 0) {
            LOG(
                "import_to_cuda_buffer dup failed handleType=%d attempt=%zu/%zu errno=%d",
                handleType,
                attempt + 1,
                handleTypeCount,
                errno
            );
            continue;
        }

        LOG(
            "import_to_cuda_buffer begin fd=%d size=%u width=%u height=%u pitch=%u offset=%u fourcc=0x%x modifier=0x%llx handleType=%d attempt=%zu/%zu",
            importAttemptFd,
            image->memorySize,
            image->width,
            image->height,
            image->pitch,
            image->offset,
            image->fourcc,
            (unsigned long long)image->mods,
            handleType,
            attempt + 1,
            handleTypeCount
        );

        CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
            .type = handleType,
            .handle.fd = importAttemptFd,
            .flags = 0,
            .size = image->memorySize,
        };

        CUresult importResult =
            drv->cu->cuImportExternalMemory(&cudaImage->extMem, &extMemDesc);
        if (importResult != CUDA_SUCCESS) {
            const char *errStr = "unknown";
            drv->cu->cuGetErrorString(importResult, &errStr);
            LOG(
                "import_to_cuda_buffer import failed handleType=%d attempt=%zu/%zu cuerr=%d (%s)",
                handleType,
                attempt + 1,
                handleTypeCount,
                importResult,
                errStr
            );
            backendCloseTrackedNvKmsFd(importAttemptFd, "import_to_cuda_buffer_import_failed");
            continue;
        }

        directInitCudaImage(cudaImage, importAttemptFd);
        if (sourceFd != 0) {
            directCloseImportedPlaneFd(sourceFd, "import_to_cuda_buffer_source_fd_replaced_by_dup");
            image->nvFd = 0;
            if (image->nvFd2 == sourceFd) {
                image->nvFd2 = 0;
            }
        }

        if (!map_external_memory_to_cuda_buffer(
                drv,
                cudaImage->extMem,
                image,
                &cudaImage->mappedBuffer,
                handleType)) {
            CHECK_CUDA_RESULT(drv->cu->cuDestroyExternalMemory(cudaImage->extMem));
            cudaImage->extMem = NULL;
            directReleaseCudaImportedFd(cudaImage, "import_to_cuda_buffer_map_failed");
            continue;
        }

        cudaImage->mappedBufferSize = image->memorySize;
        LOG(
            "import_to_cuda_buffer success buffer=%p handleType=%d",
            (void *)cudaImage->mappedBuffer,
            handleType
        );
        return true;
    }

    return false;
}

static void direct_releaseTemporaryCudaImage(NVDriver *drv, NVCudaImage *cudaImage) {
    if (cudaImage == NULL) {
        return;
    }
    cudaImage->mappedBuffer = 0;
    cudaImage->mappedBufferSize = 0;
    if (cudaImage->mipmapArray != NULL) {
        drv->cu->cuMipmappedArrayDestroy(cudaImage->mipmapArray);
        cudaImage->mipmapArray = NULL;
    }
    if (cudaImage->extMem != NULL) {
        CHECK_CUDA_RESULT(drv->cu->cuDestroyExternalMemory(cudaImage->extMem));
        cudaImage->extMem = NULL;
    }
    directReleaseCudaImportedFd(cudaImage, "temporary_release");
}

static void directReleaseCudaImportedFd(NVCudaImage *cudaImage, const char *reason) {
    if (cudaImage == NULL || cudaImage->importedFd < 0) {
        return;
    }
    backendReleaseTrackedNvKmsFd(
        cudaImage->importedFd,
        reason != NULL ? reason : "direct_release_imported_fd_owned_by_cuda"
    );
    cudaImage->importedFd = -1;
}

static bool copy_external_plane_to_cuda_array_via_import(
    NVDriver *drv,
    int importFd,
    bool usedNvKms,
    bool useDmaBufHandle,
    uint64_t modifier,
    uint32_t objectSize,
    uint32_t planeOffset,
    uint32_t planePitch,
    uint32_t visiblePlaneWidth,
    uint32_t planeHeight,
    uint32_t bytesPerComponent,
    uint32_t channelCount,
    uint32_t fourcc,
    CUarray dstArray,
    uint32_t planeIndex
) {
    if (importFd < 0 || dstArray == NULL || objectSize == 0 || planeHeight == 0) {
        if (importFd >= 0) {
            backendCloseTrackedNvKmsFd(importFd, "copy_external_array_import_invalid_input");
        }
        return false;
    }

    const uint32_t planeBytesPerPixel = bytesPerComponent * channelCount;
    if (planeBytesPerPixel == 0 || planePitch == 0 || (planePitch % planeBytesPerPixel) != 0) {
        backendCloseTrackedNvKmsFd(importFd, "copy_external_array_import_invalid_pitch");
        return false;
    }

    const uint32_t pitchWidth = planePitch / planeBytesPerPixel;
    if (pitchWidth < visiblePlaneWidth) {
        backendCloseTrackedNvKmsFd(importFd, "copy_external_array_import_short_pitch");
        return false;
    }
    const bool useVisibleWidthForNonLinear =
        !direct_isLinearModifier(modifier) &&
        isTruthyEnv(getenv("NVD_GPU_COPY_NONLINEAR_USE_VISIBLE_WIDTH"));
    const uint32_t importWidth =
        useVisibleWidthForNonLinear ? visiblePlaneWidth : pitchWidth;
    LOG(
        "GPU-copy source import width decision: visible=%u pitch_width=%u selected=%u plane=%u modifier=0x%llx use_visible_width_non_linear=%s",
        visiblePlaneWidth,
        pitchWidth,
        importWidth,
        planeIndex,
        (unsigned long long) modifier,
        useVisibleWidthForNonLinear ? "yes" : "no"
    );

    uint32_t importOffset = planeOffset;
    const bool applyNonLinearNvkmsChromaZeroOffset =
        !direct_isLinearModifier(modifier) &&
        usedNvKms &&
        planeIndex > 0 &&
        isTruthyEnv(getenv("NVD_DIRECT_IMPORT_NONLINEAR_NVKMS_CHROMA_ZERO_OFFSET"));
    if (applyNonLinearNvkmsChromaZeroOffset) {
        LOG(
            "GPU-copy source plane %u applying NVKMS non-linear chroma offset override: %u -> 0",
            planeIndex,
            importOffset
        );
        importOffset = 0;
    }

    NVDriverImage importImage = {
        .nvFd = importFd,
        .nvFd2 = importFd,
        .drmFd = -1,
        .useDmaBufHandle = useDmaBufHandle,
        .width = importWidth,
        .height = planeHeight,
        .mods = modifier,
        .memorySize = objectSize,
        .offset = importOffset,
        .pitch = planePitch,
        .fourcc = fourcc,
    };

    NVCudaImage srcCudaImage = {0};
    CUarray srcArray = NULL;
    if (!import_to_cuda(
            drv,
            &importImage,
            8 * bytesPerComponent,
            channelCount,
            &srcCudaImage,
            &srcArray)) {
        if (importImage.nvFd != 0) {
            directCloseImportedPlaneFd(importImage.nvFd, "copy_external_array_import_import_failed");
        }
        return false;
    }

    CUDA_MEMCPY2D copy = {
        .srcMemoryType = CU_MEMORYTYPE_ARRAY,
        .srcArray = srcArray,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = dstArray,
        .WidthInBytes = visiblePlaneWidth * planeBytesPerPixel,
        .Height = planeHeight,
    };
    const CUresult copyResult = drv->cu->cuMemcpy2D(&copy);
    if (copyResult != CUDA_SUCCESS) {
        const char *errStr = "unknown";
        drv->cu->cuGetErrorString(copyResult, &errStr);
        LOG(
            "GPU-copy non-linear array copy failed cuerr=%d (%s) width=%u height=%u",
            copyResult,
            errStr,
            visiblePlaneWidth,
            planeHeight
        );
    }

    direct_releaseTemporaryCudaImage(drv, &srcCudaImage);
    if (importImage.nvFd != 0) {
        directCloseImportedPlaneFd(importImage.nvFd, "copy_external_array_import_complete");
    }

    return copyResult == CUDA_SUCCESS;
}


static void debug(EGLenum error,const char *command,EGLint messageType,EGLLabelKHR threadLabel,EGLLabelKHR objectLabel,const char* message) {
    LOG("[EGL] %s: %s", command, message);
}

static bool direct_initExporter(NVDriver *drv) {
    //this is only needed to see errors in firefox
    static const EGLAttrib debugAttribs[] = {EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE, EGL_DEBUG_MSG_INFO_KHR, EGL_TRUE, EGL_NONE};
    const PFNEGLDEBUGMESSAGECONTROLKHRPROC eglDebugMessageControlKHR = (PFNEGLDEBUGMESSAGECONTROLKHRPROC) eglGetProcAddress("eglDebugMessageControlKHR");
    eglDebugMessageControlKHR(debug, debugAttribs);

    //make sure we have a drm fd
    if (drv->drmFd == -1) {
        int nvdGpu = drv->cudaGpuId;
        if (nvdGpu == -1) {
            // The default GPU is the first one we find.
            nvdGpu = 0;
        }

        int fd;
        int nvIdx = 0;
        uint8_t drmIdx = 128;
        char node[20] = {0, };
        do {
            LOG("Searching for GPU: %d %d %d", nvIdx, nvdGpu, drmIdx)
            snprintf(node, 20, "/dev/dri/renderD%d", drmIdx++);
            fd = open(node, O_RDWR|O_CLOEXEC);
            if (fd == -1) {
                LOG("Unable to find NVIDIA GPU %d", nvdGpu);
                return false;
            }

            if (!isNvidiaDrmFd(fd, true) || !checkModesetParameterFromFd(fd)) {
                backendCloseFd(fd, "direct_init_exporter_invalid_drm_fd");
                continue;
            }

            if (nvIdx != nvdGpu) {
                backendCloseFd(fd, "direct_init_exporter_skip_gpu");
                nvIdx++;
                continue;
            }
            break;
        } while (drmIdx < 128 + 16);

        drv->drmFd = fd;
        LOG("Found NVIDIA GPU %d at %s", nvdGpu, node);
    } else {
        if (!isNvidiaDrmFd(drv->drmFd, true) || !checkModesetParameterFromFd(drv->drmFd)) {
            return false;
        }

        //dup it so we can close it later and not effect firefox
        drv->drmFd = dup(drv->drmFd);
    }

    const bool ret = init_nvdriver(&drv->driverContext, drv->drmFd);

    //TODO this isn't really correct as we don't know if the driver version actually supports importing them
    //but we don't have an easy way to find out.
    drv->supports16BitSurface = true;
    drv->supports444Surface = true;
    findGPUIndexFromFd(drv);

    if (ret) {
        const int gpuId = drv->cudaGpuId;
        const bool cudaDmabufSupported =
            isCudaDmabufSupported(drv, gpuId);

        const char *preferDirectDmabufImportEnv =
            getenv("NVD_PREFER_DIRECT_DMABUF_CUDA_IMPORT");
        const char *disableDirectDmabufImportEnv =
            getenv("NVD_DISABLE_DIRECT_DMABUF_CUDA_IMPORT");
        if (isTruthyEnv(disableDirectDmabufImportEnv)) {
            drv->allowDirectDmabufCudaImport = false;
        } else {
            bool allowDirectDmabufImport = cudaDmabufSupported;
            if (isTruthyEnv(preferDirectDmabufImportEnv)) {
                allowDirectDmabufImport = true;
            }
            drv->allowDirectDmabufCudaImport = allowDirectDmabufImport;
        }

        const char *preferGpuCopyEnv = getenv("NVD_PREFER_GPU_COPY_IMPORT");
        const char *forceGpuCopyRawEnv = getenv("NVD_FORCE_GPU_COPY_RAW_DMABUF");
        const char *forceGpuCopyNvKmsEnv = getenv("NVD_FORCE_GPU_COPY_NVKMS");
        if (isTruthyEnv(preferGpuCopyEnv)) {
            drv->preferExternalImportGpuCopy = true;
            LOG(
                "external import policy: GPU-copy preferred (NVD_PREFER_GPU_COPY_IMPORT=%s)",
                preferGpuCopyEnv
            );
        } else {
            // Prefer direct-import by default. Failed imports still fall back to GPU-copy.
            drv->preferExternalImportGpuCopy = false;
        }
        drv->forceGpuCopyRawDmabuf = isTruthyEnv(forceGpuCopyRawEnv);
        drv->forceGpuCopyNvKms = isTruthyEnv(forceGpuCopyNvKmsEnv);
        if (drv->forceGpuCopyRawDmabuf && drv->forceGpuCopyNvKms) {
            LOG(
                "Both NVD_FORCE_GPU_COPY_RAW_DMABUF and NVD_FORCE_GPU_COPY_NVKMS are set; preferring raw-dmabuf GPU-copy path"
            );
            drv->forceGpuCopyNvKms = false;
        }

        LOG(
            "external import policy probe: gpu_id=%d dmabuf_sharing_supported=%s allow_direct_dmabuf=%s prefer_direct_dmabuf_env=%s disable_direct_dmabuf_env=%s prefer_gpu_copy=%s force_gpu_copy_raw=%s force_gpu_copy_nvkms=%s mode=startup_capability_probe",
            gpuId,
            cudaDmabufSupported ? "yes" : "no",
            drv->allowDirectDmabufCudaImport ? "yes" : "no",
            preferDirectDmabufImportEnv != NULL ? preferDirectDmabufImportEnv : "(null)",
            disableDirectDmabufImportEnv != NULL ? disableDirectDmabufImportEnv : "(null)",
            drv->preferExternalImportGpuCopy ? "yes" : "no",
            drv->forceGpuCopyRawDmabuf ? "yes" : "no",
            drv->forceGpuCopyNvKms ? "yes" : "no"
        );
    }

    return ret;
}

static void direct_releaseExporter(NVDriver *drv) {
    free_nvdriver(&drv->driverContext);
}

static BackingImage *direct_allocateBackingImage(NVDriver *drv, NVSurface *surface, bool importedGpuCopy) {
    NVDriverImage driverImages[3] = { 0 };
    BackingImage *backingImage = calloc(1, sizeof(BackingImage));
    if (backingImage == NULL) {
        return NULL;
    }
    directInitBackingImage(backingImage, importedGpuCopy);
    directNoteBackingCreate(backingImage);
    direct_initBackingImageFds(backingImage);

    backingImage->format = directSurfaceBackingFormat(surface);

    const NVFormatInfo *fmtInfo = &formatsInfo[backingImage->format];
    const NVFormatPlane *p = fmtInfo->plane;
    backingImage->fourcc = fmtInfo->fourcc;
    backingImage->width = surface->width;
    backingImage->height = surface->height;

    LOG("Allocating BackingImages: %p %dx%d", backingImage, surface->width, surface->height);
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (!alloc_image(&drv->driverContext,
                         surface->width >> p[i].ss.x,
                         surface->height >> p[i].ss.y,
                         p[i].channelCount,
                         8 * fmtInfo->bppc,
                         p[i].fourcc,
                         &driverImages[i])) {
            goto bail;
        }
    }
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (!import_to_cuda(drv, &driverImages[i], 8 * fmtInfo->bppc, p[i].channelCount, &backingImage->cudaImages[i], &backingImage->arrays[i]))
            goto bail;
    }

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        backingImage->fds[i] = driverImages[i].drmFd;
        backingImage->strides[i] = driverImages[i].pitch;
        backingImage->mods[i] = driverImages[i].mods;
        backingImage->size[i] = driverImages[i].memorySize;
    }

    // alloc_image() may leave auxiliary NV FDs that are not needed after
    // successful CUDA import. Close them here so they do not outlive the
    // backing image lifecycle.
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (driverImages[i].nvFd > 0 &&
            driverImages[i].nvFd != driverImages[i].drmFd) {
            backendCloseFd(driverImages[i].nvFd, "direct_allocate_backing_release_nvfd");
            driverImages[i].nvFd = 0;
        }
        if (driverImages[i].nvFd2 > 0 &&
            driverImages[i].nvFd2 != driverImages[i].nvFd &&
            driverImages[i].nvFd2 != driverImages[i].drmFd) {
            backendCloseFd(driverImages[i].nvFd2, "direct_allocate_backing_release_nvfd2");
            driverImages[i].nvFd2 = 0;
        }
    }

    return backingImage;

bail:
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (driverImages[i].nvFd != 0) {
            backendCloseFd(driverImages[i].nvFd, "direct_allocate_backing_fail_nvfd");
        }
        if (driverImages[i].nvFd2 != 0 &&
            driverImages[i].nvFd2 != driverImages[i].nvFd) {
            backendCloseFd(driverImages[i].nvFd2, "direct_allocate_backing_fail_nvfd2");
        }
        if (driverImages[i].drmFd != 0 &&
            driverImages[i].drmFd != driverImages[i].nvFd &&
            driverImages[i].drmFd != driverImages[i].nvFd2) {
            backendCloseFd(driverImages[i].drmFd, "direct_allocate_backing_fail_drmfd");
        }
    }

    if (backingImage != NULL) {
        destroyBackingImage(drv, backingImage);
    }

    return NULL;
}

static void destroyBackingImage(NVDriver *drv, BackingImage *img) {
    if (img == NULL) {
        return;
    }

    directNoteBackingDestroy(img);
    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];
    if (img->surface != NULL) {
        img->surface->backingImage = NULL;
    }

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (img->arrays[i] != NULL) {
            CHECK_CUDA_RESULT(drv->cu->cuArrayDestroy(img->arrays[i]));
        }
        img->cudaImages[i].mappedBuffer = 0;
        img->cudaImages[i].mappedBufferSize = 0;

        if (img->cudaImages[i].mipmapArray != NULL) {
            CHECK_CUDA_RESULT(drv->cu->cuMipmappedArrayDestroy(img->cudaImages[i].mipmapArray));
        }

        if (img->cudaImages[i].extMem != NULL) {
            bool alreadyDestroyed = false;
            for (uint32_t j = 0; j < i; j++) {
                if (img->cudaImages[j].extMem == img->cudaImages[i].extMem) {
                    alreadyDestroyed = true;
                    break;
                }
            }
            if (!alreadyDestroyed) {
                CUresult destroyResult = drv->cu->cuDestroyExternalMemory(img->cudaImages[i].extMem);
                CHECK_CUDA_RESULT(destroyResult);
                directReleaseCudaImportedFd(&img->cudaImages[i], "destroyBackingImage");
            } else {
                img->cudaImages[i].importedFd = -1;
            }
        }
    }

    // Keep retained local dma-buf duplicates alive until CUDA has fully torn
    // down every imported external-memory object that depends on them.
    for (int i = 0; i < 4; i++) {
        if (img->fds[i] >= 0) {
            backendCloseFd(img->fds[i], "direct_destroy_backing_fd");
            img->fds[i] = -1;
        }
    }

    memset(img, 0, sizeof(BackingImage));
    free(img);
}

static void directReleaseBackingCudaViews(NVDriver *drv, BackingImage *img, const char *reason) {
    if (drv == NULL || img == NULL) {
        return;
    }

    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (img->arrays[i] != NULL) {
            CHECK_CUDA_RESULT(drv->cu->cuArrayDestroy(img->arrays[i]));
            img->arrays[i] = NULL;
        }
        img->cudaImages[i].mappedBuffer = 0;
        img->cudaImages[i].mappedBufferSize = 0;

        if (img->cudaImages[i].mipmapArray != NULL) {
            CHECK_CUDA_RESULT(drv->cu->cuMipmappedArrayDestroy(img->cudaImages[i].mipmapArray));
            img->cudaImages[i].mipmapArray = NULL;
        }

        if (img->cudaImages[i].extMem != NULL) {
            bool alreadyDestroyed = false;
            for (uint32_t j = 0; j < i; j++) {
                if (img->cudaImages[j].extMem == img->cudaImages[i].extMem) {
                    alreadyDestroyed = true;
                    break;
                }
            }
            if (!alreadyDestroyed) {
                CUresult destroyResult = drv->cu->cuDestroyExternalMemory(img->cudaImages[i].extMem);
                CHECK_CUDA_RESULT(destroyResult);
                directReleaseCudaImportedFd(&img->cudaImages[i], reason != NULL ? reason : "releaseBackingCudaViews");
            } else {
                img->cudaImages[i].importedFd = -1;
            }
            img->cudaImages[i].extMem = NULL;
        }
    }
}

static bool directRestoreBackingCudaViews(NVDriver *drv, BackingImage *img, const char *reason) {
    if (drv == NULL || img == NULL) {
        return false;
    }

    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (img->arrays[i] != NULL || img->cudaImages[i].mappedBuffer != 0) {
            continue;
        }
        if (img->fds[i] < 0) {
            LOG(
                "cannot restore backing CUDA view: missing retained fd plane=%u reason=%s",
                i,
                reason != NULL ? reason : "(null)"
            );
            return false;
        }

        const NVFormatPlane *plane = &fmtInfo->plane[i];
        int importFd = convertDmabufFdToNvFd(
            img->fds[i],
            drv->driverContext.drmFd,
            drv->driverContext.nvctlFd,
            drv->driverContext.driverMajorVersion
        );
        if (importFd < 0) {
            LOG(
                "cannot restore backing CUDA view: convertDmabufFdToNvFd failed plane=%u retained_fd=%d reason=%s",
                i,
                img->fds[i],
                reason != NULL ? reason : "(null)"
            );
            return false;
        }

        NVDriverImage importImage = {
            .nvFd = importFd,
            .nvFd2 = importFd,
            .drmFd = img->fds[i],
            .useDmaBufHandle = false,
            .width = img->width >> plane->ss.x,
            .height = img->height >> plane->ss.y,
            .mods = img->mods[i],
            .memorySize = img->size[i],
            .offset = img->offsets[i],
            .pitch = img->strides[i],
            .fourcc = plane->fourcc
        };


        if (!import_to_cuda(
                drv,
                &importImage,
                8 * fmtInfo->bppc,
                plane->channelCount,
                &img->cudaImages[i],
                &img->arrays[i])) {
            if (importImage.nvFd > 0) {
                backendCloseFd(importImage.nvFd, "restore_backing_cuda_view_fail_nvfd");
            }
            if (importImage.nvFd2 > 0 && importImage.nvFd2 != importImage.nvFd) {
                backendCloseFd(importImage.nvFd2, "restore_backing_cuda_view_fail_nvfd2");
            }
            directReleaseBackingCudaViews(drv, img, "restore_backing_cuda_view_rollback");
            return false;
        }
    }

    return true;
}

static void direct_attachBackingImageToSurface(NVDriver *drv, NVSurface *surface, BackingImage *img) {
    surface->backingImage = img;
    img->surface = surface;
}

static void direct_detachBackingImageFromSurface(NVDriver *drv, NVSurface *surface) {
    if (surface->backingImage == NULL) {
        return;
    }

    BackingImage *img = surface->backingImage;
    img->surface = NULL;
    surface->backingImage = NULL;

    destroyBackingImage(drv, img);
}

static void direct_destroyAllBackingImage(NVDriver *drv) {
    pthread_mutex_lock(&drv->imagesMutex);

    ARRAY_FOR_EACH_REV(BackingImage*, it, &drv->images)
        destroyBackingImage(drv, it);
        remove_element_at(&drv->images, it_idx);
    END_FOR_EACH

    pthread_mutex_unlock(&drv->imagesMutex);
}

void directPurgeOldImportGpuCopyBackings(NVDriver *drv) {
    if (drv == NULL) {
        return;
    }

    const uint64_t currentEpoch = directCurrentResourceEpochValue();

    pthread_mutex_lock(&drv->objectCreationMutex);

    ARRAY_FOR_EACH(Object, obj, &drv->objects)
        if (obj == NULL || obj->type != OBJECT_TYPE_SURFACE || obj->obj == NULL) {
            continue;
        }

        NVSurface *surface = (NVSurface *) obj->obj;
        BackingImage *img = surface->backingImage;
        if (img != NULL &&
            img->resourceEpoch < currentEpoch &&
            directIsImportGpuCopyBacking(img)) {
            destroyBackingImage(drv, img);
        }
    END_FOR_EACH

    pthread_mutex_unlock(&drv->objectCreationMutex);
}

void directReleaseOldImportGpuCopyBackingCudaViews(NVDriver *drv) {
    if (drv == NULL) {
        return;
    }

    const uint64_t currentEpoch = directCurrentResourceEpochValue();

    pthread_mutex_lock(&drv->objectCreationMutex);

    ARRAY_FOR_EACH(Object, obj, &drv->objects)
        if (obj == NULL || obj->type != OBJECT_TYPE_SURFACE || obj->obj == NULL) {
            continue;
        }

        NVSurface *surface = (NVSurface *) obj->obj;
        BackingImage *img = surface->backingImage;
        if (img != NULL &&
            img->resourceEpoch < currentEpoch &&
            directIsImportGpuCopyBacking(img) &&
            img->arrays[0] != NULL) {
            directReleaseBackingCudaViews(drv, img, "release_old_import_gpu_copy_cuda_views");
        }
    END_FOR_EACH

    pthread_mutex_unlock(&drv->objectCreationMutex);
}

static bool copyFrameToSurface(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch) {
    if (surface->rtFormat == VA_RT_FORMAT_RGB32) {
        if (surface->format != cudaVideoSurfaceFormat_YUV444 &&
            surface->format != cudaVideoSurfaceFormat_YUV444_16Bit) {
            LOG("RGB32 export expects YUV444 decode output, got surface format=%d", surface->format);
            return false;
        }
        if (surface->backingImage == NULL || surface->backingImage->arrays[0] == NULL) {
            LOG("RGB32 export has no backing image/array");
            return false;
        }

        const uint32_t bytesPerSample =
            (surface->format == cudaVideoSurfaceFormat_YUV444_16Bit || surface->bitDepth > 8) ? 2 : 1;
        const uint32_t srcPlanePitchBytes = surface->width * bytesPerSample;
        if (pitch < srcPlanePitchBytes) {
            LOG("RGB32 export invalid pitch=%u (required >= %u)", pitch, srcPlanePitchBytes);
            return false;
        }

        const size_t planeBytes = (size_t)srcPlanePitchBytes * surface->height;
        const size_t argbBytes = (size_t)surface->width * surface->height * 4;
        uint8_t *yPlane = malloc(planeBytes);
        uint8_t *uPlane = malloc(planeBytes);
        uint8_t *vPlane = malloc(planeBytes);
        uint32_t *argb = (uint32_t *)malloc(argbBytes);
        if (yPlane == NULL || uPlane == NULL || vPlane == NULL || argb == NULL) {
            LOG("RGB32 export allocation failed");
            free(yPlane);
            free(uPlane);
            free(vPlane);
            free(argb);
            return false;
        }

        const size_t planeOffsetBytes = (size_t)pitch * surface->height;
        uint8_t *planes[3] = {yPlane, uPlane, vPlane};
        for (uint32_t i = 0; i < 3; i++) {
            CUDA_MEMCPY2D copyPlane = {
                .srcMemoryType = CU_MEMORYTYPE_DEVICE,
                .srcDevice = ptr + (CUdeviceptr)(i * planeOffsetBytes),
                .srcPitch = pitch,
                .dstMemoryType = CU_MEMORYTYPE_HOST,
                .dstHost = planes[i],
                .dstPitch = srcPlanePitchBytes,
                .WidthInBytes = srcPlanePitchBytes,
                .Height = surface->height
            };
            if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&copyPlane))) {
                free(yPlane);
                free(uPlane);
                free(vPlane);
                free(argb);
                return false;
            }
        }

        const uint32_t sampleX = surface->width > 1 ? surface->width / 2 : 0;
        const uint32_t sampleY = surface->height > 1 ? surface->height / 2 : 0;
        uint8_t y00 = 0, yMid = 0, u00 = 0, uMid = 0, v00 = 0, vMid = 0;
        if (bytesPerSample == 1) {
            y00 = yPlane[0];
            yMid = yPlane[(size_t)sampleY * srcPlanePitchBytes + sampleX];
            u00 = uPlane[0];
            uMid = uPlane[(size_t)sampleY * srcPlanePitchBytes + sampleX];
            v00 = vPlane[0];
            vMid = vPlane[(size_t)sampleY * srcPlanePitchBytes + sampleX];
        } else {
            const uint16_t *y16 = (const uint16_t *)yPlane;
            const uint16_t *u16 = (const uint16_t *)uPlane;
            const uint16_t *v16 = (const uint16_t *)vPlane;
            const size_t sampleIdx = (size_t)sampleY * surface->width + sampleX;
            y00 = (uint8_t)(y16[0] >> 8);
            yMid = (uint8_t)(y16[sampleIdx] >> 8);
            u00 = (uint8_t)(u16[0] >> 8);
            uMid = (uint8_t)(u16[sampleIdx] >> 8);
            v00 = (uint8_t)(v16[0] >> 8);
            vMid = (uint8_t)(v16[sampleIdx] >> 8);
        }
        LOG(
            "RGB32 export YUV samples: pitch=%u planePitch=%u y00=%u yMid=%u u00=%u uMid=%u v00=%u vMid=%u",
            pitch,
            srcPlanePitchBytes,
            y00,
            yMid,
            u00,
            uMid,
            v00,
            vMid
        );

        for (uint32_t py = 0; py < surface->height; py++) {
            for (uint32_t px = 0; px < surface->width; px++) {
                uint8_t yv = 0;
                uint8_t uv = 128;
                uint8_t vv = 128;

                if (bytesPerSample == 1) {
                    const size_t idx = (size_t)py * srcPlanePitchBytes + px;
                    yv = yPlane[idx];
                    uv = uPlane[idx];
                    vv = vPlane[idx];
                } else {
                    const size_t idx = (size_t)py * surface->width + px;
                    yv = (uint8_t)(((const uint16_t *)yPlane)[idx] >> 8);
                    uv = (uint8_t)(((const uint16_t *)uPlane)[idx] >> 8);
                    vv = (uint8_t)(((const uint16_t *)vPlane)[idx] >> 8);
                }

                const int c = yv > 16 ? (int)yv - 16 : 0;
                const int d = (int)uv - 128;
                const int e = (int)vv - 128;
                int r = (298 * c + 409 * e + 128) >> 8;
                int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
                int b = (298 * c + 516 * d + 128) >> 8;
                if (r < 0) r = 0; else if (r > 255) r = 255;
                if (g < 0) g = 0; else if (g > 255) g = 255;
                if (b < 0) b = 0; else if (b > 255) b = 255;

                argb[(size_t)py * surface->width + px] =
                    0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
        LOG(
            "RGB32 export ARGB samples: p00=0x%08x pMid=0x%08x",
            argb[0],
            argb[(size_t)sampleY * surface->width + sampleX]
        );

        CUDA_MEMCPY2D copyArgb = {
            .srcMemoryType = CU_MEMORYTYPE_HOST,
            .srcHost = argb,
            .srcPitch = surface->width * 4,
            .dstMemoryType = CU_MEMORYTYPE_ARRAY,
            .dstArray = surface->backingImage->arrays[0],
            .Height = surface->height,
            .WidthInBytes = surface->width * 4
        };
        const bool ok = !CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&copyArgb));
        free(yPlane);
        free(uPlane);
        free(vPlane);
        free(argb);
        if (!ok) {
            return false;
        }

        pthread_mutex_lock(&surface->mutex);
        surface->resolving = 0;
        pthread_cond_signal(&surface->cond);
        pthread_mutex_unlock(&surface->mutex);
        return true;
    }

    const NVFormatInfo *fmtInfo = &formatsInfo[surface->backingImage->format];
    uint32_t y = 0;

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *p = &fmtInfo->plane[i];
        CUDA_MEMCPY2D cpy = {
            .srcMemoryType = CU_MEMORYTYPE_DEVICE,
            .srcDevice = ptr,
            .srcY = y,
            .srcPitch = pitch,
            .dstMemoryType = CU_MEMORYTYPE_ARRAY,
            .dstArray = surface->backingImage->arrays[i],
            .Height = surface->height >> p->ss.y,
            .WidthInBytes = (surface->width >> p->ss.x) * fmtInfo->bppc * p->channelCount
        };
        if (i == fmtInfo->numPlanes - 1) {
            CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&cpy));
        } else {
            CHECK_CUDA_RESULT(drv->cu->cuMemcpy2DAsync(&cpy, 0));
        }
        y += surface->height >> p->ss.y;
    }

    //notify anyone waiting for us to be resolved
    pthread_mutex_lock(&surface->mutex);
    surface->resolving = 0;
    pthread_cond_signal(&surface->cond);
    pthread_mutex_unlock(&surface->mutex);

    return true;
}

static bool direct_realiseSurface(NVDriver *drv, NVSurface *surface) {
    //make sure we're the only thread updating this surface
    pthread_mutex_lock(&surface->mutex);
    //check again to see if it's just been created
    if (surface->backingImage == NULL) {
        //try to find a free surface
        BackingImage *img = direct_allocateBackingImage(drv, surface, false);
        if (img == NULL) {
            LOG("Unable to realise surface: %p (%d)", surface, surface->pictureIdx)
            pthread_mutex_unlock(&surface->mutex);
            return false;
        }

        direct_attachBackingImageToSurface(drv, surface, img);
    } else if (surface->backingImage->arrays[0] == NULL &&
               surface->backingImage->cudaImages[0].mappedBuffer == 0) {
        if (!directRestoreBackingCudaViews(drv, surface->backingImage, "realiseSurface_restore")) {
            LOG("Unable to restore surface backing CUDA views: %p (%d)", surface, surface->pictureIdx)
            pthread_mutex_unlock(&surface->mutex);
            return false;
        }
    }
    pthread_mutex_unlock(&surface->mutex);

    return true;
}

static bool direct_exportCudaPtr(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch) {
    if (!direct_realiseSurface(drv, surface)) {
        return false;
    }

    if (ptr != 0) {
        copyFrameToSurface(drv, ptr, surface, pitch);
    } else {
        LOG("exporting with null ptr")
    }

    return true;
}

static bool direct_isNv12Fourcc(uint32_t fourcc) {
    return fourcc == DRM_FORMAT_NV12 || fourcc == VA_FOURCC_NV12;
}

static bool direct_isArgbFourcc(uint32_t fourcc) {
    // Chromium may pass VA_FOURCC_BGRA for AR24/BGRA_8888 shared images.
    return fourcc == DRM_FORMAT_ARGB8888 ||
           fourcc == VA_FOURCC_ARGB ||
           fourcc == VA_FOURCC_BGRA;
}

static bool direct_is444PFourcc(uint32_t fourcc) {
    return fourcc == DRM_FORMAT_YUV444 || fourcc == VA_FOURCC_444P;
}

static bool direct_isQ416Fourcc(uint32_t fourcc) {
#ifdef DRM_FORMAT_Q410
    if (fourcc == DRM_FORMAT_Q410) {
        return true;
    }
#endif
    return fourcc == VA_FOURCC_Q416;
}

static bool direct_isLinearModifier(uint64_t modifier) {
    return modifier == 0 || modifier == DRM_FORMAT_MOD_LINEAR;
}

static bool direct_findNonLinearModifier(
    const VADRMPRIMESurfaceDescriptor *desc,
    uint32_t *outObjectIndex,
    uint64_t *outModifier
) {
    if (desc == NULL) {
        return false;
    }

    for (uint32_t i = 0; i < desc->num_objects && i < ARRAY_SIZE(desc->objects); i++) {
        const uint64_t modifier = desc->objects[i].drm_format_modifier;
        if (!direct_isLinearModifier(modifier)) {
            if (outObjectIndex != NULL) {
                *outObjectIndex = i;
            }
            if (outModifier != NULL) {
                *outModifier = modifier;
            }
            return true;
        }
    }

    return false;
}

static bool direct_getExternalImportFormat(
    const NVSurface *surface,
    uint32_t descFourcc,
    NVFormat *outFormat
) {
    if (surface == NULL || outFormat == NULL) {
        return false;
    }

    if (surface->rtFormat == VA_RT_FORMAT_RGB32 && direct_isArgbFourcc(descFourcc)) {
        *outFormat = NV_FORMAT_ARGB;
        return true;
    }

    if (surface->format == cudaVideoSurfaceFormat_NV12 && direct_isNv12Fourcc(descFourcc)) {
        *outFormat = NV_FORMAT_NV12;
        return true;
    }

    if (surface->format == cudaVideoSurfaceFormat_YUV444 && direct_is444PFourcc(descFourcc)) {
        *outFormat = NV_FORMAT_444P;
        return true;
    }

    if (surface->format == cudaVideoSurfaceFormat_YUV444_16Bit && direct_isQ416Fourcc(descFourcc)) {
        *outFormat = NV_FORMAT_Q416;
        return true;
    }

    return false;
}

static bool direct_importExternalSurfaceViaCpuCopy(
    NVDriver *drv,
    NVSurface *surface,
    const VADRMPRIMESurfaceDescriptor *desc,
    const NVFormatInfo *fmtInfo,
    const uint32_t *planeObjectIndex,
    const uint32_t *planeOffsets,
    const uint32_t *planePitches
) {
    uint32_t nonLinearObjectIndex = 0;
    uint64_t nonLinearModifier = 0;
    if (direct_findNonLinearModifier(desc, &nonLinearObjectIndex, &nonLinearModifier)) {
        LOG(
            "CPU-copy fallback rejected: object %u uses non-linear modifier=0x%llx; row-major mmap copy is unsafe",
            nonLinearObjectIndex,
            (unsigned long long)nonLinearModifier
        );
        return false;
    }

    BackingImage *backingImage = direct_allocateBackingImage(drv, surface, false);
    if (backingImage == NULL) {
        LOG("CPU-copy fallback failed: could not allocate internal backing image");
        return false;
    }

    uint64_t objectSizes[ARRAY_SIZE(desc->objects)] = {0};
    void *mappedObjects[ARRAY_SIZE(desc->objects)] = {0};

    for (uint32_t i = 0; i < desc->num_objects && i < ARRAY_SIZE(desc->objects); i++) {
        objectSizes[i] = desc->objects[i].size;
    }

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (planeObjectIndex[i] >= ARRAY_SIZE(desc->objects)) {
            LOG("CPU-copy fallback failed: plane %u object index %u out of range", i, planeObjectIndex[i]);
            goto fail;
        }
        const NVFormatPlane *plane = &fmtInfo->plane[i];
        uint32_t planeHeight = surface->height >> plane->ss.y;
        uint64_t minPlaneSize = (uint64_t)planeOffsets[i] + ((uint64_t)planePitches[i] * planeHeight);
        if (objectSizes[planeObjectIndex[i]] < minPlaneSize) {
            objectSizes[planeObjectIndex[i]] = minPlaneSize;
        }
    }

    for (uint32_t i = 0; i < desc->num_objects && i < ARRAY_SIZE(desc->objects); i++) {
        if (objectSizes[i] == 0) {
            continue;
        }
        if (objectSizes[i] > SIZE_MAX) {
            LOG(
                "CPU-copy fallback failed: object %u size too large (%llu)",
                i,
                (unsigned long long)objectSizes[i]
            );
            goto fail;
        }

        void *map = mmap(NULL, (size_t)objectSizes[i], PROT_READ, MAP_SHARED, desc->objects[i].fd, 0);
        if (map == MAP_FAILED) {
            LOG(
                "CPU-copy fallback failed: mmap object %u fd=%d size=%llu errno=%d",
                i,
                desc->objects[i].fd,
                (unsigned long long)objectSizes[i],
                errno
            );
            goto fail;
        }
        mappedObjects[i] = map;
    }

    backingImage->fourcc = desc->fourcc;
    backingImage->width = surface->width;
    backingImage->height = surface->height;

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *plane = &fmtInfo->plane[i];
        uint32_t planeWidth = surface->width >> plane->ss.x;
        uint32_t planeHeight = surface->height >> plane->ss.y;
        uint32_t planeBytesPerPixel = fmtInfo->bppc * plane->channelCount;
        uint32_t copyWidthBytes = planeWidth * planeBytesPerPixel;
        uint32_t objectIndex = planeObjectIndex[i];

        if (planePitches[i] < copyWidthBytes || planePitches[i] == 0) {
            LOG(
                "CPU-copy fallback failed: plane %u pitch %u is smaller than copy width %u",
                i,
                planePitches[i],
                copyWidthBytes
            );
            goto fail;
        }

        uint64_t requiredSize = (uint64_t)planeOffsets[i] +
                                ((uint64_t)planePitches[i] * (planeHeight > 0 ? (planeHeight - 1) : 0)) +
                                copyWidthBytes;
        if (requiredSize > objectSizes[objectIndex]) {
            LOG(
                "CPU-copy fallback failed: plane %u exceeds object %u bounds required=%llu size=%llu",
                i,
                objectIndex,
                (unsigned long long)requiredSize,
                (unsigned long long)objectSizes[objectIndex]
            );
            goto fail;
        }

        CUDA_MEMCPY2D copy = {
            .srcMemoryType = CU_MEMORYTYPE_HOST,
            .srcHost = PTROFF(mappedObjects[objectIndex], planeOffsets[i]),
            .srcPitch = planePitches[i],
            .dstMemoryType = CU_MEMORYTYPE_ARRAY,
            .dstArray = backingImage->arrays[i],
            .WidthInBytes = copyWidthBytes,
            .Height = planeHeight
        };
        if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&copy))) {
            LOG("CPU-copy fallback failed: cuMemcpy2D failed for plane %u", i);
            goto fail;
        }

        // CPU-copy imports own the internal backing allocated above. Do not
        // overwrite internal retained FDs/metadata with source dmabuf state
        // after the copy completes, or we lose the internal handles needed for
        // correct cleanup and export.
        LOG(
            "CPU-copy import plane %u copied width=%u height=%u src_pitch=%u src_offset=%u src_obj_size=%llu",
            i,
            planeWidth,
            planeHeight,
            planePitches[i],
            planeOffsets[i],
            (unsigned long long)objectSizes[objectIndex]
        );
    }

    for (uint32_t i = 0; i < desc->num_objects && i < ARRAY_SIZE(desc->objects); i++) {
        if (mappedObjects[i] != NULL) {
            munmap(mappedObjects[i], (size_t)objectSizes[i]);
        }
    }

    direct_attachBackingImageToSurface(drv, surface, backingImage);
    LOG(
        "CPU-copy fallback imported external surface %ux%u fourcc=0x%x",
        surface->width,
        surface->height,
        desc->fourcc
    );
    return true;

fail:
    for (uint32_t i = 0; i < desc->num_objects && i < ARRAY_SIZE(desc->objects); i++) {
        if (mappedObjects[i] != NULL) {
            munmap(mappedObjects[i], (size_t)objectSizes[i]);
        }
    }
    destroyBackingImage(drv, backingImage);
    return false;
}

static bool direct_importExternalSurfaceViaGpuCopy(
    NVDriver *drv,
    NVSurface *surface,
    const VADRMPRIMESurfaceDescriptor *desc,
    const NVFormatInfo *fmtInfo,
    const uint32_t *planeObjectIndex,
    const uint32_t *planeOffsets,
    const uint32_t *planePitches
) {
    BackingImage *backingImage = direct_allocateBackingImage(drv, surface, true);
    if (backingImage == NULL) {
        LOG("GPU-copy import failed: could not allocate internal backing image");
        return false;
    }

    int cachedConvertedObjectFds[ARRAY_SIZE(desc->objects)];
    bool cachedConvertedObjectUseDmaBuf[ARRAY_SIZE(desc->objects)];
    for (uint32_t i = 0; i < ARRAY_SIZE(desc->objects); i++) {
        cachedConvertedObjectFds[i] = -1;
        cachedConvertedObjectUseDmaBuf[i] = true;
    }

    backingImage->fourcc = desc->fourcc;
    backingImage->width = surface->width;
    backingImage->height = surface->height;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (planeObjectIndex[i] >= desc->num_objects) {
            LOG(
                "GPU-copy import failed: plane %u object index %u out of range (objects=%u)",
                i,
                planeObjectIndex[i],
                desc->num_objects
            );
            goto fail;
        }

        const NVFormatPlane *plane = &fmtInfo->plane[i];
        const uint32_t planeWidth = surface->width >> plane->ss.x;
        const uint32_t planeHeight = surface->height >> plane->ss.y;
        const uint32_t planeBytesPerPixel = fmtInfo->bppc * plane->channelCount;
        const uint32_t copyWidthBytes = planeWidth * planeBytesPerPixel;
        const uint32_t objectIndex = planeObjectIndex[i];

        if (planePitches[i] < copyWidthBytes || planePitches[i] == 0) {
            LOG(
                "GPU-copy import failed: plane %u pitch %u is smaller than copy width %u",
                i,
                planePitches[i],
                copyWidthBytes
            );
            goto fail;
        }

        const uint64_t modifier = desc->objects[objectIndex].drm_format_modifier;
        const bool isLinearModifier = direct_isLinearModifier(modifier);
        uint64_t minPlaneSize = (uint64_t)planeOffsets[i] + ((uint64_t)planePitches[i] * planeHeight);
        uint64_t objectSize = desc->objects[objectIndex].size;
        if (objectSize < minPlaneSize) {
            objectSize = minPlaneSize;
        }
        if (objectSize > UINT32_MAX) {
            LOG(
                "GPU-copy import failed: plane %u object size too large (%llu)",
                i,
                (unsigned long long)objectSize
            );
            goto fail;
        }
        LOG(
            "GPU-copy source plane %u object=%u modifier=0x%llx linear=%s visible=%ux%u pitch=%u offset=%u descObjSize=%u importObjSize=%u",
            i,
            objectIndex,
            (unsigned long long)modifier,
            isLinearModifier ? "yes" : "no",
            planeWidth,
            planeHeight,
            planePitches[i],
            planeOffsets[i],
            desc->objects[objectIndex].size,
            (uint32_t)objectSize
        );

        bool copiedPlane = false;
        bool usedNvKms = false;

        // Non-linear surfaces cannot be interpreted as row-major bytes.
        // Import them as CUDA arrays first, then copy array->array.
        if (!isLinearModifier) {
            int importFd = -1;
            bool useDmaBufHandle = true;
            const uint32_t importSize = (uint32_t)objectSize;

            if (drv->forceGpuCopyRawDmabuf) {
                importFd = dup(desc->objects[objectIndex].fd);
                useDmaBufHandle = true;
            } else {
                importFd =
                    convertDmabufFdToNvFd(
                        desc->objects[objectIndex].fd,
                        drv->driverContext.drmFd,
                        drv->driverContext.nvctlFd,
                        drv->driverContext.driverMajorVersion
                    );
                if (importFd >= 0) {
                    usedNvKms = true;
                    useDmaBufHandle = false;
                } else if (!drv->forceGpuCopyNvKms) {
                    importFd = dup(desc->objects[objectIndex].fd);
                    useDmaBufHandle = true;
                }
            }

            if (importFd >= 0) {
                copiedPlane = copy_external_plane_to_cuda_array_via_import(
                    drv,
                    importFd,
                    usedNvKms,
                    useDmaBufHandle,
                    modifier,
                    importSize,
                    planeOffsets[i],
                    planePitches[i],
                    planeWidth,
                    planeHeight,
                    fmtInfo->bppc,
                    plane->channelCount,
                    plane->fourcc,
                    backingImage->arrays[i],
                    i
                );
                if (copiedPlane) {
                    LOG(
                        "GPU-copy non-linear plane %u imported via CUDA array copy modifier=0x%llx source=%s",
                        i,
                        (unsigned long long)modifier,
                        usedNvKms ? "nvkms" : "raw_dmabuf"
                    );
                }
            }
        }

        const bool allowRawGpuCopy =
            isLinearModifier &&
            drv->allowDirectDmabufCudaImport &&
            !drv->forceGpuCopyNvKms;
        if (!copiedPlane && allowRawGpuCopy) {
            int rawFd = dup(desc->objects[objectIndex].fd);
            if (rawFd >= 0) {
                copiedPlane = copy_external_plane_to_cuda_array(
                    drv,
                    rawFd,
                    true,
                    (uint32_t)objectSize,
                    planeOffsets[i],
                    planePitches[i],
                    copyWidthBytes,
                    planeHeight,
                    backingImage->arrays[i]
                );
                if (!copiedPlane) {
                    if (drv->allowDirectDmabufCudaImport && !drv->forceGpuCopyRawDmabuf) {
                        drv->allowDirectDmabufCudaImport = false;
                        LOG(
                            "Disabling direct raw dmabuf CUDA import after GPU-copy failure; using NVKMS conversion path"
                        );
                    } else if (drv->forceGpuCopyRawDmabuf) {
                        LOG(
                            "GPU-copy raw dmabuf path forced and failed for plane %u; NVKMS fallback disabled",
                            i
                        );
                    }
                }
            }
        }

        if (!copiedPlane && isLinearModifier && !drv->forceGpuCopyRawDmabuf) {
            if (cachedConvertedObjectFds[objectIndex] < 0) {
                int convertedFd = convertDmabufFdToNvFd(
                    desc->objects[objectIndex].fd,
                    drv->driverContext.drmFd,
                    drv->driverContext.nvctlFd,
                    drv->driverContext.driverMajorVersion
                );
                if (convertedFd >= 0) {
                    cachedConvertedObjectFds[objectIndex] = convertedFd;
                    cachedConvertedObjectUseDmaBuf[objectIndex] = false;
                }
            }

            if (cachedConvertedObjectFds[objectIndex] >= 0) {
                int convertedDupFd = dup(cachedConvertedObjectFds[objectIndex]);
                if (convertedDupFd >= 0) {
                    backendTrackNvKmsFdDup(
                        cachedConvertedObjectFds[objectIndex],
                        convertedDupFd,
                        "gpu_copy_cached_converted_dup"
                    );
                    copiedPlane = copy_external_plane_to_cuda_array(
                        drv,
                        convertedDupFd,
                        cachedConvertedObjectUseDmaBuf[objectIndex],
                        (uint32_t)objectSize,
                        planeOffsets[i],
                        planePitches[i],
                        copyWidthBytes,
                        planeHeight,
                        backingImage->arrays[i]
                    );
                    usedNvKms = !cachedConvertedObjectUseDmaBuf[objectIndex];
                }
            }
        }

        if (!copiedPlane) {
            if (isLinearModifier) {
                LOG("GPU-copy import failed: could not copy external plane %u", i);
            } else {
                LOG(
                    "GPU-copy import failed: non-linear plane %u modifier=0x%llx could not be imported via CUDA-array path; linear fallback disabled",
                    i,
                    (unsigned long long)modifier
                );
            }
            goto fail;
        }

        LOG(
            "GPU-copy import plane %u complete width=%u height=%u pitch=%u offset=%u objSize=%u used_nvkms=%s",
            i,
            planeWidth,
            planeHeight,
            planePitches[i],
            planeOffsets[i],
            (uint32_t)objectSize,
            usedNvKms ? "yes" : "no"
        );
    }

    for (uint32_t i = 0; i < ARRAY_SIZE(desc->objects); i++) {
        if (cachedConvertedObjectFds[i] >= 0) {
            backendCloseTrackedNvKmsFd(cachedConvertedObjectFds[i], "gpu_copy_cached_converted_cleanup_success");
        }
    }

    direct_attachBackingImageToSurface(drv, surface, backingImage);
    LOG(
        "GPU-copy imported external surface %ux%u fourcc=0x%x",
        surface->width,
        surface->height,
        desc->fourcc
    );
    return true;

fail:
    for (uint32_t i = 0; i < ARRAY_SIZE(desc->objects); i++) {
        if (cachedConvertedObjectFds[i] >= 0) {
            backendCloseTrackedNvKmsFd(cachedConvertedObjectFds[i], "gpu_copy_cached_converted_cleanup_fail");
        }
    }
    destroyBackingImage(drv, backingImage);
    return false;
}

static bool direct_importExternalSurfaceImpl(NVDriver *drv, NVSurface *surface, const VADRMPRIMESurfaceDescriptor *desc) {
    if (desc == NULL) {
        LOG("external surface descriptor is NULL");
        return false;
    }

    NVFormat importFormat = NV_FORMAT_NONE;
    if (!direct_getExternalImportFormat(surface, desc->fourcc, &importFormat)) {
        LOG(
            "unsupported external surface: rt_format=0x%x surface_format=%d descriptor_fourcc=0x%x",
            surface->rtFormat,
            surface->format,
            desc->fourcc
        );
        return false;
    }

    if (desc->num_objects == 0 || desc->num_layers == 0) {
        LOG("Invalid external surface descriptor (objects=%u, layers=%u)", desc->num_objects, desc->num_layers);
        return false;
    }
    if (desc->num_objects > ARRAY_SIZE(desc->objects) ||
        desc->num_layers > ARRAY_SIZE(desc->layers)) {
        LOG(
            "External surface descriptor exceeds bounds (objects=%u max=%zu layers=%u max=%zu)",
            desc->num_objects,
            ARRAY_SIZE(desc->objects),
            desc->num_layers,
            ARRAY_SIZE(desc->layers)
        );
        return false;
    }
    for (uint32_t i = 0; i < desc->num_layers; i++) {
        if (desc->layers[i].num_planes > ARRAY_SIZE(desc->layers[i].object_index)) {
            LOG(
                "Invalid external descriptor layer[%u] num_planes=%u (max=%zu)",
                i,
                desc->layers[i].num_planes,
                ARRAY_SIZE(desc->layers[i].object_index)
            );
            return false;
        }
    }

    LOG(
        "Import external surface: fourcc=0x%x size=%ux%u objects=%u layers=%u",
        desc->fourcc,
        desc->width,
        desc->height,
        desc->num_objects,
        desc->num_layers
    );
    for (uint32_t i = 0; i < desc->num_objects; i++) {
        const int objectFd = desc->objects[i].fd;
        struct stat objectStat = {0};
        if (objectFd >= 0 && fstat(objectFd, &objectStat) == 0) {
            LOG(
                "  object[%u]: fd=%d size=%u modifier=0x%llx dev=%llu ino=%llu",
                i,
                objectFd,
                desc->objects[i].size,
                (unsigned long long)desc->objects[i].drm_format_modifier,
                (unsigned long long)objectStat.st_dev,
                (unsigned long long)objectStat.st_ino
            );
        } else {
            LOG(
                "  object[%u]: fd=%d size=%u modifier=0x%llx stat=unavailable errno=%d",
                i,
                objectFd,
                desc->objects[i].size,
                (unsigned long long)desc->objects[i].drm_format_modifier,
                errno
            );
        }
    }
    bool object0SameBacking[ARRAY_SIZE(desc->objects)] = {false};
    bool object0SameSize[ARRAY_SIZE(desc->objects)] = {false};
    if (desc->num_objects > 0 && desc->objects[0].fd >= 0) {
        struct stat object0Stat = {0};
        if (fstat(desc->objects[0].fd, &object0Stat) == 0) {
            object0SameBacking[0] = true;
            object0SameSize[0] = true;
            for (uint32_t i = 1; i < desc->num_objects && i < ARRAY_SIZE(desc->objects); i++) {
                struct stat objectIStat = {0};
                if (desc->objects[i].fd < 0 ||
                    fstat(desc->objects[i].fd, &objectIStat) != 0) {
                    continue;
                }
                object0SameBacking[i] =
                    object0Stat.st_dev == objectIStat.st_dev &&
                    object0Stat.st_ino == objectIStat.st_ino;
                object0SameSize[i] = desc->objects[0].size == desc->objects[i].size;
                LOG(
                    "  object[0]/object[%u] same-backing=%s same-size=%s",
                    i,
                    object0SameBacking[i] ? "yes" : "no",
                    object0SameSize[i] ? "yes" : "no"
                );
            }
        }
    }
    for (uint32_t i = 0; i < desc->num_layers; i++) {
        const uint32_t objectIndex0 =
            desc->layers[i].num_planes > 0 ? desc->layers[i].object_index[0] : 0;
        const uint64_t modifier0 =
            (desc->layers[i].num_planes > 0 && objectIndex0 < desc->num_objects)
                ? desc->objects[objectIndex0].drm_format_modifier
                : 0;
        LOG(
            "  layer[%u]: drm_format=0x%x num_planes=%u obj0=%u pitch0=%u offset0=%u modifier0=0x%llx",
            i,
            desc->layers[i].drm_format,
            desc->layers[i].num_planes,
            objectIndex0,
            desc->layers[i].num_planes > 0 ? desc->layers[i].pitch[0] : 0,
            desc->layers[i].num_planes > 0 ? desc->layers[i].offset[0] : 0,
            (unsigned long long)modifier0
        );
    }
    const NVFormatInfo *fmtInfo = &formatsInfo[importFormat];
    if (fmtInfo->numPlanes == 0 || fmtInfo->numPlanes > 3) {
        LOG("Unsupported import plane count=%u for format=%d", fmtInfo->numPlanes, importFormat);
        return false;
    }

    uint32_t planeObjectIndex[3] = {0};
    uint32_t planeOffsets[3] = {0};
    uint32_t planePitches[3] = {0};
    uint32_t planeCount = 0;
    for (uint32_t i = 0; i < desc->num_layers; i++) {
        for (uint32_t j = 0; j < desc->layers[i].num_planes; j++) {
            if (planeCount >= fmtInfo->numPlanes) {
                break;
            }
            const uint32_t objectIndex = desc->layers[i].object_index[j];
            if (objectIndex >= desc->num_objects) {
                LOG(
                    "Invalid external descriptor layer[%u] plane[%u] object_index=%u (objects=%u)",
                    i,
                    j,
                    objectIndex,
                    desc->num_objects
                );
                return false;
            }
            planeObjectIndex[planeCount] = objectIndex;
            planeOffsets[planeCount] = desc->layers[i].offset[j];
            planePitches[planeCount] = desc->layers[i].pitch[j];
            planeCount++;
        }
    }

    if (planeCount < fmtInfo->numPlanes) {
        LOG("External surface descriptor does not expose enough planes (%u < %u)", planeCount, fmtInfo->numPlanes);
        return false;
    }

    if (isTruthyEnv(getenv("NVD_FORCE_CPU_COPY_IMPORT"))) {
        LOG(
            "NVD_FORCE_CPU_COPY_IMPORT is enabled; forcing CPU-copy fallback path"
        );
        return direct_importExternalSurfaceViaCpuCopy(
            drv,
            surface,
            desc,
            fmtInfo,
            planeObjectIndex,
            planeOffsets,
            planePitches
        );
    }

    const bool enable444PDirectImport =
        isTruthyEnv(getenv("NVD_DIRECT_IMPORT_444P_ENABLE"));
    if (importFormat == NV_FORMAT_444P && !enable444PDirectImport) {
        LOG(
            "Bypassing direct-import path for 444P external surface; using GPU-copy import path"
        );
        return direct_importExternalSurfaceViaGpuCopy(
            drv,
            surface,
            desc,
            fmtInfo,
            planeObjectIndex,
            planeOffsets,
            planePitches
        );
    }
    if (importFormat == NV_FORMAT_444P) {
        LOG("444P direct-import experiment enabled");
    }

    if (drv->preferExternalImportGpuCopy) {
        if (surface->rtFormat != VA_RT_FORMAT_RGB32) {
            LOG(
                "Skipping direct-import path after startup probe; using GPU-copy import path"
            );
            return direct_importExternalSurfaceViaGpuCopy(
                drv,
                surface,
                desc,
                fmtInfo,
                planeObjectIndex,
                planeOffsets,
                planePitches
            );
        }

        LOG("Bypassing startup GPU-copy preference for RGB32 external surface; trying direct-import path");
    }

    BackingImage *backingImage = calloc(1, sizeof(BackingImage));
    if (backingImage == NULL) {
        return false;
    }
    directInitBackingImage(backingImage, false);
    directNoteBackingCreate(backingImage);
    direct_initBackingImageFds(backingImage);
    backingImage->format = importFormat;
    backingImage->fourcc = desc->fourcc;
    backingImage->width = surface->width;
    backingImage->height = surface->height;
    bool importedWithMappedBuffers = false;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (planeObjectIndex[i] >= desc->num_objects) {
            LOG("External plane %u has invalid object index %u (objects=%u)", i, planeObjectIndex[i], desc->num_objects);
            destroyBackingImage(drv, backingImage);
            return false;
        }

        const NVFormatPlane *plane = &fmtInfo->plane[i];
        uint32_t planeWidth = surface->width >> plane->ss.x;
        uint32_t planeHeight = surface->height >> plane->ss.y;
        uint32_t planeBytesPerPixel = fmtInfo->bppc * plane->channelCount;
        if (planePitches[i] == 0 || planeBytesPerPixel == 0) {
            LOG(
                "External plane %u has invalid pitch/bpp (pitch=%u bpp=%u)",
                i,
                planePitches[i],
                planeBytesPerPixel
            );
            destroyBackingImage(drv, backingImage);
            return false;
        }
        if ((planePitches[i] % planeBytesPerPixel) != 0) {
            LOG(
                "External plane %u pitch %u is not aligned to bytes-per-pixel %u",
                i,
                planePitches[i],
                planeBytesPerPixel
            );
            destroyBackingImage(drv, backingImage);
            return false;
        }

        const uint32_t pitchWidth = planePitches[i] / planeBytesPerPixel;
        if (pitchWidth < planeWidth) {
            LOG(
                "External plane %u pitch width %u is smaller than visible width %u",
                i,
                pitchWidth,
                planeWidth
            );
            destroyBackingImage(drv, backingImage);
            return false;
        }

        const uint32_t declaredObjectIndex = planeObjectIndex[i];
        uint32_t importObjectIndex = declaredObjectIndex;
        uint64_t declaredModifier = desc->objects[declaredObjectIndex].drm_format_modifier;
        const bool objectMatchesPlane0Backing =
            declaredObjectIndex < ARRAY_SIZE(object0SameBacking) &&
            object0SameBacking[declaredObjectIndex] &&
            object0SameSize[declaredObjectIndex];
        const bool forceObject0ForNonLinearSharedBacking =
            i > 0 &&
            isTruthyEnv(getenv("NVD_DIRECT_IMPORT_NONLINEAR_SHARED_BACKING_USE_OBJECT0")) &&
            !direct_isLinearModifier(declaredModifier) &&
            objectMatchesPlane0Backing &&
            planeObjectIndex[0] < desc->num_objects;
        if (forceObject0ForNonLinearSharedBacking &&
            importObjectIndex != planeObjectIndex[0]) {
            LOG(
                "External plane %u shared-backing remap: object %u -> %u linear=%s format=%d",
                i,
                importObjectIndex,
                planeObjectIndex[0],
                direct_isLinearModifier(declaredModifier) ? "yes" : "no",
                importFormat
            );
            importObjectIndex = planeObjectIndex[0];
        }
        uint64_t modifier = desc->objects[importObjectIndex].drm_format_modifier;
        bool isLinearModifier = direct_isLinearModifier(modifier);
        const bool usePitchWidthForNonLinear =
            isTruthyEnv(getenv("NVD_DIRECT_IMPORT_NONLINEAR_USE_PITCH_WIDTH"));
        const bool usePitchWidthForLinear444 =
            importFormat == NV_FORMAT_444P &&
            enable444PDirectImport &&
            isLinearModifier;
        const bool useBufferDirectForLinear444 = usePitchWidthForLinear444;
        uint32_t importWidth = planeWidth;
        if (!isLinearModifier && usePitchWidthForNonLinear) {
            importWidth = pitchWidth;
        }
        if (usePitchWidthForLinear444) {
            importWidth = pitchWidth;
        }
        LOG(
            "External plane %u width decision: visible=%u pitch_width=%u selected=%u linear=%s use_pitch_width_non_linear=%s use_pitch_width_linear_444=%s",
            i,
            planeWidth,
            pitchWidth,
            importWidth,
            isLinearModifier ? "yes" : "no",
            usePitchWidthForNonLinear ? "yes" : "no",
            usePitchWidthForLinear444 ? "yes" : "no"
        );

        uint64_t minPlaneSize = (uint64_t)planeOffsets[i] + ((uint64_t)planePitches[i] * planeHeight);
        uint32_t importOffset = planeOffsets[i];
        uint64_t objectSize = desc->objects[importObjectIndex].size;
        if (isLinearModifier && objectSize < minPlaneSize) {
            objectSize = minPlaneSize;
        }

        if (objectSize > UINT32_MAX) {
            LOG("External plane %u object size too large: %llu", i, (unsigned long long)objectSize);
            destroyBackingImage(drv, backingImage);
            return false;
        }

        int importFd = convertDmabufFdToNvFd(
            desc->objects[importObjectIndex].fd,
            drv->driverContext.drmFd,
            drv->driverContext.nvctlFd,
            drv->driverContext.driverMajorVersion
        );
        bool usedNvFd = importFd >= 0;
        if (importFd < 0) {
            importFd = dup(desc->objects[importObjectIndex].fd);
        }

        const bool applyNonLinearNvkmsChromaZeroOffset =
            !isLinearModifier &&
            usedNvFd &&
            i > 0 &&
            isTruthyEnv(getenv("NVD_DIRECT_IMPORT_NONLINEAR_NVKMS_CHROMA_ZERO_OFFSET"));
        if (applyNonLinearNvkmsChromaZeroOffset) {
            LOG(
                "External plane %u applying NVKMS non-linear chroma offset override: %u -> 0",
                i,
                importOffset
            );
            importOffset = 0;
        }

        if (!isLinearModifier && objectSize <= importOffset) {
            LOG(
                "External plane %u non-linear object size %llu is not larger than plane offset %u",
                i,
                (unsigned long long)objectSize,
                importOffset
            );
            destroyBackingImage(drv, backingImage);
            return false;
        }

        if (importFd < 0) {
            LOG("Failed to dup external object fd for plane %u", i);
            destroyBackingImage(drv, backingImage);
            return false;
        }

        NVDriverImage importImage = {
            .nvFd = importFd,
            .nvFd2 = importFd,
            .drmFd = -1,
            .useDmaBufHandle = !usedNvFd,
            .width = importWidth,
            .height = planeHeight,
            .mods = modifier,
            .memorySize = (uint32_t)objectSize,
            .offset = importOffset,
            .pitch = planePitches[i],
            .fourcc = plane->fourcc
        };

        LOG(
            "Import external plane %u: visible=%ux%u import=%ux%u pitch=%u pitchWidth=%u offset=%u objSize=%u object=%u object_fd=%d import_fd=%d used_nvkms=%s modifier=0x%llx",
            i,
            planeWidth,
            planeHeight,
            importImage.width,
            importImage.height,
            importImage.pitch,
            pitchWidth,
            importImage.offset,
            importImage.memorySize,
            importObjectIndex,
            desc->objects[importObjectIndex].fd,
            importFd,
            usedNvFd ? "yes" : "no",
            (unsigned long long)modifier
        );
        LOG(
            "Import external plane %u mapping mode: linear=%s importWidth=%u objectSize=%u (descSize=%u minPlaneSize=%llu) buffer_direct_linear_444=%s",
            i,
            isLinearModifier ? "yes" : "no",
            importImage.width,
            importImage.memorySize,
            desc->objects[importObjectIndex].size,
            (unsigned long long)minPlaneSize,
            useBufferDirectForLinear444 ? "yes" : "no"
        );

        bool importedPlane = false;
        const bool enableNonLinearSharedExtMem =
            i > 0 &&
            !isLinearModifier &&
            isTruthyEnv(getenv("NVD_DIRECT_IMPORT_NONLINEAR_SHARE_EXTMEM_OBJECT0"));
        const bool tryShareExtMemFromPlane0 =
            backingImage->cudaImages[0].extMem != NULL &&
            importObjectIndex == planeObjectIndex[0] &&
            enableNonLinearSharedExtMem;
        if (backingImage->cudaImages[0].extMem != NULL &&
            enableNonLinearSharedExtMem &&
            importObjectIndex != planeObjectIndex[0] &&
            objectMatchesPlane0Backing) {
            LOG(
                "Import external plane %u shared extMem mapping skipped: descriptor object=%u differs from plane0 object=%u (same-backing is not sufficient)",
                i,
                importObjectIndex,
                planeObjectIndex[0]
            );
        }
        if (useBufferDirectForLinear444) {
            importedWithMappedBuffers = true;
            importedPlane =
                import_to_cuda_buffer(drv, &importImage, &backingImage->cudaImages[i]);
        } else if (tryShareExtMemFromPlane0) {
            LOG(
                "Import external plane %u trying shared extMem mapping from plane0 extMem=%p object=%u object0=%u same-backing=%s",
                i,
                (void *)backingImage->cudaImages[0].extMem,
                importObjectIndex,
                planeObjectIndex[0],
                objectMatchesPlane0Backing ? "yes" : "no"
            );
            if (map_external_memory_to_cuda_array(
                    drv,
                    backingImage->cudaImages[0].extMem,
                    &importImage,
                    8 * fmtInfo->bppc,
                    plane->channelCount,
                    &backingImage->cudaImages[i].mipmapArray,
                    &backingImage->arrays[i],
                    CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD,
                    &backingImage->cudaImages[0])) {
                backingImage->cudaImages[i].extMem = backingImage->cudaImages[0].extMem;
                backingImage->cudaImages[i].importedFd =
                    backingImage->cudaImages[0].importedFd;
                if (importFd >= 0) {
                    backendCloseTrackedNvKmsFd(importFd, "direct_import_shared_extmem_close");
                }
                importImage.nvFd = 0;
                importImage.nvFd2 = 0;
                importedPlane = true;
                LOG(
                    "Import external plane %u shared extMem mapping succeeded extMem=%p",
                    i,
                    (void *)backingImage->cudaImages[0].extMem
                );
            } else {
                LOG(
                    "Import external plane %u shared extMem mapping failed; falling back to plane-local import",
                    i
                );
            }
        }

        if (!importedPlane &&
            !import_to_cuda(drv, &importImage, 8 * fmtInfo->bppc, plane->channelCount, &backingImage->cudaImages[i], &backingImage->arrays[i])) {
            // import_to_cuda() transfers FD ownership to CUDA on successful
            // cuImportExternalMemory(). In that case it clears image->nvFd.
            // Closing |importFd| here would double-close an FD now owned by
            // CUDA and may trigger Chromium's FD ownership checks.
            if (importImage.nvFd != 0) {
                directCloseImportedPlaneFd(importImage.nvFd, "direct_import_plane_import_failed");
            }
            LOG(
                "Failed to import external plane %u into CUDA object=%u object_fd=%d import_fd=%d used_nvkms=%s visible=%ux%u import=%ux%u pitch=%u offset=%u obj_size=%u modifier=0x%llx",
                i,
                importObjectIndex,
                desc->objects[importObjectIndex].fd,
                importFd,
                usedNvFd ? "yes" : "no",
                planeWidth,
                planeHeight,
                importImage.width,
                importImage.height,
                importImage.pitch,
                importImage.offset,
                importImage.memorySize,
                (unsigned long long)desc->objects[importObjectIndex].drm_format_modifier
            );
            destroyBackingImage(drv, backingImage);
            if (surface->rtFormat == VA_RT_FORMAT_RGB32) {
                LOG(
                    "Direct-import failed for RGB32 external plane %u; refusing GPU-copy fallback to avoid stale output surfaces",
                    i
                );
                return false;
            }
            LOG(
                "Direct-import failed for external plane %u; trying GPU-copy fallback for this surface only",
                i
            );
            if (direct_importExternalSurfaceViaGpuCopy(
                    drv,
                    surface,
                    desc,
                    fmtInfo,
                    planeObjectIndex,
                    planeOffsets,
                    planePitches)) {
                return true;
            }
            if (!usedNvFd) {
                const uint64_t failedModifier =
                    desc->objects[importObjectIndex].drm_format_modifier;
                if (direct_isLinearModifier(failedModifier)) {
                    LOG(
                        "GPU-copy fallback failed for external plane %u without NVKMS fd; trying CPU-copy fallback",
                        i
                    );
                    return direct_importExternalSurfaceViaCpuCopy(
                        drv,
                        surface,
                        desc,
                        fmtInfo,
                        planeObjectIndex,
                        planeOffsets,
                        planePitches
                    );
                }
                LOG(
                    "GPU-copy fallback failed for external plane %u without NVKMS fd and modifier=0x%llx; CPU-copy fallback disabled for non-linear layouts",
                    i,
                    (unsigned long long)failedModifier
                );
            }
            return false;
        }

        const int dupFd = dup(desc->objects[importObjectIndex].fd);
        if (dupFd < 0) {
            LOG(
                "Failed to dup external object fd=%d for plane %u errno=%d",
                desc->objects[importObjectIndex].fd,
                i,
                errno
            );
            destroyBackingImage(drv, backingImage);
            return false;
        }
        backingImage->fds[i] = dupFd;
        backingImage->offsets[i] = planeOffsets[i];
        backingImage->strides[i] = planePitches[i];
        backingImage->mods[i] = desc->objects[importObjectIndex].drm_format_modifier;
        backingImage->size[i] = desc->objects[importObjectIndex].size;
    }

    direct_attachBackingImageToSurface(drv, surface, backingImage);
    LOG(
        importedWithMappedBuffers
            ? "Imported external surface %ux%u into CUDA buffer views fourcc=0x%x"
            : "Imported external surface %ux%u into CUDA arrays fourcc=0x%x",
        surface->width,
        surface->height,
        desc->fourcc
    );
    return true;
}

static bool direct_importExternalSurface(NVDriver *drv, NVSurface *surface, const VADRMPRIMESurfaceDescriptor *desc) {
    bool imported = false;
    directWatchCallerOwnedDescriptorFds(desc, "direct_import_begin");
    pthread_mutex_lock(&drv->exportMutex);
    imported = direct_importExternalSurfaceImpl(drv, surface, desc);
    pthread_mutex_unlock(&drv->exportMutex);
    directUnwatchCallerOwnedDescriptorFds(desc, "direct_import_end");
    return imported;
}

static bool direct_fillExportDescriptor(NVDriver *drv, NVSurface *surface, VADRMPRIMESurfaceDescriptor *desc) {
    (void)drv;
    const BackingImage *img = surface->backingImage;
    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];

    // VADRMPRIMESurfaceDescriptor::fourcc must be VA_FOURCC_*.
    // Per-layer DRM formats are provided in layers[].drm_format.
    desc->fourcc = fmtInfo->vaFormat.fourcc;
    desc->width = surface->width;
    desc->height = surface->height;

    desc->num_layers = fmtInfo->numPlanes;
    desc->num_objects = fmtInfo->numPlanes;

    int exportedFds[ARRAY_SIZE(desc->objects)];
    for (uint32_t i = 0; i < ARRAY_SIZE(exportedFds); i++) {
        exportedFds[i] = -1;
        desc->objects[i].fd = -1;
    }

    uint32_t exportedObjects = 0;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *plane = &fmtInfo->plane[i];
        const uint32_t planeHeight = img->height >> plane->ss.y;
        const uint64_t minPlaneSize =
            (uint64_t)img->offsets[i] + ((uint64_t)img->strides[i] * planeHeight);
        uint64_t objectSize = img->size[i];
        if (objectSize < minPlaneSize) {
            objectSize = minPlaneSize;
        }
        if (objectSize > UINT32_MAX) {
            objectSize = UINT32_MAX;
        }

        if (img->fds[i] < 0) {
            LOG("cannot export plane=%u: invalid source fd=%d", i, img->fds[i]);
            goto fail;
        }
        const int exportedFd = dup(img->fds[i]);
        if (exportedFd < 0) {
            LOG(
                "failed to duplicate export fd plane=%u src_fd=%d errno=%d",
                i,
                img->fds[i],
                errno
            );
            goto fail;
        }
        exportedFds[i] = exportedFd;
        desc->objects[i].size = (uint32_t)objectSize;
        desc->objects[i].drm_format_modifier = img->mods[i];
        exportedObjects++;

        desc->layers[i].drm_format = fmtInfo->plane[i].fourcc;
        desc->layers[i].num_planes = 1;
        desc->layers[i].object_index[0] = i;
        desc->layers[i].offset[0] = img->offsets[i];
        desc->layers[i].pitch[0] = img->strides[i];
    }

    for (uint32_t i = 0; i < exportedObjects; i++) {
        desc->objects[i].fd = exportedFds[i];
        exportedFds[i] = -1;
    }

    return true;

fail:
    for (uint32_t i = 0; i < exportedObjects; i++) {
        if (exportedFds[i] >= 0) {
            backendCloseFd(exportedFds[i], "direct_export_descriptor_fail");
            exportedFds[i] = -1;
        }
    }
    desc->num_objects = 0;
    desc->num_layers = 0;
    return false;
}

const NVBackend DIRECT_BACKEND = {
    .name = "direct",
    .initExporter = direct_initExporter,
    .releaseExporter = direct_releaseExporter,
    .exportCudaPtr = direct_exportCudaPtr,
    .importExternalSurface = direct_importExternalSurface,
    .detachBackingImageFromSurface = direct_detachBackingImageFromSurface,
    .realiseSurface = direct_realiseSurface,
    .fillExportDescriptor = direct_fillExportDescriptor,
    .destroyAllBackingImage = direct_destroyAllBackingImage
};
