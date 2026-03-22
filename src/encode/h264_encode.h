#ifndef NVD_ENCODE_H264_ENCODE_H
#define NVD_ENCODE_H264_ENCODE_H

#include "../vabackend.h"

#define NVENC_H264_MAX_REFS 7u
#define NVENC_H264_MAX_SLICES 256u
#define NVENC_H264_MAX_ROI_REGIONS 8u
#define NVENC_H264_MAX_QUALITY_LEVEL 5u
#define NVENC_H264_MIN_QUALITY_LEVEL 1u
#define NVENC_H264_MAX_QP 51u
#define NVENC_H264_DEFAULT_GOP 60u
#define NVENC_H264_SEI_MASK_BUFFERING_PERIOD 0x01u
#define NVENC_H264_SEI_MASK_PIC_TIMING       0x02u
#define NVENC_H264_SEI_MASK_RECOVERY_POINT   0x04u

bool isH264EncodeProfile(VAProfile profile);
GUID nvenc_h264_profile_guid(VAProfile profile);
uint32_t nvenc_h264_level_from_va(uint8_t level_idc);
uint32_t nvenc_mb_width(uint32_t width);
uint32_t nvenc_mb_height(uint32_t height);
uint32_t nvenc_clamp_quality_level(uint32_t quality_level);
uint32_t nvenc_clamp_qp(int32_t qp);
GUID nvenc_preset_from_quality_level(uint32_t quality_level);
bool nvenc_is_h264_profile(VAProfile profile);
bool nvenc_is_screen_share(const NVEncodeContext *enc);
NVEncSeqSignature nvenc_seq_signature_from_va(const VAEncSequenceParameterBufferH264 *seq);
bool nvd_reconfig_force_idr(const NVEncodeContext *enc);
uint32_t nvd_default_gop(uint32_t fr_num, uint32_t fr_den);
void nvenc_update_fixed_qp(NVEncodeContext *enc);
void nvenc_update_visible_dimensions(NVEncodeContext *enc);
void nvenc_reset_bp_state(NVEncodeContext *enc);
bool nvenc_patch_h264_bp_sei(NVEncodeContext *enc, uint8_t *data, size_t size, uint64_t bits_since);
void nvenc_append_aud_from_packed(NVEncodeContext *enc, const uint8_t *data, size_t bytes);
void nvenc_append_non_sei_nals_from_packed(NVEncodeContext *enc, const uint8_t *data, size_t bytes);
void nvenc_append_h264_sei_from_packed(NVEncodeContext *enc, const uint8_t *data, size_t bytes);
void nvenc_capture_h264_sps_from_packed(NVEncodeContext *enc, const uint8_t *data, size_t bytes, bool has_emulation_bytes);
void nvenc_capture_h264_pps_from_packed(NVEncodeContext *enc, const uint8_t *data, size_t bytes, bool has_emulation_bytes);
bool nvenc_should_auto_rewrite_h264_spspps(const NVEncodeContext *enc,
                                           const NVBuffer *buf);
VAStatus nvenc_rewrite_h264_parameter_sets_in_coded_buffer(NVEncodeContext *enc,
                                                           NVBuffer *buf,
                                                           size_t prefix,
                                                           bool *rewritten);
void nvenc_clear_h264_sei_payloads(NV_ENC_SEI_PAYLOAD **payloads, uint32_t *count);
bool nvenc_build_roi_map(NVEncodeContext *enc,
                         const VAEncMiscParameterBufferROI *roi_param,
                         int8_t **out_map,
                         uint32_t *out_size,
                         NV_ENC_QP_MAP_MODE *out_mode);
bool nvenc_compute_slice_config(NVEncodeContext *enc, uint32_t *mode, uint32_t *data);
void nvenc_update_slice_reconfigure(NVEncodeContext *enc);

#endif
