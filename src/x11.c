#include "vabackend.h"
#include "x11.h"
#include "direct/nv-driver.h"

#include <xcb/xcbext.h>
#include <math.h>
#include <unistd.h>

typedef struct {
    int padding;
    int pixmap;
    int newXid;
    int client;
} xcb_nv_glx_export_pixmap_request;

typedef struct __attribute__((__packed__)) {
    int unknown1;
    int unknown2;
    short unknown2b;
    int widthInBlocks; //?
    int unknown2_1;
    int unknown2_2;
    short unknown2_3;
    int hClientSrc;
    int flags; //?
    int hObjectSrc;
    int bitdepth; //? 0x10 == 32-bit, 0xe == 24-bit
    int width;
    int height;
    int log2GobsPerBlockX; //??
    int unknown4;
    int log2GobsPerBlockY;
    int unknown6;
    int unknown7;
    int unknown8;
    int unknown9;
} xcb_nv_glx_export_pixmap_response;

xcb_extension_t xcb_nv_glx = {
    .name = "NV-GLX",
    .global_id = 0x9c
};

/**
 * This method calls a function in the NVIDIA GLX driver that returns the underlying object representing the pixmap.
 */
xcb_nv_glx_export_pixmap_response* pixmapToNvHandle(xcb_connection_t *c, int pixmap, int newXid, int client) {
    static const xcb_protocol_request_t xcb_req = {
        .count = 2,
        .ext = &xcb_nv_glx,
        .opcode = 0x2d,
        .isvoid = 0
    };

    xcb_nv_glx_export_pixmap_request xcb_out = {
        .pixmap = pixmap,
        .newXid = newXid,
        .client = client,
    };

    struct iovec xcb_parts[4];
    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    int sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);

    xcb_generic_error_t *e;
    xcb_nv_glx_export_pixmap_response *xcb_ret = xcb_wait_for_reply(c, sequence, &e);

    if (xcb_ret != NULL) {
        LOG("widthInBlocks  0x%x", xcb_ret->widthInBlocks);
        LOG("hClientSrc 0x%x", xcb_ret->hClientSrc);
        LOG("hObjectSrc 0x%x", xcb_ret->hObjectSrc);
        LOG("bitdepth 0x%x", xcb_ret->bitdepth);
        LOG("flags 0x%x", xcb_ret->flags);
        LOG("width %d", xcb_ret->width);
        LOG("height %d", xcb_ret->height);
        LOG("log2GobsPerBlockX 0x%x", xcb_ret->log2GobsPerBlockX);
        LOG("log2GobsPerBlockY 0x%x", xcb_ret->log2GobsPerBlockY);
        LOG("unknown4 0x%x", xcb_ret->unknown4);
        LOG("unknown6 0x%x", xcb_ret->unknown6);
        LOG("unknown7 0x%x", xcb_ret->unknown7);
        LOG("unknown8 0x%x", xcb_ret->unknown8);
        LOG("unknown9 0x%x", xcb_ret->unknown9);
        LOG("unknown1 0x%x", xcb_ret->unknown1);
        LOG("unknown2 0x%x", xcb_ret->unknown2);
        LOG("unknown2b 0x%x", xcb_ret->unknown2b);
        LOG("unknown2_1 0x%x", xcb_ret->unknown2_1);
        LOG("unknown2_2 0x%x", xcb_ret->unknown2_2);
        LOG("unknown2_3 0x%x", xcb_ret->unknown2_3);

    } else {
        LOG("got null response");
    }
    return xcb_ret;
}

static CUdeviceptr cuArrayToNV12(NVDriver *drv, NVSurface *surfaceObj) {
    int bytesPerPixel = 1;
    if (surfaceObj->format != cudaVideoSurfaceFormat_NV12) {
        bytesPerPixel = 2;
    }

    CUdeviceptr mem;
    CHECK_CUDA_RESULT(drv->cu->cuMemAlloc(&mem, surfaceObj->width * (surfaceObj->height + (surfaceObj->height>>1)) * bytesPerPixel));

    CUdeviceptr luma = mem;
    CUdeviceptr chroma = mem + (surfaceObj->width * surfaceObj->height * bytesPerPixel);

    //luma
    CUDA_MEMCPY2D memcpy2d = {
      .srcXInBytes = 0, .srcY = 0,
      .srcMemoryType = CU_MEMORYTYPE_ARRAY,
      .srcArray = surfaceObj->backingImage->arrays[0],

      .dstXInBytes = 0, .dstY = 0,
      .dstMemoryType = CU_MEMORYTYPE_DEVICE,
      .dstDevice = luma,
      .dstPitch = surfaceObj->width * bytesPerPixel,

      .WidthInBytes = surfaceObj->width * bytesPerPixel,
      .Height = surfaceObj->height
    };

    CUresult result = drv->cu->cuMemcpy2D(&memcpy2d);
    if (result != CUDA_SUCCESS)
    {
            LOG("cuArrayToNV12 luma failed: %d", result);
            drv->cu->cuMemFree(mem);
            return (CUdeviceptr) NULL;
    }

    //chroma
    CUDA_MEMCPY2D memcpy2dChroma = {
      .srcXInBytes = 0, .srcY = 0,
      .srcMemoryType = CU_MEMORYTYPE_ARRAY,
      .srcArray = surfaceObj->backingImage->arrays[1],

      .dstXInBytes = 0, .dstY = 0,
      .dstMemoryType = CU_MEMORYTYPE_DEVICE,
      .dstDevice = chroma,
      .dstPitch = surfaceObj->width * bytesPerPixel,

      .WidthInBytes = surfaceObj->width * bytesPerPixel,
      .Height = (surfaceObj->height>>1)
    };

    result = drv->cu->cuMemcpy2D(&memcpy2dChroma);
    if (result != CUDA_SUCCESS)
    {
            LOG("cuArrayToNV12 chroma failed: %d", result);
            drv->cu->cuMemFree(mem);
            return (CUdeviceptr) NULL;
    }

    return mem;
}



static void convert_image(NVDriver *drv, int fd, uint32_t log2GobsPerBlockX, uint32_t log2GobsPerBlockY, uint32_t width, uint32_t height, int bpc, int channels, NVCudaImage *cudaImage, NVSurface *surface) {
    uint32_t gobWidth    = 16;//px TODO calculate these from hardware
    uint32_t gobHeight   = 8;//px
    uint32_t blockWidth  = gobWidth * (1<<log2GobsPerBlockX);//px
    uint32_t blockHeight = gobHeight * (1<<log2GobsPerBlockY);//px
    uint32_t bytesPerPixel = bpc * channels / 8;

    uint32_t gobsPerX = ROUND_UP(width, gobWidth) / gobWidth;
    uint32_t gobsPerY = ROUND_UP(height, blockHeight) / gobHeight;

    uint32_t gobSize = gobWidth * gobHeight * bytesPerPixel;
    uint32_t size = gobsPerX * gobsPerY * gobSize;

    LOG("importing memory size: %dx%d = %d (%ux%u)", width, height, size, gobsPerX, gobsPerY);
    //import the fd as external memory
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
        .type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD,
        .handle.fd = fd,
        .flags = 0,
        .size = size
    };
    CHECK_CUDA_RESULT(drv->cu->cuImportExternalMemory(&cudaImage->extMem, &extMemDesc));

    //close the fd representing the object to prevent us leaking it
    close(fd);

    //now get a pointer to the buffer, mapping it as an image doesn't work for some reason
    CUdeviceptr pixmapPtr;
    CUDA_EXTERNAL_MEMORY_BUFFER_DESC desc = {
        .offset = 0,
        .size = size,
        .flags = 0
    };
    CHECK_CUDA_RESULT(drv->cu->cuExternalMemoryGetMappedBuffer(&pixmapPtr, cudaImage->extMem, &desc));

    //we shouldn't have to do this as NVDEC will produce this for us, so we should use it directly, rather than reconstructing it
    CUdeviceptr d_luma = surface->rawImageCopy;
    if (d_luma == (CUdeviceptr) NULL) {
        d_luma = cuArrayToNV12(drv, surface);
        surface->rawImageCopy = d_luma;
    }
    CUdeviceptr d_chroma = d_luma + (width*height);

    //copy/convert the NV12 image to an RGB one
    void *params[] = { &pixmapPtr, &d_luma, &d_chroma, &width, &height, &log2GobsPerBlockX, &log2GobsPerBlockY};
    CHECK_CUDA_RESULT(drv->cu->cuLaunchKernel(drv->yuvFunction, gobsPerX, gobsPerY, 1, 1, 1, 1, 0, 0, params, NULL));

    //unmap the external memory of the pixmap
    CHECK_CUDA_RESULT(drv->cu->cuDestroyExternalMemory(cudaImage->extMem));
}

VAStatus nvPutSurface(
        VADriverContextP ctx,
        VASurfaceID surface,
        void* draw, /* Drawable of window system */
        short srcx,
        short srcy,
        unsigned short srcw,
        unsigned short srch,
        short destx,
        short desty,
        unsigned short destw,
        unsigned short desth,
        VARectangle *cliprects, /* client supplied clip list */
        unsigned int number_cliprects, /* number of clip rects in the clip list */
        unsigned int flags) { /* de-interlacing flags */

    //TODO? we only support copying the whole image without any scaling
    if (srcx != 0 || srcy != 0 || destx != 0 || desty != 0 || srcw != destw || srch != desth) {
        LOG("src: %dx%d - %dx%d, dest: %dx%d - %dx%d", srcx, srcy, srcw, srch, destx, desty, destw, desth);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVSurface *surfaceObj = (NVSurface*) getObjectPtr(drv, surface);

    xcb_connection_t *conn = (xcb_connection_t*) drv->xcbConnection;
    if (conn == NULL) {
        conn = xcb_connect(NULL, NULL);
        if (conn == NULL) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        drv->xcbConnection = conn;
    }

    //call NV-GLX to get the object for the pixmap
    int newXid = xcb_generate_id (conn);
    xcb_nv_glx_export_pixmap_response *xcb_ret = pixmapToNvHandle(conn, (int) draw, newXid, drv->driverContext.clientObject);

    //duplicate the object so we can export/import it into CUDA
    uint32_t obj = 0;
    bool ret = dup_object(&drv->driverContext, xcb_ret->hClientSrc, xcb_ret->hObjectSrc, &obj);

    if (ret) {
        int fd;
        ret = export_object(&drv->driverContext, &fd, obj);

        if (ret) {
            NVCudaImage cudaImage;
            convert_image(drv, fd, xcb_ret->log2GobsPerBlockX, xcb_ret->log2GobsPerBlockY, xcb_ret->width, xcb_ret->height, 8, 4, &cudaImage, surfaceObj);
        }
    }

    return VA_STATUS_SUCCESS;// VA_STATUS_ERROR_UNIMPLEMENTED;
}



//manual method
//    uint8_t *luma, *chroma;
//    cuArrayToNV12(surfaceObj, &luma, &chroma);

//    Display *display = XOpenDisplay( NULL );
//    int screen = DefaultScreen(display);
//    GC gc = XDefaultGC(display, screen);
//    Visual *visual =  DefaultVisual(display, screen);
//    int depth = DefaultDepth(display, screen);

//    XImage *img = XCreateImage(display, visual, depth, ZPixmap, 0, data, surfaceObj->width, surfaceObj->height, 8, 0);

//    XPutImage(display, (Drawable) draw, gc, img, srcx, srcy, destx, desty, destw, desth);

//    XDestroyImage(img);
//    XCloseDisplay(display);

//    free(luma);
//    free(chroma);
//    //data is freed by XDestroyImage


//failed cuda method, this doesn't work, i guess CUDA doesn't like pixmaps
//EGLDisplay dpy = eglGetDisplay(NULL);
//eglInitialize(dpy, NULL, NULL);
//EGLImage img = eglCreateImage(dpy, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, draw, NULL);
//CUgraphicsResource gr;
//CHECK_CUDA_RESULT(drv->cu->cuGraphicsEGLRegisterImage(&gr, img, 0));
