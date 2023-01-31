#define _GNU_SOURCE

#include "vabackend.h"
#include <stdlib.h>

static const uint8_t ff_hevc_diag_scan4x4_x[16] = {
    0, 0, 1, 0,
    1, 2, 0, 1,
    2, 3, 1, 2,
    3, 2, 3, 3,
};

static const uint8_t ff_hevc_diag_scan4x4_y[16] = {
    0, 1, 0, 2,
    1, 0, 3, 2,
    1, 0, 3, 2,
    1, 3, 2, 3,
};

static const uint8_t ff_hevc_diag_scan8x8_x[64] = {
    0, 0, 1, 0,
    1, 2, 0, 1,
    2, 3, 0, 1,
    2, 3, 4, 0,
    1, 2, 3, 4,
    5, 0, 1, 2,
    3, 4, 5, 6,
    0, 1, 2, 3,
    4, 5, 6, 7,
    1, 2, 3, 4,
    5, 6, 7, 2,
    3, 4, 5, 6,
    7, 3, 4, 5,
    6, 7, 4, 5,
    6, 7, 5, 6,
    7, 6, 7, 7,
};

static const uint8_t ff_hevc_diag_scan8x8_y[64] = {
    0, 1, 0, 2,
    1, 0, 3, 2,
    1, 0, 4, 3,
    2, 1, 0, 5,
    4, 3, 2, 1,
    0, 6, 5, 4,
    3, 2, 1, 0,
    7, 6, 5, 4,
    3, 2, 1, 0,
    7, 6, 5, 4,
    3, 2, 1, 7,
    6, 5, 4, 3,
    2, 7, 6, 5,
    4, 3, 7, 6,
    5, 4, 7, 6,
    5, 7, 6, 7,
};

static int sortFunc(const unsigned char *a, const unsigned char *b, int *POCV) {
    return POCV[*a] < POCV[*b] ? -1 : 1;
}

static int sortFuncRev(const unsigned char *a, const unsigned char *b, int *POCV) {
    return POCV[*a] < POCV[*b] ? 1 : -1;
}


static void copyHEVCPicParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
{
    VAPictureParameterBufferHEVC* buf = (VAPictureParameterBufferHEVC*) buffer->ptr;

    picParams->PicWidthInMbs    = buf->pic_width_in_luma_samples / 16;
    picParams->FrameHeightInMbs = buf->pic_height_in_luma_samples / 16;

    picParams->field_pic_flag    = !!(buf->CurrPic.flags & VA_PICTURE_HEVC_FIELD_PIC);
    picParams->bottom_field_flag = !!(buf->CurrPic.flags & VA_PICTURE_HEVC_BOTTOM_FIELD);
    picParams->second_field      = 0;

    ctx->renderTarget->progressiveFrame = !picParams->field_pic_flag;

    picParams->ref_pic_flag      = 1;
    picParams->intra_pic_flag    = buf->slice_parsing_fields.bits.IntraPicFlag;

    CUVIDHEVCPICPARAMS* ppc = &picParams->CodecSpecific.hevc;

    ppc->pic_width_in_luma_samples = buf->pic_width_in_luma_samples;
    ppc->pic_height_in_luma_samples = buf->pic_height_in_luma_samples;
    ppc->log2_min_luma_coding_block_size_minus3 = buf->log2_min_luma_coding_block_size_minus3;
    ppc->log2_diff_max_min_luma_coding_block_size = buf->log2_diff_max_min_luma_coding_block_size;
    ppc->log2_min_transform_block_size_minus2 = buf->log2_min_transform_block_size_minus2;
    ppc->log2_diff_max_min_transform_block_size = buf->log2_diff_max_min_transform_block_size;
    ppc->pcm_enabled_flag = buf->pic_fields.bits.pcm_enabled_flag;
    ppc->log2_min_pcm_luma_coding_block_size_minus3 = buf->log2_min_pcm_luma_coding_block_size_minus3; //? set conditionally in va-api
    ppc->log2_diff_max_min_pcm_luma_coding_block_size = buf->log2_diff_max_min_pcm_luma_coding_block_size;
    ppc->pcm_sample_bit_depth_luma_minus1 = buf->pcm_sample_bit_depth_luma_minus1;
    ppc->pcm_sample_bit_depth_chroma_minus1 = buf->pcm_sample_bit_depth_chroma_minus1;

//    ppc->log2_max_transform_skip_block_size_minus2 = buf->; //in VAPictureParameterBufferHEVCRext
//    ppc->log2_sao_offset_scale_luma = buf->pcm_sample_bit_depth_chroma_minus1;
//    ppc->log2_sao_offset_scale_chroma = buf->pcm_sample_bit_depth_chroma_minus1;
//    ppc->high_precision_offsets_enabled_flag = buf->pcm_sample_bit_depth_chroma_minus1;

    ppc->pcm_loop_filter_disabled_flag = buf->pic_fields.bits.pcm_loop_filter_disabled_flag;
    ppc->strong_intra_smoothing_enabled_flag = buf->pic_fields.bits.strong_intra_smoothing_enabled_flag;
    ppc->max_transform_hierarchy_depth_intra = buf->max_transform_hierarchy_depth_intra;
    ppc->max_transform_hierarchy_depth_inter = buf->max_transform_hierarchy_depth_inter;
    ppc->amp_enabled_flag = buf->pic_fields.bits.amp_enabled_flag;
    ppc->separate_colour_plane_flag = buf->pic_fields.bits.separate_colour_plane_flag;
    ppc->log2_max_pic_order_cnt_lsb_minus4 = buf->log2_max_pic_order_cnt_lsb_minus4;
    ppc->num_short_term_ref_pic_sets = buf->num_short_term_ref_pic_sets;
    ppc->long_term_ref_pics_present_flag = buf->slice_parsing_fields.bits.long_term_ref_pics_present_flag;
    ppc->num_long_term_ref_pics_sps = buf->num_long_term_ref_pic_sps;
    ppc->sps_temporal_mvp_enabled_flag = buf->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag;
    ppc->sample_adaptive_offset_enabled_flag = buf->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag;
    ppc->scaling_list_enable_flag = buf->pic_fields.bits.scaling_list_enabled_flag;
    ppc->IrapPicFlag = buf->slice_parsing_fields.bits.RapPicFlag;
    ppc->IdrPicFlag = buf->slice_parsing_fields.bits.IdrPicFlag;
    ppc->bit_depth_luma_minus8 = buf->bit_depth_luma_minus8;
    ppc->bit_depth_chroma_minus8 = buf->bit_depth_chroma_minus8;

    ppc->pps_beta_offset_div2 = buf->pps_beta_offset_div2;
    ppc->pps_tc_offset_div2 = buf->pps_tc_offset_div2;

    //This is all Range Extension/Rext stuff
//    ppc->sps_range_extension_flag = buf->;
//    ppc->transform_skip_rotation_enabled_flag = buf->; in VAPictureParameterBufferHEVCRext
//    ppc->transform_skip_context_enabled_flag = buf->slice_parsing_fields.bits.IdrPicFlag;
//    ppc->implicit_rdpcm_enabled_flag = buf->slice_parsing_fields.bits.IdrPicFlag;
//    ppc->explicit_rdpcm_enabled_flag = buf->slice_parsing_fields.bits.IdrPicFlag;
//    ppc->extended_precision_processing_flag = buf->slice_parsing_fields.bits.IdrPicFlag;
//    ppc->intra_smoothing_disabled_flag = buf->slice_parsing_fields.bits.IdrPicFlag;
//    ppc->persistent_rice_adaptation_enabled_flag = buf->slice_parsing_fields.bits.IdrPicFlag;
//    ppc->cabac_bypass_alignment_enabled_flag = buf->slice_parsing_fields.bits.IdrPicFlag;

    ppc->dependent_slice_segments_enabled_flag = buf->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag;
    ppc->slice_segment_header_extension_present_flag = buf->slice_parsing_fields.bits.slice_segment_header_extension_present_flag;
    ppc->sign_data_hiding_enabled_flag = buf->pic_fields.bits.sign_data_hiding_enabled_flag;
    ppc->cu_qp_delta_enabled_flag = buf->pic_fields.bits.cu_qp_delta_enabled_flag;
    ppc->diff_cu_qp_delta_depth = buf->diff_cu_qp_delta_depth;
    ppc->init_qp_minus26 = buf->init_qp_minus26;
    ppc->pps_cb_qp_offset = buf->pps_cb_qp_offset;
    ppc->pps_cr_qp_offset = buf->pps_cr_qp_offset;
    ppc->constrained_intra_pred_flag = buf->pic_fields.bits.constrained_intra_pred_flag;
    ppc->weighted_pred_flag = buf->pic_fields.bits.weighted_pred_flag;
    ppc->weighted_bipred_flag = buf->pic_fields.bits.weighted_bipred_flag;
    ppc->transform_skip_enabled_flag = buf->pic_fields.bits.transform_skip_enabled_flag;
    ppc->transquant_bypass_enabled_flag = buf->pic_fields.bits.transquant_bypass_enabled_flag;
    ppc->entropy_coding_sync_enabled_flag = buf->pic_fields.bits.entropy_coding_sync_enabled_flag;

    ppc->log2_parallel_merge_level_minus2 = buf->log2_parallel_merge_level_minus2;
    ppc->num_extra_slice_header_bits = buf->num_extra_slice_header_bits;
    ppc->loop_filter_across_tiles_enabled_flag = buf->pic_fields.bits.loop_filter_across_tiles_enabled_flag;
    ppc->loop_filter_across_slices_enabled_flag = buf->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag;
    ppc->output_flag_present_flag = buf->slice_parsing_fields.bits.output_flag_present_flag;
    ppc->num_ref_idx_l0_default_active_minus1 = buf->num_ref_idx_l0_default_active_minus1;
    ppc->num_ref_idx_l1_default_active_minus1 = buf->num_ref_idx_l1_default_active_minus1;
    ppc->lists_modification_present_flag = buf->slice_parsing_fields.bits.lists_modification_present_flag;

    ppc->cabac_init_present_flag = buf->slice_parsing_fields.bits.cabac_init_present_flag;
    ppc->pps_slice_chroma_qp_offsets_present_flag = buf->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag;
    ppc->deblocking_filter_override_enabled_flag = buf->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag;
    ppc->pps_deblocking_filter_disabled_flag = buf->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag;

    ppc->tiles_enabled_flag = buf->pic_fields.bits.tiles_enabled_flag;
    ppc->uniform_spacing_flag = 1;
    ppc->num_tile_columns_minus1 = buf->num_tile_columns_minus1;
    ppc->num_tile_rows_minus1 = buf->num_tile_rows_minus1;

    if (ppc->tiles_enabled_flag) {
        //the uniform_spacing_flag isn't directly exposed in VA-API, so look through the columns and rows
        //and see if they're all the same, this probably isn't correct
        for (int i = 0; i < 19; i++) {
            if (buf->column_width_minus1[i] != buf->column_width_minus1[i+1]) {
                ppc->uniform_spacing_flag = 0;
                break;
            }
        }
        if (ppc->uniform_spacing_flag) {
            for (int i = 0; i < 21; i++) {
                if (buf->row_height_minus1[i] != buf->row_height_minus1[i+1]) {
                    ppc->uniform_spacing_flag = 0;
                    break;
                }
            }
        }
    }

    //More Range Extension/Rext stuff
//    ppc->pps_range_extension_flag = buf->pic_fields.bits.tiles_enabled_flag; //in VAPictureParameterBufferHEVCRext
//    ppc->cross_component_prediction_enabled_flag = buf->pic_fields.bits.tiles_enabled_flag;
//    ppc->chroma_qp_offset_list_enabled_flag = buf->pic_fields.bits.tiles_enabled_flag;
//    ppc->diff_cu_chroma_qp_offset_depth = buf->pic_fields.bits.tiles_enabled_flag;
//    ppc->chroma_qp_offset_list_len_minus1 = buf->pic_fields.bits.tiles_enabled_flag;

    ppc->NumBitsForShortTermRPSInSlice = buf->st_rps_bits;
    ppc->NumDeltaPocsOfRefRpsIdx = 0;//TODO
    ppc->NumPocTotalCurr = 0; //this looks to be the amount of reference images...
    ppc->NumPocStCurrBefore = 0; //these three are set properly below
    ppc->NumPocStCurrAfter = 0;
    ppc->NumPocLtCurr = 0;
    ppc->CurrPicOrderCntVal = buf->CurrPic.pic_order_cnt;

    //TODO can probably be replace with memcpy
    for (int i = 0; i <= ppc->num_tile_columns_minus1; i++)
        ppc->column_width_minus1[i] = buf->column_width_minus1[i];
    for (int i = 0; i <= ppc->num_tile_rows_minus1; i++)
        ppc->row_height_minus1[i] = buf->row_height_minus1[i];

    //in VAPictureParameterBufferHEVCRext
//    for (int i = 0; i <= ppc->chroma_qp_offset_list_len_minus1; i++) {
//        ppc->cb_qp_offset_list[i] = buf->cb_qp_offset_list[i];
//        ppc->cr_qp_offset_list[i] = buf->cr_qp_offset_list[i];
//    }

    //double check this
    VAPictureHEVC *pic = &buf->CurrPic;
    for (int i = 0; i < 16; i++) {
        ppc->RefPicIdx[i]      = pictureIdxFromSurfaceId(ctx->drv, pic[i].picture_id);
        ppc->PicOrderCntVal[i] = pic[i].pic_order_cnt;
        ppc->IsLongTerm[i]     = i != 0 && (pic[i].flags & VA_PICTURE_HEVC_LONG_TERM_REFERENCE) != 0;

        if (i != 0 && ppc->RefPicIdx[i] != -1) {
            ppc->NumPocTotalCurr++;
        }

        if (pic[i].flags & VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE) {
            ppc->RefPicSetStCurrBefore[ppc->NumPocStCurrBefore++] = i;
        } else if (pic[i].flags & VA_PICTURE_HEVC_RPS_ST_CURR_AFTER) {
            ppc->RefPicSetStCurrAfter[ppc->NumPocStCurrAfter++] = i;
        } else if (pic[i].flags & VA_PICTURE_HEVC_RPS_LT_CURR) {
            ppc->RefPicSetLtCurr[ppc->NumPocLtCurr++] = i;
        }
    }

    //This is required to make sure that the RefPicSetStCurrBefore and RefPicSetStCurrAfter arrays are in the correct order
    //VA-API doesn't pass this is, only marking each picture if it's in the arrays.
    //I'm not sure this is correct
    qsort_r(ppc->RefPicSetStCurrBefore, ppc->NumPocStCurrBefore, sizeof(unsigned char), (__compar_d_fn_t) sortFuncRev, ppc->PicOrderCntVal);
    qsort_r(ppc->RefPicSetStCurrAfter, ppc->NumPocStCurrAfter, sizeof(unsigned char), (__compar_d_fn_t) sortFunc, ppc->PicOrderCntVal);
}

static void copyHEVCSliceParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
{
    ctx->lastSliceParams = buffer->ptr;
    ctx->lastSliceParamsCount = buffer->elements;

    picParams->nNumSlices += buffer->elements;
}

static void copyHEVCSliceData(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    for (unsigned int i = 0; i < ctx->lastSliceParamsCount; i++)
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

static void copyHEVCIQMatrix(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    VAIQMatrixBufferHEVC *iq = (VAIQMatrixBufferHEVC*) buf->ptr;
    CUVIDHEVCPICPARAMS* ppc = &picParams->CodecSpecific.hevc;

    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 16; j++) {
            int  pos = 4 * ff_hevc_diag_scan4x4_y[j] + ff_hevc_diag_scan4x4_x[j];
            ppc->ScalingList4x4[i][j] = iq->ScalingList4x4[i][pos];
        }

        for (int j = 0; j < 64; j++) {
            int pos = 8 * ff_hevc_diag_scan8x8_y[j] + ff_hevc_diag_scan8x8_x[j];
            ppc->ScalingList8x8[i][j]   = iq->ScalingList8x8[i][pos];
            ppc->ScalingList16x16[i][j] = iq->ScalingList16x16[i][pos];

            if (i < 2)
                ppc->ScalingList32x32[i][j] = iq->ScalingList32x32[i * 3][pos];
        }

        ppc->ScalingListDCCoeff16x16[i] = iq->ScalingListDC16x16[i];
        if (i < 2)
            ppc->ScalingListDCCoeff32x32[i] = iq->ScalingListDC32x32[i * 3];
    }
}

static cudaVideoCodec computeHEVCCudaCodec(VAProfile profile) {
    switch (profile) {
        case VAProfileHEVCMain:
        case VAProfileHEVCMain10:
        case VAProfileHEVCMain12:
        case VAProfileHEVCMain444:
        case VAProfileHEVCMain444_10:
        case VAProfileHEVCMain444_12:
            return cudaVideoCodec_HEVC;
        default:
            return cudaVideoCodec_NONE;
    }
}

static const VAProfile hevcSupportedProfiles[] = {
    VAProfileHEVCMain,
    VAProfileHEVCMain10,
    VAProfileHEVCMain12,
    VAProfileHEVCMain444,
    VAProfileHEVCMain444_10,
    VAProfileHEVCMain444_12,
};

const DECLARE_CODEC(hevcCodec) = {
    .computeCudaCodec = computeHEVCCudaCodec,
    .handlers = {
        [VAPictureParameterBufferType] = copyHEVCPicParam,
        [VAIQMatrixBufferType] = copyHEVCIQMatrix,
        [VASliceParameterBufferType] = copyHEVCSliceParam,
        [VASliceDataBufferType] = copyHEVCSliceData,
    },
    .supportedProfileCount = ARRAY_SIZE(hevcSupportedProfiles),
    .supportedProfiles = hevcSupportedProfiles,
};
