#include "encode_pipeline.h"
#include "../external-surface.h"
#include "encode_common.h"
#include "encode_driver.h"
#include "encode_surface.h"
#include "h264_encode.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern CudaFunctions *cu;

#define NVENC_PADDING_CLEAR_MAX_BYTES (512u * 1024u)
#define NVENC_SCREEN_SHARE_MIN_RECOVERY_IDR_PERIOD 512u
#define NVENC_SCREEN_SHARE_INTERNAL_VBV_MIN_FRAMES 4u
#define NVENC_SCREEN_SHARE_INTERNAL_VBV_TARGET_MS 200u
#define NVENC_SCREEN_SHARE_INTERNAL_VBV_MAX_FRAMES 16u
#define NVENC_SCREEN_SHARE_STARTUP_RUNTIME_RECOVERY_FRAMES 16u
#define NVENC_SCREEN_SHARE_STARTUP_PERIODIC_RECOVERY_FRAMES 8u
#define NVENC_SCREEN_SHARE_STARTUP_PERIODIC_RECOVERY_INTERVAL 2u
#define NVENC_SCREEN_SHARE_STEADY_STATE_RECONFIG_AFTER_FRAMES 300u
#define NVENC_SCREEN_SHARE_STEADY_STATE_RECONFIG_DEBOUNCE_FRAMES 120u
#define NVENC_SCREEN_SHARE_STEADY_STATE_RECONFIG_FAST_DELTA_BPS 1000000u
#define NVENC_SCREEN_SHARE_STEADY_STATE_RECONFIG_FAST_DELTA_PERCENT 20u

static uint32_t nvenc_screen_share_internal_vbv_frames(NVEncodeContext *enc)
{
    uint32_t fr_num = 0;
    uint32_t fr_den = 0;
    uint64_t frames = NVENC_SCREEN_SHARE_INTERNAL_VBV_MIN_FRAMES;

    if (!enc || NVENC_SCREEN_SHARE_INTERNAL_VBV_TARGET_MS == 0) {
        return (uint32_t)frames;
    }

    nvenc_get_framerate(enc, &fr_num, &fr_den);
    if (fr_num == 0 || fr_den == 0) {
        return (uint32_t)frames;
    }

    frames = ((uint64_t)fr_num * (uint64_t)NVENC_SCREEN_SHARE_INTERNAL_VBV_TARGET_MS +
              ((uint64_t)fr_den * 1000ull) - 1ull) /
             ((uint64_t)fr_den * 1000ull);
    if (frames < NVENC_SCREEN_SHARE_INTERNAL_VBV_MIN_FRAMES) {
        frames = NVENC_SCREEN_SHARE_INTERNAL_VBV_MIN_FRAMES;
    }
    if (frames > NVENC_SCREEN_SHARE_INTERNAL_VBV_MAX_FRAMES) {
        frames = NVENC_SCREEN_SHARE_INTERNAL_VBV_MAX_FRAMES;
    }

    return (uint32_t)frames;
}

static uint32_t nvenc_screen_share_internal_vbv_bits(NVEncodeContext *enc,
                                                     uint32_t avg_bps)
{
    uint32_t fr_num = 0;
    uint32_t fr_den = 0;
    uint32_t frames = nvenc_screen_share_internal_vbv_frames(enc);
    uint64_t bits = 0;

    if (!enc || avg_bps == 0 || frames == 0) {
        return 0;
    }

    nvenc_get_framerate(enc, &fr_num, &fr_den);
    if (fr_num == 0 || fr_den == 0) {
        return 0;
    }

    bits = ((uint64_t)avg_bps * (uint64_t)fr_den * (uint64_t)frames + fr_num - 1u) /
           (uint64_t)fr_num;
    if (bits == 0) {
        return 1u;
    }

    return bits > UINT32_MAX ? UINT32_MAX : (uint32_t)bits;
}

static uint32_t nvenc_screen_share_vbv_initial_delay_bits(const NVEncodeContext *enc,
                                                          uint32_t requested_vbv,
                                                          uint32_t effective_vbv)
{
    uint32_t requested_initial = 0;
    uint64_t scaled = 0;

    if (!enc || effective_vbv == 0) {
        return 0;
    }

    if (enc->haveHrd && enc->hrdParams.initial_buffer_fullness > 0) {
        requested_initial = enc->hrdParams.initial_buffer_fullness;
    } else if (requested_vbv > 0) {
        requested_initial = requested_vbv / 2u;
    }

    if (requested_initial == 0) {
        requested_initial = effective_vbv / 2u;
    }
    if (requested_initial == 0) {
        return 0;
    }

    if (requested_vbv > 0) {
        scaled = ((uint64_t)requested_initial * (uint64_t)effective_vbv +
                  (uint64_t)requested_vbv - 1ull) /
                 (uint64_t)requested_vbv;
    } else {
        scaled = requested_initial;
    }

    if (scaled == 0) {
        scaled = 1;
    }
    if (scaled > effective_vbv) {
        scaled = effective_vbv;
    }

    return (uint32_t)scaled;
}

static void nvenc_apply_screen_share_low_latency_rc_policy(NVEncodeContext *enc)
{
    uint32_t requested_vbv = 0;
    uint32_t requested_avg_bps = 0;
    uint32_t clamp_vbv = 0;
    uint32_t effective_initial_delay = 0;

    if (!enc || !nvenc_is_screen_share(enc)) {
        return;
    }

    /* Keep screen-share CBR on a short VBV window to reduce pacer spikes during motion-heavy scenes. */
    enc->encConfig.rcParams.zeroReorderDelay = 1;
    enc->encConfig.rcParams.strictGOPTarget = 1;

    if (nvenc_use_ptd(enc) ||
        enc->encConfig.rcParams.rateControlMode != NV_ENC_PARAMS_RC_CBR ||
        enc->encConfig.frameIntervalP > 1) {
        return;
    }

    if (!nvenc_query_encode_caps(enc->drv) ||
        enc->drv->nvencCaps.supportCustomVbvBufSize <= 0) {
        return;
    }

    requested_avg_bps = enc->encConfig.rcParams.averageBitRate;
    if (requested_avg_bps == 0) {
        requested_avg_bps = enc->encConfig.rcParams.maxBitRate;
    }
    requested_vbv = enc->encConfig.rcParams.vbvBufferSize;
    clamp_vbv = nvenc_screen_share_internal_vbv_bits(enc, requested_avg_bps);
    if (clamp_vbv == 0 || (requested_vbv > 0 && requested_vbv <= clamp_vbv)) {
        return;
    }

    effective_initial_delay = nvenc_screen_share_vbv_initial_delay_bits(enc,
                                                                        requested_vbv,
                                                                        clamp_vbv);
    enc->encConfig.rcParams.vbvBufferSize = clamp_vbv;
    enc->encConfig.rcParams.vbvInitialDelay = effective_initial_delay;
}

bool nvenc_fill_surface_black(NVSurface *surface)
{
    if (!surface || !surface->encDevPtr || !surface->encPitch) {
        return false;
    }

    size_t y_size = surface->encPitch * (size_t)surface->height;
    size_t uv_size = surface->encPitch * (size_t)(surface->height / 2);

    if (CHECK_CUDA_RESULT(cu->cuMemsetD8Async(surface->encDevPtr, 0x10, y_size, 0))) {
        return false;
    }
    if (CHECK_CUDA_RESULT(cu->cuMemsetD8Async(surface->encDevPtr + y_size, 0x80, uv_size, 0))) {
        return false;
    }
    if (CHECK_CUDA_RESULT(cu->cuStreamSynchronize(0))) {
        return false;
    }
    return true;
}

static bool nvenc_fill_rect_from_host(CUdeviceptr dst,
                                      size_t dst_pitch,
                                      size_t width_bytes,
                                      size_t height,
                                      uint8_t value,
                                      uint8_t *scratch,
                                      size_t scratch_size)
{
    size_t rect_bytes;

    if (width_bytes == 0 || height == 0) {
        return true;
    }
    if (!scratch || width_bytes > SIZE_MAX / height) {
        return false;
    }

    rect_bytes = width_bytes * height;
    if (scratch_size < rect_bytes) {
        return false;
    }

    memset(scratch, value, rect_bytes);

    CUDA_MEMCPY2D cpy = {
        .srcMemoryType = CU_MEMORYTYPE_HOST,
        .srcHost = scratch,
        .srcPitch = width_bytes,
        .dstMemoryType = CU_MEMORYTYPE_DEVICE,
        .dstDevice = dst,
        .dstPitch = dst_pitch,
        .WidthInBytes = width_bytes,
        .Height = height
    };

    return !CHECK_CUDA_RESULT(cu->cuMemcpy2D(&cpy));
}

static size_t nvenc_rect_required_bytes(size_t width_bytes, size_t height)
{
    if (width_bytes == 0 || height == 0 || width_bytes > SIZE_MAX / height) {
        return 0;
    }
    return width_bytes * height;
}

bool nvenc_fill_surface_padding_black(NVSurface *surface,
                                      uint32_t copy_width,
                                      uint32_t copy_height)
{
    size_t y_right_bytes;
    size_t y_bottom_bytes;
    size_t uv_right_bytes;
    size_t uv_bottom_bytes;
    size_t scratch_bytes;
    uint8_t *scratch = NULL;
    bool ok = false;

    if (!surface || !surface->encDevPtr || !surface->encPitch) {
        return false;
    }
    if (copy_width == 0 || copy_height == 0 ||
        copy_width > surface->width || copy_height > surface->height) {
        return false;
    }
    if (copy_width == surface->width && copy_height == surface->height) {
        return true;
    }

    uint32_t uv_copy_width = ROUND_UP(copy_width, 2u);
    uint32_t uv_copy_height = ROUND_UP(copy_height, 2u) / 2u;
    uint32_t uv_surface_height = surface->height / 2u;

    y_right_bytes = nvenc_rect_required_bytes(surface->width - copy_width, copy_height);
    y_bottom_bytes = nvenc_rect_required_bytes(surface->width, surface->height - copy_height);
    uv_right_bytes = nvenc_rect_required_bytes(surface->width - uv_copy_width, uv_copy_height);
    uv_bottom_bytes = nvenc_rect_required_bytes(surface->width, uv_surface_height - uv_copy_height);

    scratch_bytes = y_right_bytes;
    if (y_bottom_bytes > scratch_bytes) {
        scratch_bytes = y_bottom_bytes;
    }
    if (uv_right_bytes > scratch_bytes) {
        scratch_bytes = uv_right_bytes;
    }
    if (uv_bottom_bytes > scratch_bytes) {
        scratch_bytes = uv_bottom_bytes;
    }

    if (scratch_bytes == 0) {
        return true;
    }
    if (scratch_bytes > NVENC_PADDING_CLEAR_MAX_BYTES) {
        return false;
    }

    scratch = malloc(scratch_bytes);
    if (!scratch) {
        return false;
    }

    CUdeviceptr y_base = surface->encDevPtr;
    CUdeviceptr uv_base = surface->encDevPtr + surface->encPitch * (size_t)surface->height;

    ok = nvenc_fill_rect_from_host(y_base + copy_width,
                                   surface->encPitch,
                                   surface->width - copy_width,
                                   copy_height,
                                   0x10,
                                   scratch,
                                   scratch_bytes) &&
         nvenc_fill_rect_from_host(y_base + surface->encPitch * (size_t)copy_height,
                                   surface->encPitch,
                                   surface->width,
                                   surface->height - copy_height,
                                   0x10,
                                   scratch,
                                   scratch_bytes) &&
         nvenc_fill_rect_from_host(uv_base + uv_copy_width,
                                   surface->encPitch,
                                   surface->width - uv_copy_width,
                                   uv_copy_height,
                                   0x80,
                                   scratch,
                                   scratch_bytes) &&
         nvenc_fill_rect_from_host(uv_base + surface->encPitch * (size_t)uv_copy_height,
                                   surface->encPitch,
                                   surface->width,
                                   uv_surface_height - uv_copy_height,
                                   0x80,
                                   scratch,
                                   scratch_bytes);

    free(scratch);
    return ok;
}

static bool nvenc_should_arm_recovery_idr(const NVEncodeContext *enc)
{
    return enc &&
           nvenc_is_screen_share(enc) &&
           /* Limit this to long-GOP PTD=0 screen-share sessions; parity-sensitive FFmpeg paths stay on the client timeline. */
           enc->seqParams.intra_idr_period >= NVENC_SCREEN_SHARE_MIN_RECOVERY_IDR_PERIOD &&
           !nvenc_use_ptd(enc);
}

static void nvenc_arm_recovery_idr(NVEncodeContext *enc)
{
    if (!enc) {
        return;
    }

    /* PTD=0 screen-share cannot use reconfigure.forceIDR, so queue the next picture as the recovery IDR. */
    enc->recoveryIdrPending = nvenc_should_arm_recovery_idr(enc);
}

typedef enum
{
    NVENC_RECONFIGURE_CLASS_NONE = 0,
    NVENC_RECONFIGURE_CLASS_FRAMERATE_ONLY,
    NVENC_RECONFIGURE_CLASS_BITRATE_QUALITY,
    NVENC_RECONFIGURE_CLASS_GOP_PROFILE,
    NVENC_RECONFIGURE_CLASS_SLICE_MODE,
} NVEncReconfigureClass;

static bool nvenc_same_runtime_reconfig(const NVEncodeContext *enc)
{
    if (!enc || !enc->haveAppliedReconfigSnapshot) {
        return false;
    }

    if (enc->appliedFrameRateNum != enc->initParams.frameRateNum ||
        enc->appliedFrameRateDen != enc->initParams.frameRateDen) {
        return false;
    }
    if (memcmp(&enc->appliedPresetGUID, &enc->initParams.presetGUID, sizeof(GUID)) != 0) {
        return false;
    }
    if (enc->appliedTuningInfo != enc->initParams.tuningInfo) {
        return false;
    }
    if (memcmp(&enc->appliedEncConfig, &enc->encConfig, sizeof(NV_ENC_CONFIG)) != 0) {
        return false;
    }
    return true;
}

static void nvenc_update_applied_reconfig_snapshot(NVEncodeContext *enc)
{
    if (!enc) {
        return;
    }
    enc->appliedEncConfig = enc->encConfig;
    enc->appliedFrameRateNum = enc->initParams.frameRateNum;
    enc->appliedFrameRateDen = enc->initParams.frameRateDen;
    enc->appliedPresetGUID = enc->initParams.presetGUID;
    enc->appliedTuningInfo = enc->initParams.tuningInfo;
    enc->haveAppliedReconfigSnapshot = true;
}

static NVEncReconfigureClass nvenc_reconfigure_reason_class(const NVEncodeContext *enc)
{
    if (!enc) {
        return NVENC_RECONFIGURE_CLASS_NONE;
    }
    switch ((NVEncReconfigureReason)enc->reconfigureReason) {
    case NVENC_RECONFIGURE_REASON_FRAMERATE:
        return NVENC_RECONFIGURE_CLASS_FRAMERATE_ONLY;
    case NVENC_RECONFIGURE_REASON_SEQ_BITRATE:
    case NVENC_RECONFIGURE_REASON_RATE_CONTROL:
    case NVENC_RECONFIGURE_REASON_HRD:
    case NVENC_RECONFIGURE_REASON_QUALITY:
        return NVENC_RECONFIGURE_CLASS_BITRATE_QUALITY;
    case NVENC_RECONFIGURE_REASON_SEQ_PARAMS:
    case NVENC_RECONFIGURE_REASON_RIR:
    case NVENC_RECONFIGURE_REASON_PACKED_SEI_POLICY:
        return NVENC_RECONFIGURE_CLASS_GOP_PROFILE;
    case NVENC_RECONFIGURE_REASON_QP_MAP_MODE:
    case NVENC_RECONFIGURE_REASON_SLICE_MODE:
        return NVENC_RECONFIGURE_CLASS_SLICE_MODE;
    case NVENC_RECONFIGURE_REASON_NONE:
    default:
        return NVENC_RECONFIGURE_CLASS_NONE;
    }
}

static uint32_t nvenc_applied_runtime_bitrate(const NVEncodeContext *enc)
{
    if (!enc || !enc->haveAppliedReconfigSnapshot) {
        return 0;
    }
    if (enc->appliedEncConfig.rcParams.maxBitRate > 0) {
        return enc->appliedEncConfig.rcParams.maxBitRate;
    }
    return enc->appliedEncConfig.rcParams.averageBitRate;
}

static uint32_t nvenc_pending_runtime_bitrate(const NVEncodeContext *enc)
{
    if (!enc) {
        return 0;
    }
    if (enc->reconfigureBpsHint > 0) {
        return enc->reconfigureBpsHint;
    }
    if (enc->encConfig.rcParams.maxBitRate > 0) {
        return enc->encConfig.rcParams.maxBitRate;
    }
    return enc->encConfig.rcParams.averageBitRate;
}

static uint32_t nvenc_reconfigure_debounce_frames(const NVEncodeContext *enc)
{
    uint32_t applied_bps = 0;
    uint32_t pending_bps = 0;
    uint32_t delta_bps = 0;

    if (!enc || !enc->initialized) {
        return NVENC_RECONFIG_DEBOUNCE_FRAMES;
    }
    if (!nvenc_is_screen_share(enc) || nvenc_use_ptd(enc)) {
        return NVENC_RECONFIG_DEBOUNCE_FRAMES;
    }
    if (enc->frameIdx < NVENC_SCREEN_SHARE_STEADY_STATE_RECONFIG_AFTER_FRAMES) {
        return NVENC_RECONFIG_DEBOUNCE_FRAMES;
    }
    if (nvenc_reconfigure_reason_class(enc) != NVENC_RECONFIGURE_CLASS_BITRATE_QUALITY) {
        return NVENC_RECONFIG_DEBOUNCE_FRAMES;
    }

    applied_bps = nvenc_applied_runtime_bitrate(enc);
    pending_bps = nvenc_pending_runtime_bitrate(enc);
    if (applied_bps == 0 || pending_bps == 0) {
        return NVENC_RECONFIG_DEBOUNCE_FRAMES;
    }

    delta_bps = applied_bps > pending_bps ?
                applied_bps - pending_bps :
                pending_bps - applied_bps;
    if (delta_bps >= NVENC_SCREEN_SHARE_STEADY_STATE_RECONFIG_FAST_DELTA_BPS) {
        return NVENC_RECONFIG_DEBOUNCE_FRAMES;
    }
    if ((uint64_t)delta_bps * 100u >=
        (uint64_t)applied_bps * NVENC_SCREEN_SHARE_STEADY_STATE_RECONFIG_FAST_DELTA_PERCENT) {
        return NVENC_RECONFIG_DEBOUNCE_FRAMES;
    }

    return NVENC_SCREEN_SHARE_STEADY_STATE_RECONFIG_DEBOUNCE_FRAMES;
}

static bool nvenc_should_arm_recovery_idr_for_reconfigure(const NVEncodeContext *enc)
{
    NVEncReconfigureClass reason_class = NVENC_RECONFIGURE_CLASS_NONE;

    if (!nvenc_should_arm_recovery_idr(enc)) {
        return false;
    }

    reason_class = nvenc_reconfigure_reason_class(enc);
    if (reason_class == NVENC_RECONFIGURE_CLASS_GOP_PROFILE) {
        return true;
    }

    /* Allow only a bounded startup recovery after early bitrate or framerate churn. */
    if (enc->frameIdx >= NVENC_SCREEN_SHARE_STARTUP_RUNTIME_RECOVERY_FRAMES) {
        return false;
    }

    return reason_class == NVENC_RECONFIGURE_CLASS_FRAMERATE_ONLY ||
           reason_class == NVENC_RECONFIGURE_CLASS_BITRATE_QUALITY;
}

static void nvenc_arm_recovery_idr_for_reconfigure(NVEncodeContext *enc)
{
    if (!enc || enc->recoveryIdrPending) {
        return;
    }

    enc->recoveryIdrPending = nvenc_should_arm_recovery_idr_for_reconfigure(enc);
}

static bool nvenc_should_emit_startup_recovery_idr(const NVEncodeContext *enc,
                                                   uint32_t frame_idx)
{
    if (!nvenc_should_arm_recovery_idr(enc)) {
        return false;
    }
    if (enc->encConfig.frameIntervalP > 1) {
        return false;
    }
    if (frame_idx == 0 || frame_idx >= NVENC_SCREEN_SHARE_STARTUP_PERIODIC_RECOVERY_FRAMES) {
        return false;
    }

    /* Emit a few early IDRs so decoder startup or handoff can recover without long-lived pumping. */
    return (frame_idx % NVENC_SCREEN_SHARE_STARTUP_PERIODIC_RECOVERY_INTERVAL) == 0;
}

static NV_ENC_PIC_TYPE nvenc_picture_type_from_va(uint8_t slice_type,
                                                  bool idr)
{
    if (idr) {
        return NV_ENC_PIC_TYPE_IDR;
    }
    switch (slice_type) {
    case 2: // I
    case 4: // SI
        return NV_ENC_PIC_TYPE_I;
    case 1: // B
        return NV_ENC_PIC_TYPE_B;
    case 0: // P
    case 3: // SP
    default:
        return NV_ENC_PIC_TYPE_P;
    }
}

static bool nvenc_can_skip_reconfigure(const NVEncodeContext *enc)
{
    if (!enc) {
        return false;
    }
    if (!nvenc_same_runtime_reconfig(enc)) {
        return false;
    }
    /* Keep skip rule intentionally narrow to avoid behavior regressions. */
    if (nvenc_reconfigure_reason_class(enc) != NVENC_RECONFIGURE_CLASS_FRAMERATE_ONLY) {
        return false;
    }
    return true;
}

static void nvenc_compute_encode_envelope(const NVEncodeContext *enc,
                                          uint32_t *max_width,
                                          uint32_t *max_height)
{
    uint32_t width = 0;
    uint32_t height = 0;

    if (enc) {
        width = enc->width;
        height = enc->height;

        if (enc->haveSeq) {
            if (enc->seqParams.picture_width_in_mbs > 0) {
                uint32_t coded_width = enc->seqParams.picture_width_in_mbs * 16u;
                if (coded_width > width) {
                    width = coded_width;
                }
            }
            if (enc->seqParams.picture_height_in_mbs > 0) {
                uint32_t coded_height = enc->seqParams.picture_height_in_mbs * 16u;
                if (coded_height > height) {
                    height = coded_height;
                }
            }
        }

        if (enc->context) {
            if (enc->context->width > 0 && (uint32_t)enc->context->width > width) {
                width = (uint32_t)enc->context->width;
            }
            if (enc->context->height > 0 && (uint32_t)enc->context->height > height) {
                height = (uint32_t)enc->context->height;
            }
        }
    }

    if (max_width) {
        *max_width = width;
    }
    if (max_height) {
        *max_height = height;
    }
}

static bool nvenc_refresh_runtime_dimensions(NVEncodeContext *enc)
{
    if (!enc) {
        return false;
    }

    nvenc_update_visible_dimensions(enc);
    if (enc->width == 0 || enc->height == 0) {
        return false;
    }

    uint32_t max_width = enc->width;
    uint32_t max_height = enc->height;
    nvenc_compute_encode_envelope(enc, &max_width, &max_height);

    if (enc->initialized) {
        if (max_width > enc->initParams.maxEncodeWidth ||
            max_height > enc->initParams.maxEncodeHeight) {
            LOG("NVENC reconfigure exceeds init envelope: visible %ux%u needs max %ux%u but init max is %ux%u",
                enc->width,
                enc->height,
                max_width,
                max_height,
                enc->initParams.maxEncodeWidth,
                enc->initParams.maxEncodeHeight);
            return false;
        }
    } else {
        enc->initParams.maxEncodeWidth = max_width;
        enc->initParams.maxEncodeHeight = max_height;
    }

    enc->initParams.encodeWidth = enc->width;
    enc->initParams.encodeHeight = enc->height;
    enc->initParams.darWidth = enc->width;
    enc->initParams.darHeight = enc->height;
    return true;
}

bool nvenc_has_unready_pending_output(NVEncodeContext *enc)
{
    if (!enc) {
        return false;
    }

    bool unready = false;
    pthread_mutex_lock(&enc->reorderMutex);
    for (uint32_t i = 0; i < enc->pendingOutputs.size; i++) {
        NVEncPendingOutput *pending = (NVEncPendingOutput*) get_element_at(&enc->pendingOutputs, i);
        if (pending && !pending->encodeReady) {
            unready = true;
            break;
        }
    }
    pthread_mutex_unlock(&enc->reorderMutex);
    return unready;
}

static int32_t nvenc_find_pending_output_index_locked(NVEncodeContext *enc,
                                                      const NVEncPendingOutput *pending)
{
    if (!enc || !pending) {
        return -1;
    }
    for (uint32_t i = 0; i < enc->pendingOutputs.size; i++) {
        if (get_element_at(&enc->pendingOutputs, i) == pending) {
            return (int32_t)i;
        }
    }
    return -1;
}

static bool nvenc_submit_eos(NVEncodeContext *enc)
{
    if (!enc || !enc->initialized || !enc->encoder || !enc->funcs) {
        return true;
    }

    NV_ENC_PIC_PARAMS pic = { 0 };
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.encodePicFlags = NV_ENC_PIC_FLAG_EOS;

    NVENCSTATUS st = enc->funcs->nvEncEncodePicture(enc->encoder, &pic);
    if (st != NV_ENC_SUCCESS && st != NV_ENC_ERR_NEED_MORE_INPUT) {
        LOG("NvEncEncodePicture(EOS) failed: %d", st);
        return false;
    }

    nvenc_mark_pending_ready(enc);
    return true;
}

static bool nvenc_setup_config(NVEncodeContext *enc)
{
    if (!enc->haveSeq || !enc->havePic) {
        return false;
    }

    nvenc_update_fixed_qp(enc);
    if (!enc->forceConservativeInit && !nvenc_is_screen_share(enc)) {
        uint32_t quality_level = enc->haveQuality ? enc->qualityParams.quality_level : 0;
        enc->initParams.presetGUID = nvenc_preset_from_quality_level(quality_level);
    }

    bool have_preset = false;
    if (enc->funcs) {
        NV_ENC_PRESET_CONFIG preset = { 0 };
        preset.version = NV_ENC_PRESET_CONFIG_VER;
        preset.presetCfg.version = NV_ENC_CONFIG_VER;
        NVENCSTATUS st = NV_ENC_ERR_UNSUPPORTED_PARAM;
        if (enc->funcs->nvEncGetEncodePresetConfigEx) {
            st = enc->funcs->nvEncGetEncodePresetConfigEx(enc->encoder,
                                                         NV_ENC_CODEC_H264_GUID,
                                                         enc->initParams.presetGUID,
                                                         enc->initParams.tuningInfo,
                                                         &preset);
        }
        if (st == NV_ENC_ERR_UNSUPPORTED_PARAM && enc->funcs->nvEncGetEncodePresetConfig) {
            st = enc->funcs->nvEncGetEncodePresetConfig(enc->encoder,
                                                        NV_ENC_CODEC_H264_GUID,
                                                        enc->initParams.presetGUID,
                                                        &preset);
        }
        if (st == NV_ENC_SUCCESS) {
            enc->encConfig = preset.presetCfg;
            have_preset = true;
        } else {
            LOG("NvEncGetEncodePresetConfig failed: %d", st);
        }
    }
    if (!have_preset) {
        memset(&enc->encConfig, 0, sizeof(enc->encConfig));
    }
    enc->encConfig.version = NV_ENC_CONFIG_VER;
    enc->encConfig.profileGUID = nvenc_h264_profile_guid(enc->profile);
    if (enc->forceConservativeInit) {
        memset(&enc->encConfig, 0, sizeof(enc->encConfig));
        enc->encConfig.version = NV_ENC_CONFIG_VER;
        enc->encConfig.profileGUID = nvenc_h264_profile_guid(enc->profile);
    }
    uint32_t fr_num = 0;
    uint32_t fr_den = 0;
    nvenc_get_framerate(enc, &fr_num, &fr_den);

    uint32_t fallback_gop = nvd_default_gop(fr_num, fr_den);
    if (fallback_gop == 0) {
        fallback_gop = NVENC_H264_DEFAULT_GOP;
    }

    uint32_t gop_length = 0;
    if (enc->seqParams.intra_period > 0) {
        gop_length = enc->seqParams.intra_period;
    } else if (enc->seqParams.intra_idr_period > 0) {
        gop_length = enc->seqParams.intra_idr_period;
    } else {
        if (fallback_gop > 0) {
            gop_length = fallback_gop;
            enc->seqParams.intra_period = gop_length;
            enc->seqParams.intra_idr_period = gop_length;
        }
    }
    enc->encConfig.gopLength = gop_length ? gop_length : NVENC_INFINITE_GOPLENGTH;
    uint32_t frame_interval = enc->seqParams.ip_period ? enc->seqParams.ip_period : 1;
    enc->encConfig.frameIntervalP = frame_interval;
    enc->encConfig.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    enc->encConfig.mvPrecision = NV_ENC_MV_PRECISION_DEFAULT;
    enc->encConfig.rcParams.version = NV_ENC_RC_PARAMS_VER;
    enc->encConfig.rcParams.rateControlMode = nvenc_rc_mode_from_va(enc->rcMode);

    uint32_t max_bps = 0;
    if (enc->encConfig.rcParams.rateControlMode != NV_ENC_PARAMS_RC_CONSTQP) {
        if (enc->haveRc && enc->rcParams.bits_per_second) {
            max_bps = enc->rcParams.bits_per_second;
        } else if (enc->seqParams.bits_per_second) {
            max_bps = enc->seqParams.bits_per_second;
        }
        if (max_bps == 0) {
            max_bps = 2000000;
        }
    }

    if (max_bps > 0) {
        uint32_t avg_bps = max_bps;
        if (enc->encConfig.rcParams.rateControlMode == NV_ENC_PARAMS_RC_VBR &&
            enc->haveRc && enc->rcParams.target_percentage) {
            uint64_t prod = (uint64_t)max_bps * enc->rcParams.target_percentage;
            uint32_t avg_floor = (uint32_t)(prod / 100);
            avg_bps = avg_floor;
            if (enc->rcParams.target_percentage < 100) {
                uint32_t min_bps = avg_floor;
                uint32_t max_bps_range = (uint32_t)(((uint64_t)max_bps * (enc->rcParams.target_percentage + 1)) / 100);
                if (max_bps_range > 0) {
                    max_bps_range -= 1;
                }
                uint32_t mid = min_bps + (max_bps_range - min_bps) / 2;
                const uint32_t snap = 100000;
                uint32_t candidate = (uint32_t)(((uint64_t)mid + (snap / 2)) / snap * snap);
                if (candidate >= min_bps && candidate <= max_bps_range) {
                    avg_bps = candidate;
                }
            }
        }
        if (enc->encConfig.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CBR) {
            avg_bps = max_bps;
        }

        enc->encConfig.rcParams.averageBitRate = avg_bps;
        enc->encConfig.rcParams.maxBitRate = max_bps;
    }

    if (enc->encConfig.rcParams.rateControlMode != NV_ENC_PARAMS_RC_CONSTQP && enc->haveHrd) {
        enc->encConfig.rcParams.vbvBufferSize = enc->hrdParams.buffer_size;
        /* Keep non-screen-share CBR on the old zero-delay init path; that preserved native parity. */
        if (nvenc_is_screen_share(enc)) {
            enc->encConfig.rcParams.vbvInitialDelay = enc->hrdParams.initial_buffer_fullness;
            if (enc->encConfig.rcParams.vbvInitialDelay > enc->encConfig.rcParams.vbvBufferSize) {
                enc->encConfig.rcParams.vbvInitialDelay = enc->encConfig.rcParams.vbvBufferSize;
            }
        } else {
            enc->encConfig.rcParams.vbvInitialDelay = 0;
        }
    }

    uint32_t qpP = enc->picParams.pic_init_qp;
    if (enc->haveRc && enc->rcParams.initial_qp) {
        qpP = enc->rcParams.initial_qp;
    }
    if (enc->haveQpP) {
        qpP = enc->qpP;
    }
    uint32_t qpI = enc->haveQpI ? enc->qpI : qpP;
    uint32_t qpB = enc->haveQpB ? enc->qpB : qpP;
    if (enc->encConfig.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CONSTQP) {
        if (qpI == qpP || qpB == qpP) {
            const double i_qfactor = 0.8;
            const double i_qoffset = 0.0;
            const double b_qfactor = 1.25;
            const double b_qoffset = 1.25;
            if (qpI == qpP) {
                qpI = nvenc_clamp_qp((int32_t)(qpP * i_qfactor + i_qoffset + 0.5));
            }
            if (qpB == qpP) {
                qpB = nvenc_clamp_qp((int32_t)(qpP * b_qfactor + b_qoffset + 0.5));
            }
        }
        enc->encConfig.rcParams.constQP = (NV_ENC_QP) { qpP, qpB, qpI };
    }
    if (enc->encConfig.rcParams.rateControlMode == NV_ENC_PARAMS_RC_VBR &&
        (!enc->haveRc || enc->rcParams.initial_qp == 0)) {
        int qp_inter_p = 26;
        if (enc->haveRc && enc->rcParams.min_qp && enc->rcParams.max_qp) {
            qp_inter_p = ((int)enc->rcParams.max_qp + 3 * (int)enc->rcParams.min_qp) / 4;
        } else if (enc->haveRc && enc->rcParams.min_qp) {
            qp_inter_p = (int)enc->rcParams.min_qp;
        } else if (qpP > 0) {
            qp_inter_p = (int)qpP;
        }
        qp_inter_p = (int)nvenc_clamp_qp(qp_inter_p);

        uint32_t qp_i = nvenc_clamp_qp((int32_t)(qp_inter_p * 0.8 + 0.0 + 0.5));
        uint32_t qp_b = nvenc_clamp_qp((int32_t)(qp_inter_p * 1.25 + 1.25 + 0.5));

        enc->encConfig.rcParams.enableInitialRCQP = 1;
        enc->encConfig.rcParams.initialRCQP = (NV_ENC_QP) { (uint32_t)qp_inter_p, qp_b, qp_i };
    }
    if (enc->encConfig.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CONSTQP) {
        uint32_t rc_bps = 0;
        if (enc->haveRc && enc->rcParams.bits_per_second) {
            rc_bps = enc->rcParams.bits_per_second;
        } else if (enc->seqParams.bits_per_second) {
            rc_bps = enc->seqParams.bits_per_second;
        }
        if (rc_bps == 0) {
            /* Match FFmpeg h264_nvenc default bitrate for constqp. */
            rc_bps = 2000000;
        }
        enc->encConfig.rcParams.averageBitRate = rc_bps;
        if (enc->encConfig.rcParams.vbvBufferSize == 0) {
            enc->encConfig.rcParams.vbvBufferSize = rc_bps * 2;
        }
    }
    if (enc->haveRc && enc->rcParams.min_qp) {
        enc->encConfig.rcParams.enableMinQP = 1;
        enc->encConfig.rcParams.minQP = (NV_ENC_QP) { enc->rcParams.min_qp, enc->rcParams.min_qp, enc->rcParams.min_qp };
    }
    if (enc->haveRc && enc->rcParams.max_qp) {
        enc->encConfig.rcParams.enableMaxQP = 1;
        enc->encConfig.rcParams.maxQP = (NV_ENC_QP) { enc->rcParams.max_qp, enc->rcParams.max_qp, enc->rcParams.max_qp };
    }
    if (enc->haveRc && enc->rcParams.initial_qp) {
        enc->encConfig.rcParams.enableInitialRCQP = 1;
        enc->encConfig.rcParams.initialRCQP = (NV_ENC_QP) { enc->rcParams.initial_qp, enc->rcParams.initial_qp, enc->rcParams.initial_qp };
    }
    enc->encConfig.rcParams.cbQPIndexOffset = enc->picParams.chroma_qp_index_offset;
    enc->encConfig.rcParams.crQPIndexOffset = enc->picParams.second_chroma_qp_index_offset;
    enc->encConfig.rcParams.qpMapMode = NV_ENC_QP_MAP_DISABLED;
    if (enc->roiMapMode != NV_ENC_QP_MAP_DISABLED) {
        if (!nvenc_query_encode_caps(enc->drv) || enc->drv->nvencCaps.supportEmphasisMap <= 0) {
            LOG("QP map requested but NVENC caps do not support emphasis map");
            enc->roiMapMode = NV_ENC_QP_MAP_DISABLED;
        } else {
            enc->encConfig.rcParams.qpMapMode = enc->roiMapMode;
        }
    }

    NV_ENC_CONFIG_H264 *h264 = &enc->encConfig.encodeCodecConfig.h264Config;
    h264->level = NV_ENC_LEVEL_AUTOSELECT;
    if (!enc->forceConservativeInit && enc->seqParams.level_idc) {
        uint32_t level = nvenc_h264_level_from_va(enc->seqParams.level_idc);
        /* Leave sub-2.0 requests on AUTOSELECT; NVENC rejects some valid low-level combinations at init. */
        if (level >= NV_ENC_LEVEL_H264_2) {
            h264->level = level;
        }
    }
    h264->idrPeriod = enc->seqParams.intra_idr_period ? enc->seqParams.intra_idr_period : enc->encConfig.gopLength;
    h264->repeatSPSPPS = 1;
    h264->outputBufferingPeriodSEI =
        (enc->packedSeiStreamMask & NVENC_H264_SEI_MASK_BUFFERING_PERIOD) ? 0 : 1;
    h264->outputPictureTimingSEI =
        (enc->packedSeiStreamMask & NVENC_H264_SEI_MASK_PIC_TIMING) ? 0 : 1;
    h264->outputAUD = 0;
    h264->outputRecoveryPointSEI = 0;
    h264->disableSPSPPS = 0;
    h264->chromaFormatIDC = 1;
    h264->h264VUIParameters.bitstreamRestrictionFlag =
        (enc->encConfig.gopLength != 1 || enc->profile != VAProfileH264High);
    h264->entropyCodingMode = enc->picParams.pic_fields.bits.entropy_coding_mode_flag ?
        NV_ENC_H264_ENTROPY_CODING_MODE_CABAC : NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;

    h264->enableIntraRefresh = 0;
    h264->intraRefreshPeriod = 0;
    h264->intraRefreshCnt = 0;

    h264->singleSliceIntraRefresh = 0;

    if (enc->haveRir && enc->rirParams.rir_flags.value) {
        if (!nvenc_query_encode_caps(enc->drv) || enc->drv->nvencCaps.supportIntraRefresh <= 0) {
            LOG("RIR requested but NVENC caps do not support intra refresh");
        } else {
            uint32_t mb_cols = nvenc_mb_width(enc->width);
            uint32_t mb_rows = nvenc_mb_height(enc->height);
            uint32_t units = mb_rows;
            if (enc->rirParams.rir_flags.bits.enable_rir_column) {
                units = mb_cols;
            } else if (enc->rirParams.rir_flags.bits.enable_rir_row) {
                units = mb_rows;
            }
            uint32_t size = enc->rirParams.intra_insert_size;
            if (size == 0) {
                size = 1;
            }
            uint32_t cnt = (units + size - 1) / size;
            if (cnt == 0) {
                cnt = 1;
            }
            uint32_t period = cnt + 1;
            if (period <= cnt) {
                period = cnt + 1;
            }
            h264->enableIntraRefresh = 1;
            h264->intraRefreshPeriod = period;
            h264->intraRefreshCnt = cnt;
            h264->outputRecoveryPointSEI = 1;
            if (enc->drv->nvencCaps.supportSingleSliceIntraRefresh > 0) {
                h264->singleSliceIntraRefresh = 1;
            }
        }
    }

    uint32_t slice_mode = 0;
    uint32_t slice_data = 0;
    if (nvenc_compute_slice_config(enc, &slice_mode, &slice_data)) {
        h264->sliceMode = slice_mode;
        h264->sliceModeData = slice_data;
    } else {
        h264->sliceMode = 0;
        h264->sliceModeData = 0;
    }
    enc->sliceModeConfigured = h264->sliceMode;
    enc->sliceModeDataConfigured = h264->sliceModeData;
    enc->sliceModeConfiguredValid = true;
    uint32_t desired_max_refs = enc->seqParams.max_num_ref_frames;
    uint32_t desired_l0 = 0;
    uint32_t desired_l1 = 0;

    if (enc->havePic) {
        desired_l0 = enc->picParams.num_ref_idx_l0_active_minus1 + 1;
        desired_l1 = enc->picParams.num_ref_idx_l1_active_minus1 + 1;
    }
    if (enc->haveSlice && enc->sliceParams.num_ref_idx_active_override_flag) {
        desired_l0 = enc->sliceParams.num_ref_idx_l0_active_minus1 + 1;
        desired_l1 = enc->sliceParams.num_ref_idx_l1_active_minus1 + 1;
    }
    if (enc->seqParams.ip_period <= 1) {
        desired_l1 = 0;
    }

    if (enc->encConfig.frameIntervalP > 1) {
        if (!nvenc_query_encode_caps(enc->drv) || enc->drv->nvencCaps.maxBFrames <= 0) {
            enc->encConfig.frameIntervalP = 1;
        } else if (h264->useBFramesAsRef == NV_ENC_BFRAME_REF_MODE_DISABLED) {
            if (enc->drv->nvencCaps.supportBframeRefMode > 0) {
                h264->useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_MIDDLE;
            }
        }
    }
    if (enc->encConfig.frameIntervalP <= 1) {
        h264->useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;
    }

    uint32_t effective_l0 = desired_l0;
    uint32_t effective_l1 = desired_l1;

    bool support_multi_refs = nvenc_query_encode_caps(enc->drv) &&
                              enc->drv->nvencCaps.supportMultipleRefFrames > 0;
    if (!support_multi_refs) {
        if (desired_max_refs > 1) {
            desired_max_refs = 1;
        }
        if (effective_l0 > 1) {
            effective_l0 = 1;
        }
        effective_l1 = 0;
    }
    if (enc->encConfig.frameIntervalP <= 1) {
        effective_l1 = 0;
    }

    uint32_t target_refs = desired_max_refs;
    bool honor_client_ref_counts = false;
    if (!honor_client_ref_counts &&
        !nvenc_use_ptd(enc) &&
        enc->encConfig.frameIntervalP <= 1 &&
        desired_max_refs > 1) {
        honor_client_ref_counts = true;
    }

    if (honor_client_ref_counts) {
        if (target_refs == 0) {
            target_refs = effective_l0;
        }
        if (target_refs < effective_l0) {
            target_refs = effective_l0;
        }
        if (target_refs > NVENC_H264_MAX_REFS) {
            target_refs = NVENC_H264_MAX_REFS;
        }
        h264->maxNumRefFrames = target_refs;
        h264->numRefL0 = effective_l0;
        h264->numRefL1 = effective_l1;
    } else {
        bool auto_refs = false;
        uint32_t auto_threshold = enc->encConfig.frameIntervalP > 1 ? 2 : 1;
        if (target_refs <= auto_threshold) {
            auto_refs = true;
            target_refs = 0;
        } else if (enc->encConfig.frameIntervalP > 1) {
            uint32_t min_refs = 4;
            if (target_refs < min_refs) {
                target_refs = min_refs;
            }
        } else {
            uint32_t min_refs = 3;
            if (target_refs < min_refs) {
                target_refs = min_refs;
            }
        }
        if (auto_refs) {
            effective_l0 = 0;
            effective_l1 = 0;
        } else {
            if (target_refs == 0) {
                target_refs = effective_l0;
            }
            if (target_refs < effective_l0) {
                target_refs = effective_l0;
            }
            if (target_refs > NVENC_H264_MAX_REFS) {
                target_refs = NVENC_H264_MAX_REFS;
            }
            if (h264->maxNumRefFrames == 0 || target_refs > h264->maxNumRefFrames) {
                h264->maxNumRefFrames = target_refs;
            }
        }
        if (effective_l0 > 0) {
            if (h264->numRefL0 == 0 || effective_l0 > h264->numRefL0) {
                h264->numRefL0 = effective_l0;
            }
        }
        if (effective_l1 > 0) {
            if (h264->numRefL1 == 0 || effective_l1 > h264->numRefL1) {
                h264->numRefL1 = effective_l1;
            }
        }
    }
    h264->useConstrainedIntraPred = enc->picParams.pic_fields.bits.constrained_intra_pred_flag;

    if (enc->forceConservativeInit || nvenc_is_screen_share(enc)) {
        enc->encConfig.frameIntervalP = 1;
        enc->encConfig.gopLength = NVENC_INFINITE_GOPLENGTH;
        h264->idrPeriod = NVENC_INFINITE_GOPLENGTH;
        h264->useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;
        /* Preserve client no-B ref counts for PTD=0 screen-share; zeroing them can grow the DPB and trip Chromium fallback. */
        if (enc->forceConservativeInit) {
            h264->maxNumRefFrames = 0;
            h264->numRefL0 = 0;
            h264->numRefL1 = 0;
        }
        enc->encConfig.rcParams.enableAQ = 0;
        enc->encConfig.rcParams.enableTemporalAQ = 0;
    }
    nvenc_apply_screen_share_low_latency_rc_policy(enc);

    return true;
}

static bool nvenc_init_encoder(NVEncodeContext *enc)
{
    bool screen_share = false;

retry_conservative:

    memset(&enc->initParams, 0, sizeof(enc->initParams));
    enc->enablePTD = nvenc_select_enable_ptd(enc);
    enc->initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    enc->initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
    enc->initParams.presetGUID = NV_ENC_PRESET_P4_GUID;
    if (!nvenc_refresh_runtime_dimensions(enc)) {
        LOG("NVENC init has invalid encode dimensions");
        return false;
    }
    nvenc_get_framerate(enc, &enc->initParams.frameRateNum, &enc->initParams.frameRateDen);
    screen_share = nvenc_is_screen_share(enc);
    if (enc->forceConservativeInit) {
        enc->initParams.presetGUID = NV_ENC_PRESET_P1_GUID;
        enc->initParams.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    } else if (screen_share) {
        /* Default screen-share to P5; recent 2K60 Electron/Vesktop probes showed intermittent remote corruption on P4 while P5 stayed clean. */
        enc->initParams.presetGUID = NV_ENC_PRESET_P5_GUID;
        enc->initParams.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    } else {
        enc->initParams.tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    }
    enc->initParams.enablePTD = enc->enablePTD ? 1 : 0;

    if (!nvenc_setup_config(enc)) {
        LOG("NVENC init missing sequence or picture params");
        return false;
    }

    enc->initParams.encodeConfig = &enc->encConfig;

    NVENCSTATUS st = enc->funcs->nvEncInitializeEncoder(enc->encoder, &enc->initParams);
    if (st != NV_ENC_SUCCESS) {
        if (st == NV_ENC_ERR_INVALID_PARAM && !enc->forceConservativeInit) {
            LOG("NvEncInitializeEncoder invalid param; retrying with conservative init profile");
            enc->forceConservativeInit = true;
            if (enc->encoder && enc->funcs && enc->funcs->nvEncDestroyEncoder) {
                NVENCSTATUS destroy_st = enc->funcs->nvEncDestroyEncoder(enc->encoder);
                if (destroy_st != NV_ENC_SUCCESS) {
                    LOG("NvEncDestroyEncoder before retry failed: %d", destroy_st);
                }
                enc->encoder = NULL;
            }
            if (!nvenc_context_open_session(enc, enc->drv->cudaContext)) {
                LOG("Failed to reopen NVENC session for conservative retry");
                return false;
            }
            goto retry_conservative;
        }
        LOG("NvEncInitializeEncoder failed: %d", st);
        return false;
    }
    enc->forceConservativeInit = false;
    enc->initialized = true;
    enc->reconfigure = false;
    nvenc_reset_bp_state(enc);
    nvenc_arm_recovery_idr(enc);
    nvenc_update_applied_reconfig_snapshot(enc);
    enc->framesSinceLastReconfigure = NVENC_RECONFIG_DEBOUNCE_FRAMES;
    return true;
}

static bool nvenc_reconfigure_encoder(NVEncodeContext *enc)
{
    if (!enc->reconfigure) {
        return true;
    }
    if (!nvenc_refresh_runtime_dimensions(enc)) {
        LOG("NVENC reconfigure has invalid encode dimensions");
        return false;
    }
    if (!nvenc_setup_config(enc)) {
        return false;
    }

    if (nvenc_can_skip_reconfigure(enc)) {
        enc->reconfigure = false;
        enc->reconfigureReason = NVENC_RECONFIGURE_REASON_NONE;
        enc->reconfigureBpsHint = 0;
        enc->framesSinceLastReconfigure = 0;
        return true;
    }

    NV_ENC_RECONFIGURE_PARAMS params = { 0 };
    params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    params.reInitEncodeParams = enc->initParams;
    params.reInitEncodeParams.encodeConfig = &enc->encConfig;
    if (nvd_reconfig_force_idr(enc)) {
        params.forceIDR = 1;
        params.resetEncoder = 1;
    }

    NVENCSTATUS st = enc->funcs->nvEncReconfigureEncoder(enc->encoder, &params);
    if (st != NV_ENC_SUCCESS) {
        LOG("NvEncReconfigureEncoder failed: %d", st);
        return false;
    }
    nvenc_update_applied_reconfig_snapshot(enc);
    nvenc_arm_recovery_idr_for_reconfigure(enc);
    enc->reconfigure = false;
    enc->reconfigureReason = NVENC_RECONFIGURE_REASON_NONE;
    enc->reconfigureBpsHint = 0;
    enc->framesSinceLastReconfigure = 0;
    return true;
}

static bool nvenc_get_direct_input_resource(const NVSurface *surface,
                                            CUdeviceptr *input_ptr,
                                            size_t *input_pitch)
{
    if (!surface) {
        return false;
    }
    if (surface->encDevPtr != 0 &&
        surface->encPitch != 0 &&
        nvenc_surface_uses_encode_path(surface) &&
        nvenc_surface_direct_input_is_current(surface)) {
        if (!surface->encDevPtr || !surface->encPitch) {
            return false;
        }
        if (input_ptr) {
            *input_ptr = surface->encDevPtr;
        }
        if (input_pitch) {
            *input_pitch = surface->encPitch;
        }
        return true;
    }
    if (!surface->backingImage) {
        return false;
    }

    const BackingImage *img = surface->backingImage;
    if (!img->linearPtr || img->format != NV_FORMAT_NV12) {
        return false;
    }
    if (img->width != surface->width || img->height != surface->height) {
        return false;
    }
    if (img->strides[0] <= 0 || img->strides[1] <= 0) {
        return false;
    }
    if (img->strides[0] != img->strides[1]) {
        return false;
    }
    if (img->offsets[0] < 0 || img->offsets[1] < img->offsets[0]) {
        return false;
    }

    size_t pitch = (size_t)img->strides[0];
    size_t base_offset = (size_t)img->offsets[0];
    size_t uv_offset = (size_t)img->offsets[1];
    if (uv_offset - base_offset != pitch * surface->height) {
        return false;
    }

    if (input_ptr) {
        *input_ptr = img->linearPtr + base_offset;
    }
    if (input_pitch) {
        *input_pitch = pitch;
    }
    return true;
}

bool nvenc_surface_can_direct_encode(const NVSurface *surface)
{
    return nvenc_get_direct_input_resource(surface, NULL, NULL);
}

static bool nvenc_copy_backing_to_linear(NVDriver *drv, NVSurface *surface)
{
    if (!drv || !surface) {
        return true;
    }
    if (!surface->encDevPtr) {
        return true;
    }
    if (!surface->backingImage) {
        return true;
    }

    NVFormat fmt = nvenc_surface_expected_format(surface);
    if (fmt == NV_FORMAT_NONE) {
        return true;
    }
    const NVFormatInfo *fmtInfo = &formatsInfo[fmt];
    if (fmtInfo->numPlanes == 0) {
        return true;
    }

    BackingImage *img = surface->backingImage;
    if (nvBackingImageIsHostMappedExternal(img)) {
        uint32_t y = 0;
        if (!nvSyncBackingImageHostAccess(img, false, true)) {
            return false;
        }
        for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
            const NVFormatPlane *p = &fmtInfo->plane[i];
            uint32_t plane_w = (surface->width >> p->ss.x) * fmtInfo->bppc * p->channelCount;
            uint32_t plane_h = surface->height >> p->ss.y;
            size_t src_pitch = img->strides[i] > 0 ? (size_t)img->strides[i] : (size_t)plane_w;
            const void *src_base = nv_get_host_external_plane_ptr(img, i);
            if (!src_base) {
                (void)nvSyncBackingImageHostAccess(img, false, false);
                return false;
            }
            CUDA_MEMCPY2D cpy = {
                .srcMemoryType = CU_MEMORYTYPE_HOST,
                .srcHost = src_base,
                .srcPitch = src_pitch,
                .dstMemoryType = CU_MEMORYTYPE_DEVICE,
                .dstDevice = surface->encDevPtr + (size_t)y * surface->encPitch,
                .dstPitch = surface->encPitch,
                .WidthInBytes = plane_w,
                .Height = plane_h
            };
            if (CHECK_CUDA_RESULT(cu->cuMemcpy2D(&cpy))) {
                (void)nvSyncBackingImageHostAccess(img, false, false);
                return false;
            }
            y += plane_h;
        }
        if (!nvSyncBackingImageHostAccess(img, false, false)) {
            return false;
        }
        nvenc_surface_mark_encode_input_written(surface, false);
        return true;
    }

    if (img->linearPtr) {
        uint32_t y = 0;
        for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
            const NVFormatPlane *p = &fmtInfo->plane[i];
            uint32_t plane_w = (surface->width >> p->ss.x) * fmtInfo->bppc * p->channelCount;
            uint32_t plane_h = surface->height >> p->ss.y;
            size_t src_pitch = img->strides[i] > 0 ? (size_t)img->strides[i] : (size_t)plane_w;
            size_t src_offset = img->offsets[i];
            CUDA_MEMCPY2D cpy = {
                .srcMemoryType = CU_MEMORYTYPE_DEVICE,
                .srcDevice = img->linearPtr + src_offset,
                .srcPitch = src_pitch,
                .dstMemoryType = CU_MEMORYTYPE_DEVICE,
                .dstDevice = surface->encDevPtr + (size_t)y * surface->encPitch,
                .dstPitch = surface->encPitch,
                .WidthInBytes = plane_w,
                .Height = plane_h
            };
            if (CHECK_CUDA_RESULT(cu->cuMemcpy2DAsync(&cpy, 0))) {
                return false;
            }
            y += plane_h;
        }
        if (CHECK_CUDA_RESULT(cu->cuStreamSynchronize(0))) {
            return false;
        }
        nvenc_surface_mark_encode_input_written(surface, false);
        return true;
    }

    uint32_t y = 0;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *p = &fmtInfo->plane[i];
        if (!img->arrays[i]) {
            LOG("NVENC backing image missing plane %u", i);
            return false;
        }
        CUDA_MEMCPY2D cpy = {
            .srcMemoryType = CU_MEMORYTYPE_ARRAY,
            .srcArray = img->arrays[i],
            .dstMemoryType = CU_MEMORYTYPE_DEVICE,
            .dstDevice = surface->encDevPtr + (size_t)y * surface->encPitch,
            .dstPitch = surface->encPitch,
            .WidthInBytes = (surface->width >> p->ss.x) * fmtInfo->bppc * p->channelCount,
            .Height = surface->height >> p->ss.y
        };
        if (CHECK_CUDA_RESULT(cu->cuMemcpy2DAsync(&cpy, 0))) {
            return false;
        }
        y += surface->height >> p->ss.y;
    }

    if (CHECK_CUDA_RESULT(cu->cuStreamSynchronize(0))) {
        return false;
    }

    nvenc_surface_mark_encode_input_written(surface, false);
    return true;
}

bool nvenc_ensure_surface_buffer(NVDriver *drv, NVSurface *surface)
{
    uint32_t alloc_height = surface->height + surface->height / 2;
    if (surface->encDevPtr && surface->encAllocHeight == alloc_height && surface->encPitch) {
        return true;
    }

    if (surface->encDevPtr) {
        cu->cuMemFree(surface->encDevPtr);
        surface->encDevPtr = 0;
        surface->encPitch = 0;
        surface->encAllocHeight = 0;
        surface->encDirectUploadLatest = false;
        surface->encInputValid = false;
    }

    size_t pitch = 0;
    CUdeviceptr ptr = 0;
    if (CHECK_CUDA_RESULT(cu->cuMemAllocPitch(&ptr, &pitch, surface->width, alloc_height, 16))) {
        return false;
    }
    size_t alloc_size = pitch * (size_t)alloc_height;
    if (alloc_size == 0) {
        cu->cuMemFree(ptr);
        return false;
    }
    surface->encDevPtr = ptr;
    surface->encPitch = pitch;
    surface->encAllocHeight = alloc_height;
    surface->encDirectUploadLatest = false;
    surface->encInputValid = false;
    if (!nvenc_fill_surface_black(surface)) {
        cu->cuMemFree(ptr);
        surface->encDevPtr = 0;
        surface->encPitch = 0;
        surface->encAllocHeight = 0;
        surface->encInputValid = false;
        return false;
    }
    return true;
}

static NVEncSurfaceReg *nvenc_find_surface_reg(NVEncodeContext *enc, VASurfaceID surface_id)
{
    ARRAY_FOR_EACH(NVEncSurfaceReg*, reg, &enc->registeredSurfaces)
        if (reg->surfaceId == surface_id) {
            return reg;
        }
    END_FOR_EACH
    return NULL;
}

static NVEncSurfaceReg *nvenc_register_surface(NVEncodeContext *enc,
                                               NVSurface *surface,
                                               NVContext *nvCtx,
                                               VASurfaceID surface_id,
                                               CUdeviceptr input_ptr,
                                               size_t input_pitch)
{
    if (surface->encReg && surface->encCtx && surface->encCtx != nvCtx) {
        NVEncSurfaceReg *oldReg = surface->encReg;
        NVEncodeContext *old = surface->encCtx->enc;
        if (old) {
            nvenc_unregister_surface(old, oldReg);
            nvenc_remove_surface_reg(old, oldReg);
            free(oldReg);
        }
        surface->encReg = NULL;
        surface->encCtx = NULL;
    }

    if (!input_ptr || !input_pitch) {
        return NULL;
    }

    NVEncSurfaceReg *reg = nvenc_find_surface_reg(enc, surface_id);
    if (reg) {
        if (reg->inputPtr == input_ptr &&
            reg->inputPitch == input_pitch) {
            return reg;
        }
        nvenc_unregister_surface(enc, reg);
        nvenc_remove_surface_reg(enc, reg);
        free(reg);
        reg = NULL;
    }

    NV_ENC_REGISTER_RESOURCE regParams = { 0 };
    regParams.version = NV_ENC_REGISTER_RESOURCE_VER;
    regParams.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    regParams.resourceToRegister = (void*)input_ptr;
    regParams.width = surface->width;
    regParams.height = surface->height;
    regParams.pitch = (uint32_t)input_pitch;
    regParams.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    regParams.bufferUsage = NV_ENC_INPUT_IMAGE;

    NVENCSTATUS st = enc->funcs->nvEncRegisterResource(enc->encoder, &regParams);
    if (st != NV_ENC_SUCCESS) {
        LOG("NvEncRegisterResource failed: %d", st);
        return NULL;
    }

    reg = (NVEncSurfaceReg*) calloc(1, sizeof(NVEncSurfaceReg));
    if (!reg) {
        enc->funcs->nvEncUnregisterResource(enc->encoder, regParams.registeredResource);
        return NULL;
    }
    reg->surfaceId = surface_id;
    reg->registered = regParams.registeredResource;
    reg->inputPtr = input_ptr;
    reg->inputPitch = input_pitch;
    add_element(&enc->registeredSurfaces, reg);

    surface->encReg = reg;
    surface->encCtx = nvCtx;
    return reg;
}

bool nvenc_submit_queued_pic(NVDriver *drv, NVContext *nvCtx, NVEncQueuedPic *queued)
{
    if (!drv || !nvCtx || !queued) {
        return false;
    }
    NVEncodeContext *enc = nvCtx->enc;
    if (!enc) {
        return false;
    }

    NVSurface *surface = queued->surface;
    NVBuffer *codedBuf = queued->codedBuf;
    if (!surface || !codedBuf) {
        return false;
    }

    CUdeviceptr input_ptr = 0;
    size_t input_pitch = 0;
    bool direct_input = nvenc_get_direct_input_resource(surface, &input_ptr, &input_pitch);
    if (!direct_input) {
        if (!nvenc_ensure_surface_buffer(drv, surface)) {
            return false;
        }
        if (!nvenc_copy_backing_to_linear(drv, surface)) {
            return false;
        }
        if (surface->encCopyEventPending && surface->encCopyEventValid) {
            if (CHECK_CUDA_RESULT(cu->cuEventSynchronize(surface->encCopyEvent))) {
                return false;
            }
            surface->encCopyEventPending = false;
        }
        input_ptr = surface->encDevPtr;
        input_pitch = surface->encPitch;
    }
    surface->encLastEncodedSeq = surface->encUploadSeq;

    NV_ENC_QP_MAP_MODE desired_qp_map = queued->qpMapMode;
    if (enc->roiMapMode != desired_qp_map) {
        enc->roiMapMode = desired_qp_map;
    }
    if (enc->initialized &&
        enc->encConfig.rcParams.qpMapMode != desired_qp_map) {
        uint32_t bps = nvenc_reconfig_bps_hint(enc, enc->rcParams.bits_per_second);
        nvenc_force_reconfigure(enc, NVENC_RECONFIGURE_REASON_QP_MAP_MODE, bps);
    }

    if (!enc->initialized) {
        if (!nvenc_init_encoder(enc)) {
            return false;
        }
    } else if (enc->reconfigure) {
        if (enc->framesSinceLastReconfigure >= nvenc_reconfigure_debounce_frames(enc)) {
            if (!nvenc_reconfigure_encoder(enc)) {
                return false;
            }
        }
    }

    NVEncSurfaceReg *reg = nvenc_register_surface(enc, surface, nvCtx, queued->surfaceId,
                                                  input_ptr, input_pitch);
    if (!reg) {
        return false;
    }

    NV_ENC_MAP_INPUT_RESOURCE map = { 0 };
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = reg->registered;
    NVENCSTATUS st = enc->funcs->nvEncMapInputResource(enc->encoder, &map);
    if (st != NV_ENC_SUCCESS) {
        LOG("NvEncMapInputResource failed: %d", st);
        return false;
    }
    NVEncPendingOutput *pending = (NVEncPendingOutput*) calloc(1, sizeof(NVEncPendingOutput));
    if (!pending) {
        enc->funcs->nvEncUnmapInputResource(enc->encoder, map.mappedResource);
        return false;
    }
    pending->surface = surface;
    pending->codedBuf = codedBuf;
    pending->bitstream = NULL;
    pending->encodeReady = false;
    pending->releaseBitstreamOnCleanup = true;

    pending->bitstream = nvenc_bitstream_acquire(enc);
    if (!pending->bitstream) {
        free(pending);
        enc->funcs->nvEncUnmapInputResource(enc->encoder, map.mappedResource);
        return false;
    }
    pthread_mutex_lock(&enc->reorderMutex);
    add_element(&enc->pendingOutputs, pending);
    pthread_mutex_unlock(&enc->reorderMutex);
    codedBuf->encCtx = enc;

    NV_ENC_PIC_PARAMS pic = { 0 };
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = map.mappedBufferFmt != NV_ENC_BUFFER_FORMAT_UNDEFINED ?
                    map.mappedBufferFmt :
                    NV_ENC_BUFFER_FORMAT_NV12;
    pic.inputWidth = enc->width;
    pic.inputHeight = enc->height;
    pic.inputPitch = (uint32_t)input_pitch;
    pic.outputBitstream = pending->bitstream;
    pic.frameIdx = enc->frameIdx++;
    pic.inputTimeStamp = queued->displayTs;
    pic.inputDuration = 0;
    if (queued->qpMapMode != NV_ENC_QP_MAP_DISABLED) {
        if (queued->qpDeltaMap && queued->qpDeltaMapSize > 0) {
            pic.qpDeltaMap = queued->qpDeltaMap;
            pic.qpDeltaMapSize = queued->qpDeltaMapSize;
        } else {
            LOG("QP map mode %u enabled but qpDeltaMap missing", queued->qpMapMode);
        }
    }
    pic.codecPicParams.h264PicParams.sliceMode =
        enc->encConfig.encodeCodecConfig.h264Config.sliceMode;
    pic.codecPicParams.h264PicParams.sliceModeData =
        enc->encConfig.encodeCodecConfig.h264Config.sliceModeData;
    if (queued->seiPayloads && queued->seiPayloadCount > 0) {
        pic.codecPicParams.h264PicParams.seiPayloadArray = queued->seiPayloads;
        pic.codecPicParams.h264PicParams.seiPayloadArrayCnt = queued->seiPayloadCount;
    }
    if (enc->haveRir &&
        enc->encConfig.encodeCodecConfig.h264Config.enableIntraRefresh &&
        !enc->rirTriggered &&
        enc->encConfig.frameIntervalP <= 1) {
        uint32_t cnt = enc->encConfig.encodeCodecConfig.h264Config.intraRefreshCnt;
        if (cnt == 0) {
            cnt = 1;
        }
        pic.codecPicParams.h264PicParams.forceIntraRefreshWithFrameCnt = cnt;
        enc->rirTriggered = true;
    }
    pic.codecPicParams.h264PicParams.refPicFlag = queued->refPicFlag ? 1 : 0;
    if (!nvenc_use_ptd(enc)) {
        pic.pictureType = nvenc_picture_type_from_va(queued->sliceType, queued->forceIdr);
        pic.codecPicParams.h264PicParams.displayPOCSyntax = queued->displayPoc;
        pic.codecPicParams.h264PicParams.refPicFlag = queued->refPicFlag ? 1 : 0;
    }
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    bool force_idr = queued->forceIdr;
    bool auto_idr = false;
    bool recovery_idr = false;
    if (!force_idr &&
        enc->encConfig.frameIntervalP <= 1 &&
        enc->encConfig.gopLength != NVENC_INFINITE_GOPLENGTH &&
        enc->encConfig.gopLength > 0 &&
        pic.frameIdx != 0 &&
        (pic.frameIdx % enc->encConfig.gopLength) == 0) {
        auto_idr = true;
    }
    if (enc->recoveryIdrPending) {
        recovery_idr = true;
        enc->recoveryIdrPending = false;
    }
    if (!force_idr &&
        !auto_idr &&
        !recovery_idr &&
        nvenc_should_emit_startup_recovery_idr(enc, pic.frameIdx)) {
        recovery_idr = true;
    }
    if (force_idr && (auto_idr || recovery_idr)) {
        auto_idr = false;
        recovery_idr = false;
    }
    if (auto_idr && recovery_idr) {
        recovery_idr = false;
    }
    /* Preserve the client GOP timeline, but allow one bounded recovery IDR after startup or reconfigure churn. */
    if (force_idr || auto_idr || recovery_idr) {
        if (nvenc_use_ptd(enc)) {
            pic.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        } else {
            pic.pictureType = NV_ENC_PIC_TYPE_IDR;
            pic.encodePicFlags |= NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        }
    }
    pending->qpDeltaMap = queued->qpDeltaMap;
    pending->qpDeltaMapSize = queued->qpDeltaMapSize;
    queued->qpDeltaMap = NULL;
    queued->qpDeltaMapSize = 0;

    st = enc->funcs->nvEncEncodePicture(enc->encoder, &pic);
    enc->funcs->nvEncUnmapInputResource(enc->encoder, map.mappedResource);
    if (st == NV_ENC_SUCCESS) {
        nvenc_mark_pending_ready(enc);
    }
    if (st == NV_ENC_ERR_NEED_MORE_INPUT) {
        return true;
    }
    if (st != NV_ENC_SUCCESS) {
        LOG("NvEncEncodePicture failed: %d", st);
        nvenc_clear_pending_outputs(enc);
        return false;
    }
    if (enc->framesSinceLastReconfigure < UINT32_MAX) {
        enc->framesSinceLastReconfigure++;
    }
    return true;
}

void nvenc_process_reorder_queue(NVDriver *drv, NVContext *nvCtx, bool flush)
{
    if (!drv || !nvCtx) {
        return;
    }
    NVEncodeContext *enc = nvCtx->enc;
    if (!enc) {
        return;
    }
    pthread_mutex_lock(&enc->queueMutex);
    if (!nvenc_use_internal_reorder(enc) || enc->queuedPics.size == 0) {
        pthread_mutex_unlock(&enc->queueMutex);
        return;
    }

    uint32_t poc_step = 1;
    if (enc->haveSeq && enc->seqParams.ip_period <= 1) {
        poc_step = 2;
    }

    if (!enc->haveNextDisplayPoc) {
        uint32_t min_poc = UINT32_MAX;
        ARRAY_FOR_EACH(NVEncQueuedPic*, queued, &enc->queuedPics)
            if (queued && queued->displayPoc < min_poc) {
                min_poc = queued->displayPoc;
            }
        END_FOR_EACH
        if (min_poc != UINT32_MAX) {
            enc->nextDisplayPoc = min_poc;
            enc->haveNextDisplayPoc = true;
        }
    }

    bool progressed = true;
    while (progressed && enc->queuedPics.size > 0) {
        progressed = false;
        uint32_t idx = 0;
        NVEncQueuedPic *queued = nvenc_find_queued_by_poc(enc, enc->nextDisplayPoc, &idx);
        if (!queued && flush) {
            uint32_t min_idx = 0;
            uint32_t min_poc = UINT32_MAX;
            for (uint32_t i = 0; i < enc->queuedPics.size; i++) {
                NVEncQueuedPic *cand = (NVEncQueuedPic*) get_element_at(&enc->queuedPics, i);
                if (cand && cand->displayPoc < min_poc) {
                    min_poc = cand->displayPoc;
                    min_idx = i;
                }
            }
            if (min_poc != UINT32_MAX) {
                enc->nextDisplayPoc = min_poc;
                queued = (NVEncQueuedPic*) get_element_at(&enc->queuedPics, min_idx);
                idx = min_idx;
            }
        }
        if (queued) {
            remove_element_at(&enc->queuedPics, idx);
            bool ok = nvenc_submit_queued_pic(drv, nvCtx, queued);
            if (!ok) {
                if (queued->codedBuf) {
                    queued->codedBuf->encCtx = NULL;
                    pthread_mutex_lock(&enc->reorderMutex);
                    for (uint32_t i = 0; i < enc->reorderEncode.size;) {
                        NVEncReorderEntry *entry = (NVEncReorderEntry*) get_element_at(&enc->reorderEncode, i);
                        if (!entry || entry->codedBuf != queued->codedBuf) {
                            i++;
                            continue;
                        }
                        free(entry);
                        remove_element_at(&enc->reorderEncode, i);
                    }
                    pthread_cond_broadcast(&enc->reorderCond);
                    pthread_mutex_unlock(&enc->reorderMutex);
                }
                nvenc_clear_surface_resolving(queued->surface);
                nvenc_free_queued_pic(queued);
                pthread_mutex_unlock(&enc->queueMutex);
                return;
            }
            nvenc_free_queued_pic(queued);
            enc->nextDisplayPoc += poc_step;
            progressed = true;
        }
    }

    if (enc->queuedPics.size == 0 && flush) {
        enc->haveNextDisplayPoc = false;
    }
    pthread_mutex_unlock(&enc->queueMutex);
}

VAStatus nvenc_drain_encoder(NVDriver *drv, NVContext *nvCtx)
{
    if (!drv || !nvCtx || !nvCtx->enc) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    NVEncodeContext *enc = nvCtx->enc;
    bool eos_submitted = false;

    while (nvenc_has_queued_pics(enc) || nvenc_has_pending_outputs(enc)) {
        if (nvenc_has_queued_pics(enc)) {
            nvenc_process_reorder_queue(drv, nvCtx, true);
        }

        if (!nvenc_has_queued_pics(enc) &&
            nvenc_has_pending_outputs(enc) &&
            nvenc_has_unready_pending_output(enc) && !eos_submitted) {
            if (!nvenc_submit_eos(enc)) {
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            eos_submitted = true;
        }

        while (nvenc_has_pending_outputs(enc)) {
            VAStatus st = nvenc_resolve_pending_output_ex(enc, 0, false, NULL);
            if (st != VA_STATUS_SUCCESS) {
                return st;
            }
        }
    }

    return VA_STATUS_SUCCESS;
}

VAStatus nvenc_resolve_pending_output_ex(NVEncodeContext *enc, uint32_t pending_idx, bool do_not_wait, bool *busy)
{
    if (!enc) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (busy) {
        *busy = false;
    }
    NVEncPendingOutput *pending = NULL;
    pthread_mutex_lock(&enc->reorderMutex);
    while (true) {
        if (pending_idx >= enc->pendingOutputs.size) {
            pthread_mutex_unlock(&enc->reorderMutex);
            return VA_STATUS_SUCCESS;
        }
        pending = (NVEncPendingOutput*) get_element_at(&enc->pendingOutputs, pending_idx);
        if (!pending || pending->detached) {
            pthread_mutex_unlock(&enc->reorderMutex);
            return VA_STATUS_SUCCESS;
        }
        if (!pending->codedBuf || !pending->surface || !pending->bitstream) {
            pthread_mutex_unlock(&enc->reorderMutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (!pending->encodeReady || pending->resolving) {
            if (do_not_wait) {
                pthread_mutex_unlock(&enc->reorderMutex);
                if (busy) {
                    *busy = true;
                }
                return VA_STATUS_SUCCESS;
            }
            pthread_cond_wait(&enc->reorderCond, &enc->reorderMutex);
            continue;
        }
        pending->resolving = true;
        break;
    }
    pthread_mutex_unlock(&enc->reorderMutex);

    NV_ENC_LOCK_BITSTREAM lock = { 0 };
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = pending->bitstream;
    lock.doNotWait = do_not_wait ? 1 : 0;

    VAStatus ret = VA_STATUS_SUCCESS;
    bool bitstream_locked = false;
    bool complete_pending = false;
    NVENCSTATUS st = enc->funcs->nvEncLockBitstream(enc->encoder, &lock);
    if (st == NV_ENC_ERR_LOCK_BUSY && do_not_wait) {
        if (busy) {
            *busy = true;
        }
        goto finish_pending;
    }
    if (st != NV_ENC_SUCCESS) {
        LOG("NvEncLockBitstream failed: %d", st);
        ret = VA_STATUS_ERROR_OPERATION_FAILED;
        goto finish_pending;
    }
    bitstream_locked = true;
    bool skip_output = false;
    NVBuffer *outBuf = pending->codedBuf;
    pthread_mutex_lock(&enc->reorderMutex);
    skip_output = pending->detached;
    if (!skip_output && nvenc_use_internal_reorder(enc)) {
        NVEncReorderEntry *entry = NULL;
        uint32_t entry_idx = 0;
        if (lock.pictureType == NV_ENC_PIC_TYPE_B) {
            uint32_t best_idx = UINT32_MAX;
            uint32_t out_group = nvenc_reorder_group_id(enc, (uint32_t)lock.outputTimeStamp, false);
            uint64_t best_ts = UINT64_MAX;
            for (uint32_t i = 0; i < enc->reorderEncode.size; i++) {
                NVEncReorderEntry *cand = (NVEncReorderEntry*) get_element_at(&enc->reorderEncode, i);
                if (!cand || !cand->is_b || cand->reorderGroupId != out_group) {
                    continue;
                }
                if (cand->displayTs < best_ts) {
                    best_ts = cand->displayTs;
                    best_idx = i;
                }
            }
            if (best_idx != UINT32_MAX) {
                entry_idx = best_idx;
                entry = (NVEncReorderEntry*) get_element_at(&enc->reorderEncode, best_idx);
            }
        } else {
            entry = nvenc_find_reorder_entry(enc, (uint64_t)lock.outputTimeStamp, &entry_idx);
        }
        if (entry && entry->codedBuf) {
            outBuf = entry->codedBuf;
            remove_element_at(&enc->reorderEncode, entry_idx);
            free(entry);
        }
    }
    pthread_mutex_unlock(&enc->reorderMutex);

    if (!skip_output) {
        size_t prefix = outBuf->packedHeaderSize;
        size_t bytes = lock.bitstreamSizeInBytes + prefix;
        if (bytes > outBuf->codedAllocated) {
            size_t new_alloc = outBuf->codedAllocated ? outBuf->codedAllocated : 4096;
            while (new_alloc < bytes) {
                size_t grow = new_alloc + (new_alloc >> 1);
                if (grow <= new_alloc) {
                    new_alloc = bytes;
                    break;
                }
                new_alloc = grow;
            }
            if (new_alloc < bytes) {
                new_alloc = bytes;
            }
            uint8_t *tmp = realloc(outBuf->codedBuf, new_alloc);
            if (!tmp) {
                ret = VA_STATUS_ERROR_ALLOCATION_FAILED;
                goto finish_pending;
            }
            outBuf->codedBuf = tmp;
            outBuf->codedAllocated = new_alloc;
        }
        if (prefix > 0 && outBuf->packedHeader) {
            memcpy(outBuf->codedBuf, outBuf->packedHeader, prefix);
        }
        memcpy(outBuf->codedBuf + prefix, lock.bitstreamBufferPtr, lock.bitstreamSizeInBytes);
        outBuf->codedSize = bytes;
        if (isH264EncodeProfile(enc->profile) &&
            !nvenc_use_ptd(enc) &&
            enc->encConfig.frameIntervalP <= 1 &&
            (outBuf->packedSpsSize > 0 || outBuf->packedPpsSize > 0)) {
            bool rewritten = false;
            VAStatus rewrite_st = nvenc_rewrite_h264_parameter_sets_in_coded_buffer(
                enc,
                outBuf,
                prefix,
                &rewritten);
            if (rewrite_st != VA_STATUS_SUCCESS) {
                ret = rewrite_st;
                goto finish_pending;
            }
        }
        outBuf->codedReady = true;
        outBuf->encCtx = NULL;
        if (enc->patchBpSei &&
            isH264EncodeProfile(enc->profile) &&
            (enc->encConfig.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CONSTQP ||
             enc->encConfig.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CBR) &&
            outBuf->codedSize > prefix) {
            size_t payload_bytes = outBuf->codedSize - prefix;
            uint64_t bits_current = (uint64_t)payload_bytes * 8u;
            bool patched = nvenc_patch_h264_bp_sei(enc,
                                                   outBuf->codedBuf + prefix,
                                                   payload_bytes,
                                                   enc->bpBitsSince);
            if (patched) {
                enc->bpBitsSince = 0;
            }
            enc->bpBitsSince += bits_current;
        }
        if (outBuf->packedHeader) {
            free(outBuf->packedHeader);
            outBuf->packedHeader = NULL;
            outBuf->packedHeaderSize = 0;
        }
    }

    complete_pending = true;

finish_pending:
    if (bitstream_locked) {
        enc->funcs->nvEncUnlockBitstream(enc->encoder, pending->bitstream);
    }

    pthread_mutex_lock(&enc->reorderMutex);
    pending->resolving = false;
    bool detached = pending->detached;
    if (complete_pending || detached) {
        int32_t remove_idx = nvenc_find_pending_output_index_locked(enc, pending);
        if (remove_idx >= 0) {
            remove_element_at(&enc->pendingOutputs, (uint32_t)remove_idx);
        }
        pending->detached = true;
        detached = true;
    }
    pthread_cond_broadcast(&enc->reorderCond);
    pthread_mutex_unlock(&enc->reorderMutex);

    if (complete_pending || detached) {
        nvenc_cleanup_pending_output(enc, pending);
    }
    return ret;
}
