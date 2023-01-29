#include "vabackend.h"

static void copyVP8PicParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
{
    //Untested, my 1060 (GP106) doesn't support this, however it's simple enough that it should work
    VAPictureParameterBufferVP8* buf = (VAPictureParameterBufferVP8*) buffer->ptr;

    picParams->PicWidthInMbs    = (buf->frame_width + 15) / 16;
    picParams->FrameHeightInMbs = (buf->frame_height + 15) / 16;

    picParams->CodecSpecific.vp8.width = buf->frame_width;
    picParams->CodecSpecific.vp8.height = buf->frame_height;

    picParams->CodecSpecific.vp8.LastRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->last_ref_frame);
    picParams->CodecSpecific.vp8.GoldenRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->golden_ref_frame);
    picParams->CodecSpecific.vp8.AltRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->alt_ref_frame);

    picParams->CodecSpecific.vp8.vp8_frame_tag.frame_type = buf->pic_fields.bits.key_frame;
    picParams->CodecSpecific.vp8.vp8_frame_tag.version = buf->pic_fields.bits.version;
    picParams->CodecSpecific.vp8.vp8_frame_tag.show_frame = 1;//?
    picParams->CodecSpecific.vp8.vp8_frame_tag.update_mb_segmentation_data = buf->pic_fields.bits.segmentation_enabled ? buf->pic_fields.bits.update_segment_feature_data : 0;
}

static void copyVP8SliceParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
{
    VASliceParameterBufferVP8* buf = (VASliceParameterBufferVP8*) buffer->ptr;

    picParams->CodecSpecific.vp8.first_partition_size = buf->partition_size[0] + ((buf->macroblock_offset + 7) / 8);

    ctx->lastSliceParams = buffer->ptr;
    ctx->lastSliceParamsCount = buffer->elements;

    picParams->nNumSlices += buffer->elements;
}

static void copyVP8SliceData(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    //manually pull out the show_frame field, no need to get the full bitstream parser involved
    picParams->CodecSpecific.vp8.vp8_frame_tag.show_frame = (((uint8_t*) buf->ptr)[0] & 0x10) != 0;

    for (unsigned int i = 0; i < ctx->lastSliceParamsCount; i++)
    {
        VASliceParameterBufferVP8 *sliceParams = &((VASliceParameterBufferVP8*) ctx->lastSliceParams)[i];
        uint32_t offset = (uint32_t) ctx->bitstreamBuffer.size;
        appendBuffer(&ctx->sliceOffsets, &offset, sizeof(offset));
        appendBuffer(&ctx->bitstreamBuffer, PTROFF(buf->ptr, sliceParams->slice_data_offset), sliceParams->slice_data_size + buf->offset);
        picParams->nBitstreamDataLen += sliceParams->slice_data_size + buf->offset;
    }
}

static cudaVideoCodec computeVP8CudaCodec(VAProfile profile) {
    if (profile == VAProfileVP8Version0_3) {
        return cudaVideoCodec_VP8;
    }

    return cudaVideoCodec_NONE;
}

static const VAProfile vp8SupportedProfiles[] = {
    VAProfileVP8Version0_3,
};

const DECLARE_CODEC(vp8Codec) = {
    .computeCudaCodec = computeVP8CudaCodec,
    .handlers = {
        [VAPictureParameterBufferType] = copyVP8PicParam,
        [VASliceParameterBufferType] = copyVP8SliceParam,
        [VASliceDataBufferType] = copyVP8SliceData,
    },
    .supportedProfileCount = ARRAY_SIZE(vp8SupportedProfiles),
    .supportedProfiles = vp8SupportedProfiles,
};
