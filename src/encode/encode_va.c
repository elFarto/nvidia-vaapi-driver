#include "encode_va.h"

#include "encode_common.h"
#include "encode_pipeline.h"
#include "h264_encode.h"

#include <stdlib.h>
#include <string.h>

#define NVD_VA_ENC_PACKED_HEADER_H264_SEI ((uint32_t)(0x80000000u | 1u))

static VAStatus nvenc_validate_buffer_payload(const NVBuffer *buf,
                                              size_t required_size,
                                              const char *label)
{
    if (!buf) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (required_size > 0 && buf->size < required_size) {
        LOG("%s buffer too small: have=%zu need=%zu", label, buf->size, required_size);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (required_size > 0 && !buf->ptr) {
        LOG("%s buffer missing payload", label);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus nvenc_validate_misc_buffer(const NVBuffer *buf,
                                           const VAEncMiscParameterBuffer **out_misc)
{
    VAStatus st;
    const VAEncMiscParameterBuffer *misc = NULL;
    size_t required_size = sizeof(VAEncMiscParameterBuffer);

    st = nvenc_validate_buffer_payload(buf, required_size, "Encode misc");
    if (st != VA_STATUS_SUCCESS) {
        return st;
    }

    misc = (const VAEncMiscParameterBuffer *)buf->ptr;
    switch (misc->type) {
    case VAEncMiscParameterTypeRateControl:
        required_size += sizeof(VAEncMiscParameterRateControl);
        break;
    case VAEncMiscParameterTypeHRD:
        required_size += sizeof(VAEncMiscParameterHRD);
        break;
    case VAEncMiscParameterTypeFrameRate:
        required_size += sizeof(VAEncMiscParameterFrameRate);
        break;
    case VAEncMiscParameterTypeQualityLevel:
        required_size += sizeof(VAEncMiscParameterBufferQualityLevel);
        break;
    case VAEncMiscParameterTypeRIR:
        required_size += sizeof(VAEncMiscParameterRIR);
        break;
    case VAEncMiscParameterTypeROI:
        required_size += sizeof(VAEncMiscParameterBufferROI);
        break;
    default:
        break;
    }

    st = nvenc_validate_buffer_payload(buf, required_size, "Encode misc");
    if (st != VA_STATUS_SUCCESS) {
        return st;
    }

    if (out_misc) {
        *out_misc = misc;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus nvenc_validate_render_buffer_payload(const NVBuffer *buf)
{
    size_t required_size = 0;

    if (!buf) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    switch (buf->bufferType) {
    case VAEncSequenceParameterBufferType:
        if (buf->elements == 0) {
            LOG("Encode sequence buffer has zero elements");
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        return nvenc_validate_buffer_payload(buf,
                                             sizeof(VAEncSequenceParameterBufferH264),
                                             "Encode sequence");
    case VAEncPictureParameterBufferType:
        if (buf->elements == 0) {
            LOG("Encode picture buffer has zero elements");
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        return nvenc_validate_buffer_payload(buf,
                                             sizeof(VAEncPictureParameterBufferH264),
                                             "Encode picture");
    case VAEncSliceParameterBufferType:
        if (buf->elements == 0) {
            LOG("Encode slice buffer has zero elements");
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        required_size = (size_t)buf->elements * sizeof(VAEncSliceParameterBufferH264);
        return nvenc_validate_buffer_payload(buf, required_size, "Encode slice");
    case VAEncMiscParameterBufferType:
        return nvenc_validate_misc_buffer(buf, NULL);
    case VAEncPackedHeaderParameterBufferType:
        if (buf->elements == 0) {
            LOG("Packed header parameter buffer has zero elements");
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        return nvenc_validate_buffer_payload(buf,
                                             sizeof(VAEncPackedHeaderParameterBuffer),
                                             "Packed header parameter");
    case VAEncPackedHeaderDataBufferType:
        if (buf->size > 0 && !buf->ptr) {
            LOG("Packed header data buffer missing payload");
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        return VA_STATUS_SUCCESS;
    default:
        if (buf->size > 0 && !buf->ptr) {
            LOG("Encode buffer type %d missing payload", buf->bufferType);
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        return VA_STATUS_SUCCESS;
    }
}

static void nvenc_seq_params_get_framerate(const VAEncSequenceParameterBufferH264 *seq,
                                           uint32_t *fr_num,
                                           uint32_t *fr_den)
{
    if (!fr_num || !fr_den) {
        return;
    }

    *fr_num = 30;
    *fr_den = 1;
    if (!seq) {
        return;
    }

    if (seq->vui_fields.bits.timing_info_present_flag &&
        seq->num_units_in_tick &&
        seq->time_scale) {
        *fr_num = seq->time_scale;
        *fr_den = seq->num_units_in_tick * 2u;
        if (*fr_den == 0) {
            *fr_den = 1;
        }
    }
}

static bool nvenc_replace_owned_bytes(uint8_t **dst,
                                      size_t *dst_size,
                                      const void *src,
                                      size_t src_size)
{
    if (*dst) {
        free(*dst);
        *dst = NULL;
    }
    *dst_size = 0;

    if (!src || src_size == 0) {
        return true;
    }

    uint8_t *copy = malloc(src_size);
    if (!copy) {
        return false;
    }
    memcpy(copy, src, src_size);
    *dst = copy;
    *dst_size = src_size;
    return true;
}

void nvenc_request_reconfigure_if_changed(NVEncodeContext *enc,
                                          bool changed,
                                          NVEncReconfigureReason reason,
                                          uint32_t bps)
{
    if (changed && enc->initialized) {
        nvenc_force_reconfigure(enc, reason, bps);
    }
}

void nvenc_handle_misc_rate_control(NVEncodeContext *enc, const void *data)
{
    VAEncMiscParameterRateControl new_rc = { 0 };
    memcpy(&new_rc, data, sizeof(new_rc));
    bool changed = !enc->haveRc ||
                   memcmp(&enc->rcParams, &new_rc, sizeof(new_rc)) != 0;

    enc->rcParams = new_rc;
    enc->haveRc = true;
    uint32_t bps = nvenc_reconfig_bps_hint(enc, new_rc.bits_per_second);
    nvenc_request_reconfigure_if_changed(enc, changed, NVENC_RECONFIGURE_REASON_RATE_CONTROL, bps);
}

void nvenc_handle_misc_hrd(NVEncodeContext *enc, const void *data)
{
    VAEncMiscParameterHRD new_hrd = { 0 };
    memcpy(&new_hrd, data, sizeof(new_hrd));
    bool changed = !enc->haveHrd ||
                   memcmp(&enc->hrdParams, &new_hrd, sizeof(new_hrd)) != 0;

    enc->hrdParams = new_hrd;
    enc->haveHrd = true;
    uint32_t bps = nvenc_reconfig_bps_hint(enc, enc->rcParams.bits_per_second);
    nvenc_request_reconfigure_if_changed(enc, changed, NVENC_RECONFIGURE_REASON_HRD, bps);
}

void nvenc_handle_misc_framerate(NVEncodeContext *enc, const void *data)
{
    VAEncMiscParameterFrameRate prev = enc->frParams;

    memcpy(&enc->frParams, data, sizeof(enc->frParams));
    bool changed = !enc->haveFr ||
                   memcmp(&prev, &enc->frParams, sizeof(prev)) != 0;

    enc->haveFr = true;
    uint32_t bps = nvenc_reconfig_bps_hint(enc, enc->rcParams.bits_per_second);
    nvenc_request_reconfigure_if_changed(enc, changed, NVENC_RECONFIGURE_REASON_FRAMERATE, bps);
}

void nvenc_handle_misc_quality_level(NVEncodeContext *enc, const void *data)
{
    VAEncMiscParameterBufferQualityLevel prev = enc->qualityParams;
    memcpy(&enc->qualityParams, data, sizeof(enc->qualityParams));
    bool changed = !enc->haveQuality ||
                   prev.quality_level != enc->qualityParams.quality_level;

    enc->haveQuality = true;
    uint32_t bps = nvenc_reconfig_bps_hint(enc, enc->rcParams.bits_per_second);
    nvenc_request_reconfigure_if_changed(enc, changed, NVENC_RECONFIGURE_REASON_QUALITY, bps);
}

void nvenc_handle_misc_rir(NVEncodeContext *enc, const void *data)
{
    VAEncMiscParameterRIR new_rir = { 0 };
    memcpy(&new_rir, data, sizeof(new_rir));
    bool changed = !enc->haveRir ||
                   memcmp(&new_rir, &enc->rirParams, sizeof(new_rir)) != 0;

    enc->rirParams = new_rir;
    enc->haveRir = true;
    if (changed) {
        enc->rirTriggered = false;
        uint32_t bps = nvenc_reconfig_bps_hint(enc, enc->rcParams.bits_per_second);
        nvenc_request_reconfigure_if_changed(enc, true, NVENC_RECONFIGURE_REASON_RIR, bps);
    }
}

void nvenc_handle_misc_roi(NVEncodeContext *enc, const void *data)
{
    VAEncMiscParameterBufferROI roi = { 0 };
    memcpy(&roi, data, sizeof(roi));
    int8_t *map = NULL;
    uint32_t map_size = 0;
    NV_ENC_QP_MAP_MODE mode = NV_ENC_QP_MAP_DISABLED;
    bool ok = nvenc_build_roi_map(enc, &roi, &map, &map_size, &mode);
    if (ok && map && map_size > 0) {
        if (enc->roiMapValid && enc->roiMap) {
            free(enc->roiMap);
        }
        enc->roiMap = map;
        enc->roiMapSize = map_size;
        enc->roiMapMode = mode;
        enc->roiMapValid = true;
        return;
    }
    if (map) {
        free(map);
    }
}

void nvenc_handle_seq_params(NVEncodeContext *enc,
                             const VAEncSequenceParameterBufferH264 *seq)
{
    NVEncSeqSignature sig = nvenc_seq_signature_from_va(seq);
    VAEncSequenceParameterBufferH264 prev_seq = enc->seqParams;
    bool sig_changed = !enc->haveSeqSignature ||
                       memcmp(&sig, &enc->seqSignature, sizeof(sig)) != 0;
    bool bitrate_changed = enc->haveSeq &&
                           enc->seqParams.bits_per_second != seq->bits_per_second;
    bool seq_framerate_changed = false;

    if (enc->haveSeq && !enc->haveFr) {
        uint32_t prev_fr_num = 0;
        uint32_t prev_fr_den = 0;
        uint32_t new_fr_num = 0;
        uint32_t new_fr_den = 0;

        nvenc_seq_params_get_framerate(&prev_seq, &prev_fr_num, &prev_fr_den);
        nvenc_seq_params_get_framerate(seq, &new_fr_num, &new_fr_den);
        seq_framerate_changed = prev_fr_num != new_fr_num ||
                                prev_fr_den != new_fr_den;
    }

    if (sig_changed) {
        memcpy(&enc->seqParams, seq, sizeof(enc->seqParams));
        enc->seqSignature = sig;
        enc->haveSeqSignature = true;
        enc->haveSeq = true;
        nvenc_reset_bp_state(enc);
        nvenc_update_visible_dimensions(enc);
        uint32_t bps = nvenc_reconfig_bps_hint(enc, enc->seqParams.bits_per_second);
        nvenc_request_reconfigure_if_changed(enc, true, NVENC_RECONFIGURE_REASON_SEQ_PARAMS, bps);
        return;
    }

    memcpy(&enc->seqParams, seq, sizeof(enc->seqParams));

    if (bitrate_changed) {
        if (!enc->haveRc) {
            uint32_t bps = nvenc_reconfig_bps_hint(enc, enc->seqParams.bits_per_second);
            nvenc_request_reconfigure_if_changed(enc, true, NVENC_RECONFIGURE_REASON_SEQ_BITRATE, bps);
        }
    }
    if (seq_framerate_changed) {
        uint32_t bps = nvenc_reconfig_bps_hint(enc, enc->seqParams.bits_per_second);
        nvenc_request_reconfigure_if_changed(enc, true, NVENC_RECONFIGURE_REASON_FRAMERATE, bps);
    }
    enc->haveSeq = true;
}

void nvenc_handle_picture_params(NVEncodeContext *enc, const void *data)
{
    memcpy(&enc->picParams, data, sizeof(enc->picParams));
    enc->codedBufId = enc->picParams.coded_buf;
    enc->havePic = true;
}

void nvenc_handle_slice_params(NVEncodeContext *enc,
                               VAEncSliceParameterBufferH264 *slices,
                               uint32_t slice_count)
{
    memcpy(&enc->sliceParams, &slices[0], sizeof(enc->sliceParams));
    enc->haveSlice = true;
    enc->sliceCount += slice_count;
    if (!enc->sliceRowsValid) {
        uint32_t mb_width = nvenc_mb_width(enc->width);
        uint32_t mb_height = nvenc_mb_height(enc->height);
        if (mb_width > 0 &&
            (slices[0].num_macroblocks % mb_width) == 0 &&
            (slices[0].macroblock_address % mb_width) == 0) {
            uint32_t rows = slices[0].num_macroblocks / mb_width;
            if (rows > 0 && rows <= mb_height) {
                enc->sliceRows = rows;
                enc->sliceRowsValid = true;
            }
        }
    }
    nvenc_update_slice_reconfigure(enc);
}

void nvenc_handle_packed_header_param(NVEncodeContext *enc, const void *data)
{
    memcpy(&enc->packedHeaderParam, data, sizeof(enc->packedHeaderParam));
    enc->havePackedHeaderParam = true;
}

void nvenc_handle_packed_header_data(NVEncodeContext *enc,
                                     const void *data,
                                     size_t size,
                                     uint32_t elements)
{
    if (elements == 0) {
        return;
    }
    if (!enc->havePackedHeaderParam) {
        LOG("Packed header data without param");
        return;
    }

    enc->havePackedHeaderParam = false;
    uint32_t bit_len = enc->packedHeaderParam.bit_length;
    size_t bytes = (bit_len + 7) / 8;
    if (bytes > size) {
        bytes = size;
    }
    uint32_t prev_sei_mask = enc->packedSeiStreamMask;

    bool append_all = false;
    bool append_aud = false;
    bool append_sei = false;
    switch (enc->packedHeaderParam.type) {
    case VAEncPackedHeaderRawData:
        append_all = true;
        break;
    case VAEncPackedHeaderSequence:
        if (bytes > 0) {
            nvenc_capture_h264_sps_from_packed(enc,
                                               (const uint8_t*)data,
                                               bytes,
                                               enc->packedHeaderParam.has_emulation_bytes != 0);
        }
        append_aud = true;
        break;
    case VAEncPackedHeaderPicture:
        if (bytes > 0) {
            nvenc_capture_h264_pps_from_packed(enc,
                                               (const uint8_t*)data,
                                               bytes,
                                               enc->packedHeaderParam.has_emulation_bytes != 0);
        }
        append_aud = true;
        break;
    case VAEncPackedHeaderSlice:
        append_aud = true;
        break;
    case NVD_VA_ENC_PACKED_HEADER_H264_SEI:
        append_sei = true;
        break;
    default:
        break;
    }

    if (append_all && bytes > 0) {
        nvenc_append_non_sei_nals_from_packed(enc, (const uint8_t*)data, bytes);
        nvenc_append_h264_sei_from_packed(enc, (const uint8_t*)data, bytes);
    } else if (append_aud && bytes > 0) {
        nvenc_append_aud_from_packed(enc, (const uint8_t*)data, bytes);
    } else if (append_sei && bytes > 0) {
        nvenc_append_h264_sei_from_packed(enc, (const uint8_t*)data, bytes);
    }

    if (enc->packedSeiStreamMask != prev_sei_mask) {
        uint32_t bps = nvenc_reconfig_bps_hint(enc, enc->rcParams.bits_per_second);
        nvenc_request_reconfigure_if_changed(enc, true, NVENC_RECONFIGURE_REASON_PACKED_SEI_POLICY, bps);
    }
}

VAStatus nvenc_handle_render_buffer(NVEncodeContext *enc, const NVBuffer *buf)
{
    VAStatus st;

    if (!enc || !buf) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    st = nvenc_validate_render_buffer_payload(buf);
    if (st != VA_STATUS_SUCCESS) {
        return st;
    }

    switch (buf->bufferType) {
    case VAEncSequenceParameterBufferType:
        nvenc_handle_seq_params(enc, (const VAEncSequenceParameterBufferH264 *)buf->ptr);
        return VA_STATUS_SUCCESS;
    case VAEncPictureParameterBufferType:
        nvenc_handle_picture_params(enc, buf->ptr);
        return VA_STATUS_SUCCESS;
    case VAEncSliceParameterBufferType:
        nvenc_handle_slice_params(enc,
                                  (VAEncSliceParameterBufferH264 *)buf->ptr,
                                  buf->elements);
        return VA_STATUS_SUCCESS;
    case VAEncMiscParameterBufferType: {
        const VAEncMiscParameterBuffer *misc = NULL;

        st = nvenc_validate_misc_buffer(buf, &misc);
        if (st != VA_STATUS_SUCCESS) {
            return st;
        }

        switch (misc->type) {
        case VAEncMiscParameterTypeRateControl:
            nvenc_handle_misc_rate_control(enc, misc->data);
            break;
        case VAEncMiscParameterTypeHRD:
            nvenc_handle_misc_hrd(enc, misc->data);
            break;
        case VAEncMiscParameterTypeFrameRate:
            nvenc_handle_misc_framerate(enc, misc->data);
            break;
        case VAEncMiscParameterTypeQualityLevel:
            nvenc_handle_misc_quality_level(enc, misc->data);
            break;
        case VAEncMiscParameterTypeRIR:
            nvenc_handle_misc_rir(enc, misc->data);
            break;
        case VAEncMiscParameterTypeROI:
            nvenc_handle_misc_roi(enc, misc->data);
            break;
        default:
            break;
        }
        return VA_STATUS_SUCCESS;
    }
    case VAEncPackedHeaderParameterBufferType:
        nvenc_handle_packed_header_param(enc, buf->ptr);
        return VA_STATUS_SUCCESS;
    case VAEncPackedHeaderDataBufferType:
        nvenc_handle_packed_header_data(enc, buf->ptr, buf->size, buf->elements);
        return VA_STATUS_SUCCESS;
    default:
        LOG("Unhandled buffer type: %d", buf->bufferType);
        return VA_STATUS_SUCCESS;
    }
}

void nvenc_finalize_render_picture(NVEncodeContext *enc)
{
    if (enc->havePic && enc->haveSlice) {
        nvenc_update_fixed_qp(enc);
    }
}

void nvenc_begin_picture_state(NVContext *nvCtx,
                               NVSurface *surface,
                               VASurfaceID render_target)
{
    NVEncodeContext *enc = nvCtx->enc;

    surface->context = nvCtx;
    enc->inputSurface = render_target;
    nvenc_reset_picture_state(enc);
}

void nvenc_reset_picture_state(NVEncodeContext *enc)
{
    enc->codedBufId = VA_INVALID_ID;
    enc->havePic = false;
    enc->haveSlice = false;
    enc->sliceCount = 0;
    enc->sliceRows = 0;
    enc->sliceRowsValid = false;
    nvenc_release_roi_state(enc);
    enc->havePackedHeaderParam = false;
    memset(&enc->packedHeaderParam, 0, sizeof(enc->packedHeaderParam));
    enc->packedHeaderBuf.size = 0;
    nvenc_reset_appendable_buffer(&enc->packedPpsBuf);
    nvenc_clear_h264_sei_payloads(&enc->packedSeiPayloads, &enc->packedSeiPayloadCount);
    enc->packedTimingSeiSeen = false;
}

void nvenc_move_roi_map_to_queued_pic(NVEncodeContext *enc, NVEncQueuedPic *queued)
{
    if (!enc->roiMapValid || !enc->roiMap || enc->roiMapSize == 0) {
        return;
    }
    queued->qpDeltaMap = enc->roiMap;
    queued->qpDeltaMapSize = enc->roiMapSize;
    queued->qpMapMode = enc->roiMapMode;
    nvenc_reset_roi_state(enc);
}

static void nvenc_move_packed_sei_to_queued_pic(NVEncodeContext *enc, NVEncQueuedPic *queued)
{
    if (!enc || !queued || !enc->packedSeiPayloads || enc->packedSeiPayloadCount == 0) {
        return;
    }
    queued->seiPayloads = enc->packedSeiPayloads;
    queued->seiPayloadCount = enc->packedSeiPayloadCount;
    enc->packedSeiPayloads = NULL;
    enc->packedSeiPayloadCount = 0;
}

VAStatus nvenc_prepare_coded_buffer(NVEncodeContext *enc, NVBuffer *codedBuf)
{
    codedBuf->codedSize = 0;
    codedBuf->codedReady = false;
    if (!nvenc_replace_owned_bytes(&codedBuf->packedHeader,
                                   &codedBuf->packedHeaderSize,
                                   enc->packedHeaderBuf.buf,
                                   enc->packedHeaderBuf.size) ||
        !nvenc_replace_owned_bytes(&codedBuf->packedSps,
                                   &codedBuf->packedSpsSize,
                                   enc->packedSpsBuf.buf,
                                   enc->packedSpsBuf.size) ||
        !nvenc_replace_owned_bytes(&codedBuf->packedPps,
                                   &codedBuf->packedPpsSize,
                                   enc->packedPpsBuf.buf,
                                   enc->packedPpsBuf.size)) {
        nvenc_replace_owned_bytes(&codedBuf->packedHeader, &codedBuf->packedHeaderSize, NULL, 0);
        nvenc_replace_owned_bytes(&codedBuf->packedSps, &codedBuf->packedSpsSize, NULL, 0);
        nvenc_replace_owned_bytes(&codedBuf->packedPps, &codedBuf->packedPpsSize, NULL, 0);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    codedBuf->packedTimingSeiSeen = enc->packedTimingSeiSeen;
    enc->packedHeaderBuf.size = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus nvenc_validate_picture_submit(const NVEncodeContext *enc)
{
    if (enc->inputSurface == VA_INVALID_ID ||
        enc->codedBufId == VA_INVALID_ID ||
        !enc->haveSeq ||
        !enc->havePic ||
        !enc->haveSlice) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    return VA_STATUS_SUCCESS;
}

NVEncQueuedPic *nvenc_alloc_queued_pic(NVEncodeContext *enc,
                                       NVSurface *surface,
                                       NVBuffer *codedBuf)
{
    bool is_idr = enc->picParams.pic_fields.bits.idr_pic_flag;
    if (enc->queuedPics.size == 0 && is_idr) {
        enc->haveNextDisplayPoc = false;
    }

    NVEncQueuedPic *queued = (NVEncQueuedPic*) calloc(1, sizeof(NVEncQueuedPic));
    if (!queued) {
        return NULL;
    }
    queued->surface = surface;
    queued->codedBuf = codedBuf;
    queued->surfaceId = enc->inputSurface;
    queued->displayPoc = nvenc_compute_display_poc(enc, enc->sliceParams.pic_order_cnt_lsb, is_idr);
    queued->displayTs = enc->displayTsCounter++;
    queued->sliceType = (uint8_t)(enc->sliceParams.slice_type % 5);
    queued->forceIdr = is_idr;
    queued->refPicFlag = enc->picParams.pic_fields.bits.reference_pic_flag ? true : false;
    nvenc_move_roi_map_to_queued_pic(enc, queued);
    nvenc_move_packed_sei_to_queued_pic(enc, queued);
    return queued;
}

void nvenc_queue_reorder_entry(NVEncodeContext *enc,
                               NVBuffer *codedBuf,
                               const NVEncQueuedPic *queued)
{
    pthread_mutex_lock(&enc->reorderMutex);
    NVEncReorderEntry *entry = (NVEncReorderEntry*) alloc_and_add_element(&enc->reorderEncode, sizeof(NVEncReorderEntry));
    if (entry) {
        entry->codedBuf = codedBuf;
        entry->displayTs = queued->displayTs;
        entry->reorderGroupId = nvenc_reorder_group_id(enc, (uint32_t)queued->displayTs, queued->forceIdr);
        entry->is_b = queued->sliceType == 1;
    }
    pthread_mutex_unlock(&enc->reorderMutex);
}

VAStatus nvenc_submit_prepared_picture(NVDriver *drv,
                                       NVContext *nvCtx,
                                       NVSurface *surface,
                                       NVBuffer *codedBuf)
{
    NVEncodeContext *enc = nvCtx->enc;

    if (!nvenc_surface_can_direct_encode(surface)) {
        if (!nvenc_ensure_surface_buffer(drv, surface)) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    NVEncQueuedPic *queued = nvenc_alloc_queued_pic(enc, surface, codedBuf);
    if (!queued) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (nvenc_use_internal_reorder(enc)) {
        nvenc_queue_reorder_entry(enc, codedBuf, queued);
        codedBuf->encCtx = enc;
    }

    pthread_mutex_lock(&surface->mutex);
    surface->resolving = 1;
    pthread_mutex_unlock(&surface->mutex);

    enc->inputSurface = VA_INVALID_ID;
    enc->codedBufId = VA_INVALID_ID;

    if (nvenc_use_internal_reorder(enc)) {
        pthread_mutex_lock(&enc->queueMutex);
        add_element(&enc->queuedPics, queued);
        pthread_mutex_unlock(&enc->queueMutex);
        nvenc_process_reorder_queue(drv, nvCtx, false);
        return VA_STATUS_SUCCESS;
    }

    bool ok = nvenc_submit_queued_pic(drv, nvCtx, queued);
    nvenc_free_queued_pic(queued);
    if (!ok) {
        nvenc_clear_surface_resolving(surface);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    return VA_STATUS_SUCCESS;
}
