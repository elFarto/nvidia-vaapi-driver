#include "vabackend.h"

#include <math.h>

static void copyMPEG4PicParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
{
    //Not working, it seems that the information that VA-API supplies is not enough to feed NVDEC
    //It might be possible to reconstruct it from the supplied fields like the VDPAU implementation does.
    //In addition there looks to be a race condition in FFMPEG (h263_decode.c:573 : if (avctx->pix_fmt != h263_get_format(avctx)) {)
    //where h263_get_format attempts to set pix_fmt, but the if statement has already read that value leading to a spurious failure.

    VAPictureParameterBufferMPEG4* buf = (VAPictureParameterBufferMPEG4*) buffer->ptr;

    CUVIDMPEG4PICPARAMS *ppc = &picParams->CodecSpecific.mpeg4;

    picParams->PicWidthInMbs = (int) (buf->vop_width + 15) / 16; //int
    picParams->FrameHeightInMbs = (int) (buf->vop_height + 15) / 16; //int

    ctx->renderTarget->progressiveFrame  = !buf->vol_fields.bits.interlaced;
    picParams->field_pic_flag    = buf->vol_fields.bits.interlaced;
    picParams->bottom_field_flag = buf->vop_fields.bits.top_field_first;
    picParams->second_field      = 0;

    picParams->intra_pic_flag    = buf->vop_fields.bits.vop_coding_type == 0; //Intra
    picParams->ref_pic_flag      = buf->vop_fields.bits.vop_coding_type == 0 || //Intra
                                   buf->vop_fields.bits.vop_coding_type == 1 || //Predicted
                                   buf->vop_fields.bits.vop_coding_type == 3;   //S(GMC)-VOP MPEG-4

    ppc->ForwardRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->forward_reference_picture);
    ppc->BackwardRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->backward_reference_picture);

    ppc->video_object_layer_width = buf->vop_width;
    ppc->video_object_layer_height = buf->vop_height;
    ppc->vop_time_increment_bitcount = log2l(buf->vop_time_increment_resolution - 1) + 1; //
    if (ppc->vop_time_increment_bitcount < 1) {
        ppc->vop_time_increment_bitcount = 1;
    }
    ppc->top_field_first = buf->vop_fields.bits.top_field_first;
    ppc->resync_marker_disable = buf->vol_fields.bits.resync_marker_disable;
    ppc->quant_type = buf->vol_fields.bits.quant_type;
    ppc->quarter_sample = buf->vol_fields.bits.quarter_sample;
    ppc->short_video_header = buf->vol_fields.bits.short_video_header;
    ppc->divx_flags = 5; //needed for test video...
    ppc->vop_coding_type = buf->vop_fields.bits.vop_coding_type;
    ppc->vop_coded = 1;//buf->vop_fields.bits.backward_reference_vop_coding_type; //?
    ppc->vop_rounding_type = buf->vop_fields.bits.vop_rounding_type;
    ppc->alternate_vertical_scan_flag = buf->vop_fields.bits.alternate_vertical_scan_flag;
    ppc->interlaced = buf->vol_fields.bits.interlaced;
    ppc->vop_fcode_forward = buf->vop_fcode_forward;
    ppc->vop_fcode_backward = buf->vop_fcode_backward;
    ppc->trd[0] = buf->TRD;
    //ppc->trd[1] = 2; //not correct
    ppc->trb[0] = buf->TRB;
    //ppc->trb[1] = 4; //not correct
    ppc->gmc_enabled = buf->vop_fields.bits.vop_coding_type == 3 && buf->vol_fields.bits.sprite_enable;
}

static void copyMPEG4SliceParam(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    ctx->lastSliceParams = buf->ptr;
    ctx->lastSliceParamsCount = buf->elements;

    picParams->nNumSlices += buf->elements;
}

static void copyMPEG4SliceData(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    for (int i = 0; i < ctx->lastSliceParamsCount; i++)
    {
        VASliceParameterBufferMPEG4 *sliceParams = &((VASliceParameterBufferMPEG4*) ctx->lastSliceParams)[i];
        LOG("here: %d", sliceParams->macroblock_offset);
        uint32_t offset = (uint32_t) ctx->bitstreamBuffer.size;
        appendBuffer(&ctx->sliceOffsets, &offset, sizeof(offset));
        appendBuffer(&ctx->bitstreamBuffer, PTROFF(buf->ptr, sliceParams->slice_data_offset), sliceParams->slice_data_size);
        picParams->nBitstreamDataLen += sliceParams->slice_data_size;
    }
}

static void copyMPEG4IQMatrix(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    VAIQMatrixBufferMPEG4 *iq = (VAIQMatrixBufferMPEG4*) buf->ptr;

    for (int i = 0; i < 64; i++) {
        picParams->CodecSpecific.mpeg4.QuantMatrixIntra[i] = iq->intra_quant_mat[i];
        picParams->CodecSpecific.mpeg4.QuantMatrixInter[i] = iq->non_intra_quant_mat[i];
    }

//    for (int i = 0; i < 64; ++i) {
//        printf("Intra[%d] = %d\n", i, ppc->QuantMatrixIntra[i]);
//        printf("Inter[%d] = %d\n", i, ppc->QuantMatrixInter[i]);
//    }
}

static cudaVideoCodec computeMPEG4CudaCodec(VAProfile profile) {
    switch (profile) {
        case VAProfileH263Baseline:
        case VAProfileMPEG4Main:
        case VAProfileMPEG4Simple:
        case VAProfileMPEG4AdvancedSimple:
            return cudaVideoCodec_MPEG4;
        default:
            return cudaVideoCodec_NONE;
    }
}
//uncomment this to reenable MPEG-4 support
/*
static const VAProfile mpeg4SupportProfiles[] = {
    VAProfileH263Baseline,
    VAProfileMPEG4Main,
    VAProfileMPEG4Simple,
    VAProfileMPEG4AdvancedSimple,
};

const DECLARE_CODEC(mpeg4Codec) = {
    .computeCudaCodec = computeMPEG4CudaCodec,
    .handlers = {
        [VAPictureParameterBufferType] = copyMPEG4PicParam,
        [VAIQMatrixBufferType] = copyMPEG4IQMatrix,
        [VASliceParameterBufferType] = copyMPEG4SliceParam,
        [VASliceDataBufferType] = copyMPEG4SliceData,
    },
    .supportedProfileCount = ARRAY_SIZE(mpeg4SupportProfiles),
    .supportedProfiles = mpeg4SupportProfiles,
};
*/

/*
This code needs to go in nvCreateBuffer, to realign the buffer to capture everything that's needed by NVDEC. However this hack is specific to ffmpeg
and is likely to break anything that doesn't pass in a pointer into the full data. Also, I'm not sure the values (69 and 8) are valid for all videos.
    else if ((nvCtx->profile == VAProfileMPEG4AdvancedSimple || nvCtx->profile == VAProfileMPEG4Main || nvCtx->profile == VAProfileMPEG4Simple)
             && type == VASliceDataBufferType) {
        //HACK HACK HACK
        offset = 69;
        if ((((uintptr_t) data - 8) & 0xf) == 0) {
            offset = 8;
        }
        data -= offset;
        size += offset;
    }
*/
