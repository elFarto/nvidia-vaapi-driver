#define _GNU_SOURCE 1

#include "../vabackend.h"
#include <stdio.h>
#include <ffnvcodec/dynlink_loader.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
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

static void findGPUIndexFromFd(NVDriver *drv) {
    //find the CUDA device id
    char drmUuid[16];
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

static BackingImage *direct_allocateBackingImage(NVDriver *drv, NVSurface *surface) {
    NVDriverImage driverImages[3] = { 0 };
    BackingImage *backingImage = calloc(1, sizeof(BackingImage));

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

    const NVFormatInfo *fmtInfo = &formatsInfo[backingImage->format];

    backingImage->totalSize = calculate_image_size(&drv->driverContext, driverImages, surface->width, surface->height, fmtInfo->bppc, fmtInfo->numPlanes, fmtInfo->plane);
    LOG("Allocating BackingImage: %p %ux%u = %u bytes", backingImage, surface->width, surface->height, backingImage->totalSize);

    //alloc memory - Note this requires that all the planes have the same widthInBytes
    //otherwise the value passed to the kernel driver won't be correct, luckily all the formats
    //we currently support are all the same width
    int memFd = 0, memFd2 = 0, drmFd = 0;
    if (!alloc_buffer(&drv->driverContext, backingImage->totalSize, driverImages, &memFd, &memFd2, &drmFd)) {
        goto import_fail;
    }
    LOG("Allocate Buffer: %d %d %d", memFd, memFd2, drmFd);

    //import the memory to CUDA
    const CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
        .type      = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD,
        .handle.fd = memFd,
        .flags     = 0,
        .size      = backingImage->totalSize
    };

    LOG("Importing memory to CUDA")
    if (CHECK_CUDA_RESULT(drv->cu->cuImportExternalMemory(&backingImage->extMem, &extMemDesc))) {
        goto import_fail;
    }

    close(memFd);
    close(memFd2);
    memFd = -1;
    memFd2 = -1;

    //now map the arrays
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC mipmapArrayDesc = {
            .arrayDesc = {
                .Width = driverImages[i].width,
                .Height = driverImages[i].height,
                .Depth = 0,
                .Format = fmtInfo->bppc == 1 ? CU_AD_FORMAT_UNSIGNED_INT8 : CU_AD_FORMAT_UNSIGNED_INT16,
                .NumChannels = fmtInfo->plane[i].channelCount,
                .Flags = 0
            },
            .numLevels = 1,
            .offset = driverImages[i].offset
        };

        //create a mimap array from the imported memory
        if (CHECK_CUDA_RESULT(drv->cu->cuExternalMemoryGetMappedMipmappedArray(&backingImage->cudaImages[i].mipmapArray, backingImage->extMem, &mipmapArrayDesc))) {
            goto bail;
        }

        //create an array from the mipmap array
        if (CHECK_CUDA_RESULT(drv->cu->cuMipmappedArrayGetLevel(&backingImage->arrays[i], backingImage->cudaImages[i].mipmapArray, 0))) {
            goto bail;
        }
    }

    backingImage->width = surface->width;
    backingImage->height = surface->height;
    backingImage->fds[0] = drmFd;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        backingImage->strides[i] = driverImages[i].pitch;
        backingImage->mods[i] = driverImages[i].mods;
        backingImage->offsets[i] = driverImages[i].offset;
    }

    return backingImage;

bail:
    destroyBackingImage(drv, backingImage);
    //another 'free' might occur on this pointer.
    //hence, set it to NULL to ensure no operation is performed if this really happens.
    backingImage = NULL;

import_fail:
    if (memFd >= 0) {
        close(memFd);
    }
    if (memFd2 >= 0) {
        close(memFd2);
    }
    if (drmFd >= 0) {
        close(drmFd);
    }
    free(backingImage);
    return NULL;
}

static void destroyBackingImage(NVDriver *drv, BackingImage *img) {
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
    if (img->extMem != NULL) {
        CHECK_CUDA_RESULT(drv->cu->cuDestroyExternalMemory(img->extMem));
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
        y += cpy.Height;
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
        copyFrameToSurface(drv, ptr, surface, pitch);
    } else {
        LOG("exporting with null ptr")
    }

    return true;
}

static bool direct_fillExportDescriptor(NVDriver *drv, NVSurface *surface, VADRMPRIMESurfaceDescriptor *desc) {
    const BackingImage *img = surface->backingImage;
    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];

    desc->fourcc = fmtInfo->fourcc;
    desc->width = surface->width;
    desc->height = surface->height;

    desc->num_layers = fmtInfo->numPlanes;
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
