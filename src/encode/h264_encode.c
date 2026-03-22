#include "h264_encode.h"
#include "encode_common.h"
#include <string.h>
#include <stdlib.h>
#include <sys/param.h>

static uint32_t nvenc_mb_dim(uint32_t value)
{
    return (value + 15u) / 16u;
}

bool isH264EncodeProfile(VAProfile profile)
{
    return profile == VAProfileH264ConstrainedBaseline ||
           profile == VAProfileH264Main ||
           profile == VAProfileH264High;
}

GUID nvenc_h264_profile_guid(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
        return NV_ENC_H264_PROFILE_BASELINE_GUID;
    case VAProfileH264Main:
        return NV_ENC_H264_PROFILE_MAIN_GUID;
    case VAProfileH264High:
        return NV_ENC_H264_PROFILE_HIGH_GUID;
    default:
        return NV_ENC_CODEC_PROFILE_AUTOSELECT_GUID;
    }
}

uint32_t nvenc_h264_level_from_va(uint8_t level_idc)
{
    switch (level_idc) {
    case 9:
        return NV_ENC_LEVEL_H264_1b;
    case 10:
        return NV_ENC_LEVEL_H264_1;
    case 11:
        return NV_ENC_LEVEL_H264_11;
    case 12:
        return NV_ENC_LEVEL_H264_12;
    case 13:
        return NV_ENC_LEVEL_H264_13;
    case 20:
        return NV_ENC_LEVEL_H264_2;
    case 21:
        return NV_ENC_LEVEL_H264_21;
    case 22:
        return NV_ENC_LEVEL_H264_22;
    case 30:
        return NV_ENC_LEVEL_H264_3;
    case 31:
        return NV_ENC_LEVEL_H264_31;
    case 32:
        return NV_ENC_LEVEL_H264_32;
    case 40:
        return NV_ENC_LEVEL_H264_4;
    case 41:
        return NV_ENC_LEVEL_H264_41;
    case 42:
        return NV_ENC_LEVEL_H264_42;
    case 50:
        return NV_ENC_LEVEL_H264_5;
    case 51:
        return NV_ENC_LEVEL_H264_51;
    case 52:
        return NV_ENC_LEVEL_H264_52;
    case 60:
        return NV_ENC_LEVEL_H264_60;
    case 61:
        return NV_ENC_LEVEL_H264_61;
    case 62:
        return NV_ENC_LEVEL_H264_62;
    default:
        return NV_ENC_LEVEL_AUTOSELECT;
    }
}

uint32_t nvenc_mb_width(uint32_t width)
{
    return nvenc_mb_dim(width);
}

uint32_t nvenc_mb_height(uint32_t height)
{
    return nvenc_mb_dim(height);
}

uint32_t nvenc_clamp_quality_level(uint32_t quality_level)
{
    if (quality_level < NVENC_H264_MIN_QUALITY_LEVEL) {
        return NVENC_H264_MIN_QUALITY_LEVEL;
    }
    if (quality_level > NVENC_H264_MAX_QUALITY_LEVEL) {
        return NVENC_H264_MAX_QUALITY_LEVEL;
    }
    return quality_level;
}

uint32_t nvenc_clamp_qp(int32_t qp)
{
    if (qp < 0) {
        return 0;
    }
    if ((uint32_t)qp > NVENC_H264_MAX_QP) {
        return NVENC_H264_MAX_QP;
    }
    return (uint32_t)qp;
}

GUID nvenc_preset_from_quality_level(uint32_t quality_level)
{
    if (quality_level == 0) {
        return NV_ENC_PRESET_P4_GUID;
    }
    quality_level = nvenc_clamp_quality_level(quality_level);
    switch (quality_level) {
    case 1:
        return NV_ENC_PRESET_P5_GUID;
    case 2:
        return NV_ENC_PRESET_P4_GUID;
    case 3:
        return NV_ENC_PRESET_P3_GUID;
    case 4:
        return NV_ENC_PRESET_P2_GUID;
    case 5:
    default:
        return NV_ENC_PRESET_P1_GUID;
    }
}

bool nvenc_is_h264_profile(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264Main:
    case VAProfileH264High:
    case VAProfileH264ConstrainedBaseline:
    case VAProfileH264MultiviewHigh:
    case VAProfileH264StereoHigh:
        return true;
    default:
        break;
    }
    return false;
}

bool nvenc_is_screen_share(const NVEncodeContext *enc)
{
    if (!enc || !nvenc_is_h264_profile(enc->profile) || !enc->haveSeq) {
        return false;
    }
    if (enc->seqParams.intra_period != 0) {
        return false;
    }
    if (enc->seqParams.ip_period != 1) {
        return false;
    }
    if (enc->seqParams.intra_idr_period == 0) {
        return false;
    }
    return true;
}

NVEncSeqSignature nvenc_seq_signature_from_va(const VAEncSequenceParameterBufferH264 *seq)
{
    NVEncSeqSignature sig;
    memset(&sig, 0, sizeof(sig));
    sig.intra_period = seq->intra_period;
    sig.intra_idr_period = seq->intra_idr_period;
    sig.ip_period = seq->ip_period;
    /* bit_rate changes should be driven via RC/HRD updates, not sequence reconfigure */
    sig.bits_per_second = 0;
    sig.max_num_ref_frames = seq->max_num_ref_frames;
    sig.picture_width_in_mbs = seq->picture_width_in_mbs;
    sig.picture_height_in_mbs = seq->picture_height_in_mbs;
    sig.seq_fields_value = seq->seq_fields.value;
    sig.level_idc = seq->level_idc;
    sig.bit_depth_luma_minus8 = seq->bit_depth_luma_minus8;
    sig.bit_depth_chroma_minus8 = seq->bit_depth_chroma_minus8;
    sig.frame_cropping_flag = seq->frame_cropping_flag;
    sig.frame_crop_left_offset = seq->frame_crop_left_offset;
    sig.frame_crop_right_offset = seq->frame_crop_right_offset;
    sig.frame_crop_top_offset = seq->frame_crop_top_offset;
    sig.frame_crop_bottom_offset = seq->frame_crop_bottom_offset;
    return sig;
}

bool nvd_reconfig_force_idr(const NVEncodeContext *enc)
{
    if (!enc || !nvenc_is_screen_share(enc) || !nvenc_use_ptd(enc)) {
        return false;
    }

    /*
     * NV_ENC_RECONFIGURE_PARAMS::forceIDR is only valid with PTD enabled.
     * Our H.264 VA-API path keeps PTD disabled so Chromium/FFmpeg can drive
     * picture type and GOP cadence explicitly. Preserve that contract for
     * bitrate/framerate/runtime reconfigure and only keep an IDR escape hatch
     * for true sequence-level changes on PTD-managed paths.
     */
    switch ((NVEncReconfigureReason)enc->reconfigureReason) {
    case NVENC_RECONFIGURE_REASON_SEQ_PARAMS:
    case NVENC_RECONFIGURE_REASON_RIR:
    case NVENC_RECONFIGURE_REASON_PACKED_SEI_POLICY:
        return true;
    case NVENC_RECONFIGURE_REASON_NONE:
    case NVENC_RECONFIGURE_REASON_SEQ_BITRATE:
    case NVENC_RECONFIGURE_REASON_RATE_CONTROL:
    case NVENC_RECONFIGURE_REASON_HRD:
    case NVENC_RECONFIGURE_REASON_FRAMERATE:
    case NVENC_RECONFIGURE_REASON_QUALITY:
    case NVENC_RECONFIGURE_REASON_QP_MAP_MODE:
    case NVENC_RECONFIGURE_REASON_SLICE_MODE:
    default:
        return false;
    }
}

uint32_t nvd_default_gop(uint32_t fr_num, uint32_t fr_den)
{
    if (fr_den == 0) {
        fr_den = 1;
    }
    if (fr_num == 0) {
        return NVENC_H264_DEFAULT_GOP;
    }
    uint64_t gop = ((uint64_t)fr_num * 2u + (fr_den / 2u)) / fr_den;
    if (gop == 0) {
        gop = NVENC_H264_DEFAULT_GOP;
    }
    if (gop > UINT32_MAX) {
        gop = UINT32_MAX;
    }
    return (uint32_t)gop;
}

void nvenc_update_fixed_qp(NVEncodeContext *enc)
{
    if (!enc->havePic || !enc->haveSlice) {
        return;
    }

    uint32_t base_qp = nvenc_clamp_qp((int32_t)enc->picParams.pic_init_qp +
                                      enc->sliceParams.slice_qp_delta);

    int slice_type = enc->sliceParams.slice_type % 5;
    switch (slice_type) {
    case 2: // I
    case 4: // SI
        if (!enc->haveQpI || enc->qpI != base_qp) {
            enc->qpI = base_qp;
            enc->haveQpI = true;
        }
        break;
    case 1: // B
        if (!enc->haveQpB || enc->qpB != base_qp) {
            enc->qpB = base_qp;
            enc->haveQpB = true;
        }
        break;
    case 0: // P
    case 3: // SP
    default:
        if (!enc->haveQpP || enc->qpP != base_qp) {
            enc->qpP = base_qp;
            enc->haveQpP = true;
        }
        break;
    }
}

void nvenc_update_visible_dimensions(NVEncodeContext *enc)
{
    if (!enc->haveSeq) {
        return;
    }
    if (enc->seqParams.picture_width_in_mbs == 0 || enc->seqParams.picture_height_in_mbs == 0) {
        return;
    }

    uint32_t width = enc->seqParams.picture_width_in_mbs * 16;
    uint32_t height = enc->seqParams.picture_height_in_mbs * 16;

    if (enc->seqParams.frame_cropping_flag) {
        uint32_t crop_unit_x = 1;
        uint32_t crop_unit_y = 2;
        switch (enc->seqParams.seq_fields.bits.chroma_format_idc) {
        case 0:
            crop_unit_x = 1;
            crop_unit_y = 2 - enc->seqParams.seq_fields.bits.frame_mbs_only_flag;
            break;
        case 1:
            crop_unit_x = 2;
            crop_unit_y = 2 * (2 - enc->seqParams.seq_fields.bits.frame_mbs_only_flag);
            break;
        case 2:
            crop_unit_x = 2;
            crop_unit_y = 1 * (2 - enc->seqParams.seq_fields.bits.frame_mbs_only_flag);
            break;
        case 3:
            crop_unit_x = 1;
            crop_unit_y = 1 * (2 - enc->seqParams.seq_fields.bits.frame_mbs_only_flag);
            break;
        default:
            break;
        }

        uint32_t crop_w = (enc->seqParams.frame_crop_left_offset +
                           enc->seqParams.frame_crop_right_offset) * crop_unit_x;
        uint32_t crop_h = (enc->seqParams.frame_crop_top_offset +
                           enc->seqParams.frame_crop_bottom_offset) * crop_unit_y;

        if (crop_w < width) {
            width -= crop_w;
        }
        if (crop_h < height) {
            height -= crop_h;
        }
    } else if (enc->context) {
        uint32_t context_width = enc->context->width;
        uint32_t context_height = enc->context->height;

        /*
         * Some VA-API clients keep context/surface dimensions at the intended
         * visible size and only express the coded macroblock padding through
         * picture_{width,height}_in_mbs.
         */
        if (context_width > 0 && context_width < width &&
            ROUND_UP(context_width, 16) == width) {
            width = context_width;
        }
        if (context_height > 0 && context_height < height &&
            ROUND_UP(context_height, 16) == height) {
            height = context_height;
        }
    }

    if (width != enc->width || height != enc->height) {
        enc->width = width;
        enc->height = height;
    }
}

typedef struct {
    uint8_t    *data;
    size_t      size;
    size_t      capacity;
    uint64_t    reg;
    size_t      bits_left_in_reg;
    uint32_t    bit_length;
} NVH264BitstreamBuilder;

typedef struct {
    uint8_t    *data;
    size_t      size;
    uint32_t    bit_length;
} NVH264PackedBlob;

static void nvenc_h264_packed_blob_reset(NVH264PackedBlob *blob)
{
    if (!blob) {
        return;
    }
    free(blob->data);
    blob->data = NULL;
    blob->size = 0;
    blob->bit_length = 0;
}

static bool nvenc_h264_bw_reserve(NVH264BitstreamBuilder *bs, size_t extra)
{
    if (!bs) {
        return false;
    }
    if (bs->size + extra <= bs->capacity) {
        return true;
    }

    size_t new_capacity = bs->capacity ? bs->capacity : 64u;
    while (new_capacity < bs->size + extra) {
        new_capacity *= 2u;
    }

    uint8_t *tmp = (uint8_t*)realloc(bs->data, new_capacity);
    if (!tmp) {
        return false;
    }

    bs->data = tmp;
    bs->capacity = new_capacity;
    return true;
}

static bool nvenc_h264_bw_flush_reg(NVH264BitstreamBuilder *bs)
{
    size_t bits_in_reg = 64u - bs->bits_left_in_reg;
    if (bits_in_reg == 0u) {
        return true;
    }

    size_t bytes_in_reg = (bits_in_reg + 7u) / 8u;
    if (!nvenc_h264_bw_reserve(bs, bytes_in_reg)) {
        return false;
    }

    uint64_t reg = bs->reg;
    reg <<= (64u - bits_in_reg);
    for (size_t i = 0; i < bytes_in_reg; i++) {
        size_t shift = 56u - i * 8u;
        bs->data[bs->size++] = (uint8_t)((reg >> shift) & 0xffu);
    }

    bs->reg = 0u;
    bs->bits_left_in_reg = 64u;
    return true;
}

static bool nvenc_h264_bw_append_bits(NVH264BitstreamBuilder *bs,
                                      size_t num_bits,
                                      uint64_t value)
{
    if (!bs || num_bits > 64u) {
        return false;
    }

    while (num_bits > 0u) {
        if (bs->bits_left_in_reg == 0u &&
            !nvenc_h264_bw_flush_reg(bs)) {
            return false;
        }

        size_t bits_to_write = num_bits > bs->bits_left_in_reg
                                 ? bs->bits_left_in_reg
                                 : num_bits;
        uint64_t chunk = value >> (num_bits - bits_to_write);
        if (bits_to_write < 64u) {
            chunk &= ((1ull << bits_to_write) - 1ull);
            bs->reg <<= bits_to_write;
            bs->reg |= chunk;
        } else {
            bs->reg = chunk;
        }
        bs->bits_left_in_reg -= bits_to_write;
        bs->bit_length += (uint32_t)bits_to_write;
        num_bits -= bits_to_write;
    }
    return true;
}

static bool nvenc_h264_bw_append_bool(NVH264BitstreamBuilder *bs, bool value)
{
    return nvenc_h264_bw_append_bits(bs, 1u, value ? 1u : 0u);
}

static bool nvenc_h264_bw_append_ue(NVH264BitstreamBuilder *bs, uint32_t value)
{
    uint32_t code_num = value + 1u;
    size_t leading_zero_bits = 0u;
    uint32_t tmp = code_num;

    while (tmp > 1u) {
        tmp >>= 1u;
        leading_zero_bits++;
    }

    return nvenc_h264_bw_append_bits(bs, leading_zero_bits, 0u) &&
           nvenc_h264_bw_append_bits(bs, leading_zero_bits + 1u, code_num);
}

static bool nvenc_h264_bw_append_se(NVH264BitstreamBuilder *bs, int32_t value)
{
    uint32_t code_num = value <= 0
                          ? (uint32_t)(-value * 2)
                          : (uint32_t)(value * 2 - 1);
    return nvenc_h264_bw_append_ue(bs, code_num);
}

static bool nvenc_h264_bw_flush(NVH264BitstreamBuilder *bs)
{
    if (!bs || bs->bits_left_in_reg == 64u) {
        return true;
    }
    return nvenc_h264_bw_flush_reg(bs);
}

static bool nvenc_h264_bw_begin_nalu(NVH264BitstreamBuilder *bs,
                                     uint8_t nal_type,
                                     uint8_t nal_ref_idc)
{
    return nvenc_h264_bw_append_bits(bs, 32u, 0x00000001u) &&
           nvenc_h264_bw_flush(bs) &&
           nvenc_h264_bw_append_bits(bs, 1u, 0u) &&
           nvenc_h264_bw_append_bits(bs, 2u, nal_ref_idc) &&
           nvenc_h264_bw_append_bits(bs, 5u, nal_type);
}

static bool nvenc_h264_bw_finish_nalu(NVH264BitstreamBuilder *bs)
{
    size_t align_bits = 0u;

    if (!bs) {
        return false;
    }
    if (!nvenc_h264_bw_append_bool(bs, true)) {
        return false;
    }
    align_bits = bs->bits_left_in_reg & 7u;
    if (align_bits > 0u &&
        !nvenc_h264_bw_append_bits(bs, align_bits, 0u)) {
        return false;
    }
    return nvenc_h264_bw_flush(bs);
}

static bool nvenc_h264_bw_detach(NVH264BitstreamBuilder *bs,
                                 NVH264PackedBlob *out)
{
    if (!bs || !out || !nvenc_h264_bw_flush(bs)) {
        return false;
    }

    nvenc_h264_packed_blob_reset(out);
    out->data = bs->data;
    out->size = bs->size;
    out->bit_length = bs->bit_length;

    bs->data = NULL;
    bs->size = 0;
    bs->capacity = 0;
    bs->reg = 0u;
    bs->bits_left_in_reg = 64u;
    bs->bit_length = 0u;
    return true;
}

static void nvenc_h264_bw_discard(NVH264BitstreamBuilder *bs)
{
    if (!bs) {
        return;
    }
    free(bs->data);
    memset(bs, 0, sizeof(*bs));
    bs->bits_left_in_reg = 64u;
}

static void nvenc_h264_derive_hrd_value(uint32_t bits,
                                        uint8_t constant_term,
                                        uint8_t *scale_out,
                                        uint32_t *value_minus1_out)
{
    uint64_t value = bits ? bits : 1u;
    uint8_t scale = 0u;
    uint64_t coded = 1u;

    while (scale < 15u) {
        uint64_t unit = 1ull << (scale + constant_term);
        coded = (value + unit - 1u) / unit;
        if (coded > 0u && coded <= UINT32_MAX) {
            break;
        }
        scale++;
    }

    if (scale_out) {
        *scale_out = scale;
    }
    if (value_minus1_out) {
        *value_minus1_out = coded > 0u ? (uint32_t)(coded - 1u) : 0u;
    }
}

static uint32_t nvenc_h264_effective_bitrate_bps(const NVEncodeContext *enc)
{
    if (!enc) {
        return 0;
    }
    if (enc->haveRc && enc->rcParams.bits_per_second) {
        return enc->rcParams.bits_per_second;
    }
    if (enc->haveSeq && enc->seqParams.bits_per_second) {
        return enc->seqParams.bits_per_second;
    }
    return 2000000u;
}

static uint32_t nvenc_h264_effective_cpb_bits(const NVEncodeContext *enc,
                                              uint32_t bitrate_bps)
{
    if (!enc) {
        return bitrate_bps ? bitrate_bps : 1u;
    }
    if (enc->haveHrd && enc->hrdParams.buffer_size) {
        return enc->hrdParams.buffer_size;
    }
    if (bitrate_bps >= UINT32_MAX / 2u) {
        return UINT32_MAX;
    }
    return bitrate_bps ? bitrate_bps * 2u : 1u;
}

static bool nvenc_h264_is_high_profile(const NVEncodeContext *enc)
{
    return enc && enc->profile == VAProfileH264High;
}

static __attribute__((unused)) bool
nvenc_h264_should_build_fallback_headers(const NVEncodeContext *enc)
{
    uint32_t slice_type = 0u;
    uint32_t full_mbs = 0u;

    if (!enc ||
        !isH264EncodeProfile(enc->profile) ||
        !enc->haveSeq ||
        !enc->havePic ||
        !enc->haveSlice ||
        nvenc_use_ptd(enc) ||
        enc->encConfig.frameIntervalP > 1 ||
        enc->seqParams.picture_width_in_mbs == 0 ||
        enc->seqParams.picture_height_in_mbs == 0 ||
        !enc->seqParams.seq_fields.bits.frame_mbs_only_flag ||
        enc->seqParams.seq_fields.bits.seq_scaling_matrix_present_flag ||
        enc->picParams.pic_fields.bits.pic_scaling_matrix_present_flag ||
        enc->seqParams.seq_fields.bits.chroma_format_idc != 1u ||
        enc->seqParams.bit_depth_luma_minus8 != 0u ||
        enc->seqParams.bit_depth_chroma_minus8 != 0u ||
        enc->sliceParams.macroblock_address != 0u) {
        return false;
    }

    slice_type = enc->sliceParams.slice_type % 5u;
    if (slice_type != 0u && slice_type != 2u) {
        return false;
    }
    if (enc->picParams.pic_fields.bits.weighted_pred_flag ||
        enc->picParams.pic_fields.bits.weighted_bipred_idc != 0u ||
        enc->picParams.pic_fields.bits.pic_order_present_flag ||
        enc->picParams.pic_fields.bits.redundant_pic_cnt_present_flag) {
        return false;
    }

    full_mbs = enc->seqParams.picture_width_in_mbs *
               enc->seqParams.picture_height_in_mbs;
    if (enc->sliceParams.num_macroblocks != full_mbs) {
        return false;
    }

    if (enc->sliceCount > 1u) {
        return false;
    }
    return true;
}

static void nvenc_h264_profile_sps_header(const NVEncodeContext *enc,
                                          uint8_t *profile_idc,
                                          bool *constraint_set0_flag,
                                          bool *constraint_set1_flag)
{
    if (!enc || !profile_idc || !constraint_set0_flag || !constraint_set1_flag) {
        return;
    }

    *constraint_set0_flag = false;
    *constraint_set1_flag = false;
    switch (enc->profile) {
    case VAProfileH264ConstrainedBaseline:
        *profile_idc = 66u;
        *constraint_set0_flag = true;
        *constraint_set1_flag = true;
        break;
    case VAProfileH264Main:
        *profile_idc = 77u;
        *constraint_set1_flag = true;
        break;
    case VAProfileH264High:
    default:
        *profile_idc = 100u;
        break;
    }
}

static __attribute__((unused)) bool
nvenc_h264_build_fallback_sps(const NVEncodeContext *enc,
                              NVH264PackedBlob *out)
{
    NVH264BitstreamBuilder bs = { 0 };
    uint8_t profile_idc = 100u;
    bool constraint_set0_flag = false;
    bool constraint_set1_flag = false;
    bool screen_share_defaults = false;
    bool have_vui = false;
    bool aspect_ratio_info_present_flag = false;
    bool timing_info_present_flag = false;
    bool bitstream_restriction_flag = false;
    bool fixed_frame_rate_flag = false;
    bool low_delay_hrd_flag = false;
    bool motion_vectors_over_pic_boundaries_flag = false;
    bool nal_hrd_parameters_present_flag = false;
    bool pic_struct_present_flag = false;
    uint32_t bitrate_bps = 0u;
    uint32_t cpb_bits = 0u;
    uint8_t bit_rate_scale = 0u;
    uint8_t cpb_size_scale = 0u;
    uint32_t bit_rate_value_minus1 = 0u;
    uint32_t cpb_size_value_minus1 = 0u;
    uint32_t max_num_reorder_frames = 0u;
    uint32_t max_dec_frame_buffering = 0u;
    uint32_t max_bytes_per_pic_denom = 2u;
    uint32_t max_bits_per_mb_denom = 1u;
    uint32_t log2_max_mv_length_horizontal = 16u;
    uint32_t log2_max_mv_length_vertical = 16u;

    if (!enc || !out) {
        return false;
    }

    bs.bits_left_in_reg = 64u;
    screen_share_defaults = nvenc_is_screen_share(enc);
    nvenc_h264_profile_sps_header(enc,
                                  &profile_idc,
                                  &constraint_set0_flag,
                                  &constraint_set1_flag);

    have_vui = enc->seqParams.vui_parameters_present_flag ||
               enc->seqParams.vui_fields.value != 0u ||
               enc->seqParams.aspect_ratio_idc != 0u ||
               enc->seqParams.sar_width != 0u ||
               enc->seqParams.sar_height != 0u ||
               enc->seqParams.num_units_in_tick != 0u ||
               enc->seqParams.time_scale != 0u ||
               screen_share_defaults;
    aspect_ratio_info_present_flag =
        enc->seqParams.vui_fields.bits.aspect_ratio_info_present_flag &&
        enc->seqParams.aspect_ratio_idc != 0u;
    timing_info_present_flag =
        enc->seqParams.vui_fields.bits.timing_info_present_flag ||
        screen_share_defaults;
    bitstream_restriction_flag =
        enc->seqParams.vui_fields.bits.bitstream_restriction_flag ||
        screen_share_defaults;
    fixed_frame_rate_flag =
        enc->seqParams.vui_fields.bits.fixed_frame_rate_flag ||
        screen_share_defaults;
    low_delay_hrd_flag = enc->seqParams.vui_fields.bits.low_delay_hrd_flag;
    motion_vectors_over_pic_boundaries_flag =
        enc->seqParams.vui_fields.bits.motion_vectors_over_pic_boundaries_flag ||
        screen_share_defaults;
    nal_hrd_parameters_present_flag = enc->haveHrd || screen_share_defaults;
    pic_struct_present_flag = false;
    bitrate_bps = nvenc_h264_effective_bitrate_bps(enc);
    cpb_bits = nvenc_h264_effective_cpb_bits(enc, bitrate_bps);
    nvenc_h264_derive_hrd_value(bitrate_bps, 6u,
                                &bit_rate_scale, &bit_rate_value_minus1);
    nvenc_h264_derive_hrd_value(cpb_bits, 4u,
                                &cpb_size_scale, &cpb_size_value_minus1);
    max_num_reorder_frames = 0u;
    max_dec_frame_buffering = enc->seqParams.max_num_ref_frames;
    if (max_dec_frame_buffering == 0u) {
        max_dec_frame_buffering = 1u;
    }
    if (enc->seqParams.vui_fields.bits.log2_max_mv_length_horizontal != 0u) {
        log2_max_mv_length_horizontal =
            enc->seqParams.vui_fields.bits.log2_max_mv_length_horizontal;
    }
    if (enc->seqParams.vui_fields.bits.log2_max_mv_length_vertical != 0u) {
        log2_max_mv_length_vertical =
            enc->seqParams.vui_fields.bits.log2_max_mv_length_vertical;
    }

    if (!nvenc_h264_bw_begin_nalu(&bs, 7u, 3u) ||
        !nvenc_h264_bw_append_bits(&bs, 8u, profile_idc) ||
        !nvenc_h264_bw_append_bool(&bs, constraint_set0_flag) ||
        !nvenc_h264_bw_append_bool(&bs, constraint_set1_flag) ||
        !nvenc_h264_bw_append_bool(&bs, false) ||
        !nvenc_h264_bw_append_bool(&bs, false) ||
        !nvenc_h264_bw_append_bool(&bs, false) ||
        !nvenc_h264_bw_append_bool(&bs, false) ||
        !nvenc_h264_bw_append_bits(&bs, 2u, 0u) ||
        !nvenc_h264_bw_append_bits(&bs, 8u,
                                   enc->seqParams.level_idc ?
                                   enc->seqParams.level_idc : 41u) ||
        !nvenc_h264_bw_append_ue(&bs, enc->seqParams.seq_parameter_set_id)) {
        nvenc_h264_bw_discard(&bs);
        return false;
    }

    if (nvenc_h264_is_high_profile(enc)) {
        if (!nvenc_h264_bw_append_ue(&bs, enc->seqParams.seq_fields.bits.chroma_format_idc) ||
            !nvenc_h264_bw_append_ue(&bs, enc->seqParams.bit_depth_luma_minus8) ||
            !nvenc_h264_bw_append_ue(&bs, enc->seqParams.bit_depth_chroma_minus8) ||
            !nvenc_h264_bw_append_bool(&bs, false) ||
            !nvenc_h264_bw_append_bool(&bs, false)) {
            nvenc_h264_bw_discard(&bs);
            return false;
        }
    }

    if (!nvenc_h264_bw_append_ue(&bs, enc->seqParams.seq_fields.bits.log2_max_frame_num_minus4) ||
        !nvenc_h264_bw_append_ue(&bs, enc->seqParams.seq_fields.bits.pic_order_cnt_type)) {
        nvenc_h264_bw_discard(&bs);
        return false;
    }

    if (enc->seqParams.seq_fields.bits.pic_order_cnt_type == 0u) {
        if (!nvenc_h264_bw_append_ue(&bs,
                                     enc->seqParams.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4)) {
            nvenc_h264_bw_discard(&bs);
            return false;
        }
    } else if (enc->seqParams.seq_fields.bits.pic_order_cnt_type == 1u) {
        if (!nvenc_h264_bw_append_bool(&bs,
                                       enc->seqParams.seq_fields.bits.delta_pic_order_always_zero_flag) ||
            !nvenc_h264_bw_append_se(&bs, enc->seqParams.offset_for_non_ref_pic) ||
            !nvenc_h264_bw_append_se(&bs, enc->seqParams.offset_for_top_to_bottom_field) ||
            !nvenc_h264_bw_append_ue(&bs, enc->seqParams.num_ref_frames_in_pic_order_cnt_cycle)) {
            nvenc_h264_bw_discard(&bs);
            return false;
        }
        for (uint32_t i = 0;
             i < enc->seqParams.num_ref_frames_in_pic_order_cnt_cycle;
             i++) {
            if (!nvenc_h264_bw_append_se(&bs, enc->seqParams.offset_for_ref_frame[i])) {
                nvenc_h264_bw_discard(&bs);
                return false;
            }
        }
    }

    if (!nvenc_h264_bw_append_ue(&bs, enc->seqParams.max_num_ref_frames) ||
        !nvenc_h264_bw_append_bool(&bs, screen_share_defaults) ||
        !nvenc_h264_bw_append_ue(&bs, enc->seqParams.picture_width_in_mbs - 1u) ||
        !nvenc_h264_bw_append_ue(&bs, enc->seqParams.picture_height_in_mbs - 1u) ||
        !nvenc_h264_bw_append_bool(&bs, enc->seqParams.seq_fields.bits.frame_mbs_only_flag)) {
        nvenc_h264_bw_discard(&bs);
        return false;
    }

    if (!enc->seqParams.seq_fields.bits.frame_mbs_only_flag &&
        !nvenc_h264_bw_append_bool(&bs,
                                   enc->seqParams.seq_fields.bits.mb_adaptive_frame_field_flag)) {
        nvenc_h264_bw_discard(&bs);
        return false;
    }

    if (!nvenc_h264_bw_append_bool(&bs, enc->seqParams.seq_fields.bits.direct_8x8_inference_flag) ||
        !nvenc_h264_bw_append_bool(&bs, enc->seqParams.frame_cropping_flag)) {
        nvenc_h264_bw_discard(&bs);
        return false;
    }
    if (enc->seqParams.frame_cropping_flag &&
        (!nvenc_h264_bw_append_ue(&bs, enc->seqParams.frame_crop_left_offset) ||
         !nvenc_h264_bw_append_ue(&bs, enc->seqParams.frame_crop_right_offset) ||
         !nvenc_h264_bw_append_ue(&bs, enc->seqParams.frame_crop_top_offset) ||
         !nvenc_h264_bw_append_ue(&bs, enc->seqParams.frame_crop_bottom_offset))) {
        nvenc_h264_bw_discard(&bs);
        return false;
    }

    if (!nvenc_h264_bw_append_bool(&bs, have_vui)) {
        nvenc_h264_bw_discard(&bs);
        return false;
    }
    if (have_vui) {
        if (!nvenc_h264_bw_append_bool(&bs, aspect_ratio_info_present_flag) ||
            (aspect_ratio_info_present_flag &&
             (!nvenc_h264_bw_append_bits(&bs, 8u, enc->seqParams.aspect_ratio_idc) ||
              (enc->seqParams.aspect_ratio_idc == 255u &&
               (!nvenc_h264_bw_append_bits(&bs, 16u, enc->seqParams.sar_width) ||
                !nvenc_h264_bw_append_bits(&bs, 16u, enc->seqParams.sar_height))))) ||
            !nvenc_h264_bw_append_bool(&bs, false) ||
            !nvenc_h264_bw_append_bool(&bs, false) ||
            !nvenc_h264_bw_append_bool(&bs, false) ||
            !nvenc_h264_bw_append_bool(&bs, timing_info_present_flag)) {
            nvenc_h264_bw_discard(&bs);
            return false;
        }

        if (timing_info_present_flag &&
            (!nvenc_h264_bw_append_bits(&bs, 32u,
                                        enc->seqParams.num_units_in_tick ?
                                        enc->seqParams.num_units_in_tick : 1u) ||
             !nvenc_h264_bw_append_bits(&bs, 32u,
                                        enc->seqParams.time_scale ?
                                        enc->seqParams.time_scale : 60u) ||
             !nvenc_h264_bw_append_bool(&bs, fixed_frame_rate_flag))) {
            nvenc_h264_bw_discard(&bs);
            return false;
        }

        if (!nvenc_h264_bw_append_bool(&bs, nal_hrd_parameters_present_flag)) {
            nvenc_h264_bw_discard(&bs);
            return false;
        }
        if (nal_hrd_parameters_present_flag &&
            (!nvenc_h264_bw_append_ue(&bs, 0u) ||
             !nvenc_h264_bw_append_bits(&bs, 4u, bit_rate_scale) ||
             !nvenc_h264_bw_append_bits(&bs, 4u, cpb_size_scale) ||
             !nvenc_h264_bw_append_ue(&bs, bit_rate_value_minus1) ||
             !nvenc_h264_bw_append_ue(&bs, cpb_size_value_minus1) ||
             !nvenc_h264_bw_append_bool(&bs, enc->rcMode == VA_RC_CBR) ||
             !nvenc_h264_bw_append_bits(&bs, 5u, 23u) ||
             !nvenc_h264_bw_append_bits(&bs, 5u, 15u) ||
             !nvenc_h264_bw_append_bits(&bs, 5u, 5u) ||
             !nvenc_h264_bw_append_bits(&bs, 5u, 24u))) {
            nvenc_h264_bw_discard(&bs);
            return false;
        }

        if (!nvenc_h264_bw_append_bool(&bs, false) ||
            (nal_hrd_parameters_present_flag &&
             !nvenc_h264_bw_append_bool(&bs, low_delay_hrd_flag)) ||
            !nvenc_h264_bw_append_bool(&bs, pic_struct_present_flag) ||
            !nvenc_h264_bw_append_bool(&bs, bitstream_restriction_flag)) {
            nvenc_h264_bw_discard(&bs);
            return false;
        }

        if (bitstream_restriction_flag &&
            (!nvenc_h264_bw_append_bool(&bs, motion_vectors_over_pic_boundaries_flag) ||
             !nvenc_h264_bw_append_ue(&bs, max_bytes_per_pic_denom) ||
             !nvenc_h264_bw_append_ue(&bs, max_bits_per_mb_denom) ||
             !nvenc_h264_bw_append_ue(&bs, log2_max_mv_length_horizontal) ||
             !nvenc_h264_bw_append_ue(&bs, log2_max_mv_length_vertical) ||
             !nvenc_h264_bw_append_ue(&bs, max_num_reorder_frames) ||
             !nvenc_h264_bw_append_ue(&bs, max_dec_frame_buffering))) {
            nvenc_h264_bw_discard(&bs);
            return false;
        }
    }

    if (!nvenc_h264_bw_finish_nalu(&bs) ||
        !nvenc_h264_bw_detach(&bs, out)) {
        nvenc_h264_bw_discard(&bs);
        return false;
    }
    return true;
}

static __attribute__((unused)) bool
nvenc_h264_build_fallback_pps(const NVEncodeContext *enc,
                              NVH264PackedBlob *out)
{
    NVH264BitstreamBuilder bs = { 0 };
    int32_t pic_init_qp_minus26 = 0;

    if (!enc || !out) {
        return false;
    }

    bs.bits_left_in_reg = 64u;
    pic_init_qp_minus26 = (int32_t)enc->picParams.pic_init_qp - 26;

    if (!nvenc_h264_bw_begin_nalu(&bs, 8u, 3u) ||
        !nvenc_h264_bw_append_ue(&bs, enc->picParams.pic_parameter_set_id) ||
        !nvenc_h264_bw_append_ue(&bs, enc->picParams.seq_parameter_set_id) ||
        !nvenc_h264_bw_append_bool(&bs, enc->picParams.pic_fields.bits.entropy_coding_mode_flag) ||
        !nvenc_h264_bw_append_bool(&bs, enc->picParams.pic_fields.bits.pic_order_present_flag) ||
        !nvenc_h264_bw_append_ue(&bs, 0u) ||
        !nvenc_h264_bw_append_ue(&bs, enc->picParams.num_ref_idx_l0_active_minus1) ||
        !nvenc_h264_bw_append_ue(&bs, enc->picParams.num_ref_idx_l1_active_minus1) ||
        !nvenc_h264_bw_append_bool(&bs, enc->picParams.pic_fields.bits.weighted_pred_flag) ||
        !nvenc_h264_bw_append_bits(&bs, 2u, enc->picParams.pic_fields.bits.weighted_bipred_idc) ||
        !nvenc_h264_bw_append_se(&bs, pic_init_qp_minus26) ||
        !nvenc_h264_bw_append_se(&bs, 0) ||
        !nvenc_h264_bw_append_se(&bs, enc->picParams.chroma_qp_index_offset) ||
        !nvenc_h264_bw_append_bool(&bs, enc->picParams.pic_fields.bits.deblocking_filter_control_present_flag) ||
        !nvenc_h264_bw_append_bool(&bs, enc->picParams.pic_fields.bits.constrained_intra_pred_flag) ||
        !nvenc_h264_bw_append_bool(&bs, enc->picParams.pic_fields.bits.redundant_pic_cnt_present_flag) ||
        !nvenc_h264_bw_append_bool(&bs, enc->picParams.pic_fields.bits.transform_8x8_mode_flag) ||
        !nvenc_h264_bw_append_bool(&bs, false) ||
        !nvenc_h264_bw_append_se(&bs, enc->picParams.second_chroma_qp_index_offset) ||
        !nvenc_h264_bw_finish_nalu(&bs) ||
        !nvenc_h264_bw_detach(&bs, out)) {
        nvenc_h264_bw_discard(&bs);
        return false;
    }

    return true;
}

static __attribute__((unused)) bool
nvenc_h264_build_fallback_slice(const NVEncodeContext *enc,
                                NVH264PackedBlob *out)
{
    NVH264BitstreamBuilder bs = { 0 };
    bool is_idr = false;
    uint8_t nal_ref_idc = 0u;
    uint8_t nal_type = 1u;
    uint32_t slice_type = 0u;
    uint32_t frame_num_bits = 0u;
    uint32_t poc_bits = 0u;

    if (!enc || !out) {
        return false;
    }

    bs.bits_left_in_reg = 64u;
    is_idr = enc->picParams.pic_fields.bits.idr_pic_flag != 0u;
    slice_type = enc->sliceParams.slice_type % 5u;
    nal_ref_idc = is_idr ? 3u :
                  (enc->picParams.pic_fields.bits.reference_pic_flag ? 1u : 0u);
    nal_type = is_idr ? 5u : 1u;
    frame_num_bits = enc->seqParams.seq_fields.bits.log2_max_frame_num_minus4 + 4u;
    poc_bits = enc->seqParams.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4u;

    if (!nvenc_h264_bw_begin_nalu(&bs, nal_type, nal_ref_idc) ||
        !nvenc_h264_bw_append_ue(&bs, enc->sliceParams.macroblock_address) ||
        !nvenc_h264_bw_append_ue(&bs, enc->sliceParams.slice_type) ||
        !nvenc_h264_bw_append_ue(&bs, enc->sliceParams.pic_parameter_set_id) ||
        !nvenc_h264_bw_append_bits(&bs, frame_num_bits, enc->picParams.frame_num)) {
        nvenc_h264_bw_discard(&bs);
        return false;
    }

    if (is_idr &&
        !nvenc_h264_bw_append_ue(&bs, enc->sliceParams.idr_pic_id)) {
        nvenc_h264_bw_discard(&bs);
        return false;
    }
    if (enc->seqParams.seq_fields.bits.pic_order_cnt_type == 0u &&
        !nvenc_h264_bw_append_bits(&bs, poc_bits, enc->sliceParams.pic_order_cnt_lsb)) {
        nvenc_h264_bw_discard(&bs);
        return false;
    }

    if (slice_type == 0u) {
        if (!nvenc_h264_bw_append_bool(&bs, enc->sliceParams.num_ref_idx_active_override_flag) ||
            (enc->sliceParams.num_ref_idx_active_override_flag &&
             !nvenc_h264_bw_append_ue(&bs, enc->sliceParams.num_ref_idx_l0_active_minus1))) {
            nvenc_h264_bw_discard(&bs);
            return false;
        }
    }

    if (slice_type != 2u &&
        (!nvenc_h264_bw_append_bool(&bs, false))) {
        nvenc_h264_bw_discard(&bs);
        return false;
    }

    if (nal_ref_idc != 0u) {
        if (is_idr) {
            if (!nvenc_h264_bw_append_bool(&bs, false) ||
                !nvenc_h264_bw_append_bool(&bs, false)) {
                nvenc_h264_bw_discard(&bs);
                return false;
            }
        } else if (!nvenc_h264_bw_append_bool(&bs, false)) {
            nvenc_h264_bw_discard(&bs);
            return false;
        }
    }

    if (enc->picParams.pic_fields.bits.entropy_coding_mode_flag &&
        slice_type != 2u &&
        !nvenc_h264_bw_append_ue(&bs, enc->sliceParams.cabac_init_idc)) {
        nvenc_h264_bw_discard(&bs);
        return false;
    }

    if (!nvenc_h264_bw_append_se(&bs, enc->sliceParams.slice_qp_delta)) {
        nvenc_h264_bw_discard(&bs);
        return false;
    }
    if (enc->picParams.pic_fields.bits.deblocking_filter_control_present_flag &&
        (!nvenc_h264_bw_append_ue(&bs, enc->sliceParams.disable_deblocking_filter_idc) ||
         (enc->sliceParams.disable_deblocking_filter_idc != 1u &&
          (!nvenc_h264_bw_append_se(&bs, enc->sliceParams.slice_alpha_c0_offset_div2) ||
           !nvenc_h264_bw_append_se(&bs, enc->sliceParams.slice_beta_offset_div2))))) {
        nvenc_h264_bw_discard(&bs);
        return false;
    }

    if (!nvenc_h264_bw_detach(&bs, out)) {
        nvenc_h264_bw_discard(&bs);
        return false;
    }
    return true;
}

bool nvenc_build_roi_map(NVEncodeContext *enc,
                         const VAEncMiscParameterBufferROI *roi_param,
                         int8_t **out_map,
                         uint32_t *out_size,
                         NV_ENC_QP_MAP_MODE *out_mode)
{
    if (!enc || !roi_param || !out_map || !out_size || !out_mode) {
        return false;
    }
    if (roi_param->num_roi == 0 || roi_param->roi == NULL) {
        return false;
    }
    if (!nvenc_query_encode_caps(enc->drv) || enc->drv->nvencCaps.supportEmphasisMap <= 0) {
        LOG("ROI requested but NVENC caps do not support QP/emphasis map");
        return false;
    }

    bool is_cqp = (enc->rcMode == VA_RC_CQP);
    bool use_delta = is_cqp || roi_param->roi_flags.bits.roi_value_is_qp_delta;
    if (!use_delta) {
        LOG("ROI priority mode requested but not supported (qp delta only)");
        return false;
    }

    uint32_t mb_w = nvenc_mb_width(enc->width);
    uint32_t mb_h = nvenc_mb_height(enc->height);
    if (mb_w == 0 || mb_h == 0) {
        return false;
    }
    uint32_t map_size = mb_w * mb_h;
    int8_t *map = (int8_t *)calloc(map_size, sizeof(int8_t));
    if (!map) {
        return false;
    }
    uint8_t *mask = (uint8_t *)calloc(map_size, 1);
    if (!mask) {
        free(map);
        return false;
    }

    int min_delta = roi_param->min_delta_qp;
    int max_delta = roi_param->max_delta_qp;
    if (!is_cqp && min_delta > max_delta) {
        int tmp = min_delta;
        min_delta = max_delta;
        max_delta = tmp;
    }
    bool clamp_minmax = !is_cqp && (min_delta != 0 || max_delta != 0);

    uint32_t count = roi_param->num_roi;
    if (count > NVENC_H264_MAX_ROI_REGIONS) {
        count = NVENC_H264_MAX_ROI_REGIONS;
    }

    for (uint32_t i = 0; i < count; i++) {
        VAEncROI roi = roi_param->roi[i];
        int32_t x0 = roi.roi_rectangle.x;
        int32_t y0 = roi.roi_rectangle.y;
        int32_t x1 = x0 + (int32_t)roi.roi_rectangle.width;
        int32_t y1 = y0 + (int32_t)roi.roi_rectangle.height;
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int32_t)enc->width) x1 = (int32_t)enc->width;
        if (y1 > (int32_t)enc->height) y1 = (int32_t)enc->height;
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }

        uint32_t mb_x0 = (uint32_t)x0 / 16u;
        uint32_t mb_y0 = (uint32_t)y0 / 16u;
        uint32_t mb_x1 = ((uint32_t)x1 + 15u) / 16u;
        uint32_t mb_y1 = ((uint32_t)y1 + 15u) / 16u;
        if (mb_x1 > mb_w) mb_x1 = mb_w;
        if (mb_y1 > mb_h) mb_y1 = mb_h;
        if (mb_x1 <= mb_x0 || mb_y1 <= mb_y0) {
            continue;
        }

        int val = roi.roi_value;
        if (clamp_minmax) {
            if (val < min_delta) val = min_delta;
            if (val > max_delta) val = max_delta;
        }
        if (val < -51) val = -51;
        if (val > 51) val = 51;

        for (uint32_t y = mb_y0; y < mb_y1; y++) {
            uint32_t row = y * mb_w;
            for (uint32_t x = mb_x0; x < mb_x1; x++) {
                uint32_t idx = row + x;
                if (mask[idx]) {
                    continue;
                }
                map[idx] = (int8_t)val;
                mask[idx] = 1;
            }
        }
    }
    free(mask);
    *out_map = map;
    *out_size = map_size;
    *out_mode = NV_ENC_QP_MAP_DELTA;
    return true;
}

bool nvenc_compute_slice_config(NVEncodeContext *enc, uint32_t *mode, uint32_t *data)
{
    *mode = 0;
    *data = 0;

    if (!nvenc_query_encode_caps(enc->drv) || enc->drv->nvencCaps.supportDynamicSliceMode <= 0) {
        if (enc->sliceCount > 1) {
            LOG("Slices requested but NVENC caps do not support dynamic slice mode");
        }
        return false;
    }

    if (enc->sliceCount <= 1) {
        *mode = 3; // num slices in picture
        *data = 1;
        return true;
    }

    if (enc->sliceRowsValid && enc->sliceRows > 0) {
        *mode = 2; // MB row based slices
        uint32_t mb_rows = nvenc_mb_height(enc->height);
        uint32_t rows = enc->sliceRows;
        if (rows > mb_rows) {
            rows = mb_rows;
        }
        *data = rows > 0 ? rows : 1;
    } else {
        *mode = 3; // num slices in picture
        uint32_t count = enc->sliceCount;
        if (count > NVENC_H264_MAX_SLICES) {
            count = NVENC_H264_MAX_SLICES;
        }
        *data = count > 0 ? count : 1;
    }

    return true;
}

void nvenc_update_slice_reconfigure(NVEncodeContext *enc)
{
    if (!enc->initialized) {
        return;
    }

    uint32_t mode = 0;
    uint32_t data = 0;
    (void)nvenc_compute_slice_config(enc, &mode, &data);

    if (!enc->sliceModeConfiguredValid ||
        enc->sliceModeConfigured != mode ||
        enc->sliceModeDataConfigured != data) {
        uint32_t bps = nvenc_reconfig_bps_hint(enc, enc->rcParams.bits_per_second);
        nvenc_force_reconfigure(enc, NVENC_RECONFIGURE_REASON_SLICE_MODE, bps);
    }
}

static size_t nvenc_find_start_code(const uint8_t *data, size_t size, size_t offset, size_t *sc_len)
{
    for (size_t i = offset; i + 3 <= size; i++) {
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            if (data[i + 2] == 0x01) {
                *sc_len = 3;
                return i;
            }
            if (i + 4 <= size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
                *sc_len = 4;
                return i;
            }
        }
    }
    return SIZE_MAX;
}

typedef struct {
    const uint8_t *rbsp;
    size_t rbsp_len;
    size_t bit_pos;
} NVBitReader;

static bool nvenc_br_has_bits(const NVBitReader *br, size_t bits)
{
    return br && (br->bit_pos + bits <= br->rbsp_len * 8u);
}

static bool nvenc_br_read_bits(NVBitReader *br, uint32_t bits, uint32_t *out)
{
    if (!nvenc_br_has_bits(br, bits) || bits > 32) {
        return false;
    }
    uint32_t val = 0;
    for (uint32_t i = 0; i < bits; i++) {
        size_t bit_index = br->bit_pos++;
        uint8_t byte = br->rbsp[bit_index >> 3];
        uint8_t bit = (byte >> (7u - (bit_index & 7u))) & 0x1u;
        val = (val << 1) | bit;
    }
    if (out) {
        *out = val;
    }
    return true;
}

static bool nvenc_br_read_ue(NVBitReader *br, uint32_t *out)
{
    uint32_t zeros = 0;
    uint32_t bit = 0;
    while (true) {
        if (!nvenc_br_read_bits(br, 1, &bit)) {
            return false;
        }
        if (bit) {
            break;
        }
        zeros++;
        if (zeros > 31) {
            return false;
        }
    }
    uint32_t info = 0;
    if (zeros > 0) {
        if (!nvenc_br_read_bits(br, zeros, &info)) {
            return false;
        }
    }
    if (out) {
        *out = ((1u << zeros) - 1u) + info;
    }
    return true;
}

static bool nvenc_br_read_se(NVBitReader *br, int32_t *out)
{
    uint32_t ue = 0;
    if (!nvenc_br_read_ue(br, &ue)) {
        return false;
    }
    int32_t val = (ue & 1u) ? (int32_t)((ue + 1u) / 2u) : -(int32_t)(ue / 2u);
    if (out) {
        *out = val;
    }
    return true;
}

static bool nvenc_br_skip_scaling_list(NVBitReader *br, int size)
{
    int last_scale = 8;
    int next_scale = 8;
    for (int i = 0; i < size; i++) {
        if (next_scale != 0) {
            int32_t delta_scale = 0;
            if (!nvenc_br_read_se(br, &delta_scale)) {
                return false;
            }
            next_scale = (last_scale + delta_scale + 256) % 256;
        }
        last_scale = (next_scale == 0) ? last_scale : next_scale;
    }
    return true;
}

static bool nvenc_build_rbsp(const uint8_t *ebsp, size_t ebsp_len,
                             uint8_t **out_rbsp, size_t *out_rbsp_len,
                             uint32_t **out_map)
{
    if (!ebsp || ebsp_len == 0 || !out_rbsp || !out_rbsp_len || !out_map) {
        return false;
    }
    uint8_t *rbsp = (uint8_t*) calloc(1, ebsp_len);
    uint32_t *map = (uint32_t*) calloc(ebsp_len, sizeof(uint32_t));
    if (!rbsp || !map) {
        free(rbsp);
        free(map);
        return false;
    }
    size_t rbsp_len = 0;
    int zero_count = 0;
    for (size_t i = 0; i < ebsp_len; i++) {
        uint8_t b = ebsp[i];
        if (zero_count >= 2 && b == 0x03) {
            zero_count = 0;
            continue;
        }
        rbsp[rbsp_len] = b;
        map[rbsp_len] = (uint32_t)i;
        rbsp_len++;
        if (b == 0x00) {
            zero_count++;
        } else {
            zero_count = 0;
        }
    }
    *out_rbsp = rbsp;
    *out_rbsp_len = rbsp_len;
    *out_map = map;
    return true;
}

static bool nvenc_patch_rbsp_bits(uint8_t *ebsp, const uint32_t *map, size_t rbsp_len,
                                  size_t start_bit, uint32_t bit_len, uint32_t value)
{
    if (!ebsp || !map || bit_len == 0) {
        return false;
    }
    if (start_bit + bit_len > rbsp_len * 8u) {
        return false;
    }
    for (uint32_t i = 0; i < bit_len; i++) {
        size_t bit_pos = start_bit + i;
        size_t rbsp_byte = bit_pos >> 3;
        uint8_t mask = (uint8_t)(1u << (7u - (bit_pos & 7u)));
        uint8_t bit = (uint8_t)((value >> (bit_len - 1u - i)) & 0x1u);
        uint32_t ebsp_idx = map[rbsp_byte];
        if (bit) {
            ebsp[ebsp_idx] |= mask;
        } else {
            ebsp[ebsp_idx] &= (uint8_t)~mask;
        }
    }
    return true;
}

static uint64_t nvenc_round_div_u128(__int128 num, uint64_t den)
{
    if (den == 0) {
        return 0;
    }
    if (num < 0) {
        return 0;
    }
    return (uint64_t)((num + (den / 2u)) / den);
}

static uint64_t nvenc_ceil_div_u128(__int128 num, uint64_t den)
{
    if (den == 0) {
        return 0;
    }
    if (num <= 0) {
        return 0;
    }
    return (uint64_t)((num + (den - 1u)) / den);
}

static bool nvenc_parse_hrd(NVBitReader *br, uint8_t *out_delay_len, uint8_t *out_cpb_cnt_minus1)
{
    uint32_t cpb_cnt_minus1 = 0;
    if (!nvenc_br_read_ue(br, &cpb_cnt_minus1)) {
        return false;
    }
    uint32_t tmp = 0;
    if (!nvenc_br_read_bits(br, 4, &tmp)) {
        return false;
    }
    if (!nvenc_br_read_bits(br, 4, &tmp)) {
        return false;
    }
    for (uint32_t i = 0; i <= cpb_cnt_minus1; i++) {
        if (!nvenc_br_read_ue(br, &tmp)) {
            return false;
        }
        if (!nvenc_br_read_ue(br, &tmp)) {
            return false;
        }
        if (!nvenc_br_read_bits(br, 1, &tmp)) {
            return false;
        }
    }
    if (!nvenc_br_read_bits(br, 5, &tmp)) {
        return false;
    }
    if (out_delay_len) {
        *out_delay_len = (uint8_t)(tmp + 1);
    }
    if (!nvenc_br_read_bits(br, 5, &tmp)) {
        return false;
    }
    if (!nvenc_br_read_bits(br, 5, &tmp)) {
        return false;
    }
    if (!nvenc_br_read_bits(br, 5, &tmp)) {
        return false;
    }
    if (out_cpb_cnt_minus1) {
        *out_cpb_cnt_minus1 = (uint8_t)MIN(cpb_cnt_minus1, 255u);
    }
    return true;
}

static bool nvenc_parse_h264_sps_hrd(NVEncodeContext *enc, const uint8_t *ebsp, size_t ebsp_len)
{
    uint8_t *rbsp = NULL;
    uint32_t *map = NULL;
    size_t rbsp_len = 0;
    if (!enc || !nvenc_build_rbsp(ebsp, ebsp_len, &rbsp, &rbsp_len, &map)) {
        return false;
    }

    NVBitReader br = { rbsp, rbsp_len, 0 };
    uint32_t profile_idc = 0;
    uint32_t tmp = 0;
    bool have_hrd = false;
    uint8_t delay_len = 0;
    uint8_t cpb_cnt_minus1 = 0;

    if (!nvenc_br_read_bits(&br, 8, &profile_idc) ||
        !nvenc_br_read_bits(&br, 8, &tmp) ||
        !nvenc_br_read_bits(&br, 8, &tmp) ||
        !nvenc_br_read_ue(&br, &tmp)) {
        goto done;
    }

    switch (profile_idc) {
    case 100:
    case 110:
    case 122:
    case 244:
    case 44:
    case 83:
    case 86:
    case 118:
    case 128:
    case 138:
    case 139:
    case 134:
    case 135: {
        uint32_t chroma_format_idc = 0;
        if (!nvenc_br_read_ue(&br, &chroma_format_idc)) {
            goto done;
        }
        if (chroma_format_idc == 3) {
            if (!nvenc_br_read_bits(&br, 1, &tmp)) {
                goto done;
            }
        }
        if (!nvenc_br_read_ue(&br, &tmp) ||
            !nvenc_br_read_ue(&br, &tmp) ||
            !nvenc_br_read_bits(&br, 1, &tmp)) {
            goto done;
        }
        uint32_t seq_scaling_matrix_present_flag = 0;
        if (!nvenc_br_read_bits(&br, 1, &seq_scaling_matrix_present_flag)) {
            goto done;
        }
        if (seq_scaling_matrix_present_flag) {
            int scaling_list_count = (chroma_format_idc == 3) ? 12 : 8;
            for (int i = 0; i < scaling_list_count; i++) {
                uint32_t list_present = 0;
                if (!nvenc_br_read_bits(&br, 1, &list_present)) {
                    goto done;
                }
                if (list_present) {
                    int size = (i < 6) ? 16 : 64;
                    if (!nvenc_br_skip_scaling_list(&br, size)) {
                        goto done;
                    }
                }
            }
        }
        break;
    }
    default:
        break;
    }

    if (!nvenc_br_read_ue(&br, &tmp)) {
        goto done;
    }
    uint32_t pic_order_cnt_type = 0;
    if (!nvenc_br_read_ue(&br, &pic_order_cnt_type)) {
        goto done;
    }
    if (pic_order_cnt_type == 0) {
        if (!nvenc_br_read_ue(&br, &tmp)) {
            goto done;
        }
    } else if (pic_order_cnt_type == 1) {
        if (!nvenc_br_read_bits(&br, 1, &tmp)) {
            goto done;
        }
        int32_t se = 0;
        if (!nvenc_br_read_se(&br, &se) ||
            !nvenc_br_read_se(&br, &se)) {
            goto done;
        }
        uint32_t num_ref_frames_in_pic_order_cnt_cycle = 0;
        if (!nvenc_br_read_ue(&br, &num_ref_frames_in_pic_order_cnt_cycle)) {
            goto done;
        }
        for (uint32_t i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; i++) {
            if (!nvenc_br_read_se(&br, &se)) {
                goto done;
            }
        }
    }

    if (!nvenc_br_read_ue(&br, &tmp) ||
        !nvenc_br_read_bits(&br, 1, &tmp) ||
        !nvenc_br_read_ue(&br, &tmp) ||
        !nvenc_br_read_ue(&br, &tmp)) {
        goto done;
    }
    uint32_t frame_mbs_only_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &frame_mbs_only_flag)) {
        goto done;
    }
    if (!frame_mbs_only_flag) {
        if (!nvenc_br_read_bits(&br, 1, &tmp)) {
            goto done;
        }
    }
    if (!nvenc_br_read_bits(&br, 1, &tmp)) {
        goto done;
    }
    uint32_t frame_cropping_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &frame_cropping_flag)) {
        goto done;
    }
    if (frame_cropping_flag) {
        if (!nvenc_br_read_ue(&br, &tmp) ||
            !nvenc_br_read_ue(&br, &tmp) ||
            !nvenc_br_read_ue(&br, &tmp) ||
            !nvenc_br_read_ue(&br, &tmp)) {
            goto done;
        }
    }
    uint32_t vui_parameters_present_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &vui_parameters_present_flag)) {
        goto done;
    }
    if (!vui_parameters_present_flag) {
        goto done;
    }

    uint32_t aspect_ratio_info_present_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &aspect_ratio_info_present_flag)) {
        goto done;
    }
    if (aspect_ratio_info_present_flag) {
        uint32_t aspect_ratio_idc = 0;
        if (!nvenc_br_read_bits(&br, 8, &aspect_ratio_idc)) {
            goto done;
        }
        if (aspect_ratio_idc == 255) {
            if (!nvenc_br_read_bits(&br, 16, &tmp) ||
                !nvenc_br_read_bits(&br, 16, &tmp)) {
                goto done;
            }
        }
    }
    uint32_t overscan_info_present_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &overscan_info_present_flag)) {
        goto done;
    }
    if (overscan_info_present_flag) {
        if (!nvenc_br_read_bits(&br, 1, &tmp)) {
            goto done;
        }
    }
    uint32_t video_signal_type_present_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &video_signal_type_present_flag)) {
        goto done;
    }
    if (video_signal_type_present_flag) {
        if (!nvenc_br_read_bits(&br, 3, &tmp) ||
            !nvenc_br_read_bits(&br, 1, &tmp)) {
            goto done;
        }
        uint32_t colour_description_present_flag = 0;
        if (!nvenc_br_read_bits(&br, 1, &colour_description_present_flag)) {
            goto done;
        }
        if (colour_description_present_flag) {
            if (!nvenc_br_read_bits(&br, 8, &tmp) ||
                !nvenc_br_read_bits(&br, 8, &tmp) ||
                !nvenc_br_read_bits(&br, 8, &tmp)) {
                goto done;
            }
        }
    }
    uint32_t chroma_loc_info_present_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &chroma_loc_info_present_flag)) {
        goto done;
    }
    if (chroma_loc_info_present_flag) {
        if (!nvenc_br_read_ue(&br, &tmp) ||
            !nvenc_br_read_ue(&br, &tmp)) {
            goto done;
        }
    }
    uint32_t timing_info_present_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &timing_info_present_flag)) {
        goto done;
    }
    if (timing_info_present_flag) {
        if (!nvenc_br_read_bits(&br, 32, &tmp) ||
            !nvenc_br_read_bits(&br, 32, &tmp) ||
            !nvenc_br_read_bits(&br, 1, &tmp)) {
            goto done;
        }
    }
    uint32_t nal_hrd_parameters_present_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &nal_hrd_parameters_present_flag)) {
        goto done;
    }
    if (nal_hrd_parameters_present_flag) {
        if (!nvenc_parse_hrd(&br, &delay_len, &cpb_cnt_minus1)) {
            goto done;
        }
        have_hrd = true;
    }
    uint32_t vcl_hrd_parameters_present_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &vcl_hrd_parameters_present_flag)) {
        goto done;
    }
    if (vcl_hrd_parameters_present_flag) {
        uint8_t vcl_delay_len = 0;
        uint8_t vcl_cpb_cnt_minus1 = 0;
        if (!nvenc_parse_hrd(&br, &vcl_delay_len, &vcl_cpb_cnt_minus1)) {
            goto done;
        }
        if (!have_hrd) {
            delay_len = vcl_delay_len;
            cpb_cnt_minus1 = vcl_cpb_cnt_minus1;
            have_hrd = true;
        }
    }
    if (nal_hrd_parameters_present_flag || vcl_hrd_parameters_present_flag) {
        if (!nvenc_br_read_bits(&br, 1, &tmp)) {
            goto done;
        }
    }

    if (have_hrd) {
        enc->bpDelayLen = delay_len;
        enc->bpCpbCntMinus1 = cpb_cnt_minus1;
        enc->bpHaveParams = true;
    }

done:
    free(rbsp);
    free(map);
    return have_hrd;
}

static bool nvenc_patch_h264_bp_payload(NVEncodeContext *enc, uint8_t *ebsp,
                                        const uint8_t *rbsp, size_t rbsp_len, const uint32_t *map,
                                        size_t payload_start_bit, size_t payload_bits,
                                        uint64_t bits_since, uint32_t bit_rate, uint64_t gop_ticks)
{
    if (!enc || !ebsp || !rbsp || !map) {
        return false;
    }
    if (payload_start_bit + payload_bits > rbsp_len * 8u) {
        return false;
    }
    NVBitReader br = { rbsp, rbsp_len, payload_start_bit };
    uint32_t sps_id = 0;
    if (!nvenc_br_read_ue(&br, &sps_id)) {
        return false;
    }
    (void)sps_id;
    size_t consumed = br.bit_pos - payload_start_bit;
    if (payload_bits < consumed) {
        return false;
    }
    size_t remaining = payload_bits - consumed;
    if (enc->bpDelayLen == 0) {
        return false;
    }
    size_t per_set_bits = (size_t)(enc->bpCpbCntMinus1 + 1u) * 2u * enc->bpDelayLen;
    if (per_set_bits == 0 || remaining < per_set_bits) {
        return false;
    }
    size_t sets = remaining / per_set_bits;
    if (sets == 0) {
        return false;
    }
    if (sets > 2) {
        sets = 2;
    }

    bool patching = enc->bpHavePrev;
    uint32_t next_delay = 0;
    uint32_t next_offset = 0;
    if (patching) {
        uint64_t delta_ticks = nvenc_ceil_div_u128((__int128)bits_since * 90000u, bit_rate);
        int64_t next = (int64_t)enc->bpPrevDelay + (int64_t)gop_ticks - (int64_t)delta_ticks;
        if (next < 1) {
            next_delay = 1;
            next_offset = enc->bpCpbTicks;
        } else {
            if (enc->bpCpbTicks > 0 && next > (int64_t)enc->bpCpbTicks) {
                next = enc->bpCpbTicks;
            }
            next_delay = (uint32_t)next;
            if (enc->bpCpbTicks > next_delay) {
                next_offset = enc->bpCpbTicks - next_delay - 1;
            } else {
                next_offset = 0;
            }
        }
    }

    uint32_t first_delay = 0;
    bool captured_first = false;
    for (size_t set = 0; set < sets; set++) {
        for (uint32_t i = 0; i <= enc->bpCpbCntMinus1; i++) {
            size_t delay_start = br.bit_pos;
            uint32_t delay_val = 0;
            if (!nvenc_br_read_bits(&br, enc->bpDelayLen, &delay_val)) {
                return false;
            }
            size_t offset_start = br.bit_pos;
            uint32_t offset_val = 0;
            if (!nvenc_br_read_bits(&br, enc->bpDelayLen, &offset_val)) {
                return false;
            }
            (void)offset_val;
            if (!captured_first) {
                first_delay = delay_val;
                captured_first = true;
            }
            if (patching) {
                if (!nvenc_patch_rbsp_bits(ebsp, map, rbsp_len, delay_start, enc->bpDelayLen, next_delay)) {
                    return false;
                }
                if (!nvenc_patch_rbsp_bits(ebsp, map, rbsp_len, offset_start, enc->bpDelayLen, next_offset)) {
                    return false;
                }
            }
        }
    }

    if (!enc->bpHavePrev && captured_first) {
        enc->bpPrevDelay = first_delay;
        enc->bpHavePrev = true;
        enc->bpSeen++;
        return false;
    }
    if (patching) {
        enc->bpPrevDelay = next_delay;
        enc->bpSeen++;
        return true;
    }
    return false;
}

bool nvenc_patch_h264_bp_sei(NVEncodeContext *enc, uint8_t *data, size_t size, uint64_t bits_since)
{
    if (!enc || !data || size == 0) {
        return false;
    }
    if (!enc->patchBpSei || !isH264EncodeProfile(enc->profile)) {
        return false;
    }
    if (enc->encConfig.rcParams.rateControlMode != NV_ENC_PARAMS_RC_CONSTQP &&
        enc->encConfig.rcParams.rateControlMode != NV_ENC_PARAMS_RC_CBR) {
        return false;
    }
    if (enc->encConfig.gopLength == 0 || enc->encConfig.gopLength == NVENC_INFINITE_GOPLENGTH) {
        return false;
    }

    uint32_t bit_rate = enc->encConfig.rcParams.averageBitRate;
    if (bit_rate == 0) {
        return false;
    }
    uint32_t vbv = enc->encConfig.rcParams.vbvBufferSize;
    if (vbv == 0) {
        return false;
    }
    uint64_t cpb_ticks = nvenc_round_div_u128((__int128)vbv * 90000u, bit_rate);
    enc->bpCpbTicks = (uint32_t)MIN(cpb_ticks, (uint64_t)UINT32_MAX);

    uint32_t fr_num = 0;
    uint32_t fr_den = 0;
    nvenc_get_framerate(enc, &fr_num, &fr_den);
    if (fr_num == 0) {
        return false;
    }
    uint64_t frame_ticks = nvenc_round_div_u128((__int128)90000u * fr_den, fr_num);
    uint64_t gop_ticks = frame_ticks * (uint64_t)enc->encConfig.gopLength;

    size_t sc_len = 0;
    size_t sc = nvenc_find_start_code(data, size, 0, &sc_len);
    if (sc == SIZE_MAX) {
        if (size > 1) {
            uint8_t nal_type = data[0] & 0x1f;
            if (nal_type == 7) {
                nvenc_parse_h264_sps_hrd(enc, data + 1, size - 1);
            }
        }
    } else {
        while (sc != SIZE_MAX) {
            size_t next_sc_len = 0;
            size_t nal_start = sc + sc_len;
            size_t next_sc = nvenc_find_start_code(data, size, nal_start, &next_sc_len);
            size_t nal_end = (next_sc == SIZE_MAX) ? size : next_sc;
            if (nal_start + 1 <= nal_end && nal_start < size) {
                uint8_t nal_type = data[nal_start] & 0x1f;
                if (nal_type == 7) {
                    size_t payload_len = (nal_end > (nal_start + 1)) ? (nal_end - (nal_start + 1)) : 0;
                    if (payload_len > 0) {
                        nvenc_parse_h264_sps_hrd(enc, data + nal_start + 1, payload_len);
                    }
                }
            }
            sc = next_sc;
            sc_len = next_sc_len;
        }
    }

    if (!enc->bpHaveParams || enc->bpDelayLen == 0) {
        return false;
    }

    bool patched = false;
    sc = nvenc_find_start_code(data, size, 0, &sc_len);
    if (sc == SIZE_MAX) {
        if (size > 1) {
            uint8_t nal_type = data[0] & 0x1f;
            if (nal_type == 6) {
                uint8_t *ebsp = data + 1;
                size_t ebsp_len = size - 1;
                uint8_t *rbsp = NULL;
                uint32_t *map = NULL;
                size_t rbsp_len = 0;
                if (nvenc_build_rbsp(ebsp, ebsp_len, &rbsp, &rbsp_len, &map)) {
                    size_t bit_pos = 0;
                    while (bit_pos + 16 <= rbsp_len * 8u) {
                        if ((bit_pos & 7u) != 0) {
                            break;
                        }
                        size_t byte_pos = bit_pos / 8u;
                        uint32_t payload_type = 0;
                        while (byte_pos < rbsp_len && rbsp[byte_pos] == 0xff) {
                            payload_type += 255;
                            byte_pos++;
                        }
                        if (byte_pos >= rbsp_len) {
                            break;
                        }
                        payload_type += rbsp[byte_pos++];
                        uint32_t payload_size = 0;
                        while (byte_pos < rbsp_len && rbsp[byte_pos] == 0xff) {
                            payload_size += 255;
                            byte_pos++;
                        }
                        if (byte_pos >= rbsp_len) {
                            break;
                        }
                        payload_size += rbsp[byte_pos++];
                        bit_pos = byte_pos * 8u;
                        size_t payload_bits = (size_t)payload_size * 8u;
                        if (bit_pos + payload_bits > rbsp_len * 8u) {
                            break;
                        }
                        if (payload_type == 0) {
                            if (nvenc_patch_h264_bp_payload(enc, ebsp, rbsp, rbsp_len, map,
                                                            bit_pos, payload_bits,
                                                            bits_since, bit_rate, gop_ticks)) {
                                patched = true;
                            }
                        }
                        bit_pos += payload_bits;
                    }
                }
                free(rbsp);
                free(map);
            }
        }
        return patched;
    }

    while (sc != SIZE_MAX) {
        size_t next_sc_len = 0;
        size_t nal_start = sc + sc_len;
        size_t next_sc = nvenc_find_start_code(data, size, nal_start, &next_sc_len);
        size_t nal_end = (next_sc == SIZE_MAX) ? size : next_sc;
        if (nal_start + 1 <= nal_end && nal_start < size) {
            uint8_t nal_type = data[nal_start] & 0x1f;
            if (nal_type == 6) {
                size_t payload_len = (nal_end > (nal_start + 1)) ? (nal_end - (nal_start + 1)) : 0;
                if (payload_len > 0) {
                    uint8_t *ebsp = data + nal_start + 1;
                    uint8_t *rbsp = NULL;
                    uint32_t *map = NULL;
                    size_t rbsp_len = 0;
                    if (nvenc_build_rbsp(ebsp, payload_len, &rbsp, &rbsp_len, &map)) {
                        size_t bit_pos = 0;
                        while (bit_pos + 16 <= rbsp_len * 8u) {
                            if ((bit_pos & 7u) != 0) {
                                break;
                            }
                            size_t byte_pos = bit_pos / 8u;
                            uint32_t payload_type = 0;
                            while (byte_pos < rbsp_len && rbsp[byte_pos] == 0xff) {
                                payload_type += 255;
                                byte_pos++;
                            }
                            if (byte_pos >= rbsp_len) {
                                break;
                            }
                            payload_type += rbsp[byte_pos++];
                            uint32_t payload_size = 0;
                            while (byte_pos < rbsp_len && rbsp[byte_pos] == 0xff) {
                                payload_size += 255;
                                byte_pos++;
                            }
                            if (byte_pos >= rbsp_len) {
                                break;
                            }
                            payload_size += rbsp[byte_pos++];
                            bit_pos = byte_pos * 8u;
                            size_t payload_bits = (size_t)payload_size * 8u;
                            if (bit_pos + payload_bits > rbsp_len * 8u) {
                                break;
                            }
                            if (payload_type == 0) {
                                if (nvenc_patch_h264_bp_payload(enc, ebsp, rbsp, rbsp_len, map,
                                                                bit_pos, payload_bits,
                                                                bits_since, bit_rate, gop_ticks)) {
                                    patched = true;
                                }
                            }
                            bit_pos += payload_bits;
                        }
                    }
                    free(rbsp);
                    free(map);
                }
            }
        }
        sc = next_sc;
        sc_len = next_sc_len;
    }
    return patched;
}

void nvenc_reset_bp_state(NVEncodeContext *enc)
{
    if (!enc) {
        return;
    }
    enc->bpHaveParams = false;
    enc->bpHavePrev = false;
    enc->bpDelayLen = 0;
    enc->bpCpbCntMinus1 = 0;
    enc->bpPrevDelay = 0;
    enc->bpCpbTicks = 0;
    enc->bpBitsSince = 0;
    enc->bpSeen = 0;
}

static void nvenc_append_filtered_nals_from_packed(NVEncodeContext *enc,
                                                   const uint8_t *data,
                                                   size_t bytes,
                                                   int nal_type_filter)
{
    if (!enc || !data || bytes == 0) {
        return;
    }

    size_t sc_len = 0;
    size_t sc = nvenc_find_start_code(data, bytes, 0, &sc_len);
    if (sc == SIZE_MAX) {
        uint8_t nal_type = data[0] & 0x1f;
        if (nal_type_filter < 0 || nal_type == (uint8_t)nal_type_filter) {
            static const uint8_t start_code[] = { 0x00, 0x00, 0x01 };
            appendBuffer(&enc->packedHeaderBuf, start_code, sizeof(start_code));
            appendBuffer(&enc->packedHeaderBuf, data, bytes);
        }
        return;
    }

    while (sc != SIZE_MAX) {
        size_t next_sc_len = 0;
        size_t nal_start = sc + sc_len;
        size_t next_sc = nvenc_find_start_code(data, bytes, nal_start, &next_sc_len);
        size_t nal_end = (next_sc == SIZE_MAX) ? bytes : next_sc;
        if (nal_start < bytes) {
            uint8_t nal_type = data[nal_start] & 0x1f;
            if (nal_type_filter < 0 || nal_type == (uint8_t)nal_type_filter) {
                appendBuffer(&enc->packedHeaderBuf, data + sc, nal_end - sc);
            }
        }
        sc = next_sc;
        sc_len = next_sc_len;
    }
}

static void nvenc_capture_filtered_nals_from_packed(AppendableBuffer *dst,
                                                    const uint8_t *data,
                                                    size_t bytes,
                                                    int nal_type_filter)
{
    if (!dst || !data || bytes == 0) {
        return;
    }

    size_t sc_len = 0;
    size_t sc = nvenc_find_start_code(data, bytes, 0, &sc_len);
    if (sc == SIZE_MAX) {
        uint8_t nal_type = data[0] & 0x1f;
        if (nal_type_filter < 0 || nal_type == (uint8_t)nal_type_filter) {
            static const uint8_t start_code[] = { 0x00, 0x00, 0x01 };
            appendBuffer(dst, start_code, sizeof(start_code));
            appendBuffer(dst, data, bytes);
        }
        return;
    }

    while (sc != SIZE_MAX) {
        size_t next_sc_len = 0;
        size_t nal_start = sc + sc_len;
        size_t next_sc = nvenc_find_start_code(data, bytes, nal_start, &next_sc_len);
        size_t nal_end = (next_sc == SIZE_MAX) ? bytes : next_sc;
        if (nal_start < bytes) {
            uint8_t nal_type = data[nal_start] & 0x1f;
            if (nal_type_filter < 0 || nal_type == (uint8_t)nal_type_filter) {
                appendBuffer(dst, data + sc, nal_end - sc);
            }
        }
        sc = next_sc;
        sc_len = next_sc_len;
    }
}

static void nvenc_append_h264_escaped_payload(AppendableBuffer *dst,
                                              const uint8_t *payload,
                                              size_t payload_size,
                                              uint32_t *inserted_bytes)
{
    int zero_count = 0;

    for (size_t i = 0; i < payload_size; i++) {
        uint8_t b = payload[i];
        if (zero_count >= 2 && b <= 0x03) {
            static const uint8_t epb = 0x03;
            appendBuffer(dst, &epb, 1);
            if (inserted_bytes) {
                (*inserted_bytes)++;
            }
            zero_count = 0;
        }
        appendBuffer(dst, &b, 1);
        if (b == 0x00) {
            zero_count++;
        } else {
            zero_count = 0;
        }
    }
}

static void nvenc_normalize_h264_annexb_in_place(AppendableBuffer *buf,
                                                 uint32_t *bit_length)
{
    if (!buf || !buf->buf || buf->size == 0) {
        return;
    }

    const uint8_t *data = (const uint8_t *)buf->buf;
    size_t size = buf->size;
    size_t sc_len = 0;
    size_t sc = nvenc_find_start_code(data, size, 0, &sc_len);
    if (sc == SIZE_MAX) {
        return;
    }

    AppendableBuffer normalized = { 0 };
    uint32_t inserted_bytes = 0;
    while (sc != SIZE_MAX) {
        size_t next_sc_len = 0;
        size_t nal_start = sc + sc_len;
        size_t next_sc = nvenc_find_start_code(data, size, nal_start, &next_sc_len);
        size_t nal_end = (next_sc == SIZE_MAX) ? size : next_sc;

        appendBuffer(&normalized, data + sc, sc_len);
        if (nal_start < nal_end) {
            appendBuffer(&normalized, data + nal_start, 1);
            if (nal_start + 1 < nal_end) {
                nvenc_append_h264_escaped_payload(&normalized,
                                                  data + nal_start + 1,
                                                  nal_end - nal_start - 1,
                                                  &inserted_bytes);
            }
        }

        sc = next_sc;
        sc_len = next_sc_len;
    }

    free(buf->buf);
    buf->buf = normalized.buf;
    buf->size = normalized.size;
    buf->allocated = normalized.allocated;
    if (bit_length && inserted_bytes > 0) {
        *bit_length += inserted_bytes * 8u;
    }
}

void nvenc_clear_h264_sei_payloads(NV_ENC_SEI_PAYLOAD **payloads, uint32_t *count)
{
    if (!payloads || !count) {
        return;
    }
    if (*payloads) {
        for (uint32_t i = 0; i < *count; i++) {
            free((*payloads)[i].payload);
            (*payloads)[i].payload = NULL;
            (*payloads)[i].payloadSize = 0;
        }
        free(*payloads);
    }
    *payloads = NULL;
    *count = 0;
}

static bool nvenc_append_sei_payload_entry(NV_ENC_SEI_PAYLOAD **payloads,
                                           uint32_t *count,
                                           uint32_t payload_type,
                                           const uint8_t *payload,
                                           uint32_t payload_size)
{
    if (!payloads || !count) {
        return false;
    }
    NV_ENC_SEI_PAYLOAD *grown = (NV_ENC_SEI_PAYLOAD*) realloc(
        *payloads, (size_t)(*count + 1u) * sizeof(NV_ENC_SEI_PAYLOAD));
    if (!grown) {
        return false;
    }

    uint8_t *payload_copy = NULL;
    if (payload_size > 0) {
        payload_copy = (uint8_t*) malloc(payload_size);
        if (!payload_copy) {
            return false;
        }
        memcpy(payload_copy, payload, payload_size);
    }

    *payloads = grown;
    (*payloads)[*count].payloadType = payload_type;
    (*payloads)[*count].payloadSize = payload_size;
    (*payloads)[*count].payload = payload_copy;
    (*count)++;
    return true;
}

/*
 * FFmpeg's VAAPI H.264 wrapper injects this identifier SEI by default.
 * Native h264_nvenc does not emit it unless udu_sei=1, so forwarding it
 * here breaks bitstream parity for the common FFmpeg path.
 */
static const uint8_t nvenc_ffmpeg_vaapi_identifier_uuid[16] = {
    0x59, 0x94, 0x8b, 0x28, 0x11, 0xec, 0x45, 0xaf,
    0x96, 0x75, 0x19, 0xd4, 0x1f, 0xea, 0xa9, 0x4d,
};

static bool nvenc_is_h264_timing_sei_type(uint32_t payload_type)
{
    return payload_type == 0 || payload_type == 1;
}

static bool nvenc_is_ffmpeg_vaapi_identifier_sei(const uint8_t *payload,
                                                 uint32_t payload_size)
{
    return payload &&
           payload_size >= sizeof(nvenc_ffmpeg_vaapi_identifier_uuid) &&
           memcmp(payload,
                  nvenc_ffmpeg_vaapi_identifier_uuid,
                  sizeof(nvenc_ffmpeg_vaapi_identifier_uuid)) == 0;
}

static bool nvenc_should_forward_h264_sei_payload(uint32_t payload_type,
                                                  const uint8_t *payload,
                                                  uint32_t payload_size)
{
    if (nvenc_is_h264_timing_sei_type(payload_type)) {
        return false;
    }
    if (payload_type == 5 &&
        nvenc_is_ffmpeg_vaapi_identifier_sei(payload, payload_size)) {
        return false;
    }
    return true;
}

static void nvenc_update_h264_sei_stream_mask(uint32_t *stream_mask,
                                              uint32_t payload_type)
{
    if (!stream_mask) {
        return;
    }
    switch (payload_type) {
    case 0:
        *stream_mask |= NVENC_H264_SEI_MASK_BUFFERING_PERIOD;
        break;
    case 1:
        *stream_mask |= NVENC_H264_SEI_MASK_PIC_TIMING;
        break;
    case 6:
        *stream_mask |= NVENC_H264_SEI_MASK_RECOVERY_POINT;
        break;
    default:
        break;
    }
}

static void nvenc_parse_h264_sei_rbsp(NV_ENC_SEI_PAYLOAD **payloads,
                                      uint32_t *count,
                                      uint32_t *stream_mask,
                                      bool *packed_timing_seen,
                                      const uint8_t *rbsp,
                                      size_t rbsp_len)
{
    size_t byte_pos = 0;

    while (byte_pos + 1 < rbsp_len) {
        if (byte_pos + 1 == rbsp_len && rbsp[byte_pos] == 0x80) {
            break;
        }

        uint32_t payload_type = 0;
        while (byte_pos < rbsp_len && rbsp[byte_pos] == 0xff) {
            payload_type += 255u;
            byte_pos++;
        }
        if (byte_pos >= rbsp_len) {
            break;
        }
        if (byte_pos + 1 == rbsp_len && rbsp[byte_pos] == 0x80) {
            break;
        }
        payload_type += rbsp[byte_pos++];

        uint32_t payload_size = 0;
        while (byte_pos < rbsp_len && rbsp[byte_pos] == 0xff) {
            payload_size += 255u;
            byte_pos++;
        }
        if (byte_pos >= rbsp_len) {
            break;
        }
        payload_size += rbsp[byte_pos++];
        if ((size_t)payload_size > rbsp_len - byte_pos) {
            break;
        }

        const uint8_t *payload = rbsp + byte_pos;
        if (nvenc_is_h264_timing_sei_type(payload_type)) {
            if (packed_timing_seen) {
                *packed_timing_seen = true;
            }
        } else if (!nvenc_should_forward_h264_sei_payload(payload_type,
                                                          payload,
                                                          payload_size)) {
        } else {
            if (!nvenc_append_sei_payload_entry(payloads, count, payload_type,
                                                payload, payload_size)) {
                break;
            }
            nvenc_update_h264_sei_stream_mask(stream_mask, payload_type);
        }
        byte_pos += payload_size;

        if (byte_pos == rbsp_len) {
            break;
        }
        if (byte_pos + 1 == rbsp_len && rbsp[byte_pos] == 0x80) {
            break;
        }
    }
}

void nvenc_append_h264_sei_from_packed(NVEncodeContext *enc, const uint8_t *data, size_t bytes)
{
    if (!enc || !data || bytes == 0) {
        return;
    }

    size_t sc_len = 0;
    size_t sc = nvenc_find_start_code(data, bytes, 0, &sc_len);
    if (sc == SIZE_MAX) {
        if ((data[0] & 0x1f) != 6 || bytes <= 1) {
            return;
        }
        uint8_t *rbsp = NULL;
        uint32_t *map = NULL;
        size_t rbsp_len = 0;
        if (nvenc_build_rbsp(data + 1, bytes - 1, &rbsp, &rbsp_len, &map)) {
            nvenc_parse_h264_sei_rbsp(&enc->packedSeiPayloads, &enc->packedSeiPayloadCount,
                                      &enc->packedSeiStreamMask, &enc->packedTimingSeiSeen,
                                      rbsp, rbsp_len);
        }
        free(rbsp);
        free(map);
        return;
    }

    while (sc != SIZE_MAX) {
        size_t next_sc_len = 0;
        size_t nal_start = sc + sc_len;
        size_t next_sc = nvenc_find_start_code(data, bytes, nal_start, &next_sc_len);
        size_t nal_end = (next_sc == SIZE_MAX) ? bytes : next_sc;
        if (nal_start < nal_end && ((data[nal_start] & 0x1f) == 6) && nal_start + 1 < nal_end) {
            uint8_t *rbsp = NULL;
            uint32_t *map = NULL;
            size_t rbsp_len = 0;
            if (nvenc_build_rbsp(data + nal_start + 1, nal_end - nal_start - 1,
                                 &rbsp, &rbsp_len, &map)) {
                nvenc_parse_h264_sei_rbsp(&enc->packedSeiPayloads, &enc->packedSeiPayloadCount,
                                          &enc->packedSeiStreamMask, &enc->packedTimingSeiSeen,
                                          rbsp, rbsp_len);
            }
            free(rbsp);
            free(map);
        }
        sc = next_sc;
        sc_len = next_sc_len;
    }
}

void nvenc_capture_h264_sps_from_packed(NVEncodeContext *enc,
                                        const uint8_t *data,
                                        size_t bytes,
                                        bool has_emulation_bytes)
{
    if (!enc) {
        return;
    }
    nvenc_reset_appendable_buffer(&enc->packedSpsBuf);
    nvenc_capture_filtered_nals_from_packed(&enc->packedSpsBuf, data, bytes, 7);
    if (!has_emulation_bytes) {
        nvenc_normalize_h264_annexb_in_place(&enc->packedSpsBuf, NULL);
    }
}

void nvenc_capture_h264_pps_from_packed(NVEncodeContext *enc,
                                        const uint8_t *data,
                                        size_t bytes,
                                        bool has_emulation_bytes)
{
    if (!enc) {
        return;
    }
    nvenc_reset_appendable_buffer(&enc->packedPpsBuf);
    nvenc_capture_filtered_nals_from_packed(&enc->packedPpsBuf, data, bytes, 8);
    if (!has_emulation_bytes) {
        nvenc_normalize_h264_annexb_in_place(&enc->packedPpsBuf, NULL);
    }
}

bool nvenc_should_auto_rewrite_h264_spspps(const NVEncodeContext *enc,
                                           const NVBuffer *buf)
{
    if (!enc || !buf) {
        return false;
    }
    if (!isH264EncodeProfile(enc->profile) ||
        nvenc_use_ptd(enc) ||
        !enc->haveSeq ||
        enc->seqParams.ip_period > 1 ||
        enc->encConfig.frameIntervalP > 1) {
        return false;
    }
    return buf->packedSps && buf->packedSpsSize > 0 &&
           buf->packedPps && buf->packedPpsSize > 0;
}

typedef struct {
    bool        valid;
    size_t      gaps_bit_pos;
    bool        have_pic_struct;
    size_t      pic_struct_bit_pos;
} NVH264SpsPatchPoints;

static bool nvenc_parse_h264_sps_lite(const uint8_t *ebsp,
                                      size_t ebsp_len,
                                      NVH264SpsLite *out)
{
    uint8_t *rbsp = NULL;
    uint32_t *map = NULL;
    size_t rbsp_len = 0;
    bool ok = false;
    uint32_t tmp = 0;

    if (!ebsp || !out || !nvenc_build_rbsp(ebsp, ebsp_len, &rbsp, &rbsp_len, &map)) {
        return false;
    }

    NVBitReader br = { rbsp, rbsp_len, 0 };
    uint32_t profile_idc = 0;
    memset(out, 0, sizeof(*out));

    if (!nvenc_br_read_bits(&br, 8, &profile_idc) ||
        !nvenc_br_read_bits(&br, 8, &tmp) ||
        !nvenc_br_read_bits(&br, 8, &tmp) ||
        !nvenc_br_read_ue(&br, &out->sps_id)) {
        goto done;
    }

    switch (profile_idc) {
    case 100:
    case 110:
    case 122:
    case 244:
    case 44:
    case 83:
    case 86:
    case 118:
    case 128:
    case 138:
    case 139:
    case 134:
    case 135: {
        uint32_t chroma_format_idc = 0;
        if (!nvenc_br_read_ue(&br, &chroma_format_idc)) {
            goto done;
        }
        if (chroma_format_idc == 3) {
            if (!nvenc_br_read_bits(&br, 1, &tmp)) {
                goto done;
            }
        }
        if (!nvenc_br_read_ue(&br, &tmp) ||
            !nvenc_br_read_ue(&br, &tmp) ||
            !nvenc_br_read_bits(&br, 1, &tmp)) {
            goto done;
        }
        uint32_t seq_scaling_matrix_present_flag = 0;
        if (!nvenc_br_read_bits(&br, 1, &seq_scaling_matrix_present_flag)) {
            goto done;
        }
        if (seq_scaling_matrix_present_flag) {
            int scaling_list_count = (chroma_format_idc == 3) ? 12 : 8;
            for (int i = 0; i < scaling_list_count; i++) {
                uint32_t list_present = 0;
                if (!nvenc_br_read_bits(&br, 1, &list_present)) {
                    goto done;
                }
                if (list_present) {
                    int size = (i < 6) ? 16 : 64;
                    if (!nvenc_br_skip_scaling_list(&br, size)) {
                        goto done;
                    }
                }
            }
        }
        break;
    }
    default:
        break;
    }

    if (!nvenc_br_read_ue(&br, &out->log2_max_frame_num_minus4) ||
        !nvenc_br_read_ue(&br, &out->pic_order_cnt_type)) {
        goto done;
    }
    if (out->pic_order_cnt_type == 0) {
        if (!nvenc_br_read_ue(&br, &out->log2_max_pic_order_cnt_lsb_minus4)) {
            goto done;
        }
    } else if (out->pic_order_cnt_type == 1) {
        if (!nvenc_br_read_bits(&br, 1, &tmp)) {
            goto done;
        }
        out->delta_pic_order_always_zero_flag = tmp != 0;
        int32_t stmp = 0;
        if (!nvenc_br_read_se(&br, &stmp) ||
            !nvenc_br_read_se(&br, &stmp)) {
            goto done;
        }
        uint32_t num_ref_frames_in_pic_order_cnt_cycle = 0;
        if (!nvenc_br_read_ue(&br, &num_ref_frames_in_pic_order_cnt_cycle)) {
            goto done;
        }
        for (uint32_t i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; i++) {
            if (!nvenc_br_read_se(&br, &stmp)) {
                goto done;
            }
        }
    }

    if (!nvenc_br_read_ue(&br, &tmp)) {
        goto done;
    }
    if (!nvenc_br_read_bits(&br, 1, &tmp)) {
        goto done;
    }
    out->gaps_in_frame_num_value_allowed_flag = tmp != 0;
    if (!nvenc_br_read_ue(&br, &tmp) ||
        !nvenc_br_read_ue(&br, &tmp) ||
        !nvenc_br_read_bits(&br, 1, &tmp)) {
        goto done;
    }
    out->frame_mbs_only_flag = tmp != 0;
    if (!out->frame_mbs_only_flag) {
        if (!nvenc_br_read_bits(&br, 1, &tmp)) {
            goto done;
        }
    }
    if (!nvenc_br_read_bits(&br, 1, &tmp)) {
        goto done;
    }
    uint32_t frame_cropping_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &frame_cropping_flag)) {
        goto done;
    }
    if (frame_cropping_flag) {
        if (!nvenc_br_read_ue(&br, &tmp) ||
            !nvenc_br_read_ue(&br, &tmp) ||
            !nvenc_br_read_ue(&br, &tmp) ||
            !nvenc_br_read_ue(&br, &tmp)) {
            goto done;
        }
    }
    uint32_t vui_parameters_present_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &vui_parameters_present_flag)) {
        goto done;
    }
    out->vui_parameters_present_flag = vui_parameters_present_flag != 0;
    if (out->vui_parameters_present_flag) {
        uint32_t aspect_ratio_info_present_flag = 0;
        if (!nvenc_br_read_bits(&br, 1, &aspect_ratio_info_present_flag)) {
            goto done;
        }
        if (aspect_ratio_info_present_flag) {
            uint32_t aspect_ratio_idc = 0;
            if (!nvenc_br_read_bits(&br, 8, &aspect_ratio_idc)) {
                goto done;
            }
            if (aspect_ratio_idc == 255) {
                if (!nvenc_br_read_bits(&br, 16, &tmp) ||
                    !nvenc_br_read_bits(&br, 16, &tmp)) {
                    goto done;
                }
            }
        }
        uint32_t overscan_info_present_flag = 0;
        if (!nvenc_br_read_bits(&br, 1, &overscan_info_present_flag)) {
            goto done;
        }
        if (overscan_info_present_flag) {
            if (!nvenc_br_read_bits(&br, 1, &tmp)) {
                goto done;
            }
        }
        uint32_t video_signal_type_present_flag = 0;
        if (!nvenc_br_read_bits(&br, 1, &video_signal_type_present_flag)) {
            goto done;
        }
        if (video_signal_type_present_flag) {
            if (!nvenc_br_read_bits(&br, 3, &tmp) ||
                !nvenc_br_read_bits(&br, 1, &tmp)) {
                goto done;
            }
            uint32_t colour_description_present_flag = 0;
            if (!nvenc_br_read_bits(&br, 1, &colour_description_present_flag)) {
                goto done;
            }
            if (colour_description_present_flag) {
                if (!nvenc_br_read_bits(&br, 8, &tmp) ||
                    !nvenc_br_read_bits(&br, 8, &tmp) ||
                    !nvenc_br_read_bits(&br, 8, &tmp)) {
                    goto done;
                }
            }
        }
        uint32_t chroma_loc_info_present_flag = 0;
        if (!nvenc_br_read_bits(&br, 1, &chroma_loc_info_present_flag)) {
            goto done;
        }
        if (chroma_loc_info_present_flag) {
            if (!nvenc_br_read_ue(&br, &tmp) ||
                !nvenc_br_read_ue(&br, &tmp)) {
                goto done;
            }
        }
        uint32_t timing_info_present_flag = 0;
        if (!nvenc_br_read_bits(&br, 1, &timing_info_present_flag)) {
            goto done;
        }
        if (timing_info_present_flag) {
            if (!nvenc_br_read_bits(&br, 32, &tmp) ||
                !nvenc_br_read_bits(&br, 32, &tmp) ||
                !nvenc_br_read_bits(&br, 1, &tmp)) {
                goto done;
            }
        }
        uint32_t nal_hrd_parameters_present_flag = 0;
        if (!nvenc_br_read_bits(&br, 1, &nal_hrd_parameters_present_flag)) {
            goto done;
        }
        if (nal_hrd_parameters_present_flag) {
            uint8_t delay_len = 0;
            uint8_t cpb_cnt_minus1 = 0;
            if (!nvenc_parse_hrd(&br, &delay_len, &cpb_cnt_minus1)) {
                goto done;
            }
        }
        uint32_t vcl_hrd_parameters_present_flag = 0;
        if (!nvenc_br_read_bits(&br, 1, &vcl_hrd_parameters_present_flag)) {
            goto done;
        }
        if (vcl_hrd_parameters_present_flag) {
            uint8_t delay_len = 0;
            uint8_t cpb_cnt_minus1 = 0;
            if (!nvenc_parse_hrd(&br, &delay_len, &cpb_cnt_minus1)) {
                goto done;
            }
        }
        if (nal_hrd_parameters_present_flag || vcl_hrd_parameters_present_flag) {
            if (!nvenc_br_read_bits(&br, 1, &tmp)) {
                goto done;
            }
        }
        if (!nvenc_br_read_bits(&br, 1, &tmp)) {
            goto done;
        }
        out->pic_struct_present_flag = tmp != 0;
    }

    out->valid = true;
    ok = true;

done:
    free(rbsp);
    free(map);
    return ok;
}

static bool nvenc_parse_h264_pps_lite(const uint8_t *ebsp,
                                      size_t ebsp_len,
                                      NVH264PpsLite *out)
{
    uint8_t *rbsp = NULL;
    uint32_t *map = NULL;
    size_t rbsp_len = 0;
    bool ok = false;
    uint32_t tmp = 0;
    int32_t stmp = 0;

    if (!ebsp || !out || !nvenc_build_rbsp(ebsp, ebsp_len, &rbsp, &rbsp_len, &map)) {
        return false;
    }

    NVBitReader br = { rbsp, rbsp_len, 0 };
    memset(out, 0, sizeof(*out));
    if (!nvenc_br_read_ue(&br, &out->pps_id) ||
        !nvenc_br_read_ue(&br, &out->sps_id) ||
        !nvenc_br_read_bits(&br, 1, &tmp)) {
        goto done;
    }
    out->entropy_coding_mode_flag = tmp != 0;
    if (!nvenc_br_read_bits(&br, 1, &tmp)) {
        goto done;
    }
    out->bottom_field_pic_order_in_frame_present_flag = tmp != 0;
    if (!nvenc_br_read_ue(&br, &out->num_slice_groups_minus1) ||
        !nvenc_br_read_ue(&br, &tmp) ||
        !nvenc_br_read_ue(&br, &tmp) ||
        !nvenc_br_read_bits(&br, 1, &tmp)) {
        goto done;
    }
    out->weighted_pred_flag = tmp != 0;
    if (!nvenc_br_read_bits(&br, 2, &out->weighted_bipred_idc) ||
        !nvenc_br_read_se(&br, &stmp) ||
        !nvenc_br_read_se(&br, &stmp) ||
        !nvenc_br_read_se(&br, &stmp) ||
        !nvenc_br_read_bits(&br, 1, &tmp)) {
        goto done;
    }
    out->deblocking_filter_control_present_flag = tmp != 0;
    if (!nvenc_br_read_bits(&br, 1, &tmp) ||
        !nvenc_br_read_bits(&br, 1, &tmp)) {
        goto done;
    }
    out->redundant_pic_cnt_present_flag = tmp != 0;
    out->valid = true;
    ok = true;

done:
    free(rbsp);
    free(map);
    return ok;
}

static bool nvenc_collect_h264_sps_patch_points(const uint8_t *ebsp,
                                                size_t ebsp_len,
                                                NVH264SpsPatchPoints *out)
{
    uint8_t *rbsp = NULL;
    uint32_t *map = NULL;
    size_t rbsp_len = 0;
    bool ok = false;
    uint32_t tmp = 0;

    if (!ebsp || !out || !nvenc_build_rbsp(ebsp, ebsp_len, &rbsp, &rbsp_len, &map)) {
        return false;
    }

    NVBitReader br = { rbsp, rbsp_len, 0 };
    uint32_t profile_idc = 0;
    memset(out, 0, sizeof(*out));

    if (!nvenc_br_read_bits(&br, 8, &profile_idc) ||
        !nvenc_br_read_bits(&br, 8, &tmp) ||
        !nvenc_br_read_bits(&br, 8, &tmp) ||
        !nvenc_br_read_ue(&br, &tmp)) {
        goto done;
    }

    switch (profile_idc) {
    case 100:
    case 110:
    case 122:
    case 244:
    case 44:
    case 83:
    case 86:
    case 118:
    case 128:
    case 138:
    case 139:
    case 134:
    case 135: {
        uint32_t chroma_format_idc = 0;
        if (!nvenc_br_read_ue(&br, &chroma_format_idc)) {
            goto done;
        }
        if (chroma_format_idc == 3) {
            if (!nvenc_br_read_bits(&br, 1, &tmp)) {
                goto done;
            }
        }
        if (!nvenc_br_read_ue(&br, &tmp) ||
            !nvenc_br_read_ue(&br, &tmp) ||
            !nvenc_br_read_bits(&br, 1, &tmp)) {
            goto done;
        }
        uint32_t seq_scaling_matrix_present_flag = 0;
        if (!nvenc_br_read_bits(&br, 1, &seq_scaling_matrix_present_flag)) {
            goto done;
        }
        if (seq_scaling_matrix_present_flag) {
            int scaling_list_count = (chroma_format_idc == 3) ? 12 : 8;
            for (int i = 0; i < scaling_list_count; i++) {
                uint32_t list_present = 0;
                if (!nvenc_br_read_bits(&br, 1, &list_present)) {
                    goto done;
                }
                if (list_present) {
                    int size = (i < 6) ? 16 : 64;
                    if (!nvenc_br_skip_scaling_list(&br, size)) {
                        goto done;
                    }
                }
            }
        }
        break;
    }
    default:
        break;
    }

    if (!nvenc_br_read_ue(&br, &tmp) ||
        !nvenc_br_read_ue(&br, &tmp)) {
        goto done;
    }
    if (tmp == 0) {
        if (!nvenc_br_read_ue(&br, &tmp)) {
            goto done;
        }
    } else if (tmp == 1) {
        if (!nvenc_br_read_bits(&br, 1, &tmp)) {
            goto done;
        }
        int32_t stmp = 0;
        if (!nvenc_br_read_se(&br, &stmp) ||
            !nvenc_br_read_se(&br, &stmp)) {
            goto done;
        }
        uint32_t num_ref_frames_in_pic_order_cnt_cycle = 0;
        if (!nvenc_br_read_ue(&br, &num_ref_frames_in_pic_order_cnt_cycle)) {
            goto done;
        }
        for (uint32_t i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; i++) {
            if (!nvenc_br_read_se(&br, &stmp)) {
                goto done;
            }
        }
    }

    if (!nvenc_br_read_ue(&br, &tmp)) {
        goto done;
    }
    out->gaps_bit_pos = br.bit_pos;
    if (!nvenc_br_read_bits(&br, 1, &tmp) ||
        !nvenc_br_read_ue(&br, &tmp) ||
        !nvenc_br_read_ue(&br, &tmp)) {
        goto done;
    }
    uint32_t frame_mbs_only_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &frame_mbs_only_flag)) {
        goto done;
    }
    if (!frame_mbs_only_flag) {
        if (!nvenc_br_read_bits(&br, 1, &tmp)) {
            goto done;
        }
    }
    if (!nvenc_br_read_bits(&br, 1, &tmp)) {
        goto done;
    }
    uint32_t frame_cropping_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &frame_cropping_flag)) {
        goto done;
    }
    if (frame_cropping_flag) {
        if (!nvenc_br_read_ue(&br, &tmp) ||
            !nvenc_br_read_ue(&br, &tmp) ||
            !nvenc_br_read_ue(&br, &tmp) ||
            !nvenc_br_read_ue(&br, &tmp)) {
            goto done;
        }
    }
    uint32_t vui_parameters_present_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &vui_parameters_present_flag)) {
        goto done;
    }
    if (!vui_parameters_present_flag) {
        out->valid = true;
        ok = true;
        goto done;
    }

    uint32_t aspect_ratio_info_present_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &aspect_ratio_info_present_flag)) {
        goto done;
    }
    if (aspect_ratio_info_present_flag) {
        uint32_t aspect_ratio_idc = 0;
        if (!nvenc_br_read_bits(&br, 8, &aspect_ratio_idc)) {
            goto done;
        }
        if (aspect_ratio_idc == 255) {
            if (!nvenc_br_read_bits(&br, 16, &tmp) ||
                !nvenc_br_read_bits(&br, 16, &tmp)) {
                goto done;
            }
        }
    }
    uint32_t overscan_info_present_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &overscan_info_present_flag)) {
        goto done;
    }
    if (overscan_info_present_flag) {
        if (!nvenc_br_read_bits(&br, 1, &tmp)) {
            goto done;
        }
    }
    uint32_t video_signal_type_present_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &video_signal_type_present_flag)) {
        goto done;
    }
    if (video_signal_type_present_flag) {
        if (!nvenc_br_read_bits(&br, 3, &tmp) ||
            !nvenc_br_read_bits(&br, 1, &tmp)) {
            goto done;
        }
        uint32_t colour_description_present_flag = 0;
        if (!nvenc_br_read_bits(&br, 1, &colour_description_present_flag)) {
            goto done;
        }
        if (colour_description_present_flag) {
            if (!nvenc_br_read_bits(&br, 8, &tmp) ||
                !nvenc_br_read_bits(&br, 8, &tmp) ||
                !nvenc_br_read_bits(&br, 8, &tmp)) {
                goto done;
            }
        }
    }
    uint32_t chroma_loc_info_present_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &chroma_loc_info_present_flag)) {
        goto done;
    }
    if (chroma_loc_info_present_flag) {
        if (!nvenc_br_read_ue(&br, &tmp) ||
            !nvenc_br_read_ue(&br, &tmp)) {
            goto done;
        }
    }
    uint32_t timing_info_present_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &timing_info_present_flag)) {
        goto done;
    }
    if (timing_info_present_flag) {
        if (!nvenc_br_read_bits(&br, 32, &tmp) ||
            !nvenc_br_read_bits(&br, 32, &tmp) ||
            !nvenc_br_read_bits(&br, 1, &tmp)) {
            goto done;
        }
    }
    uint32_t nal_hrd_parameters_present_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &nal_hrd_parameters_present_flag)) {
        goto done;
    }
    if (nal_hrd_parameters_present_flag) {
        uint8_t delay_len = 0;
        uint8_t cpb_cnt_minus1 = 0;
        if (!nvenc_parse_hrd(&br, &delay_len, &cpb_cnt_minus1)) {
            goto done;
        }
    }
    uint32_t vcl_hrd_parameters_present_flag = 0;
    if (!nvenc_br_read_bits(&br, 1, &vcl_hrd_parameters_present_flag)) {
        goto done;
    }
    if (vcl_hrd_parameters_present_flag) {
        uint8_t delay_len = 0;
        uint8_t cpb_cnt_minus1 = 0;
        if (!nvenc_parse_hrd(&br, &delay_len, &cpb_cnt_minus1)) {
            goto done;
        }
    }
    if (nal_hrd_parameters_present_flag || vcl_hrd_parameters_present_flag) {
        if (!nvenc_br_read_bits(&br, 1, &tmp)) {
            goto done;
        }
    }
    out->have_pic_struct = true;
    out->pic_struct_bit_pos = br.bit_pos;
    if (!nvenc_br_read_bits(&br, 1, &tmp)) {
        goto done;
    }
    out->valid = true;
    ok = true;

done:
    free(rbsp);
    free(map);
    return ok;
}

static bool nvenc_extract_first_annexb_nal(const uint8_t *data,
                                           size_t size,
                                           const uint8_t **out_nal,
                                           size_t *out_nal_size)
{
    size_t sc_len = 0;
    size_t sc = nvenc_find_start_code(data, size, 0, &sc_len);
    if (sc == SIZE_MAX) {
        if (!data || size == 0 || !out_nal || !out_nal_size) {
            return false;
        }
        *out_nal = data;
        *out_nal_size = size;
        return true;
    }

    size_t nal_start = sc + sc_len;
    size_t next_sc_len = 0;
    size_t next_sc = nvenc_find_start_code(data, size, nal_start, &next_sc_len);
    size_t nal_end = (next_sc == SIZE_MAX) ? size : next_sc;
    *out_nal = data + sc;
    *out_nal_size = nal_end - sc;
    return true;
}

static bool nvenc_extract_first_h264_annexb_payload(const uint8_t *data,
                                                    size_t size,
                                                    uint8_t expected_nal_type,
                                                    const uint8_t **out_payload,
                                                    size_t *out_payload_size)
{
    const uint8_t *nal = NULL;
    size_t nal_size = 0;
    size_t sc_len = 0;
    size_t sc = SIZE_MAX;

    if (!data || size == 0 || !out_payload || !out_payload_size ||
        !nvenc_extract_first_annexb_nal(data, size, &nal, &nal_size)) {
        return false;
    }

    sc = nvenc_find_start_code(nal, nal_size, 0, &sc_len);
    if (sc != SIZE_MAX) {
        if (nal_size <= sc + sc_len) {
            return false;
        }
        nal += sc + sc_len;
        nal_size -= sc + sc_len;
    }
    if (nal_size <= 1 || (nal[0] & 0x1f) != expected_nal_type) {
        return false;
    }

    *out_payload = nal + 1;
    *out_payload_size = nal_size - 1;
    return true;
}

static bool nvenc_try_merge_h264_sps_nal(AppendableBuffer *rebuilt,
                                         const uint8_t *segment,
                                         size_t segment_size,
                                         size_t sc_len,
                                         const uint8_t *packed_sps,
                                         size_t packed_sps_size,
                                         bool patch_pic_struct)
{
    const uint8_t *packed_payload = NULL;
    size_t packed_payload_size = 0;
    const uint8_t *native_nal = NULL;
    size_t native_nal_size = 0;
    NVH264SpsLite packed = { 0 };
    NVH264SpsPatchPoints patch = { 0 };
    uint8_t *rbsp = NULL;
    uint32_t *map = NULL;
    size_t rbsp_len = 0;
    uint8_t *merged = NULL;
    bool ok = false;

    if (!rebuilt || !segment || segment_size <= sc_len + 1 ||
        !packed_sps || packed_sps_size == 0) {
        return false;
    }
    if (!nvenc_extract_first_h264_annexb_payload(packed_sps,
                                                 packed_sps_size,
                                                 7,
                                                 &packed_payload,
                                                 &packed_payload_size)) {
        return false;
    }
    if (!nvenc_parse_h264_sps_lite(packed_payload, packed_payload_size, &packed) ||
        !packed.valid) {
        return false;
    }

    native_nal = segment + sc_len;
    native_nal_size = segment_size - sc_len;
    if ((native_nal[0] & 0x1f) != 7 || native_nal_size <= 1) {
        return false;
    }
    if (!nvenc_collect_h264_sps_patch_points(native_nal + 1, native_nal_size - 1, &patch) ||
        !patch.valid) {
        return false;
    }
    if (!nvenc_build_rbsp(native_nal + 1, native_nal_size - 1, &rbsp, &rbsp_len, &map)) {
        return false;
    }

    merged = (uint8_t *)malloc(segment_size);
    if (!merged) {
        goto done;
    }
    memcpy(merged, segment, segment_size);

    if (!nvenc_patch_rbsp_bits(merged + sc_len + 1, map, rbsp_len,
                               patch.gaps_bit_pos, 1,
                               packed.gaps_in_frame_num_value_allowed_flag ? 1u : 0u)) {
        goto done;
    }
    if (patch_pic_struct &&
        patch.have_pic_struct &&
        packed.vui_parameters_present_flag) {
        if (!nvenc_patch_rbsp_bits(merged + sc_len + 1, map, rbsp_len,
                                   patch.pic_struct_bit_pos, 1,
                                   packed.pic_struct_present_flag ? 1u : 0u)) {
            goto done;
        }
    }

    appendBuffer(rebuilt, merged, segment_size);
    ok = true;

done:
    free(rbsp);
    free(map);
    free(merged);
    return ok;
}

VAStatus nvenc_rewrite_h264_parameter_sets_in_coded_buffer(NVEncodeContext *enc,
                                                           NVBuffer *buf,
                                                           size_t prefix,
                                                           bool *rewritten)
{
    if (rewritten) {
        *rewritten = false;
    }
    if (!enc || !buf || !buf->codedBuf || prefix > buf->codedSize) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if ((!buf->packedSps || buf->packedSpsSize == 0) &&
        (!buf->packedPps || buf->packedPpsSize == 0)) {
        return VA_STATUS_SUCCESS;
    }

    const uint8_t *payload = buf->codedBuf + prefix;
    size_t payload_size = buf->codedSize - prefix;
    size_t sc_len = 0;
    size_t sc = nvenc_find_start_code(payload, payload_size, 0, &sc_len);
    if (sc == SIZE_MAX) {
        return VA_STATUS_SUCCESS;
    }

    AppendableBuffer rebuilt = { 0 };
    if (prefix > 0) {
        appendBuffer(&rebuilt, buf->codedBuf, prefix);
    }

    bool changed = false;
    NVH264SpsLite native_sps = enc->rewriteNativeH264Sps;
    NVH264PpsLite native_pps = enc->rewriteNativeH264Pps;
    bool rewrite_spspps = nvenc_should_auto_rewrite_h264_spspps(enc, buf);
    bool can_use_safe_sps_merge = rewrite_spspps &&
                                  !nvenc_use_ptd(enc) &&
                                  enc->haveSeq &&
                                  enc->seqParams.ip_period <= 1;
    bool patch_safe_merge_pic_struct = !buf->packedTimingSeiSeen;
    bool use_safe_sps_merge = can_use_safe_sps_merge;
    while (sc != SIZE_MAX) {
        size_t next_sc_len = 0;
        size_t nal_start = sc + sc_len;
        size_t next_sc = nvenc_find_start_code(payload, payload_size, nal_start, &next_sc_len);
        size_t nal_end = (next_sc == SIZE_MAX) ? payload_size : next_sc;
        if (nal_start < payload_size) {
            uint8_t nal_type = payload[nal_start] & 0x1f;
            if (nal_type == 7 && buf->packedSps && buf->packedSpsSize > 0) {
                if (nal_start + 1 < nal_end) {
                    (void)nvenc_parse_h264_sps_lite(payload + nal_start + 1,
                                                    nal_end - nal_start - 1,
                                                    &native_sps);
                    enc->rewriteNativeH264Sps = native_sps;
                }
                if (rewrite_spspps &&
                    use_safe_sps_merge &&
                    nvenc_try_merge_h264_sps_nal(&rebuilt,
                                                 payload + sc,
                                                 nal_end - sc,
                                                 sc_len,
                                                 buf->packedSps,
                                                 buf->packedSpsSize,
                                                 patch_safe_merge_pic_struct)) {
                    changed = true;
                } else if (rewrite_spspps && !use_safe_sps_merge) {
                    appendBuffer(&rebuilt, buf->packedSps, buf->packedSpsSize);
                    changed = true;
                } else {
                    appendBuffer(&rebuilt, payload + sc, nal_end - sc);
                }
            } else if (nal_type == 8 && buf->packedPps && buf->packedPpsSize > 0) {
                if (nal_start + 1 < nal_end) {
                    (void)nvenc_parse_h264_pps_lite(payload + nal_start + 1,
                                                    nal_end - nal_start - 1,
                                                    &native_pps);
                    enc->rewriteNativeH264Pps = native_pps;
                }
                if (rewrite_spspps) {
                    appendBuffer(&rebuilt, buf->packedPps, buf->packedPpsSize);
                    changed = true;
                } else {
                    appendBuffer(&rebuilt, payload + sc, nal_end - sc);
                }
            } else if (nal_type == 7) {
                if (nal_start + 1 < nal_end) {
                    (void)nvenc_parse_h264_sps_lite(payload + nal_start + 1,
                                                    nal_end - nal_start - 1,
                                                    &native_sps);
                    enc->rewriteNativeH264Sps = native_sps;
                }
                appendBuffer(&rebuilt, payload + sc, nal_end - sc);
            } else if (nal_type == 8) {
                if (nal_start + 1 < nal_end) {
                    (void)nvenc_parse_h264_pps_lite(payload + nal_start + 1,
                                                    nal_end - nal_start - 1,
                                                    &native_pps);
                    enc->rewriteNativeH264Pps = native_pps;
                }
                appendBuffer(&rebuilt, payload + sc, nal_end - sc);
            } else {
                appendBuffer(&rebuilt, payload + sc, nal_end - sc);
            }
        }
        sc = next_sc;
        sc_len = next_sc_len;
    }

    if (!changed) {
        free(rebuilt.buf);
        return VA_STATUS_SUCCESS;
    }

    if (rebuilt.size > buf->codedAllocated) {
        uint8_t *tmp = realloc(buf->codedBuf, rebuilt.size);
        if (!tmp) {
            free(rebuilt.buf);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        buf->codedBuf = tmp;
        buf->codedAllocated = rebuilt.size;
    }
    memcpy(buf->codedBuf, rebuilt.buf, rebuilt.size);
    buf->codedSize = rebuilt.size;
    free(rebuilt.buf);
    if (rewritten) {
        *rewritten = true;
    }
    return VA_STATUS_SUCCESS;
}

void nvenc_append_aud_from_packed(NVEncodeContext *enc, const uint8_t *data, size_t bytes)
{
    nvenc_append_filtered_nals_from_packed(enc, data, bytes, 9);
}

void nvenc_append_non_sei_nals_from_packed(NVEncodeContext *enc, const uint8_t *data, size_t bytes)
{
    if (!enc || !data || bytes == 0) {
        return;
    }

    size_t sc_len = 0;
    size_t sc = nvenc_find_start_code(data, bytes, 0, &sc_len);
    if (sc == SIZE_MAX) {
        uint8_t nal_type = data[0] & 0x1f;
        if (nal_type != 6) {
            static const uint8_t start_code[] = { 0x00, 0x00, 0x01 };
            appendBuffer(&enc->packedHeaderBuf, start_code, sizeof(start_code));
            appendBuffer(&enc->packedHeaderBuf, data, bytes);
        }
        return;
    }

    while (sc != SIZE_MAX) {
        size_t next_sc_len = 0;
        size_t nal_start = sc + sc_len;
        size_t next_sc = nvenc_find_start_code(data, bytes, nal_start, &next_sc_len);
        size_t nal_end = (next_sc == SIZE_MAX) ? bytes : next_sc;
        if (nal_start < bytes) {
            uint8_t nal_type = data[nal_start] & 0x1f;
            if (nal_type != 6) {
                appendBuffer(&enc->packedHeaderBuf, data + sc, nal_end - sc);
            }
        }
        sc = next_sc;
        sc_len = next_sc_len;
    }
}
