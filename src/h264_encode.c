#include "vabackend.h"
#include "nvenc.h"
#include <string.h>
#include <va/va.h>

void h264enc_handle_sequence_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    VAEncSequenceParameterBufferH264 *seq =
        (VAEncSequenceParameterBufferH264*) buffer->ptr;

    LOG("H264 encode: seq params %ux%u, intra_period=%u, ip_period=%u",
        seq->picture_width_in_mbs * 16, seq->picture_height_in_mbs * 16,
        seq->intra_period, seq->ip_period);

    /* Store basic sequence-level encode parameters */
    nvencCtx->width = seq->picture_width_in_mbs * 16;
    nvencCtx->height = seq->picture_height_in_mbs * 16;

    if (seq->intra_period > 0) {
        nvencCtx->intraPeriod = seq->intra_period;
    }
    if (seq->ip_period > 0) {
        nvencCtx->ipPeriod = seq->ip_period;
    }

    /* Frame rate from time_scale / num_units_in_tick / 2 if provided */
    if (seq->num_units_in_tick > 0 && seq->time_scale > 0) {
        nvencCtx->frameRateNum = seq->time_scale;
        nvencCtx->frameRateDen = seq->num_units_in_tick * 2;
    }

    /* Bitrate (VA-API provides in bits/sec) */
    if (seq->bits_per_second > 0) {
        nvencCtx->bitrate = seq->bits_per_second;
        if (nvencCtx->maxBitrate == 0) {
            nvencCtx->maxBitrate = seq->bits_per_second;
        }
    }

    nvencCtx->seqParamSet = true;
}

void h264enc_handle_picture_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    VAEncPictureParameterBufferH264 *pic =
        (VAEncPictureParameterBufferH264*) buffer->ptr;

    /* Only log first few frames to avoid flooding at 60fps */
    if (nvencCtx->frameCount < 3) {
        LOG("H264 encode: picture params, coded_buf=%d, pic_fields=0x%x",
            pic->coded_buf, pic->pic_fields.value);
    }

    nvencCtx->currentCodedBufId = pic->coded_buf;
    nvencCtx->forceIDR = (pic->pic_fields.bits.idr_pic_flag != 0);
    if (nvencCtx->forceIDR) {
        LOG("H264 encode: IDR requested, coded_buf=%d", pic->coded_buf);
    }
}

void h264enc_handle_slice_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    const VAEncSliceParameterBufferH264 *slice =
        (VAEncSliceParameterBufferH264*) buffer->ptr;

    /* Map VA-API H.264 slice_type to NVENC picture type.
     * Currently unused (enablePTD=1), but kept for future B-frame support. */
    switch (slice->slice_type) {
    case 2: case 7: /* I / SI */
        nvencCtx->picType = nvencCtx->forceIDR
            ? NV_ENC_PIC_TYPE_IDR : NV_ENC_PIC_TYPE_I;
        break;
    case 0: case 5: /* P / SP */
        nvencCtx->picType = NV_ENC_PIC_TYPE_P;
        break;
    case 1: case 6: /* B */
        nvencCtx->picType = NV_ENC_PIC_TYPE_B;
        break;
    default:
        nvencCtx->picType = NV_ENC_PIC_TYPE_UNKNOWN;
        break;
    }
}

void h264enc_handle_misc_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    VAEncMiscParameterBuffer *misc = (VAEncMiscParameterBuffer*) buffer->ptr;

    switch (misc->type) {
    case VAEncMiscParameterTypeRateControl: {
        VAEncMiscParameterRateControl *rc =
            (VAEncMiscParameterRateControl*) misc->data;
        LOG("H264 encode: rate control bits_per_second=%u, target_percentage=%u",
            rc->bits_per_second, rc->target_percentage);
        if (rc->bits_per_second > 0) {
            nvencCtx->maxBitrate = rc->bits_per_second;
            if (rc->target_percentage > 0) {
                nvencCtx->bitrate = (uint32_t)((uint64_t)rc->bits_per_second * rc->target_percentage / 100);
            } else {
                nvencCtx->bitrate = rc->bits_per_second;
            }
        }
        break;
    }
    case VAEncMiscParameterTypeFrameRate: {
        const VAEncMiscParameterFrameRate *fr =
            (VAEncMiscParameterFrameRate*) misc->data;
        if (fr->framerate > 0) {
            /* framerate can be packed as (num | (den << 16)) or just num */
            uint32_t num = fr->framerate & 0xffff;
            uint32_t den = (fr->framerate >> 16) & 0xffff;
            if (den == 0) den = 1;
            nvencCtx->frameRateNum = num;
            nvencCtx->frameRateDen = den;
            LOG("H264 encode: framerate %u/%u", num, den);
        }
        break;
    }
    case VAEncMiscParameterTypeHRD: {
        VAEncMiscParameterHRD *hrd =
            (VAEncMiscParameterHRD*) misc->data;
        if (hrd->buffer_size > 0)
            nvencCtx->vbvBufferSize = hrd->buffer_size;
        if (hrd->initial_buffer_fullness > 0)
            nvencCtx->vbvInitialDelay = hrd->initial_buffer_fullness;
        break;
    }
    default:
        LOG("H264 encode: unhandled misc param type %d", misc->type);
        break;
    }
}
