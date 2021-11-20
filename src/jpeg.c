#include "vabackend.h"
#include <string.h>

/* This one looks difficult to implement as NVDEC wants the whole JPEG file, and VA-API only supplied part of it */

void copyJPEGPicParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
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

void copyJPEGSliceParam(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    ctx->last_slice_params = buf->ptr;
    ctx->last_slice_params_count = buf->elements;

    picParams->nNumSlices += buf->elements;
}

void copyJPEGSliceData(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    for (int i = 0; i < ctx->last_slice_params_count; i++)
    {
        VASliceParameterBufferJPEGBaseline *sliceParams = &((VASliceParameterBufferJPEGBaseline*) ctx->last_slice_params)[i];
        uint32_t offset = (uint32_t) ctx->buf.size;
        appendBuffer(&ctx->slice_offsets, &offset, sizeof(offset));
        appendBuffer(&ctx->buf, buf->ptr + sliceParams->slice_data_offset, sliceParams->slice_data_size);
        picParams->nBitstreamDataLen += sliceParams->slice_data_size;
    }
}

cudaVideoCodec computeJPEGCudaCodec(VAProfile profile) {
    switch (profile) {
        case VAProfileJPEGBaseline:
            return cudaVideoCodec_JPEG;
    }

    return -1;
}

/*
NVCodec jpegCodec = {
    .computeCudaCodec = computeJPEGCudaCodec,
    .handlers = {
        [VAPictureParameterBufferType] = copyJPEGPicParam,
        //[VAIQMatrixBufferType] = copyJPEGIQMatrix,
        [VASliceParameterBufferType] = copyJPEGSliceParam,
        [VASliceDataBufferType] = copyJPEGSliceData,
    },
    .supportedProfileCount = 1,
    .supportedProfiles = { VAProfileJPEGBaseline }
};
DEFINE_CODEC(jpegCodec)
*/
