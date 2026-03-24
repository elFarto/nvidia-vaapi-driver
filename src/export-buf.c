#include "vabackend.h"
#include "backend-common.h"
#include <stdio.h>
#include <ffnvcodec/dynlink_loader.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif

#include <drm.h>
#include <drm_fourcc.h>
#include <stdint.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>

#ifndef VA_FOURCC_NV16
#define VA_FOURCC_NV16 0x3631564e
#endif

static void eglWatchCallerOwnedDescriptorFds(
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

static void eglUnwatchCallerOwnedDescriptorFds(
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
static PFNEGLQUERYDMABUFMODIFIERSEXTPROC eglQueryDmaBufModifiersEXT;

typedef CUresult CUDAAPI tcuGraphicsEGLRegisterImage(
    CUgraphicsResource *pCudaResource,
    EGLImage image,
    unsigned int flags
);
typedef CUresult CUDAAPI tcuGraphicsResourceGetMappedEglFrame(
    CUeglFrame *pEglFrame,
    CUgraphicsResource resource,
    unsigned int index,
    unsigned int mipLevel
);

static tcuGraphicsEGLRegisterImage *cuGraphicsEGLRegisterImageDyn;
static tcuGraphicsResourceGetMappedEglFrame *cuGraphicsResourceGetMappedEglFrameDyn;
static bool cuEglInteropSymbolsLoaded;
static bool eglModifierCacheValid;
static EGLDisplay eglModifierCachedDisplay = EGL_NO_DISPLAY;
static uint32_t eglModifierCachedFourcc;
static uint64_t eglModifierCachedModifier;
static bool eglModifierCachedSupported;
static bool eglModifierCachedQueryWorked;

static void egl_initBackingImageFds(BackingImage *img) {
    if (img == NULL) {
        return;
    }
    for (uint32_t i = 0; i < ARRAY_SIZE(img->fds); i++) {
        img->fds[i] = -1;
    }
}

static void debug(EGLenum error,const char *command,EGLint messageType,EGLLabelKHR threadLabel,EGLLabelKHR objectLabel,const char* message) {
    LOG("[EGL] %s: %s", command, message);
}

static int egl_convertDmabufFdToNvFd(NVDriver *drv, int dmaBufFd) {
    if (drv == NULL) {
        return -1;
    }
    return convertDmabufFdToNvFd(
        dmaBufFd,
        drv->drmFd,
        -1,
        drv->driverContext.driverMajorVersion
    );
}

static bool egl_loadCudaEglInteropSymbols(NVDriver *drv) {
    if (cuEglInteropSymbolsLoaded) {
        return cuGraphicsEGLRegisterImageDyn != NULL &&
               cuGraphicsResourceGetMappedEglFrameDyn != NULL;
    }
    cuEglInteropSymbolsLoaded = true;

    if (drv == NULL || drv->cu == NULL || drv->cu->lib == NULL) {
        return false;
    }

    cuGraphicsEGLRegisterImageDyn =
        (tcuGraphicsEGLRegisterImage *)FFNV_SYM_FUNC(
            drv->cu->lib,
            "cuGraphicsEGLRegisterImage"
        );
    cuGraphicsResourceGetMappedEglFrameDyn =
        (tcuGraphicsResourceGetMappedEglFrame *)FFNV_SYM_FUNC(
            drv->cu->lib,
            "cuGraphicsResourceGetMappedEglFrame"
        );

    if (cuGraphicsEGLRegisterImageDyn == NULL ||
        cuGraphicsResourceGetMappedEglFrameDyn == NULL) {
        LOG(
            "CUDA EGL interop symbols unavailable register=%p mappedFrame=%p",
            (void *)cuGraphicsEGLRegisterImageDyn,
            (void *)cuGraphicsResourceGetMappedEglFrameDyn
        );
        return false;
    }
    return true;
}

static bool egl_isDmabufModifierSupported(
    EGLDisplay display,
    uint32_t fourcc,
    uint64_t modifier,
    bool *queryWorked
) {
    if (queryWorked != NULL) {
        *queryWorked = false;
    }
    if (eglQueryDmaBufModifiersEXT == NULL || display == EGL_NO_DISPLAY) {
        return false;
    }

    if (eglModifierCacheValid &&
        eglModifierCachedDisplay == display &&
        eglModifierCachedFourcc == fourcc &&
        eglModifierCachedModifier == modifier) {
        if (queryWorked != NULL) {
            *queryWorked = eglModifierCachedQueryWorked;
        }
        return eglModifierCachedSupported;
    }

    EGLint numModifiers = 0;
    if (!eglQueryDmaBufModifiersEXT(
            display,
            (EGLint)fourcc,
            0,
            NULL,
            NULL,
            &numModifiers)) {
        const EGLint err = eglGetError();
        LOG(
            "eglQueryDmaBufModifiersEXT(count) failed fourcc=0x%x err=0x%x",
            fourcc,
            err
        );
        eglModifierCacheValid = true;
        eglModifierCachedDisplay = display;
        eglModifierCachedFourcc = fourcc;
        eglModifierCachedModifier = modifier;
        eglModifierCachedSupported = false;
        eglModifierCachedQueryWorked = false;
        return false;
    }
    if (numModifiers <= 0) {
        eglModifierCacheValid = true;
        eglModifierCachedDisplay = display;
        eglModifierCachedFourcc = fourcc;
        eglModifierCachedModifier = modifier;
        eglModifierCachedSupported = false;
        eglModifierCachedQueryWorked = true;
        if (queryWorked != NULL) {
            *queryWorked = true;
        }
        return false;
    }

    EGLuint64KHR *modifiers = calloc((size_t)numModifiers, sizeof(*modifiers));
    EGLBoolean *externalOnly = calloc((size_t)numModifiers, sizeof(*externalOnly));
    if (modifiers == NULL || externalOnly == NULL) {
        free(modifiers);
        free(externalOnly);
        eglModifierCacheValid = true;
        eglModifierCachedDisplay = display;
        eglModifierCachedFourcc = fourcc;
        eglModifierCachedModifier = modifier;
        eglModifierCachedSupported = false;
        eglModifierCachedQueryWorked = false;
        return false;
    }

    EGLint fetchedModifiers = 0;
    bool listQueryWorked = false;
    bool supported = false;
    if (!eglQueryDmaBufModifiersEXT(
            display,
            (EGLint)fourcc,
            numModifiers,
            modifiers,
            externalOnly,
            &fetchedModifiers)) {
        const EGLint err = eglGetError();
        LOG(
            "eglQueryDmaBufModifiersEXT(list) failed fourcc=0x%x err=0x%x",
            fourcc,
            err
        );
        fetchedModifiers = 0;
    } else {
        listQueryWorked = true;
        const EGLint count = fetchedModifiers < numModifiers ? fetchedModifiers : numModifiers;
        for (EGLint i = 0; i < count; i++) {
            if ((uint64_t)modifiers[i] == modifier) {
                supported = true;
                break;
            }
        }
    }

    free(modifiers);
    free(externalOnly);

    eglModifierCacheValid = true;
    eglModifierCachedDisplay = display;
    eglModifierCachedFourcc = fourcc;
    eglModifierCachedModifier = modifier;
    eglModifierCachedSupported = supported;
    eglModifierCachedQueryWorked = listQueryWorked;
    if (queryWorked != NULL) {
        *queryWorked = eglModifierCachedQueryWorked;
    }
    return supported;
}

void eglResetProcessTransientState(void) {
    eglQueryStreamConsumerEventNV = NULL;
    eglStreamReleaseImageNV = NULL;
    eglStreamAcquireImageNV = NULL;
    eglExportDMABUFImageMESA = NULL;
    eglExportDMABUFImageQueryMESA = NULL;
    eglCreateStreamKHR = NULL;
    eglDestroyStreamKHR = NULL;
    eglStreamImageConsumerConnectNV = NULL;
    eglQueryDmaBufModifiersEXT = NULL;
    cuGraphicsEGLRegisterImageDyn = NULL;
    cuGraphicsResourceGetMappedEglFrameDyn = NULL;
    cuEglInteropSymbolsLoaded = false;
    eglModifierCacheValid = false;
    eglModifierCachedDisplay = EGL_NO_DISPLAY;
    eglModifierCachedFourcc = 0;
    eglModifierCachedModifier = 0;
    eglModifierCachedSupported = false;
    eglModifierCachedQueryWorked = false;
}

static void egl_releaseExporter(NVDriver *drv) {
    //TODO not sure if this is still needed as we don't return anything now
    LOG("Releasing exporter, %d outstanding frames", drv->numFramesPresented);
    while (true) {
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

static bool reconnect(NVDriver *drv) {
    LOG("Reconnecting to stream");
    eglInitialize(drv->eglDisplay, NULL, NULL);
    if (drv->cuStreamConnection != NULL) {
        CHECK_CUDA_RESULT_RETURN(drv->cu->cuEGLStreamProducerDisconnect(&drv->cuStreamConnection), false);
    }
    if (drv->eglStream != EGL_NO_STREAM_KHR) {
        eglDestroyStreamKHR(drv->eglDisplay, drv->eglStream);
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
    return true;
}

static void findGPUIndexFromFd(NVDriver *drv) {
    struct stat buf;
    int drmDeviceIndex = 0;
    PFNEGLQUERYDEVICESEXTPROC eglQueryDevicesEXT = (PFNEGLQUERYDEVICESEXTPROC) eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLQUERYDEVICEATTRIBEXTPROC eglQueryDeviceAttribEXT = (PFNEGLQUERYDEVICEATTRIBEXTPROC) eglGetProcAddress("eglQueryDeviceAttribEXT");
    PFNEGLQUERYDEVICESTRINGEXTPROC eglQueryDeviceStringEXT = (PFNEGLQUERYDEVICESTRINGEXTPROC) eglGetProcAddress("eglQueryDeviceStringEXT");

    if (eglQueryDevicesEXT == NULL || eglQueryDeviceAttribEXT == NULL) {
        LOG("No support for EGL_EXT_device_enumeration");
        drv->cudaGpuId = 0;
        return;
    } else if (drv->cudaGpuId == -1 && drv->drmFd == -1) {
        //there's no point scanning here as we don't have anything to match, just return GPU ID 0
        LOG("Defaulting to CUDA GPU ID 0. Use NVD_GPU to select a specific CUDA GPU");
        drv->cudaGpuId = 0;
    }

    //work out how we're searching for the GPU
    if (drv->cudaGpuId == -1 && drv->drmFd != -1) {
        //figure out the 'drm device index', basically the minor number of the device node & 0x7f
        //since we don't know/want to care if we're dealing with a master or render node
        fstat(drv->drmFd, &buf);
        drmDeviceIndex = minor(buf.st_rdev);
        LOG("Looking for DRM device index: %d", drmDeviceIndex);
    } else {
        LOG("Looking for GPU index: %d", drv->cudaGpuId);
    }

    //go grab some EGL devices
    EGLDeviceEXT devices[8];
    EGLint num_devices;
    if(!eglQueryDevicesEXT(8, devices, &num_devices)) {
        LOG("Unable to query EGL devices");
        drv->cudaGpuId = 0;
        return;
    }

    LOG("Found %d EGL devices", num_devices);
    for (int i = 0; i < num_devices; i++) {
        EGLAttrib attr = -1;

        //retrieve the DRM device path for this EGLDevice
        const char* drmRenderNodeFile = eglQueryDeviceStringEXT(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT);
        if (drmRenderNodeFile != NULL) {
            //if we have one, try and get the CUDA device id
            if (eglQueryDeviceAttribEXT(devices[i], EGL_CUDA_DEVICE_NV, &attr)) {
                LOG("Got EGL_CUDA_DEVICE_NV value '%d' for EGLDevice %d", attr, i);

                //if we're looking for a matching drm device index check it here
                if (drv->cudaGpuId == -1 && drv->drmFd != -1) {
                    stat(drmRenderNodeFile, &buf);
                    int foundDrmDeviceIndex = minor(buf.st_rdev);
                    LOG("Found drmDeviceIndex: %d", foundDrmDeviceIndex);
                    if (foundDrmDeviceIndex != drmDeviceIndex) {
                        continue;
                    }
                } else if (drv->cudaGpuId != attr) {
                    //LOG("Not selected device, skipping");
                    continue;
                }

                //if it's the device we're looking for, check the modeset parameter on it.
                bool checkModeset = checkModesetParameterFromFd(drv->drmFd);
                if (!checkModeset) {
                    continue;
                }

                LOG("Selecting EGLDevice %d", i);
                drv->eglDevice = devices[i];
                drv->cudaGpuId = attr;
                return;
            } else {
                LOG("No EGL_CUDA_DEVICE_NV support for EGLDevice %d", i);
            }
        } else {
            LOG("No DRM device file for EGLDevice %d", i);
        }
    }
    LOG("No match found, falling back to default device");
    drv->cudaGpuId = 0;
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
    eglQueryDmaBufModifiersEXT = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC) eglGetProcAddress("eglQueryDmaBufModifiersEXT");

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
        bool r16 = false, rg1616 = false;
        for (int i = 0; i < formatCount; i++) {
            if (formats[i] == DRM_FORMAT_R16) {
                r16 = true;
            } else if (formats[i] == DRM_FORMAT_RG1616) {
                rg1616 = true;
            }
        }
        drv->supports16BitSurface = r16 & rg1616;
        drv->supports444Surface = false;
        if (drv->supports16BitSurface) {
            LOG("Driver supports 16-bit surfaces");
        } else {
            LOG("Driver doesn't support 16-bit surfaces");
        }
    }

    const bool dmabufSharingSupported =
        isCudaDmabufSupported(drv, drv->cudaGpuId);
    bool allowDirectDmabufImport = dmabufSharingSupported;

    const char *preferDirectDmabufImportEnv = getenv("NVD_PREFER_DIRECT_DMABUF_CUDA_IMPORT");
    const char *disableDirectDmabufImportEnv = getenv("NVD_DISABLE_DIRECT_DMABUF_CUDA_IMPORT");
    if (isTruthyEnv(preferDirectDmabufImportEnv)) {
        allowDirectDmabufImport = true;
    }
    if (isTruthyEnv(disableDirectDmabufImportEnv)) {
        allowDirectDmabufImport = false;
    }
    drv->allowDirectDmabufCudaImport = allowDirectDmabufImport;
    LOG(
        "egl external import policy: gpu_id=%d dmabuf_sharing_supported=%s allow_direct_dmabuf=%s prefer_env=%s disable_env=%s",
        drv->cudaGpuId,
        dmabufSharingSupported ? "yes" : "no",
        allowDirectDmabufImport ? "yes" : "no",
        preferDirectDmabufImportEnv != NULL ? preferDirectDmabufImportEnv : "(null)",
        disableDirectDmabufImportEnv != NULL ? disableDirectDmabufImportEnv : "(null)"
    );

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
    if (img == NULL) {
        return NULL;
    }
    egl_initBackingImageFds(img);
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

    LOG(
        "createBackingImage exported: size=%ux%u fourcc=0x%x stride0=%d stride1=%d offset0=%d offset1=%d",
        img->width,
        img->height,
        img->fourcc,
        img->strides[0],
        img->strides[1],
        img->offsets[0],
        img->offsets[1]
    );

    return img;
}


static bool egl_destroyBackingImage(NVDriver *drv, BackingImage *img) {
    //if we're attached to a surface, update the surface to remove us
    if (img->surface != NULL) {
        img->surface->backingImage = NULL;
    }

    LOG("Destroying BackingImage: %p", img);
    for (int i = 0; i < 4; i++) {
        if (img->fds[i] >= 0) {
            backendCloseFd(img->fds[i], "egl_destroy_backing_fd");
            img->fds[i] = -1;
        }
    }
    //eglStreamReleaseImageNV(drv->eglDisplay, drv->eglStream, surface->eglImage, EGL_NO_SYNC);
    //destroy them rather than releasing them
    if (img->image != EGL_NO_IMAGE) {
        eglDestroyImage(drv->eglDisplay, img->image);
        img->image = EGL_NO_IMAGE;
    }

    for (uint32_t i = 0; i < 3; i++) {
        if (img->arrays[i] != NULL) {
            CHECK_CUDA_RESULT_RETURN(drv->cu->cuArrayDestroy(img->arrays[i]), false);
            img->arrays[i] = NULL;
        }
        if (img->cudaImages[i].mipmapArray != NULL) {
            CHECK_CUDA_RESULT_RETURN(drv->cu->cuMipmappedArrayDestroy(img->cudaImages[i].mipmapArray), false);
            img->cudaImages[i].mipmapArray = NULL;
        }
        if (img->cudaImages[i].extMem != NULL) {
            CHECK_CUDA_RESULT_RETURN(drv->cu->cuDestroyExternalMemory(img->cudaImages[i].extMem), false);
            img->cudaImages[i].extMem = NULL;
        }
    }

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

    // External DRM PRIME imports create CUDA-array-only backing images.
    // These are not pooled and must be destroyed immediately.
    if (surface->backingImage->image == EGL_NO_IMAGE) {
        if (!egl_destroyBackingImage(drv, surface->backingImage)) {
            LOG("Unable to destroy imported backing image");
        }
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
    } else {
        LOG(
            "unsupported EGL surface format=%d bitDepth=%d chroma=%d",
            surface->format,
            surface->bitDepth,
            surface->chromaFormat
        );
        return NULL;
    }
    CUDA_ARRAY3D_DESCRIPTOR arrDesc = {
        .Width = eglframe.width,
        .Height = eglframe.height,
        .Depth = 0,
        .NumChannels = 1,
        .Flags = 0,
        .Format = eglframe.cuFormat
    };
    const bool chroma422 = surface->chromaFormat == cudaVideoChromaFormat_422;
    CUDA_ARRAY3D_DESCRIPTOR arr2Desc = {
        .Width = eglframe.width >> 1,
        .Height = chroma422 ? eglframe.height : (eglframe.height >> 1),
        .Depth = 0,
        .NumChannels = 2,
        .Flags = 0,
        .Format = eglframe.cuFormat
    };
    CHECK_CUDA_RESULT_RETURN(drv->cu->cuArray3DCreate(&eglframe.frame.pArray[0], &arrDesc), NULL);
    CHECK_CUDA_RESULT_RETURN(drv->cu->cuArray3DCreate(&eglframe.frame.pArray[1], &arr2Desc), NULL);

    pthread_mutex_lock(&drv->exportMutex);

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

    int bpp = 2;
    if (surface->format == cudaVideoSurfaceFormat_NV12
#if NVENCAPI_MAJOR_VERSION >= 13
        || surface->format == cudaVideoSurfaceFormat_NV16
#endif
       ) {
        bpp = 1;
    }
    const uint32_t chromaHeight =
        surface->chromaFormat == cudaVideoChromaFormat_422 ? surface->height : (surface->height >> 1);
    CUDA_MEMCPY2D cpy = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = ptr,
        .srcPitch = pitch,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = surface->backingImage->arrays[0],
        .Height = surface->height,
        .WidthInBytes = surface->width * bpp
    };
    CHECK_CUDA_RESULT_RETURN(drv->cu->cuMemcpy2DAsync(&cpy, 0), false);
    CUDA_MEMCPY2D cpy2 = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = ptr,
        .srcY = surface->height,
        .srcPitch = pitch,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = surface->backingImage->arrays[1],
        .Height = chromaHeight,
        .WidthInBytes = surface->width * bpp
    };
    CHECK_CUDA_RESULT_RETURN(drv->cu->cuMemcpy2D(&cpy2), false);

    //notify anyone waiting for us to be resolved
    pthread_mutex_lock(&surface->mutex);
    surface->resolving = 0;
    pthread_cond_signal(&surface->cond);
    pthread_mutex_unlock(&surface->mutex);

    return true;
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

static bool egl_allocateStandaloneCudaArraysForSurface(NVDriver *drv, const NVSurface *surface, BackingImage *img, int *bpp) {
    CUarray yArray = NULL;
    CUarray uvArray = NULL;
    CUarray_format cuFormat;
    int bytesPerPixel;

    if (surface->rtFormat == VA_RT_FORMAT_RGB32) {
        cuFormat = CU_AD_FORMAT_UNSIGNED_INT8;
        bytesPerPixel = 4;
        img->format = NV_FORMAT_ARGB;

        CUDA_ARRAY3D_DESCRIPTOR argbDesc = {
            .Width = surface->width,
            .Height = surface->height,
            .Depth = 0,
            .NumChannels = 4,
            .Flags = 0,
            .Format = cuFormat
        };
        if (CHECK_CUDA_RESULT(drv->cu->cuArray3DCreate(&yArray, &argbDesc))) {
            return false;
        }

        img->arrays[0] = yArray;
        img->image = EGL_NO_IMAGE;
        *bpp = bytesPerPixel;
        return true;
    } else if (surface->format == cudaVideoSurfaceFormat_NV12) {
        cuFormat = CU_AD_FORMAT_UNSIGNED_INT8;
        bytesPerPixel = 1;
        img->format = NV_FORMAT_NV12;
#if NVENCAPI_MAJOR_VERSION >= 13
    } else if (surface->format == cudaVideoSurfaceFormat_NV16) {
        cuFormat = CU_AD_FORMAT_UNSIGNED_INT8;
        bytesPerPixel = 1;
        img->format = NV_FORMAT_NV16;
#endif
    } else if (surface->format == cudaVideoSurfaceFormat_P016) {
        cuFormat = CU_AD_FORMAT_UNSIGNED_INT16;
        bytesPerPixel = 2;
        switch (surface->bitDepth) {
            case 10:
                img->format = NV_FORMAT_P010;
                break;
            case 12:
                img->format = NV_FORMAT_P012;
                break;
            default:
                img->format = NV_FORMAT_P016;
                break;
        }
#if NVENCAPI_MAJOR_VERSION >= 13
    } else if (surface->format == cudaVideoSurfaceFormat_P216) {
        cuFormat = CU_AD_FORMAT_UNSIGNED_INT16;
        bytesPerPixel = 2;
        img->format = NV_FORMAT_P210;
#endif
    } else {
        LOG(
            "unsupported surface format=%d bitDepth=%u",
            surface->format,
            surface->bitDepth
        );
        return false;
    }

    CUDA_ARRAY3D_DESCRIPTOR yDesc = {
        .Width = surface->width,
        .Height = surface->height,
        .Depth = 0,
        .NumChannels = 1,
        .Flags = 0,
        .Format = cuFormat
    };
    const bool chroma422 = surface->chromaFormat == cudaVideoChromaFormat_422;
    CUDA_ARRAY3D_DESCRIPTOR uvDesc = {
        .Width = surface->width >> 1,
        .Height = chroma422 ? surface->height : (surface->height >> 1),
        .Depth = 0,
        .NumChannels = 2,
        .Flags = 0,
        .Format = cuFormat
    };

    if (CHECK_CUDA_RESULT(drv->cu->cuArray3DCreate(&yArray, &yDesc))) {
        return false;
    }
    if (CHECK_CUDA_RESULT(drv->cu->cuArray3DCreate(&uvArray, &uvDesc))) {
        drv->cu->cuArrayDestroy(yArray);
        return false;
    }

    img->arrays[0] = yArray;
    img->arrays[1] = uvArray;
    img->image = EGL_NO_IMAGE;
    *bpp = bytesPerPixel;
    return true;
}

static bool egl_copyMappedDmabufPlaneToArray(
    NVDriver *drv,
    int fd,
    uint32_t objectSize,
    uint32_t offset,
    uint32_t pitch,
    uint32_t widthInBytes,
    uint32_t height,
    CUarray dstArray
) {
    if (fd < 0 || dstArray == NULL || pitch == 0 || height == 0 || widthInBytes == 0) {
        return false;
    }
    if (pitch < widthInBytes) {
        LOG(
            "invalid plane pitch=%u widthInBytes=%u",
            pitch,
            widthInBytes
        );
        return false;
    }

    uint64_t requiredSize = (uint64_t)offset + ((uint64_t)pitch * height);
    uint64_t mapSize = objectSize;
    if (mapSize < requiredSize) {
        mapSize = requiredSize;
    }
    if (mapSize == 0 || mapSize > SIZE_MAX) {
        return false;
    }

    const int importFd = dup(fd);
    if (importFd < 0) {
        return false;
    }

    struct stat fdStat;
    if (fstat(importFd, &fdStat) == 0 && fdStat.st_size > 0) {
        const uint64_t fdSize = (uint64_t)fdStat.st_size;
        if (fdSize < requiredSize) {
            backendCloseTrackedNvKmsFd(importFd, "egl_copy_mapped_short_fd");
            return false;
        }
        if (mapSize > fdSize) {
            mapSize = fdSize;
        }
    }

    void *mapped = mmap(NULL, (size_t)mapSize, PROT_READ, MAP_SHARED, importFd, 0);
    backendCloseTrackedNvKmsFd(importFd, "egl_copy_mapped_complete");
    if (mapped == MAP_FAILED) {
        return false;
    }

    CUDA_MEMCPY2D copy = {
        .srcMemoryType = CU_MEMORYTYPE_HOST,
        .srcHost = ((const uint8_t *)mapped) + offset,
        .srcPitch = pitch,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = dstArray,
        .WidthInBytes = widthInBytes,
        .Height = height
    };
    CUresult copyResult = drv->cu->cuMemcpy2D(&copy);
    munmap(mapped, (size_t)mapSize);
    if (copyResult != CUDA_SUCCESS) {
        const char *errStr = "unknown";
        drv->cu->cuGetErrorString(copyResult, &errStr);
        LOG(
            "plane copy failed cuerr=%d (%s)",
            copyResult,
            errStr
        );
        return false;
    }
    return true;
}

static bool egl_copyMappedEglFramePlaneToArray(
    NVDriver *drv,
    const CUeglFrame *eglFrame,
    uint32_t planeIndex,
    uint32_t widthInBytes,
    uint32_t height,
    CUarray dstArray
) {
    CUDA_MEMCPY2D copy = {
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = dstArray,
        .WidthInBytes = widthInBytes,
        .Height = height
    };

    if (eglFrame->frameType == CU_EGL_FRAME_TYPE_ARRAY) {
        copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        copy.srcArray = eglFrame->frame.pArray[planeIndex];
    } else if (eglFrame->frameType == CU_EGL_FRAME_TYPE_PITCH) {
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = (CUdeviceptr)(uintptr_t)eglFrame->frame.pPitch[planeIndex];
        copy.srcPitch = eglFrame->pitch;
    } else {
        LOG(
            "unsupported CUeglFrame type=%u",
            eglFrame->frameType
        );
        return false;
    }

    CUresult copyResult = drv->cu->cuMemcpy2D(&copy);
    if (copyResult != CUDA_SUCCESS) {
        const char *errStr = "unknown";
        drv->cu->cuGetErrorString(copyResult, &errStr);
        LOG(
            "EGLImage CUDA copy failed plane=%u cuerr=%d (%s)",
            planeIndex,
            copyResult,
            errStr
        );
        return false;
    }
    return true;
}

static bool egl_copyExternalSurfaceToArraysViaEglImage(
    NVDriver *drv,
    const VADRMPRIMESurfaceDescriptor *desc,
    const uint32_t *planeObjectIndex,
    const uint32_t *planeOffsets,
    const uint32_t *planePitches,
    uint32_t widthInBytes,
    const uint32_t *planeHeights,
    CUarray *dstArrays
) {
    if (drv == NULL || desc == NULL || dstArrays == NULL) {
        return false;
    }
    if (!egl_loadCudaEglInteropSymbols(drv)) {
        return false;
    }
    if (drv->cu->cuGraphicsUnregisterResource == NULL) {
        LOG("cuGraphicsUnregisterResource is unavailable");
        return false;
    }

    const char *eglExt = eglQueryString(drv->eglDisplay, EGL_EXTENSIONS);
    const bool hasModifierExt =
        eglExt != NULL && strstr(eglExt, "EGL_EXT_image_dma_buf_import_modifiers") != NULL;
    const EGLAttrib planeFdAttrs[2] = {
        EGL_DMA_BUF_PLANE0_FD_EXT,
        EGL_DMA_BUF_PLANE1_FD_EXT
    };
    const EGLAttrib planeOffsetAttrs[2] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT
    };
    const EGLAttrib planePitchAttrs[2] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        EGL_DMA_BUF_PLANE1_PITCH_EXT
    };
    const EGLAttrib planeModifierLoAttrs[2] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT
    };
    const EGLAttrib planeModifierHiAttrs[2] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT
    };

    EGLImage eglImage = EGL_NO_IMAGE;
    CUgraphicsResource resource = NULL;
    bool copyOk = false;
    bool tryWithModifiers = hasModifierExt;

    if (tryWithModifiers) {
        bool shouldSkipModifierAttempt = false;
        for (uint32_t plane = 0; plane < 2; plane++) {
            const uint32_t objectIndex = planeObjectIndex[plane];
            const uint64_t modifier = desc->objects[objectIndex].drm_format_modifier;
            if (modifier == DRM_FORMAT_MOD_INVALID) {
                shouldSkipModifierAttempt = true;
                LOG(
                    "skipping modifier attempt due to invalid modifier object=%u plane=%u",
                    objectIndex,
                    plane
                );
                break;
            }

            bool queryWorked = false;
            const bool modifierSupported =
                egl_isDmabufModifierSupported(
                    drv->eglDisplay,
                    desc->fourcc,
                    modifier,
                    &queryWorked
                );
            if (queryWorked && !modifierSupported) {
                shouldSkipModifierAttempt = true;
                LOG(
                    "skipping modifier attempt: unsupported fourcc=0x%x modifier=0x%llx object=%u plane=%u",
                    desc->fourcc,
                    (unsigned long long)modifier,
                    objectIndex,
                    plane
                );
                break;
            }
        }
        if (shouldSkipModifierAttempt) {
            tryWithModifiers = false;
        }
    }

    const bool modifierAttempts[2] = {tryWithModifiers, false};
    const uint32_t modifierAttemptCount = tryWithModifiers ? 2 : 1;
    for (uint32_t modifierAttempt = 0; modifierAttempt < modifierAttemptCount; modifierAttempt++) {
        const bool useModifiers = modifierAttempts[modifierAttempt];
        EGLAttrib attrs[64];
        size_t attrCount = 0;
        attrs[attrCount++] = (EGLAttrib)EGL_WIDTH;
        attrs[attrCount++] = (EGLAttrib)desc->width;
        attrs[attrCount++] = (EGLAttrib)EGL_HEIGHT;
        attrs[attrCount++] = (EGLAttrib)desc->height;
        attrs[attrCount++] = (EGLAttrib)EGL_LINUX_DRM_FOURCC_EXT;
        attrs[attrCount++] = (EGLAttrib)desc->fourcc;

        for (uint32_t plane = 0; plane < 2; plane++) {
            const uint32_t objectIndex = planeObjectIndex[plane];
            const int objectFd = desc->objects[objectIndex].fd;
            if (objectFd < 0) {
                LOG(
                    "invalid dmabuf fd object=%u plane=%u",
                    objectIndex,
                    plane
                );
                return false;
            }

            attrs[attrCount++] = planeFdAttrs[plane];
            attrs[attrCount++] = (EGLAttrib)objectFd;
            attrs[attrCount++] = planeOffsetAttrs[plane];
            attrs[attrCount++] = (EGLAttrib)planeOffsets[plane];
            attrs[attrCount++] = planePitchAttrs[plane];
            attrs[attrCount++] = (EGLAttrib)planePitches[plane];

            if (useModifiers) {
                const uint64_t mod = desc->objects[objectIndex].drm_format_modifier;
                attrs[attrCount++] = planeModifierLoAttrs[plane];
                attrs[attrCount++] = (EGLAttrib)(mod & 0xffffffffu);
                attrs[attrCount++] = planeModifierHiAttrs[plane];
                attrs[attrCount++] = (EGLAttrib)((mod >> 32) & 0xffffffffu);
            }
        }
        attrs[attrCount++] = (EGLAttrib)EGL_NONE;

        eglImage = eglCreateImage(
            drv->eglDisplay,
            EGL_NO_CONTEXT,
            EGL_LINUX_DMA_BUF_EXT,
            (EGLClientBuffer)NULL,
            attrs
        );
        if (eglImage == EGL_NO_IMAGE) {
            const EGLint eglErr = eglGetError();
            LOG(
                "eglCreateImage(dmabuf) failed modifiers=%s err=0x%x",
                useModifiers ? "yes" : "no",
                eglErr
            );
            continue;
        }
        LOG(
            "eglCreateImage(dmabuf) succeeded modifiers=%s",
            useModifiers ? "yes" : "no"
        );
        break;
    }

    if (eglImage == EGL_NO_IMAGE) {
        return false;
    }

    CUresult cuResult =
        cuGraphicsEGLRegisterImageDyn(
            &resource,
            eglImage,
            CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY
        );
    if (cuResult != CUDA_SUCCESS) {
        const char *errStr = "unknown";
        drv->cu->cuGetErrorString(cuResult, &errStr);
        LOG(
            "cuGraphicsEGLRegisterImage failed cuerr=%d (%s)",
            cuResult,
            errStr
        );
        goto cleanup;
    }

    CUeglFrame eglFrame;
    memset(&eglFrame, 0, sizeof(eglFrame));
    cuResult = cuGraphicsResourceGetMappedEglFrameDyn(&eglFrame, resource, 0, 0);
    if (cuResult != CUDA_SUCCESS) {
        const char *errStr = "unknown";
        drv->cu->cuGetErrorString(cuResult, &errStr);
        LOG(
            "cuGraphicsResourceGetMappedEglFrame failed cuerr=%d (%s)",
            cuResult,
            errStr
        );
        goto cleanup;
    }

    if (eglFrame.planeCount < 2) {
        LOG(
            "mapped EGL frame has insufficient planes=%u",
            eglFrame.planeCount
        );
        goto cleanup;
    }

    for (uint32_t plane = 0; plane < 2; plane++) {
        if (!egl_copyMappedEglFramePlaneToArray(
                drv,
                &eglFrame,
                plane,
                widthInBytes,
                planeHeights[plane],
                dstArrays[plane])) {
            goto cleanup;
        }
    }
    copyOk = true;

cleanup:
    if (resource != NULL) {
        CUresult unregResult = drv->cu->cuGraphicsUnregisterResource(resource);
        if (unregResult != CUDA_SUCCESS) {
            const char *errStr = "unknown";
            drv->cu->cuGetErrorString(unregResult, &errStr);
            LOG(
                "cuGraphicsUnregisterResource failed cuerr=%d (%s)",
                unregResult,
                errStr
            );
        }
    }
    if (eglImage != EGL_NO_IMAGE) {
        eglDestroyImage(drv->eglDisplay, eglImage);
    }

    return copyOk;
}

static bool egl_copyExternalDmabufPlaneToArrayGpu(
    NVDriver *drv,
    int fd,
    uint32_t objectSize,
    uint32_t offset,
    uint32_t pitch,
    uint32_t widthInBytes,
    uint32_t height,
    CUarray dstArray,
    bool useDmaBufHandle
) {
    if (fd < 0 || dstArray == NULL || pitch == 0 || height == 0 || widthInBytes == 0) {
        LOG(
            "GPU-copy invalid args fd=%d dstArray=%p pitch=%u height=%u widthBytes=%u",
            fd,
            (void *)dstArray,
            pitch,
            height,
            widthInBytes
        );
        return false;
    }
    if (pitch < widthInBytes) {
        LOG(
            "invalid plane pitch=%u widthInBytes=%u",
            pitch,
            widthInBytes
        );
        return false;
    }
    if (drv->cu->cuExternalMemoryGetMappedBuffer == NULL) {
        LOG("GPU-copy unavailable: cuExternalMemoryGetMappedBuffer is NULL");
        return false;
    }

    uint64_t requiredSize = (uint64_t)offset + ((uint64_t)pitch * height);
    uint64_t importSize = objectSize;
    if (importSize < requiredSize) {
        importSize = requiredSize;
    }
    if (importSize == 0) {
        LOG(
            "GPU-copy invalid import size objectSize=%u required=%llu",
            objectSize,
            (unsigned long long)requiredSize
        );
        return false;
    }
    if (importSize > UINT32_MAX) {
        LOG(
            "GPU-copy invalid import size objectSize=%u required=%llu",
            objectSize,
            (unsigned long long)requiredSize
        );
        return false;
    }

    int importFd = dup(fd);
    if (importFd < 0) {
        LOG(
            "GPU-copy dup failed fd=%d errno=%d",
            fd,
            errno
        );
        return false;
    }

    if (copy_external_plane_to_cuda_array(
            drv,
            importFd,
            useDmaBufHandle,
            (uint32_t)importSize,
            offset,
            pitch,
            widthInBytes,
            height,
            dstArray)) {
        return true;
    }

    LOG(
        "GPU-copy failed for plane fd=%d offset=%u pitch=%u widthBytes=%u height=%u; falling back to CPU map copy",
        fd,
        offset,
        pitch,
        widthInBytes,
        height
    );
    return false;
}

static bool egl_importExternalSurfaceImpl(NVDriver *drv, NVSurface *surface, const VADRMPRIMESurfaceDescriptor *desc) {
    if (desc == NULL) {
        return false;
    }

    const bool isArgbImport =
        surface->rtFormat == VA_RT_FORMAT_RGB32 &&
        (desc->fourcc == DRM_FORMAT_ARGB8888 || desc->fourcc == VA_FOURCC_ARGB);
    if (!isArgbImport &&
        surface->format != cudaVideoSurfaceFormat_NV12 &&
        surface->format != cudaVideoSurfaceFormat_P016
#if NVENCAPI_MAJOR_VERSION >= 13
        && surface->format != cudaVideoSurfaceFormat_NV16
        && surface->format != cudaVideoSurfaceFormat_P216
#endif
       ) {
        LOG(
            "unsupported surface format=%d descriptor_fourcc=0x%x",
            surface->format,
            desc->fourcc
        );
        return false;
    }

    if (desc->num_objects == 0 || desc->num_layers == 0) {
        LOG(
            "invalid external descriptor objects=%u layers=%u",
            desc->num_objects,
            desc->num_layers
        );
        return false;
    }
    if (desc->num_objects > ARRAY_SIZE(desc->objects) ||
        desc->num_layers > ARRAY_SIZE(desc->layers)) {
        LOG(
            "external descriptor exceeds bounds objects=%u(max=%zu) layers=%u(max=%zu)",
            desc->num_objects,
            ARRAY_SIZE(desc->objects),
            desc->num_layers,
            ARRAY_SIZE(desc->layers)
        );
        return false;
    }

    const uint32_t expectedPlanes = isArgbImport ? 1 : 2;
    uint32_t planeObjectIndex[2] = {0};
    uint32_t planeOffsets[2] = {0};
    uint32_t planePitches[2] = {0};
    uint32_t planeCount = 0;

    for (uint32_t i = 0; i < desc->num_layers && planeCount < expectedPlanes; i++) {
        if (desc->layers[i].num_planes > ARRAY_SIZE(desc->layers[i].object_index)) {
            LOG(
                "invalid descriptor layer[%u] num_planes=%u (max=%zu)",
                i,
                desc->layers[i].num_planes,
                ARRAY_SIZE(desc->layers[i].object_index)
            );
            return false;
        }
        for (uint32_t j = 0; j < desc->layers[i].num_planes && planeCount < expectedPlanes; j++) {
            uint32_t objectIndex = desc->layers[i].object_index[j];
            if (objectIndex >= desc->num_objects) {
                return false;
            }
            planeObjectIndex[planeCount] = objectIndex;
            planeOffsets[planeCount] = desc->layers[i].offset[j];
            planePitches[planeCount] = desc->layers[i].pitch[j];
            planeCount++;
        }
    }

    if (planeCount < expectedPlanes) {
        LOG(
            "requires %u planes, got %u",
            expectedPlanes,
            planeCount
        );
        return false;
    }

    BackingImage *img = calloc(1, sizeof(BackingImage));
    if (img == NULL) {
        return false;
    }
    egl_initBackingImageFds(img);
    img->width = surface->width;
    img->height = surface->height;
    img->fourcc = desc->fourcc;

    int bpp = 1;
    if (!egl_allocateStandaloneCudaArraysForSurface(drv, surface, img, &bpp)) {
        free(img);
        return false;
    }

    uint32_t planeWidthBytes[2] = {0};
    const bool chroma422 = surface->chromaFormat == cudaVideoChromaFormat_422;
    uint32_t planeHeights[2] = {0};
    if (isArgbImport) {
        planeWidthBytes[0] = surface->width * bpp;
        planeHeights[0] = surface->height;
    } else {
        planeWidthBytes[0] = surface->width * bpp;
        planeWidthBytes[1] = surface->width * bpp;
        planeHeights[0] = surface->height;
        planeHeights[1] = chroma422 ? surface->height : (surface->height >> 1);
    }

    if (expectedPlanes == 2 && egl_copyExternalSurfaceToArraysViaEglImage(
            drv,
            desc,
            planeObjectIndex,
            planeOffsets,
            planePitches,
            planeWidthBytes[0],
            planeHeights,
            img->arrays)) {
        for (uint32_t i = 0; i < expectedPlanes; i++) {
            const uint32_t objectIndex = planeObjectIndex[i];
            const int objectFd = desc->objects[objectIndex].fd;
            const uint32_t objectSize = desc->objects[objectIndex].size;
            int dupFd = dup(objectFd);
            if (dupFd < 0) {
                LOG(
                    "failed to duplicate object fd=%d for plane=%u errno=%d",
                    objectFd,
                    i,
                    errno
                );
                egl_destroyBackingImage(drv, img);
                return false;
            }
            img->fds[i] = dupFd;
            img->offsets[i] = planeOffsets[i];
            img->strides[i] = planePitches[i];
            img->mods[i] = desc->objects[objectIndex].drm_format_modifier;
            img->size[i] = objectSize;
        }
        egl_attachBackingImageToSurface(surface, img);
        LOG(
            "imported via EGLImage+CUDA-EGL interop: size=%ux%u fourcc=0x%x",
            surface->width,
            surface->height,
            desc->fourcc
        );
        return true;
    }

    bool usedCpuCopyFallback = false;
    int cachedConvertedObjectFds[ARRAY_SIZE(desc->objects)];
    bool cachedConvertedObjectTried[ARRAY_SIZE(desc->objects)];
    for (uint32_t i = 0; i < ARRAY_SIZE(desc->objects); i++) {
        cachedConvertedObjectFds[i] = -1;
        cachedConvertedObjectTried[i] = false;
    }

    for (uint32_t i = 0; i < expectedPlanes; i++) {
        const uint32_t objectIndex = planeObjectIndex[i];
        const int objectFd = desc->objects[objectIndex].fd;
        const uint32_t objectSize = desc->objects[objectIndex].size;
        bool usedNvKmsFallback = false;
        bool copiedPlane = false;
        if (drv->allowDirectDmabufCudaImport) {
            copiedPlane =
                egl_copyExternalDmabufPlaneToArrayGpu(
                    drv,
                    objectFd,
                    objectSize,
                    planeOffsets[i],
                    planePitches[i],
                    planeWidthBytes[i],
                    planeHeights[i],
                    img->arrays[i],
                    true
                );
            if (!copiedPlane) {
                if (drv->allowDirectDmabufCudaImport) {
                    drv->allowDirectDmabufCudaImport = false;
                    LOG(
                        "disabling direct raw dmabuf CUDA import after first failure; using NVKMS conversion path"
                    );
                }
            }
        }
        if (!copiedPlane) {
            if (!cachedConvertedObjectTried[objectIndex]) {
                cachedConvertedObjectTried[objectIndex] = true;
                cachedConvertedObjectFds[objectIndex] =
                    egl_convertDmabufFdToNvFd(drv, objectFd);
            }
            if (cachedConvertedObjectFds[objectIndex] >= 0) {
                copiedPlane =
                    egl_copyExternalDmabufPlaneToArrayGpu(
                        drv,
                        cachedConvertedObjectFds[objectIndex],
                        objectSize,
                        planeOffsets[i],
                        planePitches[i],
                        planeWidthBytes[i],
                        planeHeights[i],
                        img->arrays[i],
                        false
                    );
                usedNvKmsFallback = copiedPlane;
                if (copiedPlane) {
                    LOG(
                        "GPU-copy plane=%u succeeded via NVKMS memfd fallback object=%u",
                        i,
                        objectIndex
                    );
                }
            }
        }
        if (!copiedPlane) {
            copiedPlane =
                egl_copyMappedDmabufPlaneToArray(
                    drv,
                    objectFd,
                    objectSize,
                    planeOffsets[i],
                    planePitches[i],
                    planeWidthBytes[i],
                    planeHeights[i],
                    img->arrays[i]
                );
            if (!copiedPlane) {
                goto fail;
            }
            usedCpuCopyFallback = true;
        }

        int dupFd = dup(objectFd);
        if (dupFd < 0) {
            LOG(
                "failed to duplicate object fd=%d for plane=%u errno=%d",
                objectFd,
                i,
                errno
            );
            goto fail;
        }
        img->fds[i] = dupFd;
        img->offsets[i] = planeOffsets[i];
        img->strides[i] = planePitches[i];
        img->mods[i] = desc->objects[objectIndex].drm_format_modifier;
        img->size[i] = objectSize;
        LOG(
            "imported plane=%u object=%u used_nvkms=%s",
            i,
            objectIndex,
            usedNvKmsFallback ? "yes" : "no"
        );
    }

    for (uint32_t i = 0; i < ARRAY_SIZE(desc->objects); i++) {
        if (cachedConvertedObjectFds[i] >= 0) {
            backendCloseTrackedNvKmsFd(cachedConvertedObjectFds[i], "egl_cached_converted_cleanup_success");
        }
    }

    egl_attachBackingImageToSurface(surface, img);
    if (usedCpuCopyFallback) {
        LOG(
            "imported via CPU map+CUDA copy: size=%ux%u fourcc=0x%x",
            surface->width,
            surface->height,
            desc->fourcc
        );
    } else {
        LOG(
            "imported via GPU external-memory copy: size=%ux%u fourcc=0x%x",
            surface->width,
            surface->height,
            desc->fourcc
        );
    }
    return true;

fail:
    for (uint32_t i = 0; i < ARRAY_SIZE(desc->objects); i++) {
        if (cachedConvertedObjectFds[i] >= 0) {
            backendCloseTrackedNvKmsFd(cachedConvertedObjectFds[i], "egl_cached_converted_cleanup_fail");
        }
    }
    egl_destroyBackingImage(drv, img);
    return false;
}

static bool egl_importExternalSurface(NVDriver *drv, NVSurface *surface, const VADRMPRIMESurfaceDescriptor *desc) {
    bool imported = false;
    eglWatchCallerOwnedDescriptorFds(desc, "egl_import_begin");
    pthread_mutex_lock(&drv->exportMutex);
    imported = egl_importExternalSurfaceImpl(drv, surface, desc);
    pthread_mutex_unlock(&drv->exportMutex);
    eglUnwatchCallerOwnedDescriptorFds(desc, "egl_import_end");
    if (!imported) {
        LOG("import failed; fallback to internal surface allocation");
    }
    return imported;
}

static const NVFormatInfo *egl_getFormatInfoByEnum(NVFormat format) {
    const NVFormatInfo *fmtInfo = NULL;

    switch (format) {
    case NV_FORMAT_NV12:
        fmtInfo = &formatsInfo[NV_FORMAT_NV12];
        break;
#if NVENCAPI_MAJOR_VERSION >= 13
    case NV_FORMAT_NV16:
        fmtInfo = &formatsInfo[NV_FORMAT_NV16];
        break;
#endif
    case NV_FORMAT_P010:
        fmtInfo = &formatsInfo[NV_FORMAT_P010];
        break;
#if NVENCAPI_MAJOR_VERSION >= 13
    case NV_FORMAT_P210:
        fmtInfo = &formatsInfo[NV_FORMAT_P210];
        break;
#endif
    case NV_FORMAT_P012:
        fmtInfo = &formatsInfo[NV_FORMAT_P012];
        break;
    case NV_FORMAT_P016:
        fmtInfo = &formatsInfo[NV_FORMAT_P016];
        break;
    case NV_FORMAT_ARGB:
        fmtInfo = &formatsInfo[NV_FORMAT_ARGB];
        break;
    case NV_FORMAT_444P:
        fmtInfo = &formatsInfo[NV_FORMAT_444P];
        break;
#if VA_CHECK_VERSION(1, 20, 0)
    case NV_FORMAT_Q416:
        fmtInfo = &formatsInfo[NV_FORMAT_Q416];
        break;
#endif
    default:
        return NULL;
    }

    if (fmtInfo->numPlanes == 0) {
        return NULL;
    }
    return fmtInfo;
}

static const NVFormatInfo *egl_getFormatInfoForBackingImage(const BackingImage *img) {
    if (img == NULL) {
        return NULL;
    }

    const NVFormatInfo *fmtInfo = egl_getFormatInfoByEnum(img->format);
    if (fmtInfo != NULL) {
        return fmtInfo;
    }

    const uint32_t imgFourcc = (uint32_t)img->fourcc;
    if (imgFourcc == DRM_FORMAT_NV12
#ifdef DRM_FORMAT_NV21
        || imgFourcc == DRM_FORMAT_NV21
#endif
        || imgFourcc == VA_FOURCC_NV12) {
        return egl_getFormatInfoByEnum(NV_FORMAT_NV12);
    }

#if NVENCAPI_MAJOR_VERSION >= 13
    if (imgFourcc == DRM_FORMAT_NV16 || imgFourcc == VA_FOURCC_NV16) {
        return egl_getFormatInfoByEnum(NV_FORMAT_NV16);
    }
#endif

    if (imgFourcc == DRM_FORMAT_P010 || imgFourcc == VA_FOURCC_P010) {
        return egl_getFormatInfoByEnum(NV_FORMAT_P010);
    }

    if (imgFourcc == DRM_FORMAT_P012 || imgFourcc == VA_FOURCC_P012) {
        return egl_getFormatInfoByEnum(NV_FORMAT_P012);
    }

    if (imgFourcc == DRM_FORMAT_P016 || imgFourcc == VA_FOURCC_P016) {
        return egl_getFormatInfoByEnum(NV_FORMAT_P016);
    }

    if (imgFourcc == DRM_FORMAT_ARGB8888 || imgFourcc == VA_FOURCC_ARGB) {
        return egl_getFormatInfoByEnum(NV_FORMAT_ARGB);
    }

    if (imgFourcc == DRM_FORMAT_YUV444 || imgFourcc == VA_FOURCC_444P) {
        return egl_getFormatInfoByEnum(NV_FORMAT_444P);
    }

#if VA_CHECK_VERSION(1, 20, 0)
    if (imgFourcc == VA_FOURCC_Q416) {
        return egl_getFormatInfoByEnum(NV_FORMAT_Q416);
    }
#endif

    return NULL;
}

static bool egl_fillExportDescriptor(NVDriver *drv, NVSurface *surface, VADRMPRIMESurfaceDescriptor *desc) {
    (void)drv;
    BackingImage *img = surface->backingImage;
    const NVFormatInfo *fmtInfo = egl_getFormatInfoForBackingImage(img);
    if (fmtInfo == NULL) {
        LOG("unable to map backing image fourcc=0x%x to export format", img->fourcc);
        return false;
    }

    desc->fourcc = fmtInfo->vaFormat.fourcc;
    desc->width = img->width;
    desc->height = img->height;
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

        desc->layers[i].drm_format = plane->fourcc;
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
            backendCloseFd(exportedFds[i], "egl_export_descriptor_fail");
            exportedFds[i] = -1;
        }
    }
    desc->num_objects = 0;
    desc->num_layers = 0;
    return false;
}

const NVBackend EGL_BACKEND = {
    .name = "egl",
    .initExporter = egl_initExporter,
    .releaseExporter = egl_releaseExporter,
    .exportCudaPtr = egl_exportCudaPtr,
    .importExternalSurface = egl_importExternalSurface,
    .detachBackingImageFromSurface = egl_detachBackingImageFromSurface,
    .realiseSurface = egl_realiseSurface,
    .fillExportDescriptor = egl_fillExportDescriptor,
    .destroyAllBackingImage = egl_destroyAllBackingImage
};
