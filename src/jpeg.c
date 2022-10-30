#include "vabackend.h"
#include <string.h>

/* This one looks difficult to implement as NVDEC wants the whole JPEG file, and VA-API only supplied part of it */

static void copyJPEGPicParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
{
    VAPictureParameterBufferJPEGBaseline* buf = (VAPictureParameterBufferJPEGBaseline*) buffer->ptr;

    picParams->PicWidthInMbs = (int) ( buf->picture_width + 15) / 16; //int
    picParams->FrameHeightInMbs = (int) ( buf->picture_height + 15) / 16; //int

    picParams->field_pic_flag    = 0;
    picParams->bottom_field_flag = 0;
    picParams->second_field      = 0;

    picParams->intra_pic_flag    = 1;
    picParams->ref_pic_flag      = 0;
}

static void copyJPEGSliceParam(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    ctx->lastSliceParams = buf->ptr;
    ctx->lastSliceParamsCount = buf->elements;

    picParams->nNumSlices += buf->elements;
}

static void copyJPEGSliceData(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    for (int i = 0; i < ctx->lastSliceParamsCount; i++)
    {
        VASliceParameterBufferJPEGBaseline *sliceParams = &((VASliceParameterBufferJPEGBaseline*) ctx->lastSliceParams)[i];
        uint32_t offset = (uint32_t) ctx->bitstreamBuffer.size;
        appendBuffer(&ctx->sliceOffsets, &offset, sizeof(offset));
        appendBuffer(&ctx->bitstreamBuffer, PTROFF(buf->ptr, sliceParams->slice_data_offset), sliceParams->slice_data_size);
        picParams->nBitstreamDataLen += sliceParams->slice_data_size;
    }
}

static cudaVideoCodec computeJPEGCudaCodec(VAProfile profile) {
    switch (profile) {
        case VAProfileJPEGBaseline:
            return cudaVideoCodec_JPEG;
        default:
            return cudaVideoCodec_NONE;
    }
}

/*
static const VAProfile jpegSupportedProfiles[] = {
    VAProfileJPEGBaseline,
};

const DECLARE_CODEC(jpegCodec) = {
    .computeCudaCodec = computeJPEGCudaCodec,
    .handlers = {
        [VAPictureParameterBufferType] = copyJPEGPicParam,
        //[VAIQMatrixBufferType] = copyJPEGIQMatrix,
        [VASliceParameterBufferType] = copyJPEGSliceParam,
        [VASliceDataBufferType] = copyJPEGSliceData,
    },
    .supportedProfileCount = ARRAY_SIZE(jpegSupportedProfiles),
    .supportedProfiles = jpegSupportedProfiles,
};
*/
