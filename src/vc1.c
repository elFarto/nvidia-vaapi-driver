#include "vabackend.h"

static void copyVC1PicParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
{
    VAPictureParameterBufferVC1* buf = (VAPictureParameterBufferVC1*) buffer->ptr;

    picParams->PicWidthInMbs = (ctx->width + 15)/16;
    picParams->FrameHeightInMbs = (ctx->height + 15)/16;

    int interlaced = buf->picture_fields.bits.frame_coding_mode == 2;
    int field_mode = buf->sequence_fields.bits.interlace && interlaced;

    ctx->renderTargets->progressiveFrame = !interlaced;
    picParams->field_pic_flag    = buf->sequence_fields.bits.interlace && interlaced;
    picParams->bottom_field_flag = field_mode && !(buf->picture_fields.bits.top_field_first ^ !buf->picture_fields.bits.is_first_field);

    picParams->second_field      = !buf->picture_fields.bits.is_first_field;

    if (interlaced) {
        picParams->intra_pic_flag    = buf->picture_fields.bits.picture_type == 0 || //Intra
                                       buf->picture_fields.bits.picture_type == 7; //Bi-Intra
        picParams->ref_pic_flag      = buf->picture_fields.bits.picture_type == 0 || //Intra
                                       buf->picture_fields.bits.picture_type == 3; //Predicted
    } else {
        picParams->intra_pic_flag    = buf->picture_fields.bits.picture_type == 0 || //Intra
                                       buf->picture_fields.bits.picture_type == 3; //Bi-Intra
        picParams->ref_pic_flag      = buf->picture_fields.bits.picture_type == 0 || //Intra
                                       buf->picture_fields.bits.picture_type == 1 || //Predicted
                                       buf->picture_fields.bits.picture_type == 4; //Predicted - skipped?
    }

    CUVIDVC1PICPARAMS *pps = &picParams->CodecSpecific.vc1;

    pps->ForwardRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->forward_reference_picture);
    pps->BackwardRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->backward_reference_picture);

    pps->FrameWidth = ctx->width;
    pps->FrameHeight = ctx->height;
    pps->progressive_fcm = buf->picture_fields.bits.frame_coding_mode == 0;
    pps->profile = buf->sequence_fields.bits.profile;
    pps->postprocflag = buf->post_processing != 0;
    pps->pulldown = buf->sequence_fields.bits.pulldown;
    pps->interlace = buf->sequence_fields.bits.interlace;
    pps->tfcntrflag = buf->sequence_fields.bits.tfcntrflag;
    pps->finterpflag = buf->sequence_fields.bits.finterpflag;
    pps->psf = buf->sequence_fields.bits.psf;
    pps->multires = buf->sequence_fields.bits.multires;
    pps->syncmarker = buf->sequence_fields.bits.syncmarker;
    pps->rangered = buf->sequence_fields.bits.rangered;
    pps->maxbframes = buf->sequence_fields.bits.max_b_frames;
    pps->refdist_flag = buf->reference_fields.bits.reference_distance_flag;
    pps->extended_mv = buf->mv_fields.bits.extended_mv_flag;
    pps->dquant = buf->pic_quantizer_fields.bits.dquant;
    pps->vstransform = buf->transform_fields.bits.variable_sized_transform_flag;
    pps->loopfilter = buf->entrypoint_fields.bits.loopfilter;
    pps->fastuvmc = buf->fast_uvmc_flag;
    pps->overlap = buf->sequence_fields.bits.overlap;
    pps->quantizer = buf->pic_quantizer_fields.bits.quantizer;
    pps->extended_dmv = buf->mv_fields.bits.extended_dmv_flag;
    pps->range_mapy_flag = buf->range_mapping_fields.bits.luma_flag;
    pps->range_mapy = buf->range_mapping_fields.bits.luma;
    pps->range_mapuv_flag = buf->range_mapping_fields.bits.chroma_flag;
    pps->range_mapuv = buf->range_mapping_fields.bits.chroma;
    pps->rangeredfrm = buf->range_reduction_frame;
}

static void copyVC1SliceParam(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    ctx->lastSliceParams = buf->ptr;
    ctx->lastSliceParamsCount = buf->elements;

    picParams->nNumSlices += buf->elements;
}

static void copyVC1SliceData(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    for (int i = 0; i < ctx->lastSliceParamsCount; i++)
    {
        VASliceParameterBufferVC1 *sliceParams = &((VASliceParameterBufferVC1*) ctx->lastSliceParams)[i];
        uint32_t offset = (uint32_t) ctx->buf.size;
        appendBuffer(&ctx->sliceOffsets, &offset, sizeof(offset));
        appendBuffer(&ctx->buf, PTROFF(buf->ptr, sliceParams->slice_data_offset), sliceParams->slice_data_size);
        picParams->nBitstreamDataLen += sliceParams->slice_data_size;
    }
}

static void copyVC1BitPlane(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    //not sure anything needs to be done here, but this method is here to suppress the unhandled type error

}
static cudaVideoCodec computeVC1CudaCodec(VAProfile profile) {
    if (profile == VAProfileVC1Advanced || profile == VAProfileVC1Main || profile == VAProfileVC1Simple) {
        return cudaVideoCodec_VC1;
    }

    return cudaVideoCodec_NONE;
}

static const VAProfile vc1SupportedProfiles[] = {
    VAProfileVC1Simple,
    VAProfileVC1Main,
    VAProfileVC1Advanced,
};

static const DECLARE_CODEC(vc1Codec) = {
    .computeCudaCodec = computeVC1CudaCodec,
    .handlers = {
        [VAPictureParameterBufferType] = copyVC1PicParam,
        [VASliceParameterBufferType] = copyVC1SliceParam,
        [VASliceDataBufferType] = copyVC1SliceData,
        [VABitPlaneBufferType] = copyVC1BitPlane,
    },
    .supportedProfileCount = ARRAY_SIZE(vc1SupportedProfiles),
    .supportedProfiles = vc1SupportedProfiles,
};
