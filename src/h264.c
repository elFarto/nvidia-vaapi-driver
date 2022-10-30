#include "vabackend.h"
#include <string.h>

static void copyH264PicParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
{
    VAPictureParameterBufferH264* buf = (VAPictureParameterBufferH264*) buffer->ptr;

    picParams->PicWidthInMbs    = buf->picture_width_in_mbs_minus1 + 1; //int
    picParams->FrameHeightInMbs = buf->picture_height_in_mbs_minus1 + 1; //int

    ctx->renderTarget->progressiveFrame = !buf->pic_fields.bits.field_pic_flag;
    picParams->field_pic_flag    = buf->pic_fields.bits.field_pic_flag;
    picParams->bottom_field_flag = (buf->CurrPic.flags & VA_PICTURE_H264_BOTTOM_FIELD) != 0;
    picParams->second_field      = (buf->CurrPic.flags & VA_PICTURE_H264_TOP_FIELD) != 0 && (buf->CurrPic.flags & VA_PICTURE_H264_BOTTOM_FIELD) != 0;

    picParams->ref_pic_flag      = buf->pic_fields.bits.reference_pic_flag;
    picParams->intra_pic_flag    = 1; //this is set to 0 in copyH264SliceParam

    picParams->CodecSpecific.h264.log2_max_frame_num_minus4 = buf->seq_fields.bits.log2_max_frame_num_minus4;
    picParams->CodecSpecific.h264.pic_order_cnt_type = buf->seq_fields.bits.pic_order_cnt_type;
    picParams->CodecSpecific.h264.log2_max_pic_order_cnt_lsb_minus4 = buf->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4;
    picParams->CodecSpecific.h264.delta_pic_order_always_zero_flag = buf->seq_fields.bits.delta_pic_order_always_zero_flag;
    picParams->CodecSpecific.h264.frame_mbs_only_flag = buf->seq_fields.bits.frame_mbs_only_flag;

    picParams->CodecSpecific.h264.direct_8x8_inference_flag = buf->seq_fields.bits.direct_8x8_inference_flag;
    picParams->CodecSpecific.h264.num_ref_frames = buf->num_ref_frames;
    picParams->CodecSpecific.h264.residual_colour_transform_flag = buf->seq_fields.bits.residual_colour_transform_flag;
    picParams->CodecSpecific.h264.bit_depth_luma_minus8 = buf->bit_depth_luma_minus8;

    picParams->CodecSpecific.h264.bit_depth_chroma_minus8 = buf->bit_depth_chroma_minus8;
    picParams->CodecSpecific.h264.entropy_coding_mode_flag = buf->pic_fields.bits.entropy_coding_mode_flag;
    picParams->CodecSpecific.h264.pic_order_present_flag = buf->pic_fields.bits.pic_order_present_flag;

    picParams->CodecSpecific.h264.weighted_pred_flag = buf->pic_fields.bits.weighted_pred_flag;
    picParams->CodecSpecific.h264.weighted_bipred_idc = buf->pic_fields.bits.weighted_bipred_idc;

    picParams->CodecSpecific.h264.pic_init_qp_minus26 = buf->pic_init_qp_minus26;
    picParams->CodecSpecific.h264.deblocking_filter_control_present_flag = buf->pic_fields.bits.deblocking_filter_control_present_flag;
    picParams->CodecSpecific.h264.redundant_pic_cnt_present_flag = buf->pic_fields.bits.redundant_pic_cnt_present_flag;
    picParams->CodecSpecific.h264.transform_8x8_mode_flag = buf->pic_fields.bits.transform_8x8_mode_flag;

    picParams->CodecSpecific.h264.MbaffFrameFlag = buf->seq_fields.bits.mb_adaptive_frame_field_flag && !picParams->field_pic_flag;
    picParams->CodecSpecific.h264.constrained_intra_pred_flag = buf->pic_fields.bits.constrained_intra_pred_flag;
    picParams->CodecSpecific.h264.chroma_qp_index_offset = buf->chroma_qp_index_offset;
    picParams->CodecSpecific.h264.second_chroma_qp_index_offset = buf->second_chroma_qp_index_offset;

    picParams->CodecSpecific.h264.ref_pic_flag = buf->pic_fields.bits.reference_pic_flag;
    picParams->CodecSpecific.h264.frame_num = buf->frame_num;
    picParams->CodecSpecific.h264.CurrFieldOrderCnt[0] = buf->CurrPic.TopFieldOrderCnt;
    picParams->CodecSpecific.h264.CurrFieldOrderCnt[1] = buf->CurrPic.BottomFieldOrderCnt;

    for (int i = 0; i < 16; i++) {
        if (!(buf->ReferenceFrames[i].flags & VA_PICTURE_H264_INVALID)) {
            picParams->CodecSpecific.h264.dpb[i].PicIdx = pictureIdxFromSurfaceId(ctx->drv, buf->ReferenceFrames[i].picture_id);
            picParams->CodecSpecific.h264.dpb[i].FrameIdx = buf->ReferenceFrames[i].frame_idx;
            picParams->CodecSpecific.h264.dpb[i].FieldOrderCnt[0] = buf->ReferenceFrames[i].TopFieldOrderCnt;
            picParams->CodecSpecific.h264.dpb[i].FieldOrderCnt[1] = buf->ReferenceFrames[i].BottomFieldOrderCnt;
            picParams->CodecSpecific.h264.dpb[i].is_long_term = (buf->ReferenceFrames[i].flags & VA_PICTURE_H264_LONG_TERM_REFERENCE) != 0;
            picParams->CodecSpecific.h264.dpb[i].not_existing = 0;

            int tmp = 0;
            if ((buf->ReferenceFrames[i].flags & VA_PICTURE_H264_TOP_FIELD) != 0) tmp |= 1;
            if ((buf->ReferenceFrames[i].flags & VA_PICTURE_H264_BOTTOM_FIELD) != 0) tmp |= 2;
            if (tmp == 0) {
                tmp = 3;  //TODO seems to look better with a hardcoded 3
            }
            picParams->CodecSpecific.h264.dpb[i].used_for_reference = tmp;
        } else {
            picParams->CodecSpecific.h264.dpb[i].PicIdx = -1;
        }
    }
}

static void copyH264SliceParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
{
    VASliceParameterBufferH264* buf = (VASliceParameterBufferH264*) buffer->ptr;

    picParams->CodecSpecific.h264.num_ref_idx_l0_active_minus1 = buf->num_ref_idx_l0_active_minus1;
    picParams->CodecSpecific.h264.num_ref_idx_l1_active_minus1 = buf->num_ref_idx_l1_active_minus1;

    if (buf->slice_type != 2 && buf->slice_type != 4) { // != I && != SI
        picParams->intra_pic_flag = 0;
    }

    ctx->lastSliceParams = buffer->ptr;
    ctx->lastSliceParamsCount = buffer->elements;

    picParams->nNumSlices += buffer->elements;
}

static void copyH264SliceData(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    for (int i = 0; i < ctx->lastSliceParamsCount; i++)
    {
        static const uint8_t header[] = { 0, 0, 1 }; //1 as a 24-bit Big Endian

        VASliceParameterBufferH264 *sliceParams = &((VASliceParameterBufferH264*) ctx->lastSliceParams)[i];
        uint32_t offset = (uint32_t) ctx->bitstreamBuffer.size;
        appendBuffer(&ctx->sliceOffsets, &offset, sizeof(offset));
        appendBuffer(&ctx->bitstreamBuffer, header, sizeof(header));
        appendBuffer(&ctx->bitstreamBuffer, PTROFF(buf->ptr, sliceParams->slice_data_offset), sliceParams->slice_data_size);
        picParams->nBitstreamDataLen += sliceParams->slice_data_size + 3;
    }
}

static void copyH264IQMatrix(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    VAIQMatrixBufferH264 *iq = (VAIQMatrixBufferH264*) buf->ptr;

    memcpy(picParams->CodecSpecific.h264.WeightScale4x4, iq->ScalingList4x4, sizeof(iq->ScalingList4x4));
    memcpy(picParams->CodecSpecific.h264.WeightScale8x8, iq->ScalingList8x8, sizeof(iq->ScalingList8x8));
}

static cudaVideoCodec computeH264CudaCodec(VAProfile profile) {
    //cudaVideoCodec_H264_SVC missing in VA-API?
    if (profile == VAProfileH264Baseline || profile == VAProfileH264ConstrainedBaseline || profile == VAProfileH264Main || profile == VAProfileH264High) {
        return cudaVideoCodec_H264;
    }

    //if (profile == VAProfileH264StereoHigh || profile == VAProfileH264MultiviewHigh) {
    //    return cudaVideoCodec_H264_MVC;
    //}

    return cudaVideoCodec_NONE;
}

static const VAProfile h264SupportedProfiles[] = {
    VAProfileH264Baseline,
    VAProfileH264ConstrainedBaseline,
    VAProfileH264Main,
    VAProfileH264High,
};

const DECLARE_CODEC(h264Codec) = {
    .computeCudaCodec = computeH264CudaCodec,
    .handlers = {
        [VAPictureParameterBufferType] = copyH264PicParam,
        [VAIQMatrixBufferType] = copyH264IQMatrix,
        [VASliceParameterBufferType] = copyH264SliceParam,
        [VASliceDataBufferType] = copyH264SliceData,
    },
    .supportedProfileCount = ARRAY_SIZE(h264SupportedProfiles),
    .supportedProfiles = h264SupportedProfiles,
};
