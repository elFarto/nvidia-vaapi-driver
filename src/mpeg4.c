#include "vabackend.h"

#include <math.h>

void copyMPEG4PicParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
{
    //Not working, it seems that the information that VA-API supplies is not enough to feed NVDEC
    //It might be possible to reconstruct it from the supplied fields like the VDPAU implementation does.
    //In addition there looks to be a race condition in FFMPEG (h263_decode.c:573 : if (avctx->pix_fmt != h263_get_format(avctx)) {)
    //where h263_get_format attempts to set pix_fmt, but the if statement has already read that value leading to a spurious failure.

    VAPictureParameterBufferMPEG4* buf = (VAPictureParameterBufferMPEG4*) buffer->ptr;

    CUVIDMPEG4PICPARAMS *ppc = &picParams->CodecSpecific.mpeg4;

    picParams->PicWidthInMbs = (int) (buf->vop_width + 15) / 16; //int
    picParams->FrameHeightInMbs = (int) (buf->vop_height + 15) / 16; //int

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

//    printf("=================================\n");
//    printf("PicWidthInMbs: %d\n", picParams->PicWidthInMbs);
//    printf("FrameHeightInMbs: %d\n", picParams->FrameHeightInMbs);
//    printf("field_pic_flag: %d\n", picParams->field_pic_flag);
//    printf("bottom_field_flag: %d\n", picParams->bottom_field_flag);
//    printf("second_field: %d\n", picParams->second_field);
//    printf("intra_pic_flag: %d\n", picParams->intra_pic_flag);
//    printf("ref_pic_flag: %d\n", picParams->ref_pic_flag);
//    printf("ForwardRefIdx: %d (%d)\n", ppc->ForwardRefIdx, buf->forward_reference_picture);
//    printf("BackwardRefIdx: %d (%d)\n", ppc->BackwardRefIdx, buf->backward_reference_picture);
//    printf("video_object_layer_width: %d\n", ppc->video_object_layer_width);
//    printf("video_object_layer_height: %d\n", ppc->video_object_layer_height);
//    printf("vop_time_increment_bitcount: %d\n", ppc->vop_time_increment_bitcount);
//    printf("top_field_first: %d\n", ppc->top_field_first);
//    printf("resync_marker_disable: %d\n", ppc->resync_marker_disable);
//    printf("quant_type: %d\n", ppc->quant_type);
//    printf("quarter_sample: %d\n", ppc->quarter_sample );
//    printf("short_video_header: %d\n", ppc->short_video_header );
//    printf("divx_flags: %d\n", ppc->divx_flags );
//    printf("vop_coding_type: %d\n", ppc->vop_coding_type );
//    printf("vop_coded %d\n", ppc->vop_coded  );//?
//    printf("vop_rounding_type: %d\n", ppc->vop_rounding_type );
//    printf("alternate_vertical_scan_flag: %d\n", ppc->alternate_vertical_scan_flag );
//    printf("interlaced: %d\n", ppc->interlaced );
//    printf("vop_fcode_forward: %d\n", ppc->vop_fcode_forward );
//    printf("vop_fcode_backward: %d\n", ppc->vop_fcode_backward );
//    printf("trd[0]: %d\n", ppc->trd[0] );
//    printf("trd[1]: %d\n",  ppc->trd[1] );
//    printf("trb[0]: %d\n", ppc->trb[0] );
//    printf("trb[1]: %d\n", ppc->trb[1] );
//    printf("gmc_enabled: %d\n", ppc->gmc_enabled );
}

void copyMPEG4SliceParam(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    ctx->last_slice_params = buf->ptr;
    ctx->last_slice_params_count = buf->elements;

    picParams->nNumSlices += buf->elements;
}

void copyMPEG4SliceData(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    for (int i = 0; i < ctx->last_slice_params_count; i++)
    {
        VASliceParameterBufferMPEG4 *sliceParams = &((VASliceParameterBufferMPEG4*) ctx->last_slice_params)[i];
        uint32_t offset = (uint32_t) ctx->buf.size;
        appendBuffer(&ctx->slice_offsets, &offset, sizeof(offset));
        appendBuffer(&ctx->buf, buf->ptr + sliceParams->slice_data_offset, sliceParams->slice_data_size);
        picParams->nBitstreamDataLen += sliceParams->slice_data_size;
    }
}

void copyMPEG4IQMatrix(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
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

cudaVideoCodec computeMPEG4CudaCodec(VAProfile profile) {
    switch (profile) {
        case VAProfileH263Baseline:
        case VAProfileMPEG4Main:
        case VAProfileMPEG4Simple:
        case VAProfileMPEG4AdvancedSimple:
            return cudaVideoCodec_MPEG4;
    }

    return cudaVideoCodec_NONE;
}
//uncomment this to reenable MPEG-4 support
/*NVCodec mpeg4Codec = {
    .computeCudaCodec = computeMPEG4CudaCodec,
    .handlers = {
        [VAPictureParameterBufferType] = copyMPEG4PicParam,
        [VAIQMatrixBufferType] = copyMPEG4IQMatrix,
        [VASliceParameterBufferType] = copyMPEG4SliceParam,
        [VASliceDataBufferType] = copyMPEG4SliceData,
    },
    .supportedProfileCount = 4,
    .supportedProfiles = { VAProfileH263Baseline, VAProfileMPEG4Main, VAProfileMPEG4Simple, VAProfileMPEG4AdvancedSimple }
};
DEFINE_CODEC(mpeg4Codec)*/
