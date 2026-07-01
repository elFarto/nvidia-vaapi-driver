#define _GNU_SOURCE 1

#include "../vabackend.h"
#include <stdio.h>
#include <stdlib.h>
#include <ffnvcodec/dynlink_loader.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include <string.h>
#include "../backend-common.h"

#include <drm.h>
#include <drm_fourcc.h>

static void destroyBackingImage(NVDriver *drv, BackingImage *img);

static bool isRgbSurfaceFourcc(uint32_t fourcc) {
    return fourcc == VA_FOURCC_ARGB ||
           fourcc == VA_FOURCC_XRGB ||
           fourcc == VA_FOURCC_ABGR ||
           fourcc == VA_FOURCC_XBGR ||
           fourcc == VA_FOURCC_RGBA ||
           fourcc == VA_FOURCC_RGBX ||
           fourcc == VA_FOURCC_BGRA ||
           fourcc == VA_FOURCC_BGRX;
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

static bool import_to_cuda(NVDriver *drv, NVDriverImage *image, int bpc, int channels, NVCudaImage *cudaImage, CUarray *array) {
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
        .type      = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD,
        .handle.fd = image->nvFd,
        .flags     = 0,
        .size      = image->memorySize
    };

    //LOG("importing memory size: %dx%d = %x", image->width, image->height, image->memorySize);

    CHECK_CUDA_RESULT_RETURN(drv->cu->cuImportExternalMemory(&cudaImage->extMem, &extMemDesc), false);

    //For some reason, this close *must* be *here*, otherwise we will get random visual glitches.
    close(image->nvFd2);
    image->nvFd = 0;
    image->nvFd2 = 0;

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
        .offset = 0
    };
    //create a mimap array from the imported memory
    CHECK_CUDA_RESULT_RETURN(drv->cu->cuExternalMemoryGetMappedMipmappedArray(&cudaImage->mipmapArray, cudaImage->extMem, &mipmapArrayDesc), false);

    //create an array from the mipmap array
    CHECK_CUDA_RESULT_RETURN(drv->cu->cuMipmappedArrayGetLevel(array, cudaImage->mipmapArray, 0), false);

    return true;
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
                close(fd);
                continue;
            }

            if (nvIdx != nvdGpu) {
                close(fd);
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

    return ret;
}

static void direct_releaseExporter(NVDriver *drv) {
    free_nvdriver(&drv->driverContext);
}

static void initBackingImageSync(BackingImage *img) {
    pthread_mutex_init(&img->mutex, NULL);
    pthread_cond_init(&img->cond, NULL);
    img->syncInitialized = true;
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

static void fillBackingImageClearRows(uint8_t *rows, size_t widthInBytes, uint32_t rowsCount, NVFormat format, uint32_t plane) {
    if (format == NV_FORMAT_ARGB) {
        for (uint32_t y = 0; y < rowsCount; y++) {
            uint8_t *row = rows + (size_t) y * widthInBytes;
            for (size_t x = 0; x + 3 < widthInBytes; x += 4) {
                row[x] = 0;
                row[x + 1] = 0;
                row[x + 2] = 0;
                row[x + 3] = 0xff;
            }
        }
        return;
    }

    if (formatsInfo[format].bppc == 1) {
        memset(rows, plane == 0 ? 16 : 128, widthInBytes * rowsCount);
        return;
    }

    const uint16_t value = plane == 0 ? 0x1000 : 0x8000;
    uint16_t *samples = (uint16_t*) rows;
    const size_t sampleCount = widthInBytes * rowsCount / sizeof(uint16_t);
    for (size_t i = 0; i < sampleCount; i++) {
        samples[i] = value;
    }
}

static bool clearBackingImagePlane(NVDriver *drv, BackingImage *img, uint32_t plane) {
    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];
    const NVFormatPlane *p = &fmtInfo->plane[plane];
    const uint32_t width = img->width >> p->ss.x;
    const uint32_t height = img->height >> p->ss.y;
    const size_t widthInBytes = (size_t) width * fmtInfo->bppc * p->channelCount;
    if (widthInBytes == 0 || height == 0) {
        return true;
    }

    const size_t maxChunkBytes = 8 * 1024 * 1024;
    uint32_t chunkRows = (uint32_t) (maxChunkBytes / widthInBytes);
    if (chunkRows == 0) {
        chunkRows = 1;
    }
    if (chunkRows > height) {
        chunkRows = height;
    }

    uint8_t *rows = NULL;
    while (chunkRows > 0) {
        rows = malloc(widthInBytes * chunkRows);
        if (rows != NULL) {
            break;
        }
        chunkRows /= 2;
    }
    if (rows == NULL) {
        LOG("Unable to allocate staging memory to clear BackingImage plane");
        return false;
    }

    fillBackingImageClearRows(rows, widthInBytes, chunkRows, img->format, plane);

    bool failed = false;
    for (uint32_t y = 0; y < height; y += chunkRows) {
        const uint32_t remainingRows = height - y;
        const uint32_t rowsToCopy = chunkRows < remainingRows ? chunkRows : remainingRows;
        CUDA_MEMCPY2D cpy = {
            .srcMemoryType = CU_MEMORYTYPE_HOST,
            .srcHost = rows,
            .srcPitch = widthInBytes,
            .dstMemoryType = CU_MEMORYTYPE_ARRAY,
            .dstArray = img->arrays[plane],
            .dstY = y,
            .WidthInBytes = widthInBytes,
            .Height = rowsToCopy
        };
        if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&cpy))) {
            failed = true;
            break;
        }
    }

    free(rows);
    return !failed;
}

static bool clearBackingImage(NVDriver *drv, BackingImage *img) {
    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (!clearBackingImagePlane(drv, img, i)) {
            return false;
        }
    }
    return true;
}

static uint64_t backingImageMemorySize(const BackingImage *img) {
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

static bool backingImageCanPrune(const BackingImage *img) {
    return img != NULL &&
           img->surface == NULL &&
           atomic_load(&img->borrowCount) == 0;
}

static bool detachedBackingImagesOverLimit(uint64_t bytes, uint32_t count, const NVDriver *drv) {
    if (count == 0) {
        return false;
    }
    if (drv->maxDetachedBackingImages == 0 || drv->maxDetachedBackingImageBytes == 0) {
        return true;
    }
    return count > drv->maxDetachedBackingImages ||
           bytes > drv->maxDetachedBackingImageBytes;
}

static bool pruneOldestDetachedBackingImageLocked(NVDriver *drv, uint64_t *bytes, uint32_t *count) {
    uint32_t pruneIndex = UINT32_MAX;
    uint64_t oldestSerial = UINT64_MAX;

    ARRAY_FOR_EACH(BackingImage*, img, &drv->images)
        if (backingImageCanPrune(img) && img->detachedSerial < oldestSerial) {
            pruneIndex = img_idx;
            oldestSerial = img->detachedSerial;
        }
    END_FOR_EACH

    if (pruneIndex == UINT32_MAX) {
        return false;
    }

    BackingImage *img = get_element_at(&drv->images, pruneIndex);
    uint64_t imageBytes = backingImageMemorySize(img);
    destroyBackingImage(drv, img);
    remove_element_at(&drv->images, pruneIndex);
    if (*bytes >= imageBytes) {
        *bytes -= imageBytes;
    } else {
        *bytes = 0;
    }
    if (*count > 0) {
        (*count)--;
    }
    return true;
}

static void pruneDetachedBackingImagesToLimits(NVDriver *drv) {
    uint64_t bytes = 0;
    uint32_t count = 0;

    pthread_mutex_lock(&drv->imagesMutex);

    ARRAY_FOR_EACH(BackingImage*, img, &drv->images)
        if (backingImageCanPrune(img)) {
            bytes += backingImageMemorySize(img);
            count++;
        }
    END_FOR_EACH

    while (detachedBackingImagesOverLimit(bytes, count, drv)) {
        if (!pruneOldestDetachedBackingImageLocked(drv, &bytes, &count)) {
            break;
        }
    }

    pthread_mutex_unlock(&drv->imagesMutex);
}

static BackingImage *direct_allocateBackingImage_single(NVDriver *drv, NVSurface *surface) {
    NVDriverImage driverImages[3] = { 0 };
    BackingImage *backingImage = calloc(1, sizeof(BackingImage));
    if (backingImage == NULL) {
        return NULL;
    }
    initBackingImageSync(backingImage);

    backingImage->isSingleBuffer = true;
    for (int i = 0; i < 4; i++) {
        backingImage->fds[i] = -1;
    }

    if (isRgbSurfaceFourcc((uint32_t) surface->fourcc)) {
        backingImage->format = NV_FORMAT_ARGB;
    } else {
    switch (surface->format)
    {
    case cudaVideoSurfaceFormat_P016:
        switch (surface->bitDepth) {
        case 10:
            backingImage->format = NV_FORMAT_P010;
            break;
        case 12:
            backingImage->format = NV_FORMAT_P012;
            break;
        default:
            backingImage->format = NV_FORMAT_P016;
            break;
        }
        break;

    case cudaVideoSurfaceFormat_YUV444_16Bit:
        backingImage->format = NV_FORMAT_Q416;
        break;

    case cudaVideoSurfaceFormat_YUV444:
        backingImage->format = NV_FORMAT_444P;
        break;

    default:
        backingImage->format = NV_FORMAT_NV12;
        break;
    }
    }

    const NVFormatInfo *fmtInfo = &formatsInfo[backingImage->format];

    backingImage->totalSize = calculate_unified_image_layout(&drv->driverContext, driverImages, surface->width, surface->height,
                                                             fmtInfo->bppc, fmtInfo->numPlanes, fmtInfo->plane);
    LOG_DEBUG("Allocating single BackingImage: %p %ux%u = %u bytes", backingImage, surface->width, surface->height, backingImage->totalSize);

    int memFd = -1;
    int memFd2 = -1;
    int drmFd = -1;
    if (!alloc_buffer(&drv->driverContext, backingImage->totalSize, driverImages, &memFd, &memFd2, &drmFd)) {
        goto fail;
    }
    LOG_DEBUG("Allocate single Buffer: %d %d %d", memFd, memFd2, drmFd);

    const CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
        .type      = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD,
        .handle.fd = memFd,
        .flags     = 0,
        .size      = backingImage->totalSize
    };

    LOG_DEBUG("Importing single memory to CUDA");
    if (CHECK_CUDA_RESULT(drv->cu->cuImportExternalMemory(&backingImage->extMem, &extMemDesc))) {
        goto fail;
    }

    close(memFd2);
    memFd = -1;
    memFd2 = -1;

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        // The single buffer is exported under one DRM modifier that carries a
        // single block height (log2GobsPerBlockY, the max across all planes).
        // CUDA, however, derives a plane's block-linear layout from the array
        // height it is handed, so a shorter plane (e.g. NV12 chroma when the
        // coded height is ~86-170px, as at 144p) would be tiled with a smaller
        // block than the modifier advertises. The importer then detiles that
        // plane with the wrong block height and the chroma turns green. Create
        // the array at the block-aligned height (memorySize / pitch) so CUDA
        // lays every plane out with the same block height the modifier reports.
        const uint32_t alignedHeight = driverImages[i].pitch != 0 ?
            driverImages[i].memorySize / driverImages[i].pitch : driverImages[i].height;
        CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC mipmapArrayDesc = {
            .arrayDesc = {
                .Width = driverImages[i].width,
                .Height = alignedHeight,
                .Depth = 0,
                .Format = fmtInfo->bppc == 1 ? CU_AD_FORMAT_UNSIGNED_INT8 : CU_AD_FORMAT_UNSIGNED_INT16,
                .NumChannels = fmtInfo->plane[i].channelCount,
                .Flags = 0
            },
            .numLevels = 1,
            .offset = driverImages[i].offset
        };

        if (CHECK_CUDA_RESULT(drv->cu->cuExternalMemoryGetMappedMipmappedArray(&backingImage->cudaImages[i].mipmapArray, backingImage->extMem, &mipmapArrayDesc))) {
            goto fail;
        }

        if (CHECK_CUDA_RESULT(drv->cu->cuMipmappedArrayGetLevel(&backingImage->arrays[i], backingImage->cudaImages[i].mipmapArray, 0))) {
            goto fail;
        }
    }

    backingImage->width = surface->width;
    backingImage->height = surface->height;
    backingImage->fourcc = fmtInfo->fourcc;
    backingImage->fds[0] = drmFd;
    drmFd = -1;
    cacheBackingImageFdStat(backingImage, 0);
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        backingImage->strides[i] = driverImages[i].pitch;
        backingImage->mods[i] = driverImages[i].mods;
        backingImage->offsets[i] = driverImages[i].offset;
        backingImage->size[i] = driverImages[i].memorySize;
    }
    if (!clearBackingImage(drv, backingImage)) {
        goto fail;
    }

    return backingImage;

fail:
    if (memFd >= 0) {
        close(memFd);
    }
    if (memFd2 >= 0) {
        close(memFd2);
    }
    if (drmFd >= 0) {
        close(drmFd);
    }

    destroyBackingImage(drv, backingImage);
    return NULL;
}

static BackingImage *direct_allocateBackingImage(NVDriver *drv, NVSurface *surface) {
    // Multi-plane YUV surfaces must be exported as a single buffer holding every
    // plane at an offset, so all planes share one DRM modifier. Chromium's
    // vaapi_wrapper enforces one-modifier-per-buffer, so a per-plane export (a
    // distinct modifier per fd) trips its CHECK and aborts the GPU process.
    // Single-plane / packed surfaces (e.g. RGB) have nothing to unify and use the
    // straightforward per-plane allocator below.
    if (!isRgbSurfaceFourcc((uint32_t) surface->fourcc)) {
        return direct_allocateBackingImage_single(drv, surface);
    }

    NVDriverImage driverImages[3] = { 0 };
    BackingImage *backingImage = calloc(1, sizeof(BackingImage));
    if (backingImage == NULL) {
        return NULL;
    }
    initBackingImageSync(backingImage);
    for (int i = 0; i < 4; i++) {
        backingImage->fds[i] = -1;
    }

    if (isRgbSurfaceFourcc((uint32_t) surface->fourcc)) {
        backingImage->format = NV_FORMAT_ARGB;
    } else {
    switch (surface->format)
    {
    case cudaVideoSurfaceFormat_P016:
        switch (surface->bitDepth) {
        case 10:
            backingImage->format = NV_FORMAT_P010;
            break;
        case 12:
            backingImage->format = NV_FORMAT_P012;
            break;
        default:
            backingImage->format = NV_FORMAT_P016;
            break;
        }
        break;

    case cudaVideoSurfaceFormat_YUV444_16Bit:
        backingImage->format = NV_FORMAT_Q416;
        break;

    case cudaVideoSurfaceFormat_YUV444:
        backingImage->format = NV_FORMAT_444P;
        break;
    
    default:
        backingImage->format = NV_FORMAT_NV12;
        break;
    }
    }

    const NVFormatInfo *fmtInfo = &formatsInfo[backingImage->format];
    const NVFormatPlane *p = fmtInfo->plane;

    LOG_DEBUG("Allocating BackingImages: %p %dx%d", backingImage, surface->width, surface->height);
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        alloc_image(&drv->driverContext, surface->width >> p[i].ss.x, surface->height >> p[i].ss.y,
                    p[i].channelCount, 8 * fmtInfo->bppc, p[i].fourcc, &driverImages[i]);
    }

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (!import_to_cuda(drv, &driverImages[i], 8 * fmtInfo->bppc, p[i].channelCount, &backingImage->cudaImages[i], &backingImage->arrays[i]))
            goto bail;
    }

    backingImage->width = surface->width;
    backingImage->height = surface->height;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        backingImage->fds[i] = driverImages[i].drmFd;
        cacheBackingImageFdStat(backingImage, (int) i);
        backingImage->strides[i] = driverImages[i].pitch;
        backingImage->mods[i] = driverImages[i].mods;
        backingImage->size[i] = driverImages[i].memorySize;
    }

    return backingImage;

bail:
    //another 'free' might occur on this pointer.
    //hence, set it to NULL to ensure no operation is performed if this really happens.
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (driverImages[i].nvFd != 0) {
            close(driverImages[i].nvFd);
        }
        if (driverImages[i].nvFd2 != 0) {
            close(driverImages[i].nvFd2);
        }
        if (driverImages[i].drmFd != 0) {
            close(driverImages[i].drmFd);
        }
    }

    if (backingImage != NULL) {
        free(backingImage);
    }

    return NULL;
}

static void destroyBackingImage(NVDriver *drv, BackingImage *img) {
    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];
    if (img->surface != NULL) {
        img->surface->backingImage = NULL;
    }
    if (img->borrowedBackingImage != NULL && atomic_load(&img->borrowedBackingImage->borrowCount) > 0) {
        atomic_fetch_sub(&img->borrowedBackingImage->borrowCount, 1);
        img->borrowedBackingImage = NULL;
    }

    if (img->externalMapping != NULL) {
        munmap(img->externalMapping, img->externalMappingSize);
        img->externalMapping = NULL;
        img->externalMappingSize = 0;
    }
    if (img->externalDevicePtr != 0) {
        CHECK_CUDA_RESULT(drv->cu->cuMemFree(img->externalDevicePtr));
        img->externalDevicePtr = 0;
        img->externalDeviceSize = 0;
    }

    for (int i = 0; i < 4; i++) {
        if (img->fds[i] >= 0) {
            close(img->fds[i]);
        }
    }

    if (!img->borrowedCudaResources) {
        for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
            if (img->arrays[i] != NULL) {
                CHECK_CUDA_RESULT(drv->cu->cuArrayDestroy(img->arrays[i]));
            }

            if (img->cudaImages[i].mipmapArray != NULL) {
                CHECK_CUDA_RESULT(drv->cu->cuMipmappedArrayDestroy(img->cudaImages[i].mipmapArray));
            }
        }

        if (img->isSingleBuffer) {
            if (img->extMem != NULL) {
                CHECK_CUDA_RESULT(drv->cu->cuDestroyExternalMemory(img->extMem));
            }
        } else {
            for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
                if (img->cudaImages[i].extMem != NULL) {
                    CHECK_CUDA_RESULT(drv->cu->cuDestroyExternalMemory(img->cudaImages[i].extMem));
                }
            }
        }
    }
    if (img->syncInitialized) {
        pthread_cond_destroy(&img->cond);
        pthread_mutex_destroy(&img->mutex);
    }

    memset(img, 0, sizeof(BackingImage));
    free(img);
}

static bool pruneDetachedBackingImages(NVDriver *drv) {
    bool pruned = false;

    pthread_mutex_lock(&drv->imagesMutex);

    ARRAY_FOR_EACH_REV(BackingImage*, it, &drv->images)
        if (it->surface == NULL && atomic_load(&it->borrowCount) == 0) {
            destroyBackingImage(drv, it);
            remove_element_at(&drv->images, it_idx);
            pruned = true;
        }
    END_FOR_EACH

    pthread_mutex_unlock(&drv->imagesMutex);

    return pruned;
}

static void direct_attachBackingImageToSurface(NVSurface *surface, BackingImage *img) {
    surface->backingImage = img;
    img->surface = surface;
    img->detachedSerial = 0;
    nvBackingImageStoreSurfaceColorMetadata(img, surface);
}

static void direct_detachBackingImageFromSurface(NVDriver *drv, NVSurface *surface) {
    if (surface->backingImage == NULL) {
        return;
    }

    if (surface->backingImage->isExternalBuffer || surface->backingImage->borrowedCudaResources) {
        destroyBackingImage(drv, surface->backingImage);
        surface->backingImage = NULL;
        return;
    }

    surface->backingImage->surface = NULL;
    surface->backingImage->detachedSerial = ++drv->detachedBackingImageSerial;
    surface->backingImage = NULL;
    pruneDetachedBackingImagesToLimits(drv);
}

static void direct_destroyAllBackingImage(NVDriver *drv) {
    pthread_mutex_lock(&drv->imagesMutex);

    ARRAY_FOR_EACH_REV(BackingImage*, it, &drv->images)
        destroyBackingImage(drv, it);
        remove_element_at(&drv->images, it_idx);
    END_FOR_EACH

    pthread_mutex_unlock(&drv->imagesMutex);
}

static bool copyFrameToSurface(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch) {
    const NVFormatInfo *fmtInfo = &formatsInfo[surface->backingImage->format];
    uint32_t y = 0;

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *p = &fmtInfo->plane[i];
        const uint32_t widthInBytes = (surface->width >> p->ss.x) * fmtInfo->bppc * p->channelCount;
        const uint32_t height = surface->height >> p->ss.y;
        if (surface->backingImage->externalMapping != NULL) {
            uint8_t *plane = malloc((size_t) widthInBytes * height);
            if (plane == NULL) {
                return false;
            }
            CUDA_MEMCPY2D cpy = {
                .srcMemoryType = CU_MEMORYTYPE_DEVICE,
                .srcDevice = ptr,
                .srcY = y,
                .srcPitch = pitch,
                .dstMemoryType = CU_MEMORYTYPE_HOST,
                .dstHost = plane,
                .dstPitch = widthInBytes,
                .Height = height,
                .WidthInBytes = widthInBytes
            };
            bool failed = CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&cpy));
            if (!failed) {
                uint8_t *dst = (uint8_t*) surface->backingImage->externalMapping + surface->backingImage->offsets[i];
                for (uint32_t row = 0; row < height; row++) {
                    memcpy(dst + (size_t) row * surface->backingImage->strides[i],
                           plane + (size_t) row * widthInBytes,
                           widthInBytes);
                }
            }
            free(plane);
            if (failed) {
                return false;
            }
            y += height;
            continue;
        }

        CUDA_MEMCPY2D cpy = {
            .srcMemoryType = CU_MEMORYTYPE_DEVICE,
            .srcDevice = ptr,
            .srcY = y,
            .srcPitch = pitch,
            .dstMemoryType = CU_MEMORYTYPE_ARRAY,
            .dstArray = surface->backingImage->arrays[i],
            .Height = height,
            .WidthInBytes = widthInBytes
        };
        if (i == fmtInfo->numPlanes - 1) {
            CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&cpy));
        } else {
            CHECK_CUDA_RESULT(drv->cu->cuMemcpy2DAsync(&cpy, 0));
        }
        y += height;
    }

    //notify anyone waiting for us to be resolved
    pthread_mutex_lock(&surface->mutex);
    surface->resolving = 0;
    pthread_cond_broadcast(&surface->cond);
    pthread_mutex_unlock(&surface->mutex);

    return true;
}

static bool direct_realiseSurface(NVDriver *drv, NVSurface *surface) {
    //make sure we're the only thread updating this surface
    pthread_mutex_lock(&surface->mutex);
    //check again to see if it's just been created
    if (surface->backingImage == NULL) {
        //try to find a free surface
        BackingImage *img = direct_allocateBackingImage(drv, surface);
        if (img == NULL) {
            if (pruneDetachedBackingImages(drv)) {
                LOG("Pruned detached BackingImages after allocation failure")
                img = direct_allocateBackingImage(drv, surface);
            }
            if (img == NULL) {
                LOG("Unable to realise surface: %p (%d)", surface, surface->pictureIdx)
                pthread_mutex_unlock(&surface->mutex);
                return false;
            }
        }

        direct_attachBackingImageToSurface(surface, img);
        pthread_mutex_lock(&drv->imagesMutex);
        add_element(&drv->images, img);
        pthread_mutex_unlock(&drv->imagesMutex);
    }
    pthread_mutex_unlock(&surface->mutex);

    return true;
}

static BackingImage *resolveSyncImage(BackingImage *img) {
    if (img != NULL && img->borrowedBackingImage != NULL) {
        return img->borrowedBackingImage;
    }
    return img;
}

static void finishSurfaceResolve(NVSurface *surface) {
    pthread_mutex_lock(&surface->mutex);
    surface->resolving = 0;
    pthread_cond_broadcast(&surface->cond);
    pthread_mutex_unlock(&surface->mutex);
}

static bool direct_exportCudaPtr(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch) {
    if (!direct_realiseSurface(drv, surface)) {
        finishSurfaceResolve(surface);
        return false;
    }

    if (ptr != 0) {
        BackingImage *img = surface->backingImage;
        BackingImage *syncImg = resolveSyncImage(img);
        if (syncImg != NULL && syncImg->syncInitialized) {
            pthread_mutex_lock(&syncImg->mutex);
            syncImg->resolving = true;
            pthread_mutex_unlock(&syncImg->mutex);
        }
        nvStatsIncrement(drv, NV_STAT_EXPORT_COPIES);
        if (img != NULL && img->externalMapping != NULL) {
            nvStatsIncrement(drv, NV_STAT_EXPORT_HOST_COPIES);
        }
        nvBackingImageStoreSurfaceColorMetadata(img, surface);
        bool copied = copyFrameToSurface(drv, ptr, surface, pitch);
        if (syncImg != NULL && syncImg->syncInitialized) {
            pthread_mutex_lock(&syncImg->mutex);
            syncImg->resolving = false;
            pthread_cond_broadcast(&syncImg->cond);
            pthread_mutex_unlock(&syncImg->mutex);
        }
        if (!copied) {
            finishSurfaceResolve(surface);
            return false;
        }
    } else {
        LOG("exporting with null ptr")
    }

    return true;
}

static bool direct_fillExportDescriptor(NVDriver *drv, NVSurface *surface, VADRMPRIMESurfaceDescriptor *desc) {
    const BackingImage *img = surface->backingImage;
    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];

    nvBackingImageStoreSurfaceColorMetadata(surface->backingImage, surface);

    desc->fourcc = fmtInfo->fourcc;
    desc->width = surface->width;
    desc->height = surface->height;

    desc->num_layers = fmtInfo->numPlanes;
    nvStatsIncrement(drv, NV_STAT_EXPORT_DESCRIPTORS);
    if (img->isSingleBuffer) {
        nvStatsIncrement(drv, NV_STAT_EXPORT_DESCRIPTORS_SINGLE);
        desc->num_objects = 1;
        desc->objects[0].fd = dup(img->fds[0]);
        desc->objects[0].size = img->totalSize;
        desc->objects[0].drm_format_modifier = img->mods[0];

        for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
            desc->layers[i].drm_format = fmtInfo->plane[i].fourcc;
            desc->layers[i].num_planes = 1;
            desc->layers[i].object_index[0] = 0;
            desc->layers[i].offset[0] = img->offsets[i];
            desc->layers[i].pitch[0] = img->strides[i];
        }
    } else {
        nvStatsIncrement(drv, NV_STAT_EXPORT_DESCRIPTORS_MULTI);
        desc->num_objects = fmtInfo->numPlanes;

        for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
            desc->objects[i].fd = dup(img->fds[i]);
            desc->objects[i].size = img->size[i];
            desc->objects[i].drm_format_modifier = img->mods[i];

            desc->layers[i].drm_format = fmtInfo->plane[i].fourcc;
            desc->layers[i].num_planes = 1;
            desc->layers[i].object_index[0] = i;
            desc->layers[i].offset[0] = img->offsets[i];
            desc->layers[i].pitch[0] = img->strides[i];
        }
    }

    return true;
}

const NVBackend DIRECT_BACKEND = {
    .name = "direct",
    .initExporter = direct_initExporter,
    .releaseExporter = direct_releaseExporter,
    .exportCudaPtr = direct_exportCudaPtr,
    .detachBackingImageFromSurface = direct_detachBackingImageFromSurface,
    .realiseSurface = direct_realiseSurface,
    .fillExportDescriptor = direct_fillExportDescriptor,
    .destroyAllBackingImage = direct_destroyAllBackingImage
};
