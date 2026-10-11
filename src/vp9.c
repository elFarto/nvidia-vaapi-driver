#include "vabackend.h"
#include <string.h>

//squash a compile warning, we know this is an unstable API
#define GST_USE_UNSTABLE_API
#include <gst/codecparsers/gstvp9parser.h>

static void copyVP9PicParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
{
    VADecPictureParameterBufferVP9* buf = (VADecPictureParameterBufferVP9*) buffer->ptr;

    picParams->PicWidthInMbs    = (buf->frame_width + 15) / 16;
    picParams->FrameHeightInMbs = (buf->frame_height + 15) / 16;

    picParams->CodecSpecific.vp9.width = buf->frame_width;
    picParams->CodecSpecific.vp9.height = buf->frame_height;

    picParams->CodecSpecific.vp9.LastRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->reference_frames[buf->pic_fields.bits.last_ref_frame]);
    picParams->CodecSpecific.vp9.GoldenRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->reference_frames[buf->pic_fields.bits.golden_ref_frame]);
    picParams->CodecSpecific.vp9.AltRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->reference_frames[buf->pic_fields.bits.alt_ref_frame]);

    picParams->CodecSpecific.vp9.profile = buf->profile;
    picParams->CodecSpecific.vp9.frameContextIdx = buf->pic_fields.bits.frame_context_idx;

    picParams->CodecSpecific.vp9.frameContextIdx = buf->pic_fields.bits.frame_context_idx;
    picParams->CodecSpecific.vp9.frameType = buf->pic_fields.bits.frame_type;
    picParams->CodecSpecific.vp9.showFrame = buf->pic_fields.bits.show_frame;
    picParams->CodecSpecific.vp9.errorResilient = buf->pic_fields.bits.error_resilient_mode;
    picParams->CodecSpecific.vp9.frameParallelDecoding = buf->pic_fields.bits.frame_parallel_decoding_mode;

    picParams->CodecSpecific.vp9.subSamplingX = buf->pic_fields.bits.subsampling_x;
    picParams->CodecSpecific.vp9.subSamplingY = buf->pic_fields.bits.subsampling_y;
    picParams->CodecSpecific.vp9.intraOnly = buf->pic_fields.bits.intra_only;
    picParams->CodecSpecific.vp9.allow_high_precision_mv = buf->pic_fields.bits.allow_high_precision_mv;
    picParams->CodecSpecific.vp9.refreshEntropyProbs = buf->pic_fields.bits.refresh_frame_context;

    picParams->CodecSpecific.vp9.bitDepthMinus8Luma = buf->bit_depth - 8;
    picParams->CodecSpecific.vp9.bitDepthMinus8Chroma = buf->bit_depth - 8;

    picParams->CodecSpecific.vp9.loopFilterLevel = buf->filter_level;
    picParams->CodecSpecific.vp9.loopFilterSharpness = buf->sharpness_level;

    picParams->CodecSpecific.vp9.log2_tile_columns = buf->log2_tile_columns;
    picParams->CodecSpecific.vp9.log2_tile_rows = buf->log2_tile_rows;

    picParams->CodecSpecific.vp9.segmentEnabled = buf->pic_fields.bits.segmentation_enabled;
    picParams->CodecSpecific.vp9.segmentMapUpdate = buf->pic_fields.bits.segmentation_update_map;
    picParams->CodecSpecific.vp9.segmentMapTemporalUpdate = buf->pic_fields.bits.segmentation_temporal_update;

    picParams->CodecSpecific.vp9.resetFrameContext = buf->pic_fields.bits.reset_frame_context;
    picParams->CodecSpecific.vp9.mcomp_filter_type = buf->pic_fields.bits.mcomp_filter_type;
    picParams->CodecSpecific.vp9.frameTagSize = buf->frame_header_length_in_bytes;
    picParams->CodecSpecific.vp9.offsetToDctParts = buf->first_partition_size;

    for (int i = 0; i < 7; i++) {
        picParams->CodecSpecific.vp9.mb_segment_tree_probs[i] = buf->mb_segment_tree_probs[i];
    }

    //yes, refFrameSignBias is meant to be offset by 1
    picParams->CodecSpecific.vp9.activeRefIdx[0]     = buf->pic_fields.bits.last_ref_frame;
    picParams->CodecSpecific.vp9.refFrameSignBias[1] = buf->pic_fields.bits.last_ref_frame_sign_bias;
    picParams->CodecSpecific.vp9.activeRefIdx[1]     = buf->pic_fields.bits.golden_ref_frame;
    picParams->CodecSpecific.vp9.refFrameSignBias[2] = buf->pic_fields.bits.golden_ref_frame_sign_bias;
    picParams->CodecSpecific.vp9.activeRefIdx[2]     = buf->pic_fields.bits.alt_ref_frame;
    picParams->CodecSpecific.vp9.refFrameSignBias[3] = buf->pic_fields.bits.alt_ref_frame_sign_bias;

    for (int i = 0; i < 3; i++) {
        picParams->CodecSpecific.vp9.segment_pred_probs[i] = buf->segment_pred_probs[i];
    }
}

static GstVp9Parser *parser;

// Loop-filter deltas and segment features persist across frames until a
// frame header updates them. The parser reports only the current frame's
// updates, so keep the decoder state here, next to the parser it follows.
static int8_t refLfDeltas[GST_VP9_MAX_REF_LF_DELTAS];
static int8_t modeLfDeltas[GST_VP9_MAX_MODE_LF_DELTAS];
static bool segmentAbsDelta;
static bool segmentFeatureEnabled[GST_VP9_MAX_SEGMENTS][4];
static int16_t segmentFeatureData[GST_VP9_MAX_SEGMENTS][4];

static void updatePersistentState(const GstVp9FrameHdr *hdr) {
    // setup_past_independence: intra and error-resilient frames restore the
    // default deltas and clear every segment feature.
    if (hdr->frame_type == GST_VP9_KEY_FRAME || hdr->intra_only || hdr->error_resilient_mode) {
        static const int8_t defaultRefLfDeltas[GST_VP9_MAX_REF_LF_DELTAS] = {1, 0, -1, -1};
        memcpy(refLfDeltas, defaultRefLfDeltas, sizeof(refLfDeltas));
        memset(modeLfDeltas, 0, sizeof(modeLfDeltas));
        segmentAbsDelta = false;
        memset(segmentFeatureEnabled, 0, sizeof(segmentFeatureEnabled));
        memset(segmentFeatureData, 0, sizeof(segmentFeatureData));
    }

    const GstVp9LoopFilter *lf = &hdr->loopfilter;
    if (lf->mode_ref_delta_enabled && lf->mode_ref_delta_update) {
        for (int i = 0; i < GST_VP9_MAX_REF_LF_DELTAS; i++) {
            if (lf->update_ref_deltas[i]) {
                refLfDeltas[i] = lf->ref_deltas[i];
            }
        }
        for (int i = 0; i < GST_VP9_MAX_MODE_LF_DELTAS; i++) {
            if (lf->update_mode_deltas[i]) {
                modeLfDeltas[i] = lf->mode_deltas[i];
            }
        }
    }

    // New segment data replaces every feature; features left disabled are
    // cleared.
    const GstVp9SegmentationInfo *seg = &hdr->segmentation;
    if (seg->enabled && seg->update_data) {
        segmentAbsDelta = seg->abs_delta;
        for (int i = 0; i < GST_VP9_MAX_SEGMENTS; i++) {
            const GstVp9SegmentationInfoData *data = &seg->data[i];
            const bool enabled[4] = {data->alternate_quantizer_enabled,
                                     data->alternate_loop_filter_enabled,
                                     data->reference_frame_enabled,
                                     data->reference_skip};
            const int16_t values[4] = {data->alternate_quantizer,
                                       data->alternate_loop_filter,
                                       (int16_t) data->reference_frame,
                                       0};
            for (int j = 0; j < 4; j++) {
                segmentFeatureEnabled[i][j] = enabled[j];
                segmentFeatureData[i][j] = enabled[j] ? values[j] : 0;
            }
        }
    }
}

static VAProcColorStandardType vp9ColorStandard(GstVp9ColorSpace colorSpace) {
    switch (colorSpace) {
    case GST_VP9_CS_BT_601:
        return VAProcColorStandardBT601;
    case GST_VP9_CS_BT_709:
        return VAProcColorStandardBT709;
    case GST_VP9_CS_SMPTE_170:
        return VAProcColorStandardSMPTE170M;
    case GST_VP9_CS_SMPTE_240:
        return VAProcColorStandardSMPTE240M;
    case GST_VP9_CS_BT_2020:
        return VAProcColorStandardBT2020;
    case GST_VP9_CS_UNKNOWN:
    case GST_VP9_CS_RESERVED_2:
    case GST_VP9_CS_SRGB:
    default:
        return VAProcColorStandardNone;
    }
}

static void parseExtraInfo(NVContext *ctx, void *buf, uint32_t size, CUVIDPICPARAMS *picParams) {
    //TODO a bit of a hack as we don't have per decoder init/deinit functions atm
    if (parser == NULL) {
        parser = gst_vp9_parser_new ();
    }

    //parse all the extra information that VA-API doesn't support, but NVDEC requires
    GstVp9FrameHdr hdr;
    GstVp9ParserResult res = gst_vp9_parser_parse_frame_header(parser, &hdr, buf, size);

    if (res == GST_VP9_PARSER_OK) {
        updatePersistentState(&hdr);

        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 4; j++) {
                picParams->CodecSpecific.vp9.segmentFeatureEnable[i][j] = segmentFeatureEnabled[i][j];
                picParams->CodecSpecific.vp9.segmentFeatureData[i][j] = segmentFeatureData[i][j];
            }
        }

        picParams->CodecSpecific.vp9.segmentFeatureMode = segmentAbsDelta;

        picParams->CodecSpecific.vp9.modeRefLfEnabled = hdr.loopfilter.mode_ref_delta_enabled;
        for (int i = 0; i < 2; i++)
            picParams->CodecSpecific.vp9.mbModeLfDelta[i] = modeLfDeltas[i];

        for (int i = 0; i < 4; i++)
            picParams->CodecSpecific.vp9.mbRefLfDelta[i] = refLfDeltas[i];

        picParams->CodecSpecific.vp9.qpYAc = hdr.quant_indices.y_ac_qi;
        picParams->CodecSpecific.vp9.qpYDc = hdr.quant_indices.y_dc_delta;
        picParams->CodecSpecific.vp9.qpChDc = hdr.quant_indices.uv_dc_delta;
        picParams->CodecSpecific.vp9.qpChAc = hdr.quant_indices.uv_ac_delta;

        picParams->CodecSpecific.vp9.colorSpace = parser->color_space;
        VAProcColorStandardType colorStandard = vp9ColorStandard((GstVp9ColorSpace) parser->color_space);
        bool colorRangeFull = parser->color_range == GST_VP9_CR_FULL;
        nvSurfaceSetColorMetadata(ctx->renderTarget, colorStandard, colorRangeFull);
        LOG_DEBUG("VP9 color metadata: color_space=%u color_standard=%s(%d) full_range=%d render=%p",
            parser->color_space, nvColorStandardName(colorStandard), colorStandard, colorRangeFull,
            ctx->renderTarget);
    }

    //gst_vp9_parser_free(parser);
}

static void copyVP9SliceParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
{
    //don't bother doing anything here, we can just read it from the reparsed header

    ctx->lastSliceParams = buffer->ptr;
    ctx->lastSliceParamsCount = buffer->elements;

    picParams->nNumSlices += buffer->elements;
}

static void copyVP9SliceData(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    for (unsigned int i = 0; i < ctx->lastSliceParamsCount; i++)
    {
        VASliceParameterBufferVP9 *sliceParams = &((VASliceParameterBufferVP9*) ctx->lastSliceParams)[i];
        uint32_t offset = (uint32_t) ctx->bitstreamBuffer.size;
        appendBuffer(&ctx->sliceOffsets, &offset, sizeof(offset));
        appendBuffer(&ctx->bitstreamBuffer, PTROFF(buf->ptr, sliceParams->slice_data_offset), sliceParams->slice_data_size);

        //TODO this might not be the best place to call as we may not have a complete packet yet...
        parseExtraInfo(ctx, PTROFF(buf->ptr, sliceParams->slice_data_offset), sliceParams->slice_data_size, picParams);
        picParams->nBitstreamDataLen += sliceParams->slice_data_size;
    }
}

static cudaVideoCodec computeVP9CudaCodec(VAProfile profile) {
    switch (profile) {
        case VAProfileVP9Profile0:
        case VAProfileVP9Profile1:
        case VAProfileVP9Profile2:
        case VAProfileVP9Profile3:
            return cudaVideoCodec_VP9;
        default:
            return cudaVideoCodec_NONE;
    }
}

static const VAProfile vp9SupportedProfiles[] = {
    VAProfileVP9Profile0,
    VAProfileVP9Profile1,
    VAProfileVP9Profile2,
    VAProfileVP9Profile3,
};

const DECLARE_CODEC(vp9Codec) = {
    .computeCudaCodec = computeVP9CudaCodec,
    .handlers = {
        [VAPictureParameterBufferType] = copyVP9PicParam,
        [VASliceParameterBufferType] = copyVP9SliceParam,
        [VASliceDataBufferType] = copyVP9SliceData,
    },
    .supportedProfileCount = ARRAY_SIZE(vp9SupportedProfiles),
    .supportedProfiles = vp9SupportedProfiles,
};
