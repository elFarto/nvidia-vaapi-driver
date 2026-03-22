#include "vabackend.h"
#include "backend-common.h"
#include "external-surface.h"
#include <stdio.h>
#include <ffnvcodec/dynlink_loader.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif

#include <drm.h>
#include <drm_fourcc.h>

#ifndef EGL_NV_stream_consumer_eglimage
#define EGL_NV_stream_consumer_eglimage 1
#define EGL_STREAM_CONSUMER_IMAGE_NV      0x3373
#define EGL_STREAM_IMAGE_ADD_NV           0x3374
#define EGL_STREAM_IMAGE_REMOVE_NV        0x3375
#define EGL_STREAM_IMAGE_AVAILABLE_NV     0x3376
typedef EGLBoolean (EGLAPIENTRYP PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLint num_modifiers, EGLuint64KHR *modifiers, EGLAttrib *attrib_list);
typedef EGLint (EGLAPIENTRYP PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLTime timeout, EGLenum *event, EGLAttrib *aux);
typedef EGLBoolean (EGLAPIENTRYP PFNEGLSTREAMACQUIREIMAGENVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLImage *pImage, EGLSync sync);
typedef EGLBoolean (EGLAPIENTRYP PFNEGLSTREAMRELEASEIMAGENVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLImage image, EGLSync sync);
#ifdef EGL_EGLEXT_PROTOTYPES
EGLAPI EGLBoolean EGLAPIENTRY eglStreamImageConsumerConnectNV (EGLDisplay dpy, EGLStreamKHR stream, EGLint num_modifiers, EGLuint64KHR *modifiers, EGLAttrib *attrib_list);
EGLAPI EGLint EGLAPIENTRY eglQueryStreamConsumerEventNV (EGLDisplay dpy, EGLStreamKHR stream, EGLTime timeout, EGLenum *event, EGLAttrib *aux);
EGLAPI EGLBoolean EGLAPIENTRY eglStreamAcquireImageNV (EGLDisplay dpy, EGLStreamKHR stream, EGLImage *pImage, EGLSync sync);
EGLAPI EGLBoolean EGLAPIENTRY eglStreamReleaseImageNV (EGLDisplay dpy, EGLStreamKHR stream, EGLImage image, EGLSync sync);
#endif
#endif

#ifndef EGL_EXT_device_drm
#define EGL_DRM_MASTER_FD_EXT                   0x333C
#endif

#ifndef EGL_EXT_device_drm_render_node
#define EGL_DRM_RENDER_NODE_FILE_EXT      0x3377
#endif

#ifndef EGL_NV_stream_reset
#define EGL_SUPPORT_REUSE_NV              0x3335
#endif

static PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC eglQueryStreamConsumerEventNV;
static PFNEGLSTREAMRELEASEIMAGENVPROC eglStreamReleaseImageNV;
static PFNEGLSTREAMACQUIREIMAGENVPROC eglStreamAcquireImageNV;
static PFNEGLEXPORTDMABUFIMAGEMESAPROC eglExportDMABUFImageMESA;
static PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC eglExportDMABUFImageQueryMESA;
static PFNEGLCREATESTREAMKHRPROC eglCreateStreamKHR;
static PFNEGLDESTROYSTREAMKHRPROC eglDestroyStreamKHR;
static PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC eglStreamImageConsumerConnectNV;

static void debug(EGLenum error,const char *command,EGLint messageType,EGLLabelKHR threadLabel,EGLLabelKHR objectLabel,const char* message) {
    LOG("[EGL] %s: %s", command, message);
}

static void egl_releaseExporter(NVDriver *drv) {
    if (drv == NULL || drv->eglDisplay == EGL_NO_DISPLAY) {
        return;
    }

    //TODO not sure if this is still needed as we don't return anything now
    LOG("Releasing exporter, %d outstanding frames", drv->numFramesPresented);
    while (drv->cuStreamConnection != NULL) {
      CUeglFrame eglframe;
      CUresult cuStatus = drv->cu->cuEGLStreamProducerReturnFrame(&drv->cuStreamConnection, &eglframe, NULL);
      if (cuStatus == CUDA_SUCCESS) {
        drv->numFramesPresented--;
        for (int i = 0; i < 3; i++) {
            if (eglframe.frame.pArray[i] != NULL) {
                LOG("Cleaning up CUDA array %p (%d outstanding)", eglframe.frame.pArray[i], drv->numFramesPresented);
                drv->cu->cuArrayDestroy(eglframe.frame.pArray[i]);
                eglframe.frame.pArray[i] = NULL;
            }
        }
      } else {
          break;
      }
    }
    LOG("Done releasing frames");

    if (drv->cuStreamConnection != NULL) {
        drv->cu->cuEGLStreamProducerDisconnect(&drv->cuStreamConnection);
        drv->cuStreamConnection = NULL;
    }

    if (drv->eglDisplay != EGL_NO_DISPLAY) {
        if (drv->eglStream != EGL_NO_STREAM_KHR) {
            eglDestroyStreamKHR(drv->eglDisplay, drv->eglStream);
            drv->eglStream = EGL_NO_STREAM_KHR;
        }
    }

    drv->eglExporterReady = false;
    drv->numFramesPresented = 0;
}

static bool reconnect_internal(NVDriver *drv) {
    LOG("Reconnecting to stream");
    eglInitialize(drv->eglDisplay, NULL, NULL);
    drv->eglExporterReady = false;
    if (drv->cuStreamConnection != NULL) {
        CHECK_CUDA_RESULT_RETURN(drv->cu->cuEGLStreamProducerDisconnect(&drv->cuStreamConnection), false);
        drv->cuStreamConnection = NULL;
    }
    if (drv->eglStream != EGL_NO_STREAM_KHR) {
        eglDestroyStreamKHR(drv->eglDisplay, drv->eglStream);
        drv->eglStream = EGL_NO_STREAM_KHR;
    }
    drv->numFramesPresented = 0;
    //tell the driver we don't want it to reuse any EGLImages
    EGLint stream_attrib_list[] = { EGL_SUPPORT_REUSE_NV, EGL_FALSE, EGL_NONE };
    drv->eglStream = eglCreateStreamKHR(drv->eglDisplay, stream_attrib_list);
    if (drv->eglStream == EGL_NO_STREAM_KHR) {
        LOG("Unable to create EGLStream");
        return false;
    }
    if (!eglStreamImageConsumerConnectNV(drv->eglDisplay, drv->eglStream, 0, 0, NULL)) {
        LOG("Unable to connect EGLImage stream consumer");
        return false;
    }
    CHECK_CUDA_RESULT_RETURN(drv->cu->cuEGLStreamProducerConnect(&drv->cuStreamConnection, drv->eglStream, 0, 0), false);
    drv->eglExporterReady = true;
    return true;
}

static bool reconnect(NVDriver *drv)
{
    bool ok;
    CHECK_CUDA_RESULT_RETURN(drv->cu->cuCtxPushCurrent(drv->cudaContext), false);
    ok = reconnect_internal(drv);
    CHECK_CUDA_RESULT_RETURN(drv->cu->cuCtxPopCurrent(NULL), false);
    return ok;
}

static void findGPUIndexFromFd(NVDriver *drv) {
    uint8_t drmUuid[16];
    int drmCudaGpuId = -1;
    bool have_nvidia_drm_fd = drv->drmFd != -1 && isNvidiaDrmFd(drv->drmFd, true);
    bool prefer_cuda_index = drv->cudaGpuId != -1;
    PFNEGLQUERYDEVICESEXTPROC eglQueryDevicesEXT = (PFNEGLQUERYDEVICESEXTPROC) eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLQUERYDEVICEATTRIBEXTPROC eglQueryDeviceAttribEXT = (PFNEGLQUERYDEVICEATTRIBEXTPROC) eglGetProcAddress("eglQueryDeviceAttribEXT");
    PFNEGLQUERYDEVICESTRINGEXTPROC eglQueryDeviceStringEXT = (PFNEGLQUERYDEVICESTRINGEXTPROC) eglGetProcAddress("eglQueryDeviceStringEXT");
    bool logged_missing_cuda_device_attr = false;

    if (drv->drmFd != -1 && !have_nvidia_drm_fd) {
        LOG("Provided DRM fd is not an NVIDIA render node, falling back to EGL device enumeration");
    }

    if (!eglQueryDevicesEXT || !eglQueryDeviceStringEXT) {
        LOG("No support for EGL_EXT_device_enumeration");
        drv->cudaGpuId = 0;
        return;
    }

    if (prefer_cuda_index) {
        LOG("Looking for GPU index: %d", drv->cudaGpuId);
    } else if (drv->drmFd == -1) {
        LOG("Defaulting to CUDA GPU ID 0. Use NVD_GPU to select a specific CUDA GPU");
        drv->cudaGpuId = 0;
    }

    if (have_nvidia_drm_fd) {
        NVDriverContext probeContext = {0};
        int probeFd = dup(drv->drmFd);
        if (probeFd == -1) {
            LOG("Unable to duplicate DRM fd for GPU UUID lookup");
        } else if (!init_nvdriver(&probeContext, probeFd)) {
            LOG("Unable to initialize NV driver context for GPU UUID lookup");
            close(probeFd);
        } else {
            if (get_device_uuid(&probeContext, drmUuid)) {
                int gpuCount = 0;
                if (!CHECK_CUDA_RESULT(drv->cu->cuDeviceGetCount(&gpuCount))) {
                    for (int gpu_idx = 0; gpu_idx < gpuCount; gpu_idx++) {
                        CUuuid uuid;
                        if (!CHECK_CUDA_RESULT(drv->cu->cuDeviceGetUuid(&uuid, gpu_idx)) &&
                            memcmp(drmUuid, uuid.bytes, sizeof(drmUuid)) == 0) {
                            drmCudaGpuId = gpu_idx;
                            LOG("Matched DRM UUID to CUDA GPU index %d", drmCudaGpuId);
                            break;
                        }
                    }
                }
                if (drmCudaGpuId == -1) {
                    LOG("Failed to match DRM UUID to a CUDA GPU index");
                }
            }
            free_nvdriver(&probeContext);
        }
    }

    if (prefer_cuda_index && drmCudaGpuId != -1 && drv->cudaGpuId != drmCudaGpuId) {
        LOG("Provided DRM fd maps to CUDA GPU index %d, but CUDA GPU %d was requested", drmCudaGpuId, drv->cudaGpuId);
        return;
    }

    if (!prefer_cuda_index && drmCudaGpuId != -1) {
        drv->cudaGpuId = drmCudaGpuId;
    }

    EGLDeviceEXT devices[8];
    EGLint num_devices;
    if(!eglQueryDevicesEXT(8, devices, &num_devices)) {
        LOG("Unable to query EGL devices");
        drv->cudaGpuId = 0;
        return;
    }

    LOG("Found %d EGL devices", num_devices);

    for (int i = 0; i < num_devices; i++) {
        const char* drmRenderNodeFile = eglQueryDeviceStringEXT(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT);
        if (drmRenderNodeFile != NULL) {
            struct stat buf;
            if (stat(drmRenderNodeFile, &buf) != 0) {
                LOG("Unable to stat DRM render node for EGLDevice %d", i);
                continue;
            }
            int foundDrmDeviceIndex = minor(buf.st_rdev);
            LOG("Found drmDeviceIndex: %d", foundDrmDeviceIndex);
            if (have_nvidia_drm_fd) {
                struct stat current;
                if (fstat(drv->drmFd, &current) != 0) {
                    LOG("Unable to stat provided DRM fd for EGL device matching");
                    return;
                }
                if ((int)minor(current.st_rdev) != foundDrmDeviceIndex) {
                    continue;
                }
            }

            if (have_nvidia_drm_fd && !checkModesetParameterFromFd(drv->drmFd)) {
                continue;
            }

            EGLAttrib attr = -1;
            bool have_cuda_device_attr = false;
            if (eglQueryDeviceAttribEXT && eglQueryDeviceAttribEXT(devices[i], EGL_CUDA_DEVICE_NV, &attr)) {
                have_cuda_device_attr = true;
                LOG("Got EGL_CUDA_DEVICE_NV value '%d' for EGLDevice %d", (int)attr, i);
            } else if (eglQueryDeviceAttribEXT) {
                LOG("No EGL_CUDA_DEVICE_NV support for EGLDevice %d", i);
            } else if (!logged_missing_cuda_device_attr) {
                if (drv->cudaGpuId != -1) {
                    LOG("No support for EGL_CUDA_DEVICE_NV matching; unable to verify CUDA GPU %d", drv->cudaGpuId);
                } else {
                    LOG("No support for EGL_CUDA_DEVICE_NV matching; unable to derive a coupled CUDA GPU");
                }
                logged_missing_cuda_device_attr = true;
            }

            if (drmCudaGpuId == -1) {
                if (!have_cuda_device_attr) {
                    continue;
                }
                if (drv->cudaGpuId == -1) {
                    drv->cudaGpuId = (int)attr;
                }
                if ((int)attr != drv->cudaGpuId) {
                    continue;
                }
            } else if (have_cuda_device_attr && (int)attr != drv->cudaGpuId) {
                continue;
            }

            if (drv->cudaGpuId == -1) {
                if (have_cuda_device_attr) {
                    drv->cudaGpuId = (int)attr;
                } else {
                    LOG("Unable to determine CUDA GPU for EGLDevice %d", i);
                    continue;
                }
            }

            LOG("Selecting EGLDevice %d", i);
            drv->eglDevice = devices[i];
            return;
        } else {
            LOG("No DRM device file for EGLDevice %d", i);
        }
    }

    if (drv->cudaGpuId != -1) {
        LOG("No EGL device found for CUDA GPU index %d", drv->cudaGpuId);
    } else {
        LOG("No coupled EGL/CUDA device match found");
    }
}

static bool egl_initExporter(NVDriver *drv) {
    findGPUIndexFromFd(drv);

    //if we didn't find an EGLDevice, then exit now
    if (drv->eglDevice == NULL) {
        return false;
    }

    static const EGLAttrib debugAttribs[] = {EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE, EGL_DEBUG_MSG_INFO_KHR, EGL_TRUE, EGL_NONE};

    eglQueryStreamConsumerEventNV = (PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC) eglGetProcAddress("eglQueryStreamConsumerEventNV");
    eglStreamReleaseImageNV = (PFNEGLSTREAMRELEASEIMAGENVPROC) eglGetProcAddress("eglStreamReleaseImageNV");
    eglStreamAcquireImageNV = (PFNEGLSTREAMACQUIREIMAGENVPROC) eglGetProcAddress("eglStreamAcquireImageNV");
    eglExportDMABUFImageMESA = (PFNEGLEXPORTDMABUFIMAGEMESAPROC) eglGetProcAddress("eglExportDMABUFImageMESA");
    eglExportDMABUFImageQueryMESA = (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC) eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    eglCreateStreamKHR = (PFNEGLCREATESTREAMKHRPROC) eglGetProcAddress("eglCreateStreamKHR");
    eglDestroyStreamKHR = (PFNEGLDESTROYSTREAMKHRPROC) eglGetProcAddress("eglDestroyStreamKHR");
    eglStreamImageConsumerConnectNV = (PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC) eglGetProcAddress("eglStreamImageConsumerConnectNV");

    PFNEGLQUERYDMABUFFORMATSEXTPROC eglQueryDmaBufFormatsEXT = (PFNEGLQUERYDMABUFFORMATSEXTPROC) eglGetProcAddress("eglQueryDmaBufFormatsEXT");
    PFNEGLDEBUGMESSAGECONTROLKHRPROC eglDebugMessageControlKHR = (PFNEGLDEBUGMESSAGECONTROLKHRPROC) eglGetProcAddress("eglDebugMessageControlKHR");

    drv->eglDisplay = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, (EGLDeviceEXT) drv->eglDevice, NULL);
    if (drv->eglDisplay == NULL) {
        LOG("Falling back to using default EGLDisplay");
        drv->eglDisplay = eglGetDisplay(NULL);
    }

    if (drv->eglDisplay == NULL) {
        return false;
    }

    if (!eglInitialize(drv->eglDisplay, NULL, NULL)) {
        LOG("Unable to initialise EGL for display: %p", drv->eglDisplay);
        return false;
    }
    //setup debug logging
    eglDebugMessageControlKHR(debug, debugAttribs);

    //see if the driver supports 16-bit exports
    EGLint formats[64];
    EGLint formatCount;
    if (eglQueryDmaBufFormatsEXT(drv->eglDisplay, 64, formats, &formatCount)) {
        bool r16 = false, gr1616 = false, rg1616 = false;
        for (int i = 0; i < formatCount; i++) {
            if (formats[i] == DRM_FORMAT_R16) {
                r16 = true;
            } else if (formats[i] == DRM_FORMAT_GR1616) {
                gr1616 = true;
            } else if (formats[i] == DRM_FORMAT_RG1616) {
                rg1616 = true;
            }
        }
        drv->supports16BitSurface = r16 && (gr1616 || rg1616);
        drv->supports444Surface = false;
        if (drv->supports16BitSurface) {
            LOG("Driver supports 16-bit surfaces");
        } else {
            LOG("Driver doesn't support 16-bit surfaces");
        }
    }

    drv->eglExporterReady = false;
    drv->cuStreamConnection = NULL;
    drv->eglStream = EGL_NO_STREAM_KHR;
    return true;
}

static bool exportBackingImage(NVDriver *drv, BackingImage *img) {
    int planes = 0;
    if (!eglExportDMABUFImageQueryMESA(drv->eglDisplay, img->image, &img->fourcc, &planes, img->mods)) {
        LOG("eglExportDMABUFImageQueryMESA failed");
        return false;
    }

    LOG("eglExportDMABUFImageQueryMESA: %p %.4s (%x) planes:%d mods:%lx %lx", img, (char*)&img->fourcc, img->fourcc, planes, img->mods[0], img->mods[1]);
    EGLBoolean r = eglExportDMABUFImageMESA(drv->eglDisplay, img->image, img->fds, img->strides, img->offsets);
    //LOG("Offset/Pitch: %d %d %d %d", surface->offsets[0], surface->offsets[1], surface->strides[0], surface->strides[1]);

    if (!r) {
        LOG("Unable to export image");
        return false;
    }
    return true;
}

static BackingImage* createBackingImage(NVDriver *drv, uint32_t width, uint32_t height, EGLImage image, CUarray arrays[]) {
    BackingImage* img = (BackingImage*) calloc(1, sizeof(BackingImage));
    img->image = image;
    img->arrays[0] = arrays[0];
    img->arrays[1] = arrays[1];
    img->width = width;
    img->height = height;

    if (!exportBackingImage(drv, img)) {
        LOG("Unable to export Backing Image");
        free(img);
        return NULL;
    }

    return img;
}


static bool egl_destroyBackingImage(NVDriver *drv, BackingImage *img) {
    //if we're attached to a surface, update the surface to remove us
    if (img->surface != NULL) {
        img->surface->backingImage = NULL;
    }

    LOG("Destroying BackingImage: %p", img);
    for (int i = 0; i < 4; i++) {
        if (img->fds[i] != 0) {
            close(img->fds[i]);
        }
    }
    if (drv->eglDisplay != EGL_NO_DISPLAY && drv->eglStream != EGL_NO_STREAM_KHR && img->image != EGL_NO_IMAGE) {
        eglStreamReleaseImageNV(drv->eglDisplay, drv->eglStream, img->image, EGL_NO_SYNC_NV);
    }
    eglDestroyImage(drv->eglDisplay, img->image);
    CHECK_CUDA_RESULT_RETURN(drv->cu->cuArrayDestroy(img->arrays[0]), false);
    CHECK_CUDA_RESULT_RETURN(drv->cu->cuArrayDestroy(img->arrays[1]), false);
    if (img->linearPtr != 0) {
        CHECK_CUDA_RESULT_RETURN(drv->cu->cuMemFree(img->linearPtr), false);
    }
    img->arrays[0] = NULL;
    img->arrays[1] = NULL;
    free(img);
    return true;
}

static void egl_attachBackingImageToSurface(NVSurface *surface, BackingImage *img) {
    surface->backingImage = img;
    img->surface = surface;
}

static void egl_detachBackingImageFromSurface(NVDriver *drv, NVSurface *surface) {
    if (surface->backingImage == NULL) {
        LOG("Cannot detach NULL BackingImage from Surface");
        return;
    }

    if (nvBackingImageIsImportedExternal(surface->backingImage)) {
        nvDestroyImportedBackingImage(drv, surface->backingImage);
        surface->backingImage = NULL;
        return;
    }

    if (surface->backingImage->fourcc == DRM_FORMAT_NV21) {
        if (!egl_destroyBackingImage(drv, surface->backingImage)) {
            LOG("Unable to destroy backing image");
        }
    } else {
        pthread_mutex_lock(&drv->imagesMutex);

        ARRAY_FOR_EACH(BackingImage*, img, &drv->images)
            //find the entry for this surface
            if (img->surface == surface) {
                LOG("Detaching BackingImage %p from Surface %p", img, surface);
                img->surface = NULL;
                break;
            }
        END_FOR_EACH

        pthread_mutex_unlock(&drv->imagesMutex);
    }

    surface->backingImage = NULL;
}

static void egl_destroyAllBackingImage(NVDriver *drv) {
    pthread_mutex_lock(&drv->imagesMutex);

    ARRAY_FOR_EACH_REV(BackingImage*, it, &drv->images)
        egl_destroyBackingImage(drv, it);
        remove_element_at(&drv->images, it_idx);
    END_FOR_EACH

    pthread_mutex_unlock(&drv->imagesMutex);
}

static BackingImage* findFreeBackingImage(NVDriver *drv, NVSurface *surface) {
    BackingImage *ret = NULL;
    pthread_mutex_lock(&drv->imagesMutex);
    //look through the free'd surfaces and see if we can reuse one
    ARRAY_FOR_EACH(BackingImage*, img, &drv->images)
        if (img->surface == NULL && img->width == surface->width && img->height == surface->height) {
            LOG("Using BackingImage %p for Surface %p", img, surface);
            egl_attachBackingImageToSurface(surface, img);
            ret = img;
            break;
        }
    END_FOR_EACH
    pthread_mutex_unlock(&drv->imagesMutex);
    return ret;
}


static BackingImage *egl_allocateBackingImage(NVDriver *drv, const NVSurface *surface) {
    CUeglFrame eglframe = {
        .width = surface->width,
        .height = surface->height,
        .depth = 1,
        .pitch = 0,
        .planeCount = 2,
        .numChannels = 1,
        .frameType = CU_EGL_FRAME_TYPE_ARRAY,
    };

    if (surface->format == cudaVideoSurfaceFormat_NV12) {
        eglframe.eglColorFormat = drv->useCorrectNV12Format ? CU_EGL_COLOR_FORMAT_YUV420_SEMIPLANAR :
                                                              CU_EGL_COLOR_FORMAT_YVU420_SEMIPLANAR;
        eglframe.cuFormat = CU_AD_FORMAT_UNSIGNED_INT8;
    } else if (surface->format == cudaVideoSurfaceFormat_P016) {
        if (surface->bitDepth == 10) {
            eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_Y10V10U10_420_SEMIPLANAR;
        } else if (surface->bitDepth == 12) {
            // Logically, we should use the explicit 12bit format here, but it fails
            // to export to a dmabuf if we do. In practice, that should be fine as the
            // data is still stored in 16 bits and they (surely?) aren't going to
            // zero out the extra bits.
            // eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_Y12V12U12_420_SEMIPLANAR;
            eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_Y10V10U10_420_SEMIPLANAR;
        } else {
            LOG("Unknown bitdepth");
        }
        eglframe.cuFormat = CU_AD_FORMAT_UNSIGNED_INT16;
    }
    CUDA_ARRAY3D_DESCRIPTOR arrDesc = {
        .Width = eglframe.width,
        .Height = eglframe.height,
        .Depth = 0,
        .NumChannels = 1,
        .Flags = 0,
        .Format = eglframe.cuFormat
    };
    CUDA_ARRAY3D_DESCRIPTOR arr2Desc = {
        .Width = eglframe.width >> 1,
        .Height = eglframe.height >> 1,
        .Depth = 0,
        .NumChannels = 2,
        .Flags = 0,
        .Format = eglframe.cuFormat
    };
    CHECK_CUDA_RESULT_RETURN(drv->cu->cuArray3DCreate(&eglframe.frame.pArray[0], &arrDesc), NULL);
    CHECK_CUDA_RESULT_RETURN(drv->cu->cuArray3DCreate(&eglframe.frame.pArray[1], &arr2Desc), NULL);

    pthread_mutex_lock(&drv->exportMutex);

    if (!drv->eglExporterReady) {
        if (!reconnect(drv)) {
            pthread_mutex_unlock(&drv->exportMutex);
            return NULL;
        }
    }

    LOG("Presenting frame %d %dx%d (%p, %p, %p)", surface->pictureIdx, eglframe.width, eglframe.height, surface, eglframe.frame.pArray[0], eglframe.frame.pArray[1]);
    if (CHECK_CUDA_RESULT(drv->cu->cuEGLStreamProducerPresentFrame( &drv->cuStreamConnection, eglframe, NULL))) {
        //if we got an error here, try to reconnect to the EGLStream
        if (!reconnect(drv) ||
            CHECK_CUDA_RESULT(drv->cu->cuEGLStreamProducerPresentFrame( &drv->cuStreamConnection, eglframe, NULL))) {
            pthread_mutex_unlock(&drv->exportMutex);

            return NULL;
        }
    }

    BackingImage *ret = NULL;
    while (1) {
        EGLenum event = 0;
        EGLAttrib aux = 0;
        //check for the next event
        if (eglQueryStreamConsumerEventNV(drv->eglDisplay, drv->eglStream, 0, &event, &aux) != EGL_TRUE) {
            break;
        }

        if (event == EGL_STREAM_IMAGE_ADD_NV) {
            EGLImage image = eglCreateImage(drv->eglDisplay, EGL_NO_CONTEXT, EGL_STREAM_CONSUMER_IMAGE_NV, drv->eglStream, NULL);
            LOG("Adding frame from EGLStream: %p", image);
        } else if (event == EGL_STREAM_IMAGE_REMOVE_NV) {
            //Not sure if this is ever called
            eglDestroyImage(drv->eglDisplay, (EGLImage) aux);
            LOG("Removing frame from EGLStream: %p", aux);
        } else if (event == EGL_STREAM_IMAGE_AVAILABLE_NV) {
            EGLImage img;
            if (!eglStreamAcquireImageNV(drv->eglDisplay, drv->eglStream, &img, EGL_NO_SYNC_NV)) {
                LOG("eglStreamAcquireImageNV failed");
                break;
            }
            LOG("Acquired image from EGLStream: %p", img);

            ret = createBackingImage(drv, surface->width, surface->height, img, eglframe.frame.pArray);
        } else {
            LOG("Unhandled event: %X", event);
        }
    }

    pthread_mutex_unlock(&drv->exportMutex);
    return ret;
}

static bool copyFrameToSurface(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch) {
    bool ok = false;

    if (surface->backingImage && nvBackingImageIsImportedExternal(surface->backingImage)) {
        ok = nv_copy_device_to_imported_backing_image(drv,
                                                      surface->backingImage,
                                                      surface->width,
                                                      surface->height,
                                                      ptr,
                                                      pitch);
        goto out;
    }

    int bpp = surface->format == cudaVideoSurfaceFormat_NV12 ? 1 : 2;
    CUDA_MEMCPY2D cpy = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = ptr,
        .srcPitch = pitch,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = surface->backingImage->arrays[0],
        .Height = surface->height,
        .WidthInBytes = surface->width * bpp
    };
    if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2DAsync(&cpy, 0))) {
        goto out;
    }
    CUDA_MEMCPY2D cpy2 = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = ptr,
        .srcY = surface->height,
        .srcPitch = pitch,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = surface->backingImage->arrays[1],
        .Height = surface->height >> 1,
        .WidthInBytes = surface->width * bpp
    };
    if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&cpy2))) {
        goto out;
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

static bool egl_realiseSurface(NVDriver *drv, NVSurface *surface) {
    //make sure we're the only thread updating this surface
    pthread_mutex_lock(&surface->mutex);
    //check again to see if it's just been created
    if (surface->backingImage == NULL) {
        //try to find a free surface
        BackingImage *img = findFreeBackingImage(drv, surface);

        //if we can't find a free backing image...
        if (img == NULL) {
            LOG("No free surfaces found");

            //...allocate one
            img = egl_allocateBackingImage(drv, surface);
            if (img == NULL) {
                LOG("Unable to realise surface: %p (%d)", surface, surface->pictureIdx)
                pthread_mutex_unlock(&surface->mutex);
                return false;
            }

            if (img->fourcc == DRM_FORMAT_NV21) {
                LOG("Detected NV12/NV21 NVIDIA driver bug, attempting to work around");
                //free the old surface to prevent leaking them
                if (!egl_destroyBackingImage(drv, img)) {
                    LOG("Unable to destroy backing image");
                }
                //this is a caused by a bug in old versions the driver that was fixed in the 510 series
                drv->useCorrectNV12Format = !drv->useCorrectNV12Format;
                //re-export the frame in the correct format
                img = egl_allocateBackingImage(drv, surface);
                if (img->fourcc != DRM_FORMAT_NV12) {
                    LOG("Work around unsuccessful");
                }
            }

            egl_attachBackingImageToSurface(surface, img);
            //add our newly created BackingImage to the list
            pthread_mutex_lock(&drv->imagesMutex);
            add_element(&drv->images, img);
            pthread_mutex_unlock(&drv->imagesMutex);
        }
    }
    pthread_mutex_unlock(&surface->mutex);

    return true;
}

static bool egl_exportCudaPtr(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch) {
    if (!egl_realiseSurface(drv, surface)) {
        return false;
    }

    if (ptr != 0 && !copyFrameToSurface(drv, ptr, surface, pitch)) {
        LOG("Unable to update surface from frame");
        return false;
    } else if (ptr == 0) {
        LOG("exporting with null ptr");
    }

    return true;
}

static bool egl_fillExportDescriptor(NVDriver *drv, NVSurface *surface, VADRMPRIMESurfaceDescriptor *desc) {
    BackingImage *img = surface->backingImage;

    if (nvBackingImageIsImportedExternal(img)) {
        return nvFillImportedExportDescriptor(img, desc);
    }

    int bpp = img->fourcc == DRM_FORMAT_NV12 ? 1 : 2;

    //TODO only support 420 images (either NV12, P010 or P012)
    desc->fourcc = img->fourcc;
    desc->width = img->width;
    desc->height = img->height;
    desc->num_layers = 2;
    desc->num_objects = 2;

    desc->objects[0].fd = dup(img->fds[0]);
    desc->objects[0].size = img->width * img->height * bpp;
    desc->objects[0].drm_format_modifier = img->mods[0];

    desc->objects[1].fd = dup(img->fds[1]);
    desc->objects[1].size = img->width * (img->height >> 1) * bpp;
    desc->objects[1].drm_format_modifier = img->mods[1];

    desc->layers[0].drm_format = nvGetExportLayerDrmFormat(img->format, 0,
                                                           img->fourcc == DRM_FORMAT_NV12 ? DRM_FORMAT_R8 : DRM_FORMAT_R16);
    desc->layers[0].num_planes = 1;
    desc->layers[0].object_index[0] = 0;
    desc->layers[0].offset[0] = img->offsets[0];
    desc->layers[0].pitch[0] = img->strides[0];

    desc->layers[1].drm_format = nvGetExportLayerDrmFormat(img->format, 1,
                                                           img->fourcc == DRM_FORMAT_NV12 ? DRM_FORMAT_RG88 : DRM_FORMAT_RG1616);
    desc->layers[1].num_planes = 1;
    desc->layers[1].object_index[0] = 1;
    desc->layers[1].offset[0] = img->offsets[1];
    desc->layers[1].pitch[0] = img->strides[1];

    return true;
}

const NVBackend EGL_BACKEND = {
    .name = "egl",
    .initExporter = egl_initExporter,
    .releaseExporter = egl_releaseExporter,
    .exportCudaPtr = egl_exportCudaPtr,
    .detachBackingImageFromSurface = egl_detachBackingImageFromSurface,
    .realiseSurface = egl_realiseSurface,
    .fillExportDescriptor = egl_fillExportDescriptor,
    .destroyAllBackingImage = egl_destroyAllBackingImage
};
