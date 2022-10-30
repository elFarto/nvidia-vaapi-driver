#include "vabackend.h"
#include <sys/param.h>

//TODO incomplete as no hardware to test with
int get_relative_dist(CUVIDAV1PICPARAMS *pps, int ref_hint, int order_hint) {
    if (!pps->enable_order_hint) {
        return 0;
    }
    int diff = ref_hint - order_hint;
    int m = 1 << pps->order_hint_bits_minus1;
    return (diff & (m - 1)) - (diff & m);
}

static void copyAV1PicParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams) {
    static const int bit_depth_map[] = {0, 2, 4}; //8-bpc, 10-bpc, 12-bpc

    VADecPictureParameterBufferAV1* buf = (VADecPictureParameterBufferAV1*) buffer->ptr;
    CUVIDAV1PICPARAMS *pps = &picParams->CodecSpecific.av1;

    picParams->PicWidthInMbs = (ctx->width + 15)/16;
    picParams->FrameHeightInMbs = (ctx->height + 15)/16;

    picParams->intra_pic_flag    = buf->pic_info_fields.bits.frame_type == 0 || //Key
                                   buf->pic_info_fields.bits.frame_type == 2; //Intra-Only

    //TODO if it's not a key or switch frame type, it still *might* be a ref_pic
    picParams->ref_pic_flag      = buf->pic_info_fields.bits.frame_type == 0 ||
                                  (buf->pic_info_fields.bits.frame_type == 3 && buf->pic_info_fields.bits.show_frame);

    pps->width = ctx->width;
    pps->height = ctx->height;

    pps->frame_offset = buf->order_hint;
    pps->decodePicIdx = picParams->CurrPicIdx; //TODO not sure about this

    pps->profile = buf->profile;
    pps->use_128x128_superblock = buf->seq_info_fields.fields.use_128x128_superblock;
    pps->subsampling_x = buf->seq_info_fields.fields.subsampling_x;
    pps->subsampling_y = buf->seq_info_fields.fields.subsampling_y;
    pps->mono_chrome = buf->seq_info_fields.fields.mono_chrome;
    pps->bit_depth_minus8 = bit_depth_map[buf->bit_depth_idx];
    pps->enable_filter_intra = buf->seq_info_fields.fields.enable_filter_intra;
    pps->enable_intra_edge_filter = buf->seq_info_fields.fields.enable_intra_edge_filter;
    pps->enable_interintra_compound = buf->seq_info_fields.fields.enable_interintra_compound;
    pps->enable_masked_compound = buf->seq_info_fields.fields.enable_masked_compound;
    pps->enable_dual_filter = buf->seq_info_fields.fields.enable_dual_filter;
    pps->enable_order_hint = buf->seq_info_fields.fields.enable_order_hint;
    pps->order_hint_bits_minus1 = buf->order_hint_bits_minus_1;
    pps->enable_jnt_comp = buf->seq_info_fields.fields.enable_jnt_comp;
    //TODO not quite correct, use_superres can be 0, and enable_superres can be 1
    pps->enable_superres = buf->pic_info_fields.bits.use_superres;
    pps->enable_cdef = buf->seq_info_fields.fields.enable_cdef;
    //TODO this flag just seems to be missing from libva, however we should be able to recover it from the lr_type fields
    pps->enable_restoration = buf->loop_restoration_fields.bits.yframe_restoration_type != 0 ||
                              buf->loop_restoration_fields.bits.cbframe_restoration_type != 0 ||
                              buf->loop_restoration_fields.bits.crframe_restoration_type != 0;
    pps->enable_fgs = buf->seq_info_fields.fields.film_grain_params_present;

    pps->frame_type = buf->pic_info_fields.bits.frame_type;
    pps->show_frame = buf->pic_info_fields.bits.show_frame;
    pps->disable_cdf_update = buf->pic_info_fields.bits.disable_cdf_update;
    pps->allow_screen_content_tools = buf->pic_info_fields.bits.allow_screen_content_tools;
    pps->force_integer_mv = buf->pic_info_fields.bits.force_integer_mv || picParams->intra_pic_flag;
    pps->coded_denom = buf->superres_scale_denominator;
    pps->allow_intrabc = buf->pic_info_fields.bits.allow_intrabc;
    pps->allow_high_precision_mv = buf->pic_info_fields.bits.allow_high_precision_mv;

    pps->interp_filter = buf->interp_filter;
    pps->switchable_motion_mode = buf->pic_info_fields.bits.is_motion_mode_switchable;
    pps->use_ref_frame_mvs = buf->pic_info_fields.bits.use_ref_frame_mvs;
    pps->disable_frame_end_update_cdf = buf->pic_info_fields.bits.disable_frame_end_update_cdf;
    pps->delta_q_present = buf->mode_control_fields.bits.delta_q_present_flag;
    pps->delta_q_res = buf->mode_control_fields.bits.log2_delta_q_res;
    pps->using_qmatrix = buf->qmatrix_fields.bits.using_qmatrix;
    pps->use_superres = buf->pic_info_fields.bits.use_superres;
    pps->tx_mode = buf->mode_control_fields.bits.tx_mode;
    pps->reference_mode = buf->mode_control_fields.bits.reference_select;
    pps->allow_warped_motion = buf->pic_info_fields.bits.allow_warped_motion;
    pps->reduced_tx_set = buf->mode_control_fields.bits.reduced_tx_set_used;
    pps->skip_mode = buf->mode_control_fields.bits.skip_mode_present;

    pps->num_tile_cols = buf->tile_cols;
    pps->num_tile_rows = buf->tile_rows;
    pps->context_update_tile_id = buf->context_update_tile_id;
    picParams->nNumSlices = pps->num_tile_cols * pps->num_tile_rows;

    pps->cdef_damping_minus_3 = buf->cdef_damping_minus_3;
    pps->cdef_bits = buf->cdef_bits;
    pps->SkipModeFrame0 = 0;
    pps->SkipModeFrame1 = 0;

    //we'll need this value in a future frame
    ctx->renderTarget->order_hint = pps->frame_offset;

    if (pps->skip_mode) {
        int forwardIdx = -1;
        int backwardIdx = -1;
        int forwardHint = 0;
        int backwardHint = 0;
        int RefOrderHint[8] = {0};
        for (int i = 0; i < 8; i++) {
            NVSurface *surf = nvSurfaceFromSurfaceId(ctx->drv, buf->ref_frame_map[i]);
            if (surf != NULL && surf->pictureIdx != -1) {
                RefOrderHint[i] = surf->order_hint;
            }
        }

        for (int i = 0; i < 7; i++ ) {
            int refHint = RefOrderHint[ buf->ref_frame_idx[ i ] ];
            if ( get_relative_dist( pps, refHint, buf->order_hint ) < 0 ) {
                if ( forwardIdx < 0 || get_relative_dist( pps, refHint, forwardHint) > 0 ) {
                    forwardIdx = i;
                    forwardHint = refHint;
                }
            } else if ( get_relative_dist( pps, refHint, buf->order_hint) > 0 ) {
                if ( backwardIdx < 0 || get_relative_dist( pps, refHint, backwardHint) < 0 ) {
                    backwardIdx = i;
                    backwardHint = refHint;
                }
            }
        }
        if ( forwardIdx < 0 ) {
            //skipModeAllowed = 0
        } else if ( backwardIdx >= 0 ) {
            //skipModeAllowed = 1
            pps->SkipModeFrame0 = MIN(forwardIdx, backwardIdx) + 1;
            pps->SkipModeFrame1 = MAX(forwardIdx, backwardIdx) + 1;
        } else {
            int secondForwardIdx = -1;
            int secondForwardHint = 0;
            for (int i = 0; i < 7; i++ ) {
                int refHint = RefOrderHint[ buf->ref_frame_idx[ i ] ];
                if ( get_relative_dist( pps, refHint, forwardHint ) < 0 ) {
                    if ( secondForwardIdx < 0 || get_relative_dist( pps, refHint, secondForwardHint ) > 0 ) {
                        secondForwardIdx = i;
                        secondForwardHint = refHint;
                    }
                }
            }
            if ( secondForwardIdx < 0 ) {
                //skipModeAllowed = 0
            } else {
                //skipModeAllowed = 1
                pps->SkipModeFrame0 = MIN(forwardIdx, secondForwardIdx) + 1;
                pps->SkipModeFrame1 = MAX(forwardIdx, secondForwardIdx) + 1;
            }
        }
    }

    for (int i = 0; i < 8; i++) { //MAX_REF_FRAMES
        pps->loop_filter_ref_deltas[i] = buf->ref_deltas[i];
        pps->ref_frame_map[i] = pictureIdxFromSurfaceId(ctx->drv, buf->ref_frame_map[i]);
    }

    pps->base_qindex = buf->base_qindex;
    pps->qp_y_dc_delta_q = buf->y_dc_delta_q;
    pps->qp_u_dc_delta_q = buf->u_dc_delta_q;
    pps->qp_v_dc_delta_q = buf->v_dc_delta_q;
    pps->qp_u_ac_delta_q = buf->u_ac_delta_q;
    pps->qp_v_ac_delta_q = buf->v_ac_delta_q;
    pps->qm_y = buf->qmatrix_fields.bits.qm_y;
    pps->qm_u = buf->qmatrix_fields.bits.qm_u;
    pps->qm_v = buf->qmatrix_fields.bits.qm_v;

    pps->segmentation_enabled = buf->seg_info.segment_info_fields.bits.enabled;
    pps->segmentation_update_map = buf->seg_info.segment_info_fields.bits.update_map;
    pps->segmentation_update_data = buf->seg_info.segment_info_fields.bits.update_data;
    pps->segmentation_temporal_update = buf->seg_info.segment_info_fields.bits.temporal_update;

    pps->loop_filter_level[0] = buf->filter_level[0];
    pps->loop_filter_level[1] = buf->filter_level[1];
    pps->loop_filter_level_u = buf->filter_level_u;
    pps->loop_filter_level_v = buf->filter_level_v;
    pps->loop_filter_sharpness = buf->loop_filter_info_fields.bits.sharpness_level;
    pps->loop_filter_delta_enabled = buf->loop_filter_info_fields.bits.mode_ref_delta_enabled;
    pps->loop_filter_delta_update = buf->loop_filter_info_fields.bits.mode_ref_delta_update;
    pps->loop_filter_mode_deltas[0] = buf->mode_deltas[0];
    pps->loop_filter_mode_deltas[1] = buf->mode_deltas[1];
    pps->delta_lf_present = buf->mode_control_fields.bits.delta_lf_present_flag;
    pps->delta_lf_res = buf->mode_control_fields.bits.log2_delta_lf_res;
    pps->delta_lf_multi = buf->mode_control_fields.bits.delta_lf_multi;

    pps->lr_type[0] = buf->loop_restoration_fields.bits.yframe_restoration_type;
    pps->lr_type[1] = buf->loop_restoration_fields.bits.cbframe_restoration_type;
    pps->lr_type[2] = buf->loop_restoration_fields.bits.crframe_restoration_type;
    pps->lr_unit_size[0] = 1 + buf->loop_restoration_fields.bits.lr_unit_shift;
    pps->lr_unit_size[1] = 1 + buf->loop_restoration_fields.bits.lr_unit_shift - buf->loop_restoration_fields.bits.lr_uv_shift;
    pps->lr_unit_size[2] = pps->lr_unit_size[1];

    //TODO looks like these need to be derived from the frame itself? They're part of an extension, might just be able to set them to 0
    //pps->temporal_layer_id = buf->;
    //pps->spatial_layer_id = buf->;

    pps->apply_grain = buf->film_grain_info.film_grain_info_fields.bits.apply_grain;
    pps->overlap_flag = buf->film_grain_info.film_grain_info_fields.bits.overlap_flag;
    pps->scaling_shift_minus8 = buf->film_grain_info.film_grain_info_fields.bits.grain_scaling_minus_8;
    pps->chroma_scaling_from_luma = buf->film_grain_info.film_grain_info_fields.bits.chroma_scaling_from_luma;
    pps->ar_coeff_lag = buf->film_grain_info.film_grain_info_fields.bits.ar_coeff_lag;
    pps->ar_coeff_shift_minus6 = buf->film_grain_info.film_grain_info_fields.bits.ar_coeff_shift_minus_6;
    pps->grain_scale_shift = buf->film_grain_info.film_grain_info_fields.bits.grain_scale_shift;
    pps->clip_to_restricted_range = buf->film_grain_info.film_grain_info_fields.bits.clip_to_restricted_range;
    pps->num_y_points = buf->film_grain_info.num_y_points;
    pps->num_cb_points = buf->film_grain_info.num_cb_points;
    pps->num_cr_points = buf->film_grain_info.num_cr_points;
    pps->random_seed = buf->film_grain_info.grain_seed;
    pps->cb_mult = buf->film_grain_info.cb_mult;
    pps->cb_luma_mult = buf->film_grain_info.cb_luma_mult;
    pps->cb_offset = buf->film_grain_info.cb_offset;
    pps->cr_mult = buf->film_grain_info.cr_mult;
    pps->cr_luma_mult = buf->film_grain_info.cr_luma_mult;
    pps->cr_offset = buf->film_grain_info.cr_offset;

    for (int i = 0; i < pps->num_tile_cols; i++) {
        pps->tile_widths[i] = 1 + buf->width_in_sbs_minus_1[i];
    }
    for (int i = 0; i < pps->num_tile_rows; i++) {
        pps->tile_heights[i] = 1 + buf->height_in_sbs_minus_1[i];
    }

    for (int i = 0; i < (1<<pps->cdef_bits); i++) {
        pps->cdef_y_strength[i] = ((buf->cdef_y_strengths[i] >> 2) & 0x0f) |
                                  ((buf->cdef_y_strengths[i] & 0x03) << 4);
        pps->cdef_uv_strength[i] = ((buf->cdef_uv_strengths[i] >> 2) & 0x0f) |
                                   ((buf->cdef_uv_strengths[i] & 0x03) << 4);
    }

    //TODO replace with memcpy?
    for (int i = 0; i < 8; i++) { //MAX_SEGMENTS
        pps->segmentation_feature_mask[i] = buf->seg_info.feature_mask[i];
        for (int j = 0; j < 8; j++) { //MAX_SEGMENT_LEVEL
            //TODO these values are clipped when supplied via ffmpeg
            pps->segmentation_feature_data[i][j] = buf->seg_info.feature_data[i][j];
        }
    }

    //TODO i think it is correct
    pps->coded_lossless = 1;
    if (buf->y_dc_delta_q != 0 || buf->u_dc_delta_q != 0 || buf->v_dc_delta_q != 0 || buf->u_ac_delta_q != 0 || buf->v_ac_delta_q != 0) {
        pps->coded_lossless = 0;
    } else {
        for (int i = 0; i < 8; i++) {
            if (((pps->segmentation_feature_mask[i] & 1) != 0
                && (pps->base_qindex + pps->segmentation_feature_data[i][0] != 0))
                || pps->base_qindex != 0) {
                pps->coded_lossless = 0;
                break;
            }
        }
    }

    if (buf->primary_ref_frame == 7) { //PRIMARY_REF_NONE
        pps->primary_ref_frame = -1;
    } else {
        //TODO check this
        pps->primary_ref_frame = pps->ref_frame_map[buf->ref_frame_idx[buf->primary_ref_frame]];
    }

    for (int i = 0; i < 7; i++) { //REFS_PER_FRAME
        int ref_idx = buf->ref_frame_idx[i];
        pps->ref_frame[i].index = pps->ref_frame_map[ref_idx];
        //pull these from the surface itself
        NVSurface *surf = nvSurfaceFromSurfaceId(ctx->drv, buf->ref_frame_map[i]);
        if (surf != NULL) {
            pps->ref_frame[i].width = surf->width;
            pps->ref_frame[i].height = surf->height;
        }

        //TODO not sure on this one
        pps->global_motion[i].invalid = (buf->wm[i].wmtype == 0);
        pps->global_motion[i].wmtype = buf->wm[i].wmtype;
        for (int j = 0; j < 6; j++) {
            pps->global_motion[i].wmmat[j] = buf->wm[i].wmmat[j];
        }
    }

    if (pps->apply_grain) {
        for (int i = 0; i < 14; i++) {
            pps->scaling_points_y[i][0] = buf->film_grain_info.point_y_value[i];
            pps->scaling_points_y[i][1] = buf->film_grain_info.point_y_scaling[i];
        }
        for (int i = 0; i < 10; i++) {
            pps->scaling_points_cb[i][0] = buf->film_grain_info.point_cb_value[i];
            pps->scaling_points_cb[i][1] = buf->film_grain_info.point_cb_scaling[i];
            pps->scaling_points_cr[i][0] = buf->film_grain_info.point_cr_value[i];
            pps->scaling_points_cr[i][1] = buf->film_grain_info.point_cr_scaling[i];
        }
        //TODO memcpy?
        for (int i = 0; i < 24; i++) {
            pps->ar_coeffs_y[i] = buf->film_grain_info.ar_coeffs_y[i];
        }
        //TODO memcpy?
        for (int i = 0; i < 25; i++) {
            pps->ar_coeffs_cb[i] = buf->film_grain_info.ar_coeffs_cb[i];
            pps->ar_coeffs_cr[i] = buf->film_grain_info.ar_coeffs_cr[i];
        }
    }

    //printCUVIDPICPARAMS(picParams);
}

static void copyAV1SliceParam(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams) {
    ctx->lastSliceParams = buf->ptr;
    ctx->lastSliceParamsCount = buf->elements;
}

static void copyAV1SliceData(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams) {
    uint32_t offset = (uint32_t) ctx->bitstreamBuffer.size;
    for (int i = 0; i < ctx->lastSliceParamsCount; i++) {
        VASliceParameterBufferAV1 *sliceParams = &((VASliceParameterBufferAV1*) ctx->lastSliceParams)[i];

        //copy just the slice we're looking at
        appendBuffer(&ctx->bitstreamBuffer, PTROFF(buf->ptr, sliceParams->slice_data_offset), sliceParams->slice_data_size);

        //now append the offset and size of the slice we just copied
        appendBuffer(&ctx->sliceOffsets, &offset, sizeof(offset));
        offset += sliceParams->slice_data_size;
        appendBuffer(&ctx->sliceOffsets, &offset, sizeof(offset));
    }

    picParams->nBitstreamDataLen = ctx->bitstreamBuffer.size;
}

static cudaVideoCodec computeAV1CudaCodec(VAProfile profile) {
    switch (profile) {
        case VAProfileAV1Profile0:
        case VAProfileAV1Profile1:
            return cudaVideoCodec_AV1;
        default:
            return cudaVideoCodec_NONE;
    }
}

static const VAProfile av1SupportedProfiles[] =  {
    VAProfileAV1Profile0,
    VAProfileAV1Profile1,
};

const DECLARE_CODEC(av1Codec) = {
    .computeCudaCodec = computeAV1CudaCodec,
    .handlers = {
        [VAPictureParameterBufferType] = copyAV1PicParam,
        [VASliceParameterBufferType] = copyAV1SliceParam,
        [VASliceDataBufferType] = copyAV1SliceData
    },
    .supportedProfileCount = ARRAY_SIZE(av1SupportedProfiles),
    .supportedProfiles = av1SupportedProfiles,
};
