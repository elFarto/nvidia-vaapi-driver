#ifndef NVD_ENCODE_PIPELINE_H
#define NVD_ENCODE_PIPELINE_H

#include "../vabackend.h"

bool nvenc_ensure_surface_buffer(NVDriver *drv, NVSurface *surface);
bool nvenc_fill_surface_black(NVSurface *surface);
bool nvenc_fill_surface_padding_black(NVSurface *surface, uint32_t copy_width, uint32_t copy_height);
bool nvenc_submit_queued_pic(NVDriver *drv, NVContext *nvCtx, NVEncQueuedPic *queued);
void nvenc_process_reorder_queue(NVDriver *drv, NVContext *nvCtx, bool flush);
bool nvenc_surface_can_direct_encode(const NVSurface *surface);
bool nvenc_has_unready_pending_output(NVEncodeContext *enc);
VAStatus nvenc_drain_encoder(NVDriver *drv, NVContext *nvCtx);
VAStatus nvenc_resolve_pending_output_ex(NVEncodeContext *enc, uint32_t pending_idx, bool do_not_wait, bool *busy);
void nvenc_unregister_surface(NVEncodeContext *enc, NVEncSurfaceReg *reg);
void nvenc_remove_surface_reg(NVEncodeContext *enc, NVEncSurfaceReg *reg);

#endif
