#ifndef NVD_ENCODE_VA_H
#define NVD_ENCODE_VA_H

#include "encode_common.h"

void nvenc_request_reconfigure_if_changed(NVEncodeContext *enc,
                                          bool changed,
                                          NVEncReconfigureReason reason,
                                          uint32_t bps);
void nvenc_handle_misc_rate_control(NVEncodeContext *enc, const void *data);
void nvenc_handle_misc_hrd(NVEncodeContext *enc, const void *data);
void nvenc_handle_misc_framerate(NVEncodeContext *enc, const void *data);
void nvenc_handle_misc_quality_level(NVEncodeContext *enc, const void *data);
void nvenc_handle_misc_rir(NVEncodeContext *enc, const void *data);
void nvenc_handle_misc_roi(NVEncodeContext *enc, const void *data);
void nvenc_handle_seq_params(NVEncodeContext *enc, const VAEncSequenceParameterBufferH264 *seq);
void nvenc_handle_picture_params(NVEncodeContext *enc, const void *data);
void nvenc_handle_slice_params(NVEncodeContext *enc,
                               VAEncSliceParameterBufferH264 *slices,
                               uint32_t slice_count);
void nvenc_handle_packed_header_param(NVEncodeContext *enc, const void *data);
void nvenc_handle_packed_header_data(NVEncodeContext *enc,
                                     const void *data,
                                     size_t size,
                                     uint32_t elements);
VAStatus nvenc_handle_render_buffer(NVEncodeContext *enc, const NVBuffer *buf);
void nvenc_finalize_render_picture(NVEncodeContext *enc);
void nvenc_begin_picture_state(NVContext *nvCtx,
                               NVSurface *surface,
                               VASurfaceID render_target);
void nvenc_reset_picture_state(NVEncodeContext *enc);
void nvenc_move_roi_map_to_queued_pic(NVEncodeContext *enc, NVEncQueuedPic *queued);
VAStatus nvenc_prepare_coded_buffer(NVEncodeContext *enc, NVBuffer *codedBuf);
VAStatus nvenc_validate_picture_submit(const NVEncodeContext *enc);
NVEncQueuedPic *nvenc_alloc_queued_pic(NVEncodeContext *enc,
                                       NVSurface *surface,
                                       NVBuffer *codedBuf);
void nvenc_queue_reorder_entry(NVEncodeContext *enc,
                               NVBuffer *codedBuf,
                               const NVEncQueuedPic *queued);
VAStatus nvenc_submit_prepared_picture(NVDriver *drv,
                                       NVContext *nvCtx,
                                       NVSurface *surface,
                                       NVBuffer *codedBuf);

#endif
