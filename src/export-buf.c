#include "export-buf.h"
#include <stdio.h>
#include <cuda.h>
#include <cudaEGL.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <fcntl.h>

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
#endif /* EGL_NV_stream_consumer_eglimage */

#include <execinfo.h>
#include <unistd.h>

PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC eglQueryStreamConsumerEventNV;
PFNEGLSTREAMRELEASEIMAGENVPROC eglStreamReleaseImageNV;
PFNEGLSTREAMACQUIREIMAGENVPROC eglStreamAcquireImageNV;
PFNEGLEXPORTDMABUFIMAGEMESAPROC eglExportDMABUFImageMESA;
PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC eglExportDMABUFImageQueryMESA;
PFNEGLCREATESTREAMKHRPROC eglCreateStreamKHR;
PFNEGLDESTROYSTREAMKHRPROC eglDestroyStreamKHR;
PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC eglStreamImageConsumerConnectNV;


/*
[17929] export-buf.c:  40                    debug [EGL] eglQueryStreamConsumerEventNV: EGL_BAD_STREAM_KHR error: In EGL Access Table::stream.consumer.getCaps: Invalid EGLStream handle (0xdff9531)

[17929] export-buf.c:  46                    debug ptr 0: 0x7f0b0f60c9cf /usr/lib64/dri/nvidia_drv_video.so(debug+0x7e) [0x7f0b0f60c9cf]
[17929] export-buf.c:  46                    debug ptr 1: 0x7f0b0216c0c3 /lib64/libEGL_nvidia.so.0(+0xb30c3) [0x7f0b0216c0c3]
[17929] export-buf.c:  46                    debug ptr 2: 0x7f0b0210678e /lib64/libEGL_nvidia.so.0(+0x4d78e) [0x7f0b0210678e]
[17929] export-buf.c:  46                    debug ptr 3: 0x7f0b021068d4 /lib64/libEGL_nvidia.so.0(+0x4d8d4) [0x7f0b021068d4]
[17929] export-buf.c:  46                    debug ptr 4: 0x7f0b020f26e8 /lib64/libEGL_nvidia.so.0(+0x396e8) [0x7f0b020f26e8]
[17929] export-buf.c:  46                    debug ptr 5: 0x7f0b0bb29816 /lib64/libcuda.so.1(+0x11e816) [0x7f0b0bb29816]
[17929] export-buf.c:  46                    debug ptr 6: 0x7f0b0bb67c99 /lib64/libcuda.so.1(+0x15cc99) [0x7f0b0bb67c99]
[17929] export-buf.c:  46                    debug ptr 7: 0x7f0b0bc04224 /lib64/libcuda.so.1(+0x1f9224) [0x7f0b0bc04224]
[17929] export-buf.c:  46                    debug ptr 8: 0x7f0b0f60d1d4 /usr/lib64/dri/nvidia_drv_video.so(exportCudaPtr+0x50e) [0x7f0b0f60d1d4]
[17929] export-buf.c:  46                    debug ptr 9: 0x7f0b0f612e49 /usr/lib64/dri/nvidia_drv_video.so(nvExportSurfaceHandle+0x249) [0x7f0b0f612e49]


*/

void debug(EGLenum error,const char *command,EGLint messageType,EGLLabelKHR threadLabel,EGLLabelKHR objectLabel,const char* message) {
    LOG("[EGL] %s: %s\n", command, message);

//    void* ptrs[50];
//    int x = backtrace(&ptrs, 50);
//    char **names = backtrace_symbols(ptrs, 50);
//    for (int i = 0; i < x; i++) {
//        LOG("ptr %d: %p %s\n", i, ptrs[i], names[i]);
//    }

}

void releaseExporter(NVDriver *drv) {
    if (drv->cuStreamConnection != NULL) {
        cuEGLStreamConsumerDisconnect(&drv->cuStreamConnection);
    }
    if (drv->eglDisplay != EGL_NO_DISPLAY) {
        if (drv->eglStream != EGL_NO_STREAM_KHR) {
            eglDestroyStreamKHR(drv->eglDisplay, drv->eglStream);
            drv->eglStream = EGL_NO_STREAM_KHR;
        }
        //TODO terminate the EGLDisplay here, sounds like that could break stuff
        drv->eglDisplay = EGL_NO_DISPLAY;
    }

    cuEGLStreamProducerDisconnect(&drv->cuStreamConnection);
}

void reconnect(NVDriver *drv) {
    LOG("Reconnecting to stream\n");
    eglInitialize(drv->eglDisplay, NULL, NULL);
    if (drv->cuStreamConnection != NULL) {
        cuEGLStreamConsumerDisconnect(&drv->cuStreamConnection);
    }
    if (drv->eglStream != EGL_NO_STREAM_KHR) {
        eglDestroyStreamKHR(drv->eglDisplay, drv->eglStream);
    }
    drv->eglStream = eglCreateStreamKHR(drv->eglDisplay, NULL);
    eglStreamImageConsumerConnectNV(drv->eglDisplay, drv->eglStream, 0, 0, NULL);
    checkCudaErrors(cuEGLStreamProducerConnect(&drv->cuStreamConnection, drv->eglStream, 1024, 1024));
    drv->numFramesPresented = 0;
}

void initExporter(NVDriver *drv) {
    eglQueryStreamConsumerEventNV = (PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC) eglGetProcAddress("eglQueryStreamConsumerEventNV");
    eglStreamReleaseImageNV = (PFNEGLSTREAMRELEASEIMAGENVPROC) eglGetProcAddress("eglStreamReleaseImageNV");
    eglStreamAcquireImageNV = (PFNEGLSTREAMACQUIREIMAGENVPROC) eglGetProcAddress("eglStreamAcquireImageNV");
    eglExportDMABUFImageMESA = (PFNEGLEXPORTDMABUFIMAGEMESAPROC) eglGetProcAddress("eglExportDMABUFImageMESA");
    eglExportDMABUFImageQueryMESA = (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC) eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    eglCreateStreamKHR = (PFNEGLCREATESTREAMKHRPROC) eglGetProcAddress("eglCreateStreamKHR");
    eglDestroyStreamKHR = (PFNEGLDESTROYSTREAMKHRPROC) eglGetProcAddress("eglDestroyStreamKHR");
    eglStreamImageConsumerConnectNV = (PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC) eglGetProcAddress("eglStreamImageConsumerConnectNV");

    drv->eglDisplay = eglGetDisplay(NULL);
    eglInitialize(drv->eglDisplay, NULL, NULL);
//    drv->eglContext = eglCreateContext(drv->eglDisplay, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, NULL);
//    eglMakeCurrent(drv->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, drv->eglContext);
    reconnect(drv);

    PFNEGLDEBUGMESSAGECONTROLKHRPROC eglDebugMessageControlKHR = (PFNEGLDEBUGMESSAGECONTROLKHRPROC) eglGetProcAddress("eglDebugMessageControlKHR");
    //setup debug logging
    EGLAttrib debugAttribs[] = {EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE, EGL_DEBUG_MSG_INFO_KHR, EGL_TRUE, EGL_NONE};
    eglDebugMessageControlKHR(debug, debugAttribs);
}

void exportCudaPtr(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch, int *fourcc, int *fds, int *offsets, int *strides, uint64_t *mods, int *bppOut) {
//    EGLDisplay oldDisplay = eglGetCurrentDisplay();
//    EGLContext oldContext = eglGetCurrentContext();
//    EGLSurface oldReadSurface = eglGetCurrentSurface(EGL_READ);
//    EGLSurface oldDrawSurface = eglGetCurrentSurface(EGL_DRAW);
//    eglMakeCurrent(drv->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, drv->eglContext);

    // If there is a frame presented before we check if consumer
    // is done with it using cuEGLStreamProducerReturnFrame.
    //LOG("outstanding frames: %d\n", numFramesPresented);
    CUeglFrame eglframe = {
        .frame = {
            .pArray = {0, 0, 0}
        }
    };
    //TODO if we ever have more than 1 frame returned a frame, we'll leak that memory
    while (drv->numFramesPresented > 0) {
      LOG("waiting for returned frame: %lx %d\n", drv->cuStreamConnection, drv->numFramesPresented);
      CUresult cuStatus = cuEGLStreamProducerReturnFrame(&drv->cuStreamConnection, &eglframe, NULL);
      if (cuStatus == CUDA_ERROR_LAUNCH_TIMEOUT) {
        //LOG("timeout with %d outstanding\n", numFramesPresented);
        break;
      } else if (cuStatus != CUDA_SUCCESS) {
        checkCudaErrors(cuStatus);
      } else {
        LOG("returned frame %dx%d %p %p\n", eglframe.width, eglframe.height, eglframe.frame.pArray[0], eglframe.frame.pArray[1]);
        drv->numFramesPresented--;
      }
    }

    uint32_t width = surface->width;
    uint32_t height = surface->height;

    //check if the frame size if different and release the arrays
    //TODO figure out how to get the EGLimage freed aswell
    if (eglframe.width != width && eglframe.height != height) {
        if (eglframe.frame.pArray[0] != NULL) {
            cuArrayDestroy(eglframe.frame.pArray[0]);
            eglframe.frame.pArray[0] = NULL;
        }
        if (eglframe.frame.pArray[1] != NULL) {
            cuArrayDestroy(eglframe.frame.pArray[1]);
            eglframe.frame.pArray[1] = NULL;
        }
    }
    eglframe.width = width;
    eglframe.height = height;
    eglframe.depth = 1;
    eglframe.pitch = 0;
    eglframe.planeCount = 2;
    eglframe.numChannels = 1;
    eglframe.frameType = CU_EGL_FRAME_TYPE_ARRAY;

    int bpp = 1;
    if (surface->format == cudaVideoSurfaceFormat_NV12) {
        eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_YVU420_SEMIPLANAR;
        eglframe.cuFormat = CU_AD_FORMAT_UNSIGNED_INT8;
    } else if (surface->format == cudaVideoSurfaceFormat_P016) {
        //TODO not working, produces this error in mpv:
        //EGL_BAD_MATCH error: In eglCreateImageKHR: requested LINUX_DRM_FORMAT is not supported
        //this error seems to be coming from the NVIDIA EGL driver
        //this might be caused by the DRM_FORMAT_*'s in nvExportSurfaceHandle
        if (surface->bitdepth == 10) {
            eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_Y10V10U10_420_SEMIPLANAR;
        } else if (surface->bitdepth == 12) {
            eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_Y12V12U12_420_SEMIPLANAR;
        } else {
            LOG("Unknown bitdepth\n");
        }
        eglframe.cuFormat = CU_AD_FORMAT_UNSIGNED_INT16;
        bpp = 2;
    }
    *bppOut = bpp;

    //TODO in theory this should work, but the application attempting to bind that texture gets the following error:
    //GL_INVALID_OPERATION error generated. <image> and <target> are incompatible
    //eglframe.frameType = CU_EGL_FRAME_TYPE_PITCH;
    //eglframe.pitch = pitch;
    //eglframe.frame.pPitch[0] = (void*) ptr;
    //eglframe.frame.pPitch[1] = (void*) ptr + (height*pitch);

    //reuse the arrays if we can
    //creating new arrays will cause a new EGLimage to be created and we'll eventually run out of resources
    if (eglframe.frame.pArray[0] == NULL) {
        CUDA_ARRAY3D_DESCRIPTOR arrDesc = {
            .Width = width,
            .Height = height,
            .Depth = 0,
            .NumChannels = 1,
            .Flags = 0,
            .Format = eglframe.cuFormat
        };
        checkCudaErrors(cuArray3DCreate(&eglframe.frame.pArray[0], &arrDesc));
    }
    if (eglframe.frame.pArray[1] == NULL) {
        CUDA_ARRAY3D_DESCRIPTOR arr2Desc = {
            .Width = width >> 1,
            .Height = height >> 1,
            .Depth = 0,
            .NumChannels = 2,
            .Flags = 0,
            .Format = eglframe.cuFormat
        };
        checkCudaErrors(cuArray3DCreate(&eglframe.frame.pArray[1], &arr2Desc));
    }
    CUDA_MEMCPY3D cpy = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = ptr,
        .srcPitch = pitch,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = eglframe.frame.pArray[0],
        .Height = height,
        .WidthInBytes = width * bpp,
        .Depth = 1
    };
    checkCudaErrors(cuMemcpy3D(&cpy));
    CUDA_MEMCPY3D cpy2 = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = ptr,
        .srcY = height,
        .srcPitch = pitch,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = eglframe.frame.pArray[1],
        .Height = height >> 1,
        .WidthInBytes = (width >> 1) * 2 * bpp,
        .Depth = 1
    };
    checkCudaErrors(cuMemcpy3D(&cpy2));

//    static int counter = 0;
//    char filename[64];
//    int size = surface->height * surface->width;//(pitch * surface->height) + (pitch * surface->height>>1);
//    char *buf = malloc(size);
//    snprintf(filename, 64, "/tmp/frame-%03d.data\0", counter++);
//    LOG("writing %d to %s\n", surface->pictureIdx, filename);
//    int fd = open(filename, O_RDWR | O_TRUNC | O_CREAT, 0644);
//    //cuMemcpyAtoH(buf, eglframe.frame.pArray[0], 0, size);
//    CUDA_MEMCPY3D cpy3 = {
//        .srcMemoryType = CU_MEMORYTYPE_ARRAY,
//        .srcArray = eglframe.frame.pArray[0],
//        .dstMemoryType = CU_MEMORYTYPE_HOST,
//        .dstHost = buf,
//        .Height = height,
//        .WidthInBytes = width,
//        .Depth = 1
//    };
//    checkCudaErrors(cuMemcpy3D(&cpy3));

//    write(fd, buf, size);
//    free(buf);
//    close(fd);


    LOG("PrePresent drv %p, dsp: %p, str: %p, sc: %p\n", drv, drv->eglDisplay, drv->eglStream, drv->cuStreamConnection);
    //LOG("Presenting frame %dx%d %p %p\n", eglframe.width, eglframe.height, eglframe.frame.pArray[0], eglframe.frame.pArray[1]);
    CUresult ret = cuEGLStreamProducerPresentFrame( &drv->cuStreamConnection, eglframe, NULL );
    if (ret == CUDA_ERROR_UNKNOWN) {
        reconnect(drv);
        checkCudaErrors(cuEGLStreamProducerPresentFrame( &drv->cuStreamConnection, eglframe, NULL ));
    }

    drv->numFramesPresented++;

    while (1) {
        EGLenum event = 0;
        EGLAttrib aux = 0;
        LOG("eglQueryStreamConsumerEventNV: %p\n", drv->eglStream);
        EGLint eventRet = eglQueryStreamConsumerEventNV(drv->eglDisplay, drv->eglStream, 0, &event, &aux);
        if (eventRet == EGL_TIMEOUT_EXPIRED_KHR) {
            break;
        }

        if (event == EGL_STREAM_IMAGE_ADD_NV) {
            EGLImage image = eglCreateImage(drv->eglDisplay, EGL_NO_CONTEXT, EGL_STREAM_CONSUMER_IMAGE_NV, drv->eglStream, NULL);
            LOG("Adding image from EGLStream, eglCreateImage: %p\n", image);
        } else if (event == EGL_STREAM_IMAGE_AVAILABLE_NV) {
            EGLImage img;
            //somehow we get here with the previous frame, not the next one
            if (!eglStreamAcquireImageNV(drv->eglDisplay, drv->eglStream, &img, EGL_NO_SYNC_NV)) {
                LOG("eglStreamAcquireImageNV failed\n");
                exit(EXIT_FAILURE);
            }

            int planes = 0;
            if (!eglExportDMABUFImageQueryMESA(drv->eglDisplay, img, fourcc, &planes, mods)) {
                LOG("eglExportDMABUFImageQueryMESA failed\n");
                exit(EXIT_FAILURE);
            }

            LOG("eglExportDMABUFImageQueryMESA: %p %.4s (%x) planes:%d mods:%lx %lx\n", img, (char*)fourcc, *fourcc, planes, mods[0], mods[1]);

            EGLBoolean r = eglExportDMABUFImageMESA(drv->eglDisplay, img, fds, strides, offsets);

            if (!r) {
                LOG("Unable to export image\n");
            }
            LOG("eglExportDMABUFImageMESA: %d %d %d %d %d\n", r, fds[0], fds[1], fds[2], fds[3]);
            LOG("strides: %d %d %d %d\n", strides[0], strides[1], strides[2], strides[3]);
            LOG("offsets: %d %d %d %d\n", offsets[0], offsets[1], offsets[2], offsets[3]);

            r = eglStreamReleaseImageNV(drv->eglDisplay, drv->eglStream, img, EGL_NO_SYNC_NV);
            if (!r) {
                LOG("Unable to release image\n");
            }
        } else if (event == EGL_STREAM_IMAGE_REMOVE_NV) {
            LOG("Removing image from EGLStream, eglDestroyImage: %p\n", (EGLImage) aux);
            eglDestroyImage(drv->eglDisplay, (EGLImage) aux);
        } else {
            LOG("Unhandled event: %X\n", event);
        }
    }

//    if (oldDisplay != EGL_NO_DISPLAY) {
//        eglMakeCurrent(oldDisplay, oldReadSurface, oldDrawSurface, oldContext);
//    }
}
