#include "encode_image.h"
#include "encode_common.h"
#include "encode_pipeline.h"
#include "encode_surface.h"

#include <stdlib.h>
#include <string.h>

extern CudaFunctions *cu;

static bool nvenc_image_plane_span_valid(const NVBuffer *buffer,
                                         size_t pitch,
                                         size_t offset,
                                         size_t row_width,
                                         size_t rows)
{
    size_t required;

    if (!buffer) {
        return false;
    }
    if (row_width == 0 || rows == 0) {
        return true;
    }
    if (pitch < row_width || offset > buffer->size) {
        return false;
    }

    required = pitch * (rows - 1) + row_width;
    return required <= buffer->size - offset;
}

VAStatus nvenc_put_image(NVDriver *drv,
                         NVSurface *surfaceObj,
                         NVImage *imageObj,
                         uint32_t copy_width,
                         uint32_t copy_height)
{
    if (!drv || !surfaceObj || !imageObj) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (imageObj->format != NV_FORMAT_NV12) {
        return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
    }

    if (copy_width == 0 || copy_height == 0 ||
        copy_width > surfaceObj->width || copy_height > surfaceObj->height ||
        copy_width > imageObj->width || copy_height > imageObj->height) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
    if (!nvenc_ensure_surface_buffer(drv, surfaceObj)) {
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (copy_width != surfaceObj->width || copy_height != surfaceObj->height) {
        if (!nvenc_fill_surface_padding_black(surfaceObj, copy_width, copy_height) &&
            !nvenc_fill_surface_black(surfaceObj)) {
            CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    uint8_t *src = (uint8_t*) imageObj->imageBuffer->ptr;
    size_t pitch = surfaceObj->encPitch;
    const NVFormatInfo *fmtInfo = &formatsInfo[imageObj->format];
    size_t srcPitchY = imageObj->pitches[0] ?
                       imageObj->pitches[0] :
                       nv_format_plane_row_bytes(fmtInfo, 0, imageObj->width);
    size_t srcPitchUV = imageObj->pitches[1] ?
                        imageObj->pitches[1] :
                        nv_format_plane_row_bytes(fmtInfo, 1, imageObj->width);
    size_t srcOffsetY = imageObj->offsets[0];
    size_t srcOffsetUV = imageObj->offsets[1] ?
                         imageObj->offsets[1] :
                         nv_format_plane_size(fmtInfo, 0, imageObj->width, imageObj->height);
    size_t uv_copy_width = nv_format_plane_row_bytes(fmtInfo, 1, copy_width);
    size_t uv_copy_height = nv_format_plane_height(fmtInfo, 1, copy_height);
    const uint8_t *y_src = src + srcOffsetY;
    const uint8_t *uv_src = src + srcOffsetUV;

    if (!nvenc_image_plane_span_valid(imageObj->imageBuffer,
                                      srcPitchY,
                                      srcOffsetY,
                                      copy_width,
                                      copy_height) ||
        !nvenc_image_plane_span_valid(imageObj->imageBuffer,
                                      srcPitchUV,
                                      srcOffsetUV,
                                      uv_copy_width,
                                      uv_copy_height)) {
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    CUDA_MEMCPY2D cpyY = {
        .srcXInBytes = 0, .srcY = 0,
        .srcMemoryType = CU_MEMORYTYPE_HOST,
        .srcHost = y_src,
        .srcPitch = srcPitchY,
        .dstXInBytes = 0, .dstY = 0,
        .dstMemoryType = CU_MEMORYTYPE_DEVICE,
        .dstDevice = surfaceObj->encDevPtr,
        .dstPitch = pitch,
        .WidthInBytes = copy_width,
        .Height = copy_height
    };
    if (CHECK_CUDA_RESULT(cu->cuMemcpy2D(&cpyY))) {
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    CUDA_MEMCPY2D cpyUV = {
        .srcXInBytes = 0, .srcY = 0,
        .srcMemoryType = CU_MEMORYTYPE_HOST,
        .srcHost = (void *)uv_src,
        .srcPitch = srcPitchUV,
        .dstXInBytes = 0, .dstY = 0,
        .dstMemoryType = CU_MEMORYTYPE_DEVICE,
        .dstDevice = surfaceObj->encDevPtr + pitch * surfaceObj->height,
        .dstPitch = pitch,
        .WidthInBytes = uv_copy_width,
        .Height = uv_copy_height
    };
    if (CHECK_CUDA_RESULT(cu->cuMemcpy2D(&cpyUV))) {
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    nvenc_surface_mark_encode_input_written(surfaceObj, true);
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
    return VA_STATUS_SUCCESS;
}
