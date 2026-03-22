#define _GNU_SOURCE 1

#include "../vabackend.h"
#include "../external-surface.h"
#include <stdio.h>
#include <ffnvcodec/dynlink_loader.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include <string.h>
#include "../backend-common.h"

#include <drm.h>
#include <drm_fourcc.h>

static void destroyBackingImage(NVDriver *drv, BackingImage *img);

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

static bool direct_import_blocklinear_to_cuda_arrays(NVDriver *drv,
                                                     NVDriverImage *image,
                                                     BackingImage *backingImage,
                                                     const NVFormatInfo *fmtInfo)
{
    size_t plane_offset = 0;

    CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
        .type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD,
        .handle.fd = image->nvFd,
        .flags = 0,
        .size = image->memorySize
    };

    if (CHECK_CUDA_RESULT(drv->cu->cuImportExternalMemory(&backingImage->cudaImages[0].extMem,
                                                          &extMemDesc))) {
        return false;
    }

    if (image->nvFd2 != 0) {
        close(image->nvFd2);
    }
    image->nvFd = 0;
    image->nvFd2 = 0;

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *p = &fmtInfo->plane[i];
        uint32_t plane_width = backingImage->width >> p->ss.x;
        uint32_t plane_height = backingImage->height >> p->ss.y;
        size_t plane_size = (size_t)image->pitch * plane_height;
        CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC mipmapArrayDesc = {
            .arrayDesc = {
                .Width = plane_width,
                .Height = plane_height,
                .Depth = 0,
                .Format = fmtInfo->bppc == 1 ? CU_AD_FORMAT_UNSIGNED_INT8
                                             : CU_AD_FORMAT_UNSIGNED_INT16,
                .NumChannels = p->channelCount,
                .Flags = 0
            },
            .numLevels = 1,
            .offset = plane_offset
        };

        if (plane_offset > INT_MAX || plane_size > image->memorySize ||
            plane_offset > image->memorySize - plane_size) {
            return false;
        }

        backingImage->offsets[i] = (int)plane_offset;
        backingImage->strides[i] = image->pitch;

        if (CHECK_CUDA_RESULT(drv->cu->cuExternalMemoryGetMappedMipmappedArray(
                &backingImage->cudaImages[i].mipmapArray,
                backingImage->cudaImages[0].extMem,
                &mipmapArrayDesc))) {
            return false;
        }

        if (CHECK_CUDA_RESULT(drv->cu->cuMipmappedArrayGetLevel(&backingImage->arrays[i],
                                                                backingImage->cudaImages[i].mipmapArray,
                                                                0))) {
            drv->cu->cuMipmappedArrayDestroy(backingImage->cudaImages[i].mipmapArray);
            backingImage->cudaImages[i].mipmapArray = NULL;
            return false;
        }

        plane_offset += plane_size;
    }

    return true;
}

static bool direct_debug_enabled(void)
{
    static int cached = -1;
    const char *value;

    if (cached != -1) {
        return cached == 1;
    }

    value = getenv("NVD_DIRECT_DEBUG");
    cached = (value != NULL && value[0] != '\0' && strcmp(value, "0") != 0) ? 1 : 0;
    return cached == 1;
}

static void debug(EGLenum error,const char *command,EGLint messageType,EGLLabelKHR threadLabel,EGLLabelKHR objectLabel,const char* message) {
    (void)error;
    (void)messageType;
    (void)threadLabel;
    (void)objectLabel;
    LOG("[EGL] %s: %s", command, message);
}

static bool direct_initExporter(NVDriver *drv) {
    if (direct_debug_enabled()) {
        /*
         * The driver used to install a process-wide EGL debug callback
         * unconditionally. Keep it available for targeted debugging, but
         * leave it disabled in normal runs so unrelated client EGL errors do
         * not flood production logs.
         */
        static const EGLAttrib debugAttribs[] = {
            EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE,
            EGL_DEBUG_MSG_INFO_KHR, EGL_TRUE,
            EGL_NONE
        };
        const PFNEGLDEBUGMESSAGECONTROLKHRPROC eglDebugMessageControlKHR =
            (PFNEGLDEBUGMESSAGECONTROLKHRPROC) eglGetProcAddress("eglDebugMessageControlKHR");

        if (eglDebugMessageControlKHR != NULL) {
            eglDebugMessageControlKHR(debug, debugAttribs);
        }
    }

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
            if (direct_debug_enabled()) {
                LOG("Searching for GPU: %d %d %d", nvIdx, nvdGpu, drmIdx)
            }
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

static bool direct_format_prefers_linear_export(NVFormat format) {
    switch (format) {
    case NV_FORMAT_NV12:
    case NV_FORMAT_P010:
    case NV_FORMAT_P012:
    case NV_FORMAT_P016:
        return true;
    default:
        return false;
    }
}

static bool import_linear_to_cuda(NVDriver *drv, NVDriverImage *image, NVCudaImage *cudaImage, CUdeviceptr *ptr) {
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
        .type      = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD,
        .handle.fd = image->nvFd,
        .flags     = 0,
        .size      = image->memorySize
    };
    CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufDesc = {
        .offset = 0,
        .size = image->memorySize
    };

    CHECK_CUDA_RESULT_RETURN(drv->cu->cuImportExternalMemory(&cudaImage->extMem, &extMemDesc), false);

    if (image->nvFd2 != 0) {
        close(image->nvFd2);
    }
    image->nvFd = 0;
    image->nvFd2 = 0;

    if (CHECK_CUDA_RESULT(drv->cu->cuExternalMemoryGetMappedBuffer(ptr, cudaImage->extMem, &bufDesc))) {
        CHECK_CUDA_RESULT_RETURN(drv->cu->cuDestroyExternalMemory(cudaImage->extMem), false);
        cudaImage->extMem = NULL;
        return false;
    }

    return true;
}

static bool direct_format_prefers_combined_blocklinear_export(NVFormat format)
{
    return direct_format_prefers_linear_export(format);
}

static BackingImage *direct_allocateLinearBackingImage(NVDriver *drv, NVSurface *surface, NVFormat format) {
    NVDriverImage driverImage = { 0 };
    BackingImage *backingImage = calloc(1, sizeof(BackingImage));
    const NVFormatInfo *fmtInfo = &formatsInfo[format];
    size_t total_size = 0;

    if (!backingImage) {
        return NULL;
    }

    backingImage->format = format;
    backingImage->width = surface->width;
    backingImage->height = surface->height;
    backingImage->numObjects = 1;
    backingImage->numLayers = fmtInfo->numPlanes;

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *p = &fmtInfo->plane[i];
        size_t plane_width = (size_t)(surface->width >> p->ss.x) * fmtInfo->bppc * p->channelCount;
        size_t plane_height = surface->height >> p->ss.y;
        size_t plane_stride = ROUND_UP(plane_width, 256);

        if (plane_stride > INT_MAX || total_size > INT_MAX || total_size > UINT32_MAX) {
            goto bail;
        }

        backingImage->offsets[i] = (int)total_size;
        backingImage->strides[i] = (int)plane_stride;
        backingImage->layerDrmFormats[i] = nvGetExportLayerDrmFormat(format, i, fmtInfo->plane[i].fourcc);
        backingImage->layerNumPlanes[i] = 1;
        backingImage->planeObjectIndices[i] = 0;
        backingImage->planeLayerIndices[i] = i;
        backingImage->planeIndicesInLayer[i] = 0;
        if (direct_debug_enabled()) {
            LOG("linear plane %u offset=%d stride=%d", i, backingImage->offsets[i], backingImage->strides[i]);
        }

        total_size += plane_stride * plane_height;
        if (total_size > UINT32_MAX) {
            goto bail;
        }
    }

    if (direct_debug_enabled()) {
        LOG("Allocating linear BackingImage: %p %dx%d", backingImage, surface->width, surface->height);
    }
    if (!alloc_linear_buffer(&drv->driverContext, (uint32_t)total_size, &driverImage)) {
        goto bail;
    }
    if (!import_linear_to_cuda(drv, &driverImage, &backingImage->cudaImages[0], &backingImage->linearPtr)) {
        goto bail;
    }

    backingImage->linearSize = driverImage.memorySize;
    backingImage->fds[0] = driverImage.drmFd;
    backingImage->size[0] = driverImage.memorySize;
    backingImage->mods[0] = driverImage.mods;

    return backingImage;

bail:
    if (driverImage.nvFd != 0) {
        close(driverImage.nvFd);
    }
    if (driverImage.nvFd2 != 0) {
        close(driverImage.nvFd2);
    }
    if (driverImage.drmFd != 0) {
        close(driverImage.drmFd);
    }
    if (backingImage != NULL) {
        if (backingImage->linearPtr != 0) {
            CHECK_CUDA_RESULT(drv->cu->cuMemFree(backingImage->linearPtr));
        }
        if (backingImage->cudaImages[0].extMem != NULL) {
            CHECK_CUDA_RESULT(drv->cu->cuDestroyExternalMemory(backingImage->cudaImages[0].extMem));
        }
        free(backingImage);
    }

    return NULL;
}

static BackingImage *direct_allocateCombinedBlockLinearBackingImage(NVDriver *drv,
                                                                    NVSurface *surface,
                                                                    NVFormat format)
{
    NVDriverImage driverImage = { 0 };
    BackingImage *backingImage = calloc(1, sizeof(BackingImage));
    const NVFormatInfo *fmtInfo = &formatsInfo[format];
    uint32_t combined_height = 0;

    if (!backingImage || fmtInfo->numPlanes != 2) {
        free(backingImage);
        return NULL;
    }

    backingImage->format = format;
    backingImage->width = surface->width;
    backingImage->height = surface->height;
    backingImage->numObjects = 1;
    backingImage->numLayers = fmtInfo->numPlanes;

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        combined_height += surface->height >> fmtInfo->plane[i].ss.y;
        backingImage->layerDrmFormats[i] =
            nvGetExportLayerDrmFormat(format, i, fmtInfo->plane[i].fourcc);
        backingImage->layerNumPlanes[i] = 1;
        backingImage->planeObjectIndices[i] = 0;
        backingImage->planeLayerIndices[i] = i;
        backingImage->planeIndicesInLayer[i] = 0;
    }

    if (!alloc_image(&drv->driverContext,
                     surface->width,
                     combined_height,
                     1,
                     8 * fmtInfo->bppc,
                     fmtInfo->plane[0].fourcc,
                     &driverImage)) {
        goto bail;
    }

    backingImage->fds[0] = driverImage.drmFd;
    backingImage->mods[0] = driverImage.mods;
    backingImage->size[0] = driverImage.memorySize;
    driverImage.drmFd = 0;

    if (!direct_import_blocklinear_to_cuda_arrays(drv, &driverImage, backingImage, fmtInfo)) {
        goto bail;
    }

    return backingImage;

bail:
    if (driverImage.nvFd != 0) {
        close(driverImage.nvFd);
    }
    if (driverImage.nvFd2 != 0) {
        close(driverImage.nvFd2);
    }
    if (driverImage.drmFd != 0) {
        close(driverImage.drmFd);
    }
    if (backingImage != NULL) {
        destroyBackingImage(drv, backingImage);
    }
    return NULL;
}

static BackingImage *direct_allocateBackingImage(NVDriver *drv, NVSurface *surface) {
    NVDriverImage driverImages[3] = { 0 };
    BackingImage *backingImage = NULL;
    NVFormat format;

    switch (surface->format)
    {
    case cudaVideoSurfaceFormat_P016:
        switch (surface->bitDepth) {
        case 10:
            format = NV_FORMAT_P010;
            break;
        case 12:
            format = NV_FORMAT_P012;
            break;
        default:
            format = NV_FORMAT_P016;
            break;
        }
        break;

    case cudaVideoSurfaceFormat_YUV444_16Bit:
        format = NV_FORMAT_Q416;
        break;

    case cudaVideoSurfaceFormat_YUV444:
        format = NV_FORMAT_444P;
        break;
    
    default:
        format = NV_FORMAT_NV12;
        break;
    }

    if (direct_format_prefers_linear_export(format)) {
        backingImage = direct_allocateLinearBackingImage(drv, surface, format);
        if (backingImage != NULL) {
            return backingImage;
        }
        if (direct_debug_enabled()) {
            LOG("Falling back to block-linear BackingImage allocation for format %d", format)
        }
    }

    if (direct_format_prefers_combined_blocklinear_export(format)) {
        backingImage = direct_allocateCombinedBlockLinearBackingImage(drv, surface, format);
        if (backingImage != NULL) {
            return backingImage;
        }
        if (direct_debug_enabled()) {
            LOG("Falling back to legacy block-linear BackingImage allocation for format %d", format)
        }
    }

    backingImage = calloc(1, sizeof(BackingImage));
    if (backingImage == NULL) {
        return NULL;
    }
    backingImage->format = format;

    const NVFormatInfo *fmtInfo = &formatsInfo[backingImage->format];
    const NVFormatPlane *p = fmtInfo->plane;

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

    backingImage->width = surface->width;
    backingImage->height = surface->height;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        backingImage->fds[i] = driverImages[i].drmFd;
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
    if (nvBackingImageIsImportedExternal(img)) {
        nvDestroyImportedBackingImage(drv, img);
        return;
    }

    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];
    if (img->surface != NULL) {
        img->surface->backingImage = NULL;
    }

    for (int i = 0; i < 4; i++) {
        if (img->fds[i] > 0) {
            close(img->fds[i]);
        }
    }

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (img->arrays[i] != NULL) {
            CHECK_CUDA_RESULT(drv->cu->cuArrayDestroy(img->arrays[i]));
        }

        if (img->cudaImages[i].mipmapArray != NULL) {
            CHECK_CUDA_RESULT(drv->cu->cuMipmappedArrayDestroy(img->cudaImages[i].mipmapArray));
        }
    }
    if (img->linearPtr != 0) {
        CHECK_CUDA_RESULT(drv->cu->cuMemFree(img->linearPtr));
    }
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {

        if (img->cudaImages[i].extMem != NULL) {
            CHECK_CUDA_RESULT(drv->cu->cuDestroyExternalMemory(img->cudaImages[i].extMem));
        }
    }

    memset(img, 0, sizeof(BackingImage));
    free(img);
}

static void direct_attachBackingImageToSurface(NVSurface *surface, BackingImage *img) {
    surface->backingImage = img;
    img->surface = surface;
}

static void direct_detachBackingImageFromSurface(NVDriver *drv, NVSurface *surface) {
    if (surface->backingImage == NULL) {
        return;
    }

    destroyBackingImage(drv, surface->backingImage);
    surface->backingImage = NULL;
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
    bool ok = false;
    uint32_t y = 0;

    if (surface->backingImage && nvBackingImageIsImportedExternal(surface->backingImage)) {
        ok = nv_copy_device_to_imported_backing_image(drv,
                                                      surface->backingImage,
                                                      surface->width,
                                                      surface->height,
                                                      ptr,
                                                      pitch);
        goto out;
    }

    const NVFormatInfo *fmtInfo = &formatsInfo[surface->backingImage->format];

    if (surface->backingImage->linearPtr != 0) {
        for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
            const NVFormatPlane *p = &fmtInfo->plane[i];
            size_t plane_width = (size_t)(surface->width >> p->ss.x) * fmtInfo->bppc * p->channelCount;
            size_t dst_pitch = surface->backingImage->strides[i] > 0 ? (size_t)surface->backingImage->strides[i] : plane_width;
            CUDA_MEMCPY2D cpy = {
                .srcMemoryType = CU_MEMORYTYPE_DEVICE,
                .srcDevice = ptr,
                .srcY = y,
                .srcPitch = pitch,
                .dstMemoryType = CU_MEMORYTYPE_DEVICE,
                .dstDevice = surface->backingImage->linearPtr + surface->backingImage->offsets[i],
                .dstPitch = dst_pitch,
                .Height = surface->height >> p->ss.y,
                .WidthInBytes = plane_width
            };
            if (i == fmtInfo->numPlanes - 1) {
                if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&cpy))) {
                    goto out;
                }
            } else {
                if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2DAsync(&cpy, 0))) {
                    goto out;
                }
            }
            y += surface->height >> p->ss.y;
        }

        ok = true;
        goto out;
    }

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
            if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&cpy))) {
                goto out;
            }
        } else {
            if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2DAsync(&cpy, 0))) {
                goto out;
            }
        }
        y += surface->height >> p->ss.y;
    }

    ok = true;

out:
    //notify anyone waiting for us to be resolved
    pthread_mutex_lock(&surface->mutex);
    if (!ok) {
        surface->decodeFailed = true;
    }
    surface->resolving = 0;
    pthread_cond_signal(&surface->cond);
    pthread_mutex_unlock(&surface->mutex);

    return ok;
}

static bool direct_realiseSurface(NVDriver *drv, NVSurface *surface) {
    //make sure we're the only thread updating this surface
    pthread_mutex_lock(&surface->mutex);
    //check again to see if it's just been created
    if (surface->backingImage == NULL) {
        //try to find a free surface
        BackingImage *img = direct_allocateBackingImage(drv, surface);
        if (img == NULL) {
            LOG("Unable to realise surface: %p (%d)", surface, surface->pictureIdx)
            pthread_mutex_unlock(&surface->mutex);
            return false;
        }

        direct_attachBackingImageToSurface(surface, img);
    }
    pthread_mutex_unlock(&surface->mutex);

    return true;
}

static bool direct_exportCudaPtr(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch) {
    if (!direct_realiseSurface(drv, surface)) {
        return false;
    }

    if (ptr != 0) {
        if (!copyFrameToSurface(drv, ptr, surface, pitch)) {
            LOG("Unable to update surface from frame");
            return false;
        }
    } else {
        LOG("exporting with null ptr")
    }

    return true;
}

static bool direct_fillExportDescriptor(NVDriver *drv, NVSurface *surface, VADRMPRIMESurfaceDescriptor *desc) {
    const BackingImage *img = surface->backingImage;
    if (nvBackingImageIsImportedExternal(img)) {
        return nvFillImportedExportDescriptor(img, desc);
    }
    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];

    memset(desc, 0, sizeof(*desc));
    desc->fourcc = fmtInfo->fourcc;
    desc->width = surface->width;
    desc->height = surface->height;

    if (img->numObjects > 0 && img->numLayers > 0) {
        if (img->numObjects > ARRAY_SIZE(desc->objects) || img->numLayers > ARRAY_SIZE(desc->layers)) {
            return false;
        }

        desc->num_objects = img->numObjects;
        desc->num_layers = img->numLayers;

        for (uint32_t i = 0; i < desc->num_objects; i++) {
            desc->objects[i].fd = dup(img->fds[i]);
            if (desc->objects[i].fd < 0) {
                for (uint32_t j = 0; j < i; j++) {
                    close(desc->objects[j].fd);
                    desc->objects[j].fd = -1;
                }
                return false;
            }
            desc->objects[i].size = img->size[i];
            desc->objects[i].drm_format_modifier = img->mods[i];
        }

        for (uint32_t layer_idx = 0; layer_idx < desc->num_layers; layer_idx++) {
            desc->layers[layer_idx].drm_format = img->layerDrmFormats[layer_idx] ?
                                                 img->layerDrmFormats[layer_idx] :
                                                 nvGetExportLayerDrmFormat(img->format, layer_idx, fmtInfo->plane[layer_idx].fourcc);
            desc->layers[layer_idx].num_planes = img->layerNumPlanes[layer_idx] ? img->layerNumPlanes[layer_idx] : 1;
        }

        for (uint32_t plane_idx = 0; plane_idx < fmtInfo->numPlanes; plane_idx++) {
            uint32_t object_idx = img->planeObjectIndices[plane_idx];
            uint32_t layer_idx = img->planeLayerIndices[plane_idx];
            uint32_t plane_in_layer = img->planeIndicesInLayer[plane_idx];

            if (object_idx >= desc->num_objects || layer_idx >= desc->num_layers ||
                plane_in_layer >= ARRAY_SIZE(desc->layers[layer_idx].object_index)) {
                for (uint32_t i = 0; i < desc->num_objects; i++) {
                    if (desc->objects[i].fd >= 0) {
                        close(desc->objects[i].fd);
                        desc->objects[i].fd = -1;
                    }
                }
                return false;
            }

            desc->layers[layer_idx].object_index[plane_in_layer] = object_idx;
            desc->layers[layer_idx].offset[plane_in_layer] = img->offsets[plane_idx];
            desc->layers[layer_idx].pitch[plane_in_layer] = img->strides[plane_idx];
        }

        return true;
    }

    desc->num_layers = fmtInfo->numPlanes;
    desc->num_objects = fmtInfo->numPlanes;

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        desc->objects[i].fd = dup(img->fds[i]);
        desc->objects[i].size = img->size[i];
        desc->objects[i].drm_format_modifier = img->mods[i];

        desc->layers[i].drm_format = nvGetExportLayerDrmFormat(img->format, i, fmtInfo->plane[i].fourcc);
        desc->layers[i].num_planes = 1;
        desc->layers[i].object_index[0] = i;
        desc->layers[i].offset[0] = img->offsets[i];
        desc->layers[i].pitch[0] = img->strides[i];
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
