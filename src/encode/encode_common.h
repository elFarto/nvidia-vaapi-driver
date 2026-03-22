#ifndef NVD_ENCODE_COMMON_H
#define NVD_ENCODE_COMMON_H

#include "../vabackend.h"

#define NVENC_DEFAULT_RATE_CONTROL VA_RC_CQP
#define NVENC_DEFAULT_BITSTREAM_POOL_MAX 32u
#define NVENC_RECONFIG_DEBOUNCE_FRAMES 15u

typedef enum
{
    NVENC_RECONFIGURE_REASON_NONE = 0,
    NVENC_RECONFIGURE_REASON_SEQ_PARAMS,
    NVENC_RECONFIGURE_REASON_SEQ_BITRATE,
    NVENC_RECONFIGURE_REASON_RATE_CONTROL,
    NVENC_RECONFIGURE_REASON_HRD,
    NVENC_RECONFIGURE_REASON_FRAMERATE,
    NVENC_RECONFIGURE_REASON_QUALITY,
    NVENC_RECONFIGURE_REASON_RIR,
    NVENC_RECONFIGURE_REASON_QP_MAP_MODE,
    NVENC_RECONFIGURE_REASON_SLICE_MODE,
    NVENC_RECONFIGURE_REASON_PACKED_SEI_POLICY,
} NVEncReconfigureReason;

static inline void nvenc_surface_invalidate_encode_input(NVSurface *surface)
{
    if (!surface) {
        return;
    }
    surface->encInputValid = false;
}

static inline bool nvenc_surface_direct_input_is_current(const NVSurface *surface)
{
    return surface &&
           surface->encInputValid &&
           (surface->encUploadSeq > surface->encLastEncodedSeq ||
            surface->encDirectUploadLatest ||
            (!surface->backingImage && surface->encUploadSeq > 0));
}

static inline void nvenc_surface_mark_encode_input_written(NVSurface *surface,
                                                           bool direct_upload)
{
    if (!surface) {
        return;
    }
    surface->encUploadSeq++;
    surface->encCopyEventPending = false;
    surface->encDirectUploadLatest = direct_upload;
    surface->encInputValid = true;
}

uint32_t nvenc_reconfig_bps_hint(const NVEncodeContext *enc, uint32_t candidate);
void nvenc_get_framerate(NVEncodeContext *enc, uint32_t *fr_num, uint32_t *fr_den);
bool nvenc_query_encode_caps(NVDriver *drv);
bool nvenc_use_ptd(const NVEncodeContext *enc);
bool nvenc_use_internal_reorder(const NVEncodeContext *enc);
bool nvenc_select_enable_ptd(const NVEncodeContext *enc);
NV_ENC_PARAMS_RC_MODE nvenc_rc_mode_from_va(uint32_t rc_mode);
void nvenc_force_reconfigure(NVEncodeContext *enc, NVEncReconfigureReason reason, uint32_t new_bps);
const char *nvenc_reconfigure_reason_name(NVEncReconfigureReason reason);
NV_ENC_OUTPUT_PTR nvenc_bitstream_acquire(NVEncodeContext *enc);
void nvenc_bitstream_release(NVEncodeContext *enc, NV_ENC_OUTPUT_PTR bitstream);
void nvenc_destroy_bitstream_pool(NVEncodeContext *enc);
void nvenc_reset_appendable_buffer(AppendableBuffer *buffer);
NVFormat nvenc_surface_format_to_nv_format(cudaVideoSurfaceFormat surface_format, int bitdepth);
NVFormat nvenc_surface_expected_format(const NVSurface *surface);
void nvenc_clear_surface_resolving(NVSurface *surface);
uint32_t nvenc_compute_display_poc(NVEncodeContext *enc, uint32_t poc_lsb, bool idr);
uint32_t nvenc_reorder_group_size(const NVEncodeContext *enc);
uint32_t nvenc_reorder_group_id(const NVEncodeContext *enc, uint32_t display_poc, bool is_idr);
void nvenc_mark_pending_ready(NVEncodeContext *enc);
bool nvenc_has_pending_outputs(NVEncodeContext *enc);
bool nvenc_has_queued_pics(NVEncodeContext *enc);
bool nvenc_surface_has_pending_output(NVEncodeContext *enc, NVSurface *surface);
int32_t nvenc_find_pending_output_index_for_surface(NVEncodeContext *enc, NVSurface *surface);
void nvenc_cleanup_pending_output(NVEncodeContext *enc, NVEncPendingOutput *pending);
void nvenc_clear_pending_outputs(NVEncodeContext *enc);
void nvenc_clear_queued_pics(NVEncodeContext *enc);
void nvenc_abandon_pending_outputs(NVEncodeContext *enc);
void nvenc_abandon_queued_pics(NVEncodeContext *enc);
void nvenc_abandon_coded_buffer(NVEncodeContext *enc, NVBuffer *buf);
bool nvenc_coded_buffer_has_pending_output(NVEncodeContext *enc, NVBuffer *buf);
int32_t nvenc_find_pending_output_index_for_coded_buffer(NVEncodeContext *enc, NVBuffer *buf);
bool nvenc_coded_buffer_is_queued_or_reordered(NVEncodeContext *enc, NVBuffer *buf);
void nvenc_discard_unsubmitted_coded_buffer(NVEncodeContext *enc, NVBuffer *buf);
void nvenc_clear_reorder_groups(NVEncodeContext *enc);
void nvenc_reset_roi_state(NVEncodeContext *enc);
void nvenc_release_roi_state(NVEncodeContext *enc);
void nvenc_free_queued_pic(NVEncQueuedPic *queued);
NVEncReorderEntry *nvenc_find_reorder_entry(NVEncodeContext *enc, uint64_t display_ts, uint32_t *out_idx);
NVEncQueuedPic *nvenc_find_queued_by_poc(NVEncodeContext *enc, uint32_t poc, uint32_t *out_idx);
void nvenc_decode_api_version(uint32_t version, uint32_t *major, uint32_t *minor);

#endif
