#include "encode_common.h"
#include "h264_encode.h"
#include <stdlib.h>
#include <string.h>

extern CudaFunctions *cu;

bool nvenc_use_ptd(const NVEncodeContext *enc)
{
    return !enc || enc->enablePTD;
}

bool nvenc_select_enable_ptd(const NVEncodeContext *enc)
{
    if (!enc) {
        return true;
    }
    if (isH264EncodeProfile(enc->profile) && enc->havePic && enc->haveSlice) {
        /*
         * VA-API H.264 clients already submit explicit picture type / POC /
         * ref metadata. FFmpeg issues B-frame jobs in encoding order and can
         * synchronously wait on the next reference picture buffer before later
         * B pictures have been queued, so enabling NVENC PTD here deadlocks the
         * stream by forcing a display-order input contract. Keep PTD disabled
         * and submit in client encode order instead.
         *
         * Chromium stays on the same client-driven path as well: it uses
         * ip_period=1, provides explicit picture params, and does not rely on
         * NVENC to choose frame types.
         */
        return false;
    }

    return true;
}

bool nvenc_use_internal_reorder(const NVEncodeContext *enc)
{
    return enc && enc->enablePTD && enc->haveSeq && enc->seqParams.ip_period > 1;
}

void nvenc_reset_appendable_buffer(AppendableBuffer *buffer)
{
    if (!buffer) {
        return;
    }

    free(buffer->buf);
    buffer->buf = NULL;
    buffer->size = 0;
    buffer->allocated = 0;
}

NVFormat nvenc_surface_format_to_nv_format(cudaVideoSurfaceFormat surface_format,
                                           int bitdepth)
{
    switch (surface_format) {
    case cudaVideoSurfaceFormat_NV12:
        return NV_FORMAT_NV12;
    case cudaVideoSurfaceFormat_P016:
        if (bitdepth == 10) {
            return NV_FORMAT_P010;
        }
        if (bitdepth == 12) {
            return NV_FORMAT_P012;
        }
        return NV_FORMAT_P016;
    case cudaVideoSurfaceFormat_YUV444:
        return NV_FORMAT_444P;
    case cudaVideoSurfaceFormat_YUV444_16Bit:
        return NV_FORMAT_Q416;
    default:
        break;
    }
    return NV_FORMAT_NONE;
}

NVFormat nvenc_surface_expected_format(const NVSurface *surface)
{
    if (!surface) {
        return NV_FORMAT_NONE;
    }
    if (surface->backingImage && surface->backingImage->format != NV_FORMAT_NONE) {
        return surface->backingImage->format;
    }
    return nvenc_surface_format_to_nv_format(surface->format, surface->bitDepth);
}

bool nvenc_query_encode_caps(NVDriver *drv)
{
    if (!drv->nvencAvailable || !drv->nvenc) {
        return false;
    }

    pthread_mutex_lock(&drv->nvencCapsMutex);
    if (drv->nvencCaps.valid) {
        pthread_mutex_unlock(&drv->nvencCapsMutex);
        return true;
    }

    pthread_mutex_lock(&drv->cudaMutex);
    if (CHECK_CUDA_RESULT(cu->cuCtxPushCurrent(drv->cudaContext))) {
        pthread_mutex_unlock(&drv->cudaMutex);
        pthread_mutex_unlock(&drv->nvencCapsMutex);
        return false;
    }

    void *encoder = NULL;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams = { 0 };
    openParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    openParams.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    openParams.device = drv->cudaContext;
    openParams.apiVersion = NVENCAPI_VERSION;
    NVENCSTATUS st = drv->nvencFuncs.nvEncOpenEncodeSessionEx(&openParams, &encoder);
    if (st != NV_ENC_SUCCESS || !encoder) {
        LOG("NvEncOpenEncodeSessionEx (caps) failed: %d", st);
        pthread_mutex_unlock(&drv->nvencCapsMutex);
        CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL));
        pthread_mutex_unlock(&drv->cudaMutex);
        return false;
    }

    struct {
        NV_ENC_CAPS caps;
        int *out;
    } queries[] = {
        { NV_ENC_CAPS_SUPPORT_INTRA_REFRESH, &drv->nvencCaps.supportIntraRefresh },
        { NV_ENC_CAPS_SINGLE_SLICE_INTRA_REFRESH, &drv->nvencCaps.supportSingleSliceIntraRefresh },
        { NV_ENC_CAPS_SUPPORT_DYNAMIC_SLICE_MODE, &drv->nvencCaps.supportDynamicSliceMode },
        { NV_ENC_CAPS_SUPPORT_EMPHASIS_LEVEL_MAP, &drv->nvencCaps.supportEmphasisMap },
        { NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE, &drv->nvencCaps.supportCustomVbvBufSize },
        { NV_ENC_CAPS_SUPPORT_LOOKAHEAD, &drv->nvencCaps.supportLookahead },
        { NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ, &drv->nvencCaps.supportTemporalAQ },
        { NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION, &drv->nvencCaps.supportWeightedPrediction },
        { NV_ENC_CAPS_SUPPORT_MULTIPLE_REF_FRAMES, &drv->nvencCaps.supportMultipleRefFrames },
        { NV_ENC_CAPS_SUPPORT_BFRAME_REF_MODE, &drv->nvencCaps.supportBframeRefMode },
        { NV_ENC_CAPS_NUM_MAX_BFRAMES, &drv->nvencCaps.maxBFrames },
        { NV_ENC_CAPS_WIDTH_MAX, &drv->nvencCaps.widthMax },
        { NV_ENC_CAPS_HEIGHT_MAX, &drv->nvencCaps.heightMax },
        { NV_ENC_CAPS_WIDTH_MIN, &drv->nvencCaps.widthMin },
        { NV_ENC_CAPS_HEIGHT_MIN, &drv->nvencCaps.heightMin },
    };

    for (size_t i = 0; i < ARRAY_SIZE(queries); i++) {
        NV_ENC_CAPS_PARAM capsParam = { 0 };
        capsParam.version = NV_ENC_CAPS_PARAM_VER;
        capsParam.capsToQuery = queries[i].caps;
        int capValue = 0;
        NVENCSTATUS capSt = drv->nvencFuncs.nvEncGetEncodeCaps(encoder, NV_ENC_CODEC_H264_GUID,
                                                              &capsParam, &capValue);
        if (capSt != NV_ENC_SUCCESS) {
            LOG("NvEncGetEncodeCaps failed: %d", capSt);
            capValue = 0;
        }
        *queries[i].out = capValue;
    }

    drv->nvencFuncs.nvEncDestroyEncoder(encoder);
    drv->nvencCaps.valid = true;
    CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL));
    pthread_mutex_unlock(&drv->cudaMutex);
    pthread_mutex_unlock(&drv->nvencCapsMutex);
    return true;
}

uint32_t nvenc_reconfig_bps_hint(const NVEncodeContext *enc, uint32_t candidate)
{
    if (candidate > 0) {
        return candidate;
    }
    if (enc->haveRc && enc->rcParams.bits_per_second) {
        return enc->rcParams.bits_per_second;
    }
    if (enc->haveSeq && enc->seqParams.bits_per_second) {
        return enc->seqParams.bits_per_second;
    }
    return 0;
}

void nvenc_reset_roi_state(NVEncodeContext *enc)
{
    if (!enc) {
        return;
    }

    enc->roiMap = NULL;
    enc->roiMapSize = 0;
    enc->roiMapValid = false;
    enc->roiMapMode = NV_ENC_QP_MAP_DISABLED;
}

void nvenc_release_roi_state(NVEncodeContext *enc)
{
    if (!enc) {
        return;
    }

    if (enc->roiMap) {
        free(enc->roiMap);
    }
    nvenc_reset_roi_state(enc);
}

void nvenc_get_framerate(NVEncodeContext *enc, uint32_t *fr_num, uint32_t *fr_den)
{
    *fr_num = 30;
    *fr_den = 1;

    if (enc->haveFr) {
        uint32_t fr = enc->frParams.framerate;
        uint32_t den = (fr >> 16) & 0xffff;
        uint32_t num = fr & 0xffff;
        if (den == 0) {
            den = 1;
        }
        if (num != 0) {
            *fr_num = num;
            *fr_den = den;
            return;
        }
    }

    if (enc->haveSeq &&
        enc->seqParams.vui_fields.bits.timing_info_present_flag &&
        enc->seqParams.num_units_in_tick &&
        enc->seqParams.time_scale) {
        *fr_num = enc->seqParams.time_scale;
        *fr_den = enc->seqParams.num_units_in_tick * 2;
        if (*fr_den == 0) {
            *fr_den = 1;
        }
    }
}

NV_ENC_PARAMS_RC_MODE nvenc_rc_mode_from_va(uint32_t rc_mode)
{
    if (rc_mode & VA_RC_CBR) {
        return NV_ENC_PARAMS_RC_CBR;
    }
    if (rc_mode & VA_RC_VBR) {
        return NV_ENC_PARAMS_RC_VBR;
    }
    if (rc_mode & VA_RC_CQP) {
        return NV_ENC_PARAMS_RC_CONSTQP;
    }
    return NV_ENC_PARAMS_RC_CONSTQP;
}

void nvenc_force_reconfigure(NVEncodeContext *enc, NVEncReconfigureReason reason, uint32_t new_bps)
{
    if (!enc) {
        return;
    }
    enc->reconfigureReason = reason;
    enc->reconfigureBpsHint = new_bps;
    enc->reconfigure = true;
}

const char *nvenc_reconfigure_reason_name(NVEncReconfigureReason reason)
{
    switch (reason) {
    case NVENC_RECONFIGURE_REASON_SEQ_PARAMS:
        return "seq_params";
    case NVENC_RECONFIGURE_REASON_SEQ_BITRATE:
        return "seq_bitrate";
    case NVENC_RECONFIGURE_REASON_RATE_CONTROL:
        return "rate_control";
    case NVENC_RECONFIGURE_REASON_HRD:
        return "hrd";
    case NVENC_RECONFIGURE_REASON_FRAMERATE:
        return "framerate";
    case NVENC_RECONFIGURE_REASON_QUALITY:
        return "quality";
    case NVENC_RECONFIGURE_REASON_RIR:
        return "rir";
    case NVENC_RECONFIGURE_REASON_QP_MAP_MODE:
        return "qp_map_mode";
    case NVENC_RECONFIGURE_REASON_SLICE_MODE:
        return "slice_mode";
    case NVENC_RECONFIGURE_REASON_PACKED_SEI_POLICY:
        return "packed_sei_policy";
    case NVENC_RECONFIGURE_REASON_NONE:
    default:
        return "none";
    }
}

NV_ENC_OUTPUT_PTR nvenc_bitstream_acquire(NVEncodeContext *enc)
{
    if (!enc || !enc->funcs || !enc->encoder) {
        return NULL;
    }

    NV_ENC_OUTPUT_PTR bitstream = NULL;
    if (enc->bitstreamPoolMax > 0) {
        pthread_mutex_lock(&enc->bitstreamPoolMutex);
        if (enc->bitstreamPool.size > 0) {
            uint32_t idx = enc->bitstreamPool.size - 1;
            bitstream = (NV_ENC_OUTPUT_PTR) get_element_at(&enc->bitstreamPool, idx);
            remove_element_at(&enc->bitstreamPool, idx);
        }
        pthread_mutex_unlock(&enc->bitstreamPoolMutex);
    }

    if (bitstream) {
        return bitstream;
    }

    NV_ENC_CREATE_BITSTREAM_BUFFER bs = { 0 };
    bs.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    NVENCSTATUS st = enc->funcs->nvEncCreateBitstreamBuffer(enc->encoder, &bs);
    if (st != NV_ENC_SUCCESS) {
        LOG("NvEncCreateBitstreamBuffer failed: %d", st);
        return NULL;
    }
    return bs.bitstreamBuffer;
}

void nvenc_bitstream_release(NVEncodeContext *enc, NV_ENC_OUTPUT_PTR bitstream)
{
    if (!enc || !bitstream || !enc->funcs || !enc->encoder) {
        return;
    }

    if (enc->bitstreamPoolMax == 0) {
        NVENCSTATUS st = enc->funcs->nvEncDestroyBitstreamBuffer(enc->encoder, bitstream);
        if (st != NV_ENC_SUCCESS) {
            LOG("NvEncDestroyBitstreamBuffer failed: %d", st);
        }
        return;
    }

    pthread_mutex_lock(&enc->bitstreamPoolMutex);
    if (enc->bitstreamPool.size < enc->bitstreamPoolMax) {
        add_element(&enc->bitstreamPool, (void*) bitstream);
        bitstream = NULL;
    }
    pthread_mutex_unlock(&enc->bitstreamPoolMutex);

    if (bitstream) {
        NVENCSTATUS st = enc->funcs->nvEncDestroyBitstreamBuffer(enc->encoder, bitstream);
        if (st != NV_ENC_SUCCESS) {
            LOG("NvEncDestroyBitstreamBuffer failed: %d", st);
        }
    }
}

void nvenc_destroy_bitstream_pool(NVEncodeContext *enc)
{
    if (!enc || !enc->funcs || !enc->encoder) {
        return;
    }

    pthread_mutex_lock(&enc->bitstreamPoolMutex);
    ARRAY_FOR_EACH(NV_ENC_OUTPUT_PTR, bs, &enc->bitstreamPool)
        if (bs) {
            NVENCSTATUS st = enc->funcs->nvEncDestroyBitstreamBuffer(enc->encoder, bs);
            if (st != NV_ENC_SUCCESS) {
                LOG("NvEncDestroyBitstreamBuffer failed: %d", st);
            }
        }
    END_FOR_EACH
    free(enc->bitstreamPool.buf);
    enc->bitstreamPool.buf = NULL;
    enc->bitstreamPool.size = 0;
    enc->bitstreamPool.capacity = 0;
    pthread_mutex_unlock(&enc->bitstreamPoolMutex);
}

void nvenc_clear_surface_resolving(NVSurface *surface)
{
    if (!surface) {
        return;
    }
    pthread_mutex_lock(&surface->mutex);
    surface->resolving = 0;
    pthread_cond_signal(&surface->cond);
    pthread_mutex_unlock(&surface->mutex);
}

uint32_t nvenc_compute_display_poc(NVEncodeContext *enc, uint32_t poc_lsb, bool idr)
{
    if (!enc || !enc->haveSeq) {
        return poc_lsb;
    }
    uint32_t max_poc_lsb =
        1u << (enc->seqParams.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4);
    if (max_poc_lsb == 0) {
        return poc_lsb;
    }

    if (idr || !enc->havePrevPoc) {
        enc->pocMsb = 0;
        enc->prevPocLsb = poc_lsb;
        enc->havePrevPoc = true;
        return poc_lsb;
    }

    uint32_t prev_lsb = enc->prevPocLsb;
    uint32_t msb = enc->pocMsb;
    uint32_t half = max_poc_lsb / 2;
    if (poc_lsb < prev_lsb && (prev_lsb - poc_lsb) >= half) {
        msb += max_poc_lsb;
    } else if (poc_lsb > prev_lsb && (poc_lsb - prev_lsb) > half) {
        if (msb >= max_poc_lsb) {
            msb -= max_poc_lsb;
        } else {
            msb = 0;
        }
    }

    enc->pocMsb = msb;
    enc->prevPocLsb = poc_lsb;
    enc->havePrevPoc = true;
    return msb + poc_lsb;
}

uint32_t nvenc_reorder_group_size(const NVEncodeContext *enc)
{
    if (!enc || !enc->haveSeq || enc->seqParams.ip_period == 0) {
        return 1;
    }
    return enc->seqParams.ip_period;
}

uint32_t nvenc_reorder_group_id(const NVEncodeContext *enc, uint32_t display_poc, bool is_idr)
{
    uint32_t group_size = nvenc_reorder_group_size(enc);
    if (group_size <= 1) {
        return 0;
    }
    if (is_idr || display_poc == 0) {
        return 0;
    }
    if (display_poc <= 1) {
        return 1;
    }
    return 1 + (uint32_t)((display_poc - 1) / group_size);
}

void nvenc_free_queued_pic(NVEncQueuedPic *queued)
{
    if (!queued) {
        return;
    }
    if (queued->qpDeltaMap) {
        free(queued->qpDeltaMap);
        queued->qpDeltaMap = NULL;
        queued->qpDeltaMapSize = 0;
    }
    nvenc_clear_h264_sei_payloads(&queued->seiPayloads, &queued->seiPayloadCount);
    free(queued);
}

void nvenc_cleanup_pending_output(NVEncodeContext *enc,
                                  NVEncPendingOutput *pending)
{
    if (!pending) {
        return;
    }
    if (pending->codedBuf) {
        pending->codedBuf->encCtx = NULL;
    }
    if (pending->releaseBitstreamOnCleanup && pending->bitstream) {
        nvenc_bitstream_release(enc, pending->bitstream);
    }
    pending->bitstream = NULL;
    if (pending->surface) {
        nvenc_clear_surface_resolving(pending->surface);
    }
    if (pending->qpDeltaMap) {
        free(pending->qpDeltaMap);
        pending->qpDeltaMap = NULL;
        pending->qpDeltaMapSize = 0;
    }
    free(pending);
}

static void nvenc_cleanup_pending_output_list(NVEncodeContext *enc,
                                              Array *cleanup)
{
    if (!cleanup) {
        return;
    }
    ARRAY_FOR_EACH(NVEncPendingOutput*, pending, cleanup)
        nvenc_cleanup_pending_output(enc, pending);
    END_FOR_EACH
    free(cleanup->buf);
    cleanup->buf = NULL;
    cleanup->size = 0;
    cleanup->capacity = 0;
}

static void nvenc_cleanup_queued_pic_list(Array *cleanup)
{
    if (!cleanup) {
        return;
    }
    ARRAY_FOR_EACH(NVEncQueuedPic*, queued, cleanup)
        nvenc_free_queued_pic(queued);
    END_FOR_EACH
    free(cleanup->buf);
    cleanup->buf = NULL;
    cleanup->size = 0;
    cleanup->capacity = 0;
}

void nvenc_mark_pending_ready(NVEncodeContext *enc)
{
    if (!enc) {
        return;
    }
    pthread_mutex_lock(&enc->reorderMutex);
    ARRAY_FOR_EACH(NVEncPendingOutput*, pending, &enc->pendingOutputs)
        if (pending) {
            pending->encodeReady = true;
        }
    END_FOR_EACH
    pthread_cond_broadcast(&enc->reorderCond);
    pthread_mutex_unlock(&enc->reorderMutex);
}

bool nvenc_has_pending_outputs(NVEncodeContext *enc)
{
    if (!enc) {
        return false;
    }
    pthread_mutex_lock(&enc->reorderMutex);
    bool has_pending = enc->pendingOutputs.size > 0;
    pthread_mutex_unlock(&enc->reorderMutex);
    return has_pending;
}

bool nvenc_has_queued_pics(NVEncodeContext *enc)
{
    if (!enc) {
        return false;
    }
    pthread_mutex_lock(&enc->queueMutex);
    bool has_queued = enc->queuedPics.size > 0;
    pthread_mutex_unlock(&enc->queueMutex);
    return has_queued;
}

bool nvenc_surface_has_pending_output(NVEncodeContext *enc, NVSurface *surface)
{
    return nvenc_find_pending_output_index_for_surface(enc, surface) >= 0;
}

int32_t nvenc_find_pending_output_index_for_surface(NVEncodeContext *enc, NVSurface *surface)
{
    if (!enc || !surface) {
        return -1;
    }

    int32_t found = -1;
    pthread_mutex_lock(&enc->reorderMutex);
    for (uint32_t i = 0; i < enc->pendingOutputs.size; i++) {
        NVEncPendingOutput *pending = (NVEncPendingOutput*) get_element_at(&enc->pendingOutputs, i);
        if (pending && !pending->detached && pending->surface == surface) {
            found = (int32_t)i;
            break;
        }
    }
    pthread_mutex_unlock(&enc->reorderMutex);
    return found;
}

static void nvenc_detach_all_pending_outputs(NVEncodeContext *enc,
                                             Array *cleanup,
                                             bool release_bitstream)
{
    if (!enc || !cleanup) {
        return;
    }

    pthread_mutex_lock(&enc->reorderMutex);
    while (enc->pendingOutputs.size > 0) {
        NVEncPendingOutput *pending = (NVEncPendingOutput*) get_element_at(&enc->pendingOutputs, 0);
        remove_element_at(&enc->pendingOutputs, 0);
        if (!pending) {
            continue;
        }
        if (pending->resolving) {
            pending->detached = true;
            pending->releaseBitstreamOnCleanup = release_bitstream;
            continue;
        }
        pending->detached = true;
        pending->releaseBitstreamOnCleanup = release_bitstream;
        add_element(cleanup, pending);
    }
    free(enc->pendingOutputs.buf);
    enc->pendingOutputs.buf = NULL;
    enc->pendingOutputs.size = 0;
    enc->pendingOutputs.capacity = 0;
    pthread_cond_broadcast(&enc->reorderCond);
    pthread_mutex_unlock(&enc->reorderMutex);

    nvenc_cleanup_pending_output_list(enc, cleanup);
}

static void nvenc_detach_all_queued_pics(NVEncodeContext *enc, Array *cleanup)
{
    if (!enc || !cleanup) {
        return;
    }

    pthread_mutex_lock(&enc->queueMutex);
    while (enc->queuedPics.size > 0) {
        NVEncQueuedPic *queued = (NVEncQueuedPic*) get_element_at(&enc->queuedPics, 0);
        remove_element_at(&enc->queuedPics, 0);
        if (!queued) {
            continue;
        }
        if (queued->codedBuf) {
            queued->codedBuf->encCtx = NULL;
        }
        if (queued->surface) {
            nvenc_clear_surface_resolving(queued->surface);
        }
        add_element(cleanup, queued);
    }
    free(enc->queuedPics.buf);
    enc->queuedPics.buf = NULL;
    enc->queuedPics.size = 0;
    enc->queuedPics.capacity = 0;
    enc->haveNextDisplayPoc = false;
    enc->havePrevPoc = false;
    enc->prevPocLsb = 0;
    enc->pocMsb = 0;
    pthread_mutex_unlock(&enc->queueMutex);

    nvenc_cleanup_queued_pic_list(cleanup);
}

void nvenc_clear_pending_outputs(NVEncodeContext *enc)
{
    if (!enc) {
        return;
    }
    Array cleanup = { 0 };
    nvenc_detach_all_pending_outputs(enc, &cleanup, true);
    nvenc_clear_reorder_groups(enc);
}

void nvenc_abandon_pending_outputs(NVEncodeContext *enc)
{
    if (!enc) {
        return;
    }
    Array cleanup = { 0 };
    nvenc_detach_all_pending_outputs(enc, &cleanup, false);
    nvenc_clear_reorder_groups(enc);
}

void nvenc_clear_queued_pics(NVEncodeContext *enc)
{
    if (!enc) {
        return;
    }
    Array cleanup = { 0 };
    nvenc_detach_all_queued_pics(enc, &cleanup);
}

void nvenc_abandon_queued_pics(NVEncodeContext *enc)
{
    if (!enc) {
        return;
    }
    Array cleanup = { 0 };
    nvenc_detach_all_queued_pics(enc, &cleanup);
}

void nvenc_abandon_coded_buffer(NVEncodeContext *enc, NVBuffer *buf)
{
    if (!enc || !buf) {
        return;
    }

    Array queued_cleanup = { 0 };
    Array pending_cleanup = { 0 };

    pthread_mutex_lock(&enc->queueMutex);
    for (uint32_t i = 0; i < enc->queuedPics.size;) {
        NVEncQueuedPic *queued = (NVEncQueuedPic*) get_element_at(&enc->queuedPics, i);
        if (!queued || queued->codedBuf != buf) {
            i++;
            continue;
        }
        if (queued->surface) {
            nvenc_clear_surface_resolving(queued->surface);
        }
        remove_element_at(&enc->queuedPics, i);
        add_element(&queued_cleanup, queued);
    }
    pthread_mutex_unlock(&enc->queueMutex);
    nvenc_cleanup_queued_pic_list(&queued_cleanup);

    pthread_mutex_lock(&enc->reorderMutex);
    for (uint32_t i = 0; i < enc->pendingOutputs.size;) {
        NVEncPendingOutput *pending = (NVEncPendingOutput*) get_element_at(&enc->pendingOutputs, i);
        if (!pending || pending->codedBuf != buf) {
            i++;
            continue;
        }
        remove_element_at(&enc->pendingOutputs, i);
        if (pending->resolving) {
            pending->detached = true;
            pending->releaseBitstreamOnCleanup = true;
            continue;
        }
        pending->detached = true;
        pending->releaseBitstreamOnCleanup = true;
        add_element(&pending_cleanup, pending);
    }

    for (uint32_t i = 0; i < enc->reorderEncode.size;) {
        NVEncReorderEntry *entry = (NVEncReorderEntry*) get_element_at(&enc->reorderEncode, i);
        if (!entry || entry->codedBuf != buf) {
            i++;
            continue;
        }
        free(entry);
        remove_element_at(&enc->reorderEncode, i);
    }
    pthread_cond_broadcast(&enc->reorderCond);
    pthread_mutex_unlock(&enc->reorderMutex);
    nvenc_cleanup_pending_output_list(enc, &pending_cleanup);

    buf->encCtx = NULL;
}

bool nvenc_coded_buffer_has_pending_output(NVEncodeContext *enc, NVBuffer *buf)
{
    return nvenc_find_pending_output_index_for_coded_buffer(enc, buf) >= 0;
}

int32_t nvenc_find_pending_output_index_for_coded_buffer(NVEncodeContext *enc, NVBuffer *buf)
{
    if (!enc || !buf) {
        return -1;
    }

    int32_t found = -1;
    pthread_mutex_lock(&enc->reorderMutex);
    for (uint32_t i = 0; i < enc->pendingOutputs.size; i++) {
        NVEncPendingOutput *pending = (NVEncPendingOutput*) get_element_at(&enc->pendingOutputs, i);
        if (pending && !pending->detached && pending->codedBuf == buf) {
            found = (int32_t)i;
            break;
        }
    }
    pthread_mutex_unlock(&enc->reorderMutex);
    return found;
}

bool nvenc_coded_buffer_is_queued_or_reordered(NVEncodeContext *enc, NVBuffer *buf)
{
    if (!enc || !buf) {
        return false;
    }

    pthread_mutex_lock(&enc->queueMutex);
    for (uint32_t i = 0; i < enc->queuedPics.size; i++) {
        NVEncQueuedPic *queued = (NVEncQueuedPic*) get_element_at(&enc->queuedPics, i);
        if (queued && queued->codedBuf == buf) {
            pthread_mutex_unlock(&enc->queueMutex);
            return true;
        }
    }
    pthread_mutex_unlock(&enc->queueMutex);

    pthread_mutex_lock(&enc->reorderMutex);
    for (uint32_t i = 0; i < enc->reorderEncode.size; i++) {
        NVEncReorderEntry *entry = (NVEncReorderEntry*) get_element_at(&enc->reorderEncode, i);
        if (entry && entry->codedBuf == buf) {
            pthread_mutex_unlock(&enc->reorderMutex);
            return true;
        }
    }
    pthread_mutex_unlock(&enc->reorderMutex);

    return false;
}

void nvenc_discard_unsubmitted_coded_buffer(NVEncodeContext *enc, NVBuffer *buf)
{
    if (!enc || !buf) {
        return;
    }

    pthread_mutex_lock(&enc->queueMutex);
    for (uint32_t i = 0; i < enc->queuedPics.size;) {
        NVEncQueuedPic *queued = (NVEncQueuedPic*) get_element_at(&enc->queuedPics, i);
        if (!queued || queued->codedBuf != buf) {
            i++;
            continue;
        }
        if (queued->surface) {
            nvenc_clear_surface_resolving(queued->surface);
        }
        nvenc_free_queued_pic(queued);
        remove_element_at(&enc->queuedPics, i);
    }
    enc->haveNextDisplayPoc = false;
    pthread_mutex_unlock(&enc->queueMutex);

    pthread_mutex_lock(&enc->reorderMutex);
    for (uint32_t i = 0; i < enc->reorderEncode.size;) {
        NVEncReorderEntry *entry = (NVEncReorderEntry*) get_element_at(&enc->reorderEncode, i);
        if (!entry || entry->codedBuf != buf) {
            i++;
            continue;
        }
        free(entry);
        remove_element_at(&enc->reorderEncode, i);
    }
    pthread_cond_broadcast(&enc->reorderCond);
    pthread_mutex_unlock(&enc->reorderMutex);

    buf->encCtx = NULL;
}

void nvenc_clear_reorder_groups(NVEncodeContext *enc)
{
    if (!enc) {
        return;
    }
    pthread_mutex_lock(&enc->reorderMutex);
    ARRAY_FOR_EACH(NVEncReorderEntry*, entry, &enc->reorderEncode)
        free(entry);
    END_FOR_EACH
    free(enc->reorderEncode.buf);
    enc->reorderEncode.buf = NULL;
    enc->reorderEncode.size = 0;
    enc->reorderEncode.capacity = 0;
    pthread_cond_broadcast(&enc->reorderCond);
    pthread_mutex_unlock(&enc->reorderMutex);
}

NVEncReorderEntry *nvenc_find_reorder_entry(NVEncodeContext *enc, uint64_t display_ts, uint32_t *out_idx)
{
    if (!enc) {
        return NULL;
    }
    for (uint32_t i = 0; i < enc->reorderEncode.size; i++) {
        NVEncReorderEntry *entry = (NVEncReorderEntry*) get_element_at(&enc->reorderEncode, i);
        if (entry && entry->displayTs == display_ts) {
            if (out_idx) {
                *out_idx = i;
            }
            return entry;
        }
    }
    return NULL;
}

void nvenc_decode_api_version(uint32_t version, uint32_t *major, uint32_t *minor)
{
    if (version & 0xFF000000u) {
        *major = version & 0xFFu;
        *minor = version >> 24;
    } else {
        *major = version >> 4;
        *minor = version & 0xFu;
    }
}

NVEncQueuedPic *nvenc_find_queued_by_poc(NVEncodeContext *enc, uint32_t poc, uint32_t *out_idx)
{
    if (!enc) {
        return NULL;
    }
    for (uint32_t i = 0; i < enc->queuedPics.size; i++) {
        NVEncQueuedPic *queued = (NVEncQueuedPic*) get_element_at(&enc->queuedPics, i);
        if (queued && queued->displayPoc == poc) {
            if (out_idx) {
                *out_idx = i;
            }
            return queued;
        }
    }
    return NULL;
}
