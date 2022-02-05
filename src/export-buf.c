#include "export-buf.h"
#include <stdio.h>
#include <ffnvcodec/dynlink_loader.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#if defined __has_include && __has_include(<libdrm/drm.h>)
#  include <libdrm/drm.h>
#  include <libdrm/drm_fourcc.h>
#else
#  include <drm/drm.h>
#  include <drm/drm_fourcc.h>
#endif

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

#ifndef EGL_EXT_device_drm_render_node
#define EGL_DRM_RENDER_NODE_FILE_EXT      0x3377
#endif

static PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC eglQueryStreamConsumerEventNV;
static PFNEGLSTREAMRELEASEIMAGENVPROC eglStreamReleaseImageNV;
static PFNEGLSTREAMACQUIREIMAGENVPROC eglStreamAcquireImageNV;
static PFNEGLEXPORTDMABUFIMAGEMESAPROC eglExportDMABUFImageMESA;
static PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC eglExportDMABUFImageQueryMESA;
static PFNEGLCREATESTREAMKHRPROC eglCreateStreamKHR;
static PFNEGLDESTROYSTREAMKHRPROC eglDestroyStreamKHR;
static PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC eglStreamImageConsumerConnectNV;
static PFNEGLQUERYDEVICESTRINGEXTPROC eglQueryDeviceStringEXT;

static void debug(EGLenum error,const char *command,EGLint messageType,EGLLabelKHR threadLabel,EGLLabelKHR objectLabel,const char* message) {
    LOG("[EGL] %s: %s", command, message);
}

void releaseExporter(NVDriver *drv) {
    //TODO not sure if this is still needed as we don't return anything now
    LOG("Releasing exporter, %d outstanding frames", drv->numFramesPresented);
    while (drv->numFramesPresented > 0) {
      CUeglFrame eglframe;
      CUresult cuStatus = drv->cu->cuEGLStreamProducerReturnFrame(&drv->cuStreamConnection, &eglframe, NULL);
      if (cuStatus == CUDA_SUCCESS) {
        drv->numFramesPresented--;
        for (int i = 0; i < 3; i++) {
            if (eglframe.frame.pArray[i] != NULL) {
                LOG("Cleaning up CUDA array %p", eglframe.frame.pArray[i]);
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
    }

    if (drv->cuStreamConnection != NULL) {
        drv->cu->cuEGLStreamConsumerDisconnect(&drv->cuStreamConnection);
    }

    if (drv->eglDisplay != EGL_NO_DISPLAY) {
        if (drv->eglStream != EGL_NO_STREAM_KHR) {
            eglDestroyStreamKHR(drv->eglDisplay, drv->eglStream);
            drv->eglStream = EGL_NO_STREAM_KHR;
        }
        //TODO terminate the EGLDisplay here?, sounds like that could break stuff
        drv->eglDisplay = EGL_NO_DISPLAY;
    }
}

static void reconnect(NVDriver *drv) {
    LOG("Reconnecting to stream");
    eglInitialize(drv->eglDisplay, NULL, NULL);
    if (drv->cuStreamConnection != NULL) {
        drv->cu->cuEGLStreamConsumerDisconnect(&drv->cuStreamConnection);
    }
    if (drv->eglStream != EGL_NO_STREAM_KHR) {
        eglDestroyStreamKHR(drv->eglDisplay, drv->eglStream);
    }
    drv->eglStream = eglCreateStreamKHR(drv->eglDisplay, NULL);
    if (drv->eglStream == EGL_NO_STREAM_KHR) {
        LOG("Unable to create EGLStream");
        return;
    }
    if (!eglStreamImageConsumerConnectNV(drv->eglDisplay, drv->eglStream, 0, 0, NULL)) {
        LOG("Unable to connect EGLImage stream consumer");
        return;
    }
    CHECK_CUDA_RESULT(drv->cu->cuEGLStreamProducerConnect(&drv->cuStreamConnection, drv->eglStream, 1024, 1024));
    drv->numFramesPresented = 0;
}

static bool checkModesetParameter(EGLDeviceEXT device) {
    //we need to check to see if modeset=1 has been passed to the nvidia_drm driver
    //since you need to be root to read the parameter out of /sys, we'll have to find the
    //DRM device file, open it and issue an ioctl to see if the ASYNC_PAGE_FLIP cap is set
    const char* drmDeviceFile = eglQueryDeviceStringEXT(device, EGL_DRM_RENDER_NODE_FILE_EXT);
    char tmpDeviceName[20]; // /dev/dri/renderD128
    if (drmDeviceFile == NULL) {
        LOG("Unable to retrieve render node, deriving render node from master node");
        drmDeviceFile = eglQueryDeviceStringEXT(device, EGL_DRM_DEVICE_FILE_EXT);
        //compute the render node device path. opening the master node logs an error to syslog
        //which we shouldn't really do.
        if (drmDeviceFile != NULL) {
            // /dev/dri/card == 12
            const char* cardIdxStr = drmDeviceFile + 12;
            int cardIdx = atoi(cardIdxStr);
            snprintf(tmpDeviceName, 20, "/dev/dri/renderD%u", cardIdx + 128);
            drmDeviceFile = tmpDeviceName;
        }
    }
    LOG("Checking device file: %s", drmDeviceFile);
    if (drmDeviceFile != NULL) {
        int fd = open(drmDeviceFile, O_RDONLY);
        if (fd > 0) {
            //this ioctl should fail if modeset=0
            struct drm_get_cap caps = { .capability = DRM_CAP_DUMB_BUFFER };
            int ret = ioctl(fd, DRM_IOCTL_GET_CAP, &caps);
            close(fd);
            if (ret != 0) {
                //the modeset parameter is set to 0
                LOG("ERROR: This driver requires the nvidia_drm.modeset kernel module parameter set to 1");
                return false;
            }
        } else {
            LOG("Unable to check nvidia_drm modeset setting");
        }
    }
    return true;
}

static int findCudaDisplay(EGLDisplay *eglDisplay) {
    PFNEGLQUERYDEVICESEXTPROC eglQueryDevicesEXT = (PFNEGLQUERYDEVICESEXTPROC) eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLQUERYDEVICEATTRIBEXTPROC eglQueryDeviceAttribEXT = (PFNEGLQUERYDEVICEATTRIBEXTPROC) eglGetProcAddress("eglQueryDeviceAttribEXT");

    *eglDisplay = EGL_NO_DISPLAY;

    if (eglQueryDevicesEXT == NULL || eglQueryDeviceAttribEXT == NULL) {
        return 0;
    }

    EGLDeviceEXT devices[8];
    EGLint num_devices;
    if(!eglQueryDevicesEXT(8, devices, &num_devices)) {
        return 0;
    }

    LOG("Found %d EGL devices", num_devices);
    for (int i = 0; i < num_devices; i++) {
        EGLAttrib attr;
        if (eglQueryDeviceAttribEXT(devices[i], EGL_CUDA_DEVICE_NV, &attr)) {
            LOG("Got EGL_CUDA_DEVICE_NV value '%d' from device %d", attr, i);
            //TODO: currently we're hardcoding the CUDA device to 0, so only create the display on that device
            if (attr == 0) {
                //attr contains the cuda device id
                if (!checkModesetParameter(devices[i])) {
                    return -1;
                }

                *eglDisplay = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devices[i], NULL);
                return 1;
            }
        }
    }

    return 0;
}

bool initExporter(NVDriver *drv) {
    static const EGLAttrib debugAttribs[] = {EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE, EGL_DEBUG_MSG_INFO_KHR, EGL_TRUE, EGL_NONE};

    eglQueryStreamConsumerEventNV = (PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC) eglGetProcAddress("eglQueryStreamConsumerEventNV");
    eglStreamReleaseImageNV = (PFNEGLSTREAMRELEASEIMAGENVPROC) eglGetProcAddress("eglStreamReleaseImageNV");
    eglStreamAcquireImageNV = (PFNEGLSTREAMACQUIREIMAGENVPROC) eglGetProcAddress("eglStreamAcquireImageNV");
    eglExportDMABUFImageMESA = (PFNEGLEXPORTDMABUFIMAGEMESAPROC) eglGetProcAddress("eglExportDMABUFImageMESA");
    eglExportDMABUFImageQueryMESA = (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC) eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    eglCreateStreamKHR = (PFNEGLCREATESTREAMKHRPROC) eglGetProcAddress("eglCreateStreamKHR");
    eglDestroyStreamKHR = (PFNEGLDESTROYSTREAMKHRPROC) eglGetProcAddress("eglDestroyStreamKHR");
    eglStreamImageConsumerConnectNV = (PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC) eglGetProcAddress("eglStreamImageConsumerConnectNV");
    eglQueryDeviceStringEXT = (PFNEGLQUERYDEVICESTRINGEXTPROC) eglGetProcAddress("eglQueryDeviceStringEXT");

    PFNEGLQUERYDMABUFFORMATSEXTPROC eglQueryDmaBufFormatsEXT = (PFNEGLQUERYDMABUFFORMATSEXTPROC) eglGetProcAddress("eglQueryDmaBufFormatsEXT");
    PFNEGLDEBUGMESSAGECONTROLKHRPROC eglDebugMessageControlKHR = (PFNEGLDEBUGMESSAGECONTROLKHRPROC) eglGetProcAddress("eglDebugMessageControlKHR");

    int ret = findCudaDisplay(&drv->eglDisplay);
    if (ret == 1) {
        LOG("Got EGLDisplay from CUDA device");
    } else if (ret == 0) {
        LOG("Falling back to using default EGLDisplay");
        drv->eglDisplay = eglGetDisplay(NULL);
    } else if (ret == -1) {
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
        bool r16 = false, rg1616 = false;
        for (int i = 0; i < formatCount; i++) {
            if (formats[i] == DRM_FORMAT_R16) {
                r16 = true;
            } else if (formats[i] == DRM_FORMAT_RG1616) {
                rg1616 = true;
            }
        }
        drv->supports16BitSurface = r16 & rg1616;
        if (drv->supports16BitSurface) {
            LOG("Driver supports 16-bit surfaces");
        } else {
            LOG("Driver doesn't support 16-bit surfaces");
        }
    }

    reconnect(drv);

    return true;
}


bool allocateSurface(NVDriver *drv, NVSurface *surface) {
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
        //TODO not working, produces this error in mpv:
        //EGL_BAD_MATCH error: In eglCreateImageKHR: requested LINUX_DRM_FORMAT is not supported
        //this error seems to be coming from the NVIDIA EGL driver
        //this might be caused by the DRM_FORMAT_*'s in nvExportSurfaceHandle
        if (surface->bitDepth == 10) {
            eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_Y10V10U10_420_SEMIPLANAR;
        } else if (surface->bitDepth == 12) {
            eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_Y12V12U12_420_SEMIPLANAR;
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
    CHECK_CUDA_RESULT(drv->cu->cuArray3DCreate(&surface->cuImages[0], &arrDesc));
    CHECK_CUDA_RESULT(drv->cu->cuArray3DCreate(&surface->cuImages[1], &arr2Desc));

    eglframe.frame.pArray[0] = surface->cuImages[0];
    eglframe.frame.pArray[1] = surface->cuImages[1];

    LOG("presenting frame %dx%d %p %p", eglframe.width, eglframe.height, eglframe.frame.pArray[0], eglframe.frame.pArray[1]);
    CUresult ret = drv->cu->cuEGLStreamProducerPresentFrame( &drv->cuStreamConnection, eglframe, NULL );
    if (ret == CUDA_ERROR_UNKNOWN) {
        reconnect(drv);
        CHECK_CUDA_RESULT(drv->cu->cuEGLStreamProducerPresentFrame( &drv->cuStreamConnection, eglframe, NULL ));
    }

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
            //NVEGLImage* nvEglImage = (NVEGLImage*) calloc(1, sizeof(NVEGLImage));
//            nvEglImage->image = image;
//            nvEglImage->next = drv->allocatedEGLImages;
//            drv->allocatedEGLImages = nvEglImage;
        } else if (event == EGL_STREAM_IMAGE_AVAILABLE_NV) {
            EGLImage img;
            if (!eglStreamAcquireImageNV(drv->eglDisplay, drv->eglStream, &img, EGL_NO_SYNC_NV)) {
                LOG("eglStreamAcquireImageNV failed");
                freeSurface(drv, surface);
                return false;
            }

            LOG("Acquired image from EGLStream: %p", img);
            surface->eglImage = img;

            int planes = 0;
            if (!eglExportDMABUFImageQueryMESA(drv->eglDisplay, surface->eglImage, &surface->fourcc, &planes, surface->mods)) {
                LOG("eglExportDMABUFImageQueryMESA failed");
                freeSurface(drv, surface);
                return false;
            }

            //LOG("eglExportDMABUFImageQueryMESA: %p %.4s (%x) planes:%d mods:%lx %lx", img, (char*)fourcc, *fourcc, planes, mods[0], mods[1]);
            EGLBoolean r = eglExportDMABUFImageMESA(drv->eglDisplay, surface->eglImage, surface->fds, surface->strides, surface->offsets);

            if (!r) {
                LOG("Unable to export image");
                freeSurface(drv, surface);
                return false;
            }
        } else {
            LOG("Unhandled event: %X", event);
        }
    }

    return true;
}

bool freeSurface(NVDriver *drv, NVSurface *surface) {
    for (int i = 0; i < 4; i++) {
        if (surface->fds[i] != 0) {
            close(surface->fds[i]);
            surface->fds[i] = 0;
        }
    }
    if (surface->eglImage != EGL_NO_IMAGE) {
        LOG("Destroying EGLImage: %p", surface->eglImage);
        eglDestroyImage(drv->eglDisplay, surface->eglImage);
        surface->eglImage = EGL_NO_IMAGE;
    }
    for (int i = 0; i < 2; i++) {
        if (surface->cuImages[i] != NULL) {
            LOG("Destroying CUarray: %p", surface->cuImages[i]);
            drv->cu->cuArrayDestroy(surface->cuImages[i]);
            surface->cuImages[i] = NULL;
        }
    }
    return true;
}

bool hasAllocatedSurface(NVSurface *surface) {
    return surface->eglImage != EGL_NO_IMAGE;
}

bool copyFrameToSurface(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch) {
    int bpp = surface->format == cudaVideoSurfaceFormat_NV12 ? 1 : 2;
    //frameNo++;
    CUDA_MEMCPY2D cpy = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = ptr,
        //.srcXInBytes = frameNo++ % 80,
        .srcPitch = pitch,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = surface->cuImages[0],
        .Height = surface->height,
        .WidthInBytes = surface->width * bpp
    };
    CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&cpy));
    CUDA_MEMCPY2D cpy2 = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = ptr,
        .srcY = surface->height,
        .srcPitch = pitch,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = surface->cuImages[1],
        .Height = surface->height >> 1,
        .WidthInBytes = surface->width * bpp
    };
    CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&cpy2));

    drv->cu->cuStreamSynchronize(0);
    return true;
}

bool exportCudaPtr(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch, int *fourcc, int *fds, int *offsets, int *strides, uint64_t *mods, int *bppOut) {
    *bppOut = surface->format == cudaVideoSurfaceFormat_NV12 ? 1 : 2;

    if (!hasAllocatedSurface(surface) && !allocateSurface(drv, surface)) {
        LOG("Unable to allocate surface: %d", surface->pictureIdx);
        return false;
    }

    if (surface->fourcc == DRM_FORMAT_NV21) {
        LOG("Detected NV12/NV21 NVIDIA driver bug, attempting to work around");
        //free the old surface to prevent leaking them
        freeSurface(drv, surface);
        //this is a caused by a bug in old versions the driver that was fixed in the 510 series
        drv->useCorrectNV12Format = true;
        //re-export the frame in the correct format
        allocateSurface(drv, surface);
        if (surface->fourcc != DRM_FORMAT_NV12) {
            LOG("Work around unsuccessful");
        } else {
            LOG("Work around successful!");
        }
    }


    if (ptr != 0 && !copyFrameToSurface(drv, ptr, surface, pitch)) {
        LOG("Unable to update surface from frame");
        return false;
    }

    *fourcc = surface->fourcc;
    for (int i = 0; i < 4; i++) {
        if (surface->fds[i] != 0) {
            fds[i] = dup(surface->fds[i]);
        }
        offsets[i] = surface->offsets[i];
        strides[i] = surface->strides[i];
        mods[i] = surface->mods[i];
    }

    return true;
}
