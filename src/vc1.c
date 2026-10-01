#include "vabackend.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

// Context-owned state and copied slice parameters share one allocation, released
// by the backend's normal codecData teardown. Pending surface IDs are protected
// by drv->objectCreationMutex; never retain a surface pointer across submissions.
typedef struct {
    VAStatus pictureStatus;
    bool pictureParameters;
    bool pictureActive;
    bool fieldPending;
    VASurfaceID fieldSurface;
    VASurfaceID renderSurface;
    int topFieldFirst;
    VASliceParameterBufferVC1 slices[];
} VC1Context;

static VAStatus checkVC1Picture(NVContext *ctx, NVPictureOperation operation) {
    VC1Context *vc1 = ctx->codecData;
    if (operation == NV_PICTURE_BEGIN) {
        if (vc1 == NULL) {
            vc1 = malloc(sizeof(*vc1));
            if (vc1 == NULL) return VA_STATUS_ERROR_ALLOCATION_FAILED;
            *vc1 = (VC1Context) { .fieldSurface = VA_INVALID_ID, .renderSurface = VA_INVALID_ID };
            ctx->codecData = vc1;
        }
        return vc1->pictureActive ? VA_STATUS_ERROR_OPERATION_FAILED : VA_STATUS_SUCCESS;
    }
    if (vc1 == NULL || !vc1->pictureActive) return VA_STATUS_ERROR_OPERATION_FAILED;
    if (operation == NV_PICTURE_END) vc1->pictureActive = false;
    return VA_STATUS_SUCCESS;
}

static void cancelVC1FieldLocked(NVContext *ctx) {
    VC1Context *vc1 = ctx->codecData;
    if (vc1 == NULL || !vc1->fieldPending) return;
    NVSurface *surface = nvGetSurface(ctx->drv, vc1->fieldSurface);
    if (surface != NULL) {
        surface->decodeFailed = true;
        nvSetSurfaceResolving(surface, false);
    }
    vc1->fieldPending = false;
}

static void abortVC1Picture(NVContext *ctx) {
    VC1Context *vc1 = ctx->codecData;
    ctx->bitstreamBuffer.size = 0;
    ctx->sliceOffsets.size = 0;
    if (vc1 == NULL) return;
    pthread_mutex_lock(&ctx->drv->objectCreationMutex);
    cancelVC1FieldLocked(ctx);
    NVSurface *surface = nvGetSurface(ctx->drv, vc1->renderSurface);
    if (surface != NULL) {
        surface->decodeFailed = true;
        nvSetSurfaceResolving(surface, false);
    }
    pthread_mutex_unlock(&ctx->drv->objectCreationMutex);
}

static void vc1SurfaceDestroyed(NVContext *ctx, VASurfaceID id, NVSurface *surface) {
    VC1Context *vc1 = ctx->codecData;
    if (vc1 != NULL && vc1->fieldPending && vc1->fieldSurface == id) {
        vc1->fieldPending = false;
        surface->decodeFailed = true;
        nvSetSurfaceResolving(surface, false);
    }
}

static VAStatus prepareVC1Decode(NVContext *ctx) {
    VC1Context *vc1 = ctx->codecData;
    if (vc1 == NULL) {
        abortVC1Picture(ctx);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    CUVIDPICPARAMS *params = &ctx->pPicParams;
    VAStatus status = vc1->pictureStatus;
    if (status == VA_STATUS_SUCCESS && (!vc1->pictureParameters || params->nNumSlices == 0 ||
        ctx->lastSliceParamsCount != 0)) status = VA_STATUS_ERROR_INVALID_BUFFER;
    pthread_mutex_lock(&ctx->drv->objectCreationMutex);
    if (status == VA_STATUS_SUCCESS && params->field_pic_flag && params->second_field) {
        if (!vc1->fieldPending || vc1->fieldSurface != vc1->renderSurface)
            status = VA_STATUS_ERROR_INVALID_PARAMETER;
    } else {
        cancelVC1FieldLocked(ctx);
        if (status == VA_STATUS_SUCCESS) nvSetSurfaceResolving(ctx->renderTarget, true);
    }
    pthread_mutex_unlock(&ctx->drv->objectCreationMutex);
    if (status != VA_STATUS_SUCCESS) abortVC1Picture(ctx);
    return status;
}

static bool finishVC1Decode(NVContext *ctx, VAStatus status) {
    VC1Context *vc1 = ctx->codecData;
    if (vc1 == NULL) {
        abortVC1Picture(ctx);
        return false;
    }
    ctx->renderTarget->topFieldFirst = vc1->topFieldFirst;
    // The first field is decoded, but the shared output is not displayable yet.
    if (ctx->profile == VAProfileVC1Advanced && ctx->pPicParams.field_pic_flag &&
        !ctx->pPicParams.second_field) {
        if (status != VA_STATUS_SUCCESS) {
            abortVC1Picture(ctx);
        } else {
            pthread_mutex_lock(&ctx->drv->objectCreationMutex);
            vc1->fieldPending = true;
            vc1->fieldSurface = vc1->renderSurface;
            pthread_mutex_unlock(&ctx->drv->objectCreationMutex);
        }
        return false;
    }
    pthread_mutex_lock(&ctx->drv->objectCreationMutex);
    vc1->fieldPending = false;
    pthread_mutex_unlock(&ctx->drv->objectCreationMutex);
    return true;
}

static void beginVC1Picture(NVContext *ctx, VASurfaceID renderTarget) {
    VC1Context *vc1 = ctx->codecData;
    if (vc1 == NULL) {
        abortVC1Picture(ctx);
        return;
    }
    vc1->renderSurface = renderTarget;
    vc1->pictureStatus = VA_STATUS_SUCCESS;
    vc1->pictureParameters = false;
    vc1->pictureActive = true;
    ctx->lastSliceParams = NULL;
    ctx->lastSliceParamsCount = 0;
    ctx->bitstreamBuffer.size = 0;
    ctx->sliceOffsets.size = 0;
    ctx->pPicParams.nNumSlices = 0;
    ctx->pPicParams.nBitstreamDataLen = 0;
}

static void failVC1Picture(NVContext *ctx, VAStatus status) {
    VC1Context *vc1 = ctx->codecData;
    if (vc1 != NULL && vc1->pictureStatus == VA_STATUS_SUCCESS) vc1->pictureStatus = status;
    ctx->bitstreamBuffer.size = 0;
    ctx->sliceOffsets.size = 0;
    ctx->pPicParams.nNumSlices = 0;
    ctx->pPicParams.nBitstreamDataLen = 0;
}

// The generic appendBuffer cannot report allocation failure. Reserve both VC-1
// buffers before appending so no partial submission reaches the decoder.
static bool reserveVC1Buffer(AppendableBuffer *buffer, uint64_t required) {
    if (required > SIZE_MAX || required > UINT_MAX) return false;
    if (required <= buffer->allocated) return true;
    void *data = realloc(buffer->buf, (size_t) required);
    if (data == NULL) return false;
    buffer->buf = data;
    buffer->allocated = required;
    return true;
}

static void copyVC1PicParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
{
    if (ctx->codecData == NULL) {
        failVC1Picture(ctx, VA_STATUS_ERROR_OPERATION_FAILED);
        return;
    }
    VC1Context *vc1 = ctx->codecData;
    if (buffer->ptr == NULL || buffer->elements != 1 ||
        buffer->size < sizeof(VAPictureParameterBufferVC1)) {
        failVC1Picture(ctx, VA_STATUS_ERROR_INVALID_BUFFER);
        return;
    }
    VAPictureParameterBufferVC1* buf = (VAPictureParameterBufferVC1*) buffer->ptr;
    unsigned int fcm = buf->picture_fields.bits.frame_coding_mode;
    unsigned int type = buf->picture_fields.bits.picture_type;
    unsigned int profile = ctx->profile == VAProfileVC1Advanced ? 3 :
        (ctx->profile == VAProfileVC1Main ? 1 : 0);
    if (vc1->pictureParameters || buf->coded_width == 0 || buf->coded_height == 0 ||
        buf->coded_width > ctx->width || buf->coded_height > ctx->height ||
        fcm > 2 || (fcm != 0 && (!buf->sequence_fields.bits.interlace || profile != 3)) ||
        (fcm != 2 && type > 4) || buf->sequence_fields.bits.profile != profile) {
        failVC1Picture(ctx, VA_STATUS_ERROR_INVALID_PARAMETER);
        return;
    }
    vc1->pictureParameters = true;

    picParams->PicWidthInMbs = (buf->coded_width + 15)/16;
    picParams->FrameHeightInMbs = (buf->coded_height + 15)/16;

    int interlaced = buf->picture_fields.bits.frame_coding_mode == 2;
    int field_mode = buf->sequence_fields.bits.interlace && interlaced;

    ctx->renderTarget->progressiveFrame = buf->picture_fields.bits.frame_coding_mode == 0;
    ctx->renderTarget->topFieldFirst = buf->picture_fields.bits.top_field_first;
    vc1->topFieldFirst = ctx->renderTarget->topFieldFirst;
    picParams->field_pic_flag    = buf->sequence_fields.bits.interlace && interlaced;
    picParams->bottom_field_flag = field_mode && !(buf->picture_fields.bits.top_field_first ^ !buf->picture_fields.bits.is_first_field);

    picParams->second_field      = field_mode && !buf->picture_fields.bits.is_first_field;

    if (field_mode) {
        // FPTYPE describes a pair: I/I, I/P, P/I, P/P, B/B, B/BI, BI/B, BI/BI.
        static const uint8_t firstType[]  = {0, 0, 1, 1, 2, 2, 3, 3};
        static const uint8_t secondType[] = {0, 1, 0, 1, 2, 3, 2, 3};
        type = picParams->second_field ? secondType[type] : firstType[type];
    }
    picParams->intra_pic_flag = type == 0 || type == 3;
    picParams->ref_pic_flag = type == 0 || type == 1 || type == 4;

    CUVIDVC1PICPARAMS *pps = &picParams->CodecSpecific.vc1;
    pps->intra_pic_flag = picParams->intra_pic_flag;
    pps->ref_pic_flag = picParams->ref_pic_flag;

    pps->ForwardRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->forward_reference_picture);
    pps->BackwardRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->backward_reference_picture);

    pps->FrameWidth = buf->coded_width;
    pps->FrameHeight = buf->coded_height;
    pps->progressive_fcm = buf->picture_fields.bits.frame_coding_mode == 0;
    pps->profile = buf->sequence_fields.bits.profile;
    pps->postprocflag = buf->post_processing != 0;
    pps->pulldown = buf->sequence_fields.bits.pulldown;
    pps->interlace = buf->sequence_fields.bits.interlace;
    pps->tfcntrflag = buf->sequence_fields.bits.tfcntrflag;
    pps->finterpflag = buf->sequence_fields.bits.finterpflag;
    pps->psf = buf->sequence_fields.bits.psf;
    pps->multires = buf->sequence_fields.bits.multires;
    pps->syncmarker = buf->sequence_fields.bits.syncmarker;
    pps->rangered = buf->sequence_fields.bits.rangered;
    pps->maxbframes = buf->sequence_fields.bits.max_b_frames;
    pps->panscan_flag = buf->entrypoint_fields.bits.panscan_flag;
    pps->refdist_flag = buf->reference_fields.bits.reference_distance_flag;
    pps->extended_mv = buf->mv_fields.bits.extended_mv_flag;
    pps->dquant = buf->pic_quantizer_fields.bits.dquant;
    pps->vstransform = buf->transform_fields.bits.variable_sized_transform_flag;
    pps->loopfilter = buf->entrypoint_fields.bits.loopfilter;
    pps->fastuvmc = buf->fast_uvmc_flag;
    pps->overlap = buf->sequence_fields.bits.overlap;
    pps->quantizer = buf->pic_quantizer_fields.bits.quantizer;
    pps->extended_dmv = buf->mv_fields.bits.extended_dmv_flag;
    pps->range_mapy_flag = buf->range_mapping_fields.bits.luma_flag;
    pps->range_mapy = buf->range_mapping_fields.bits.luma;
    pps->range_mapuv_flag = buf->range_mapping_fields.bits.chroma_flag;
    pps->range_mapuv = buf->range_mapping_fields.bits.chroma;
    pps->rangeredfrm = buf->range_reduction_frame;
}

static void copyVC1SliceParam(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    if (ctx->codecData == NULL) {
        failVC1Picture(ctx, VA_STATUS_ERROR_OPERATION_FAILED);
        return;
    }
    (void) picParams;
    if (ctx->lastSliceParamsCount != 0 || buf->ptr == NULL || buf->elements == 0 ||
        buf->elements > buf->size / sizeof(VASliceParameterBufferVC1)) {
        failVC1Picture(ctx, VA_STATUS_ERROR_INVALID_BUFFER);
        return;
    }
    // RenderPicture consumes buffers before returning. The client may destroy
    // its parameter buffer before submitting slice data in a later call.
    size_t size = sizeof(VASliceParameterBufferVC1) * buf->elements;
    if (size > SIZE_MAX - sizeof(VC1Context)) {
        failVC1Picture(ctx, VA_STATUS_ERROR_ALLOCATION_FAILED);
        return;
    }
    // Surface teardown also reads codecData; keep its lookup synchronized with
    // realloc moving the context-owned state.
    pthread_mutex_lock(&ctx->drv->objectCreationMutex);
    VC1Context *params = realloc(ctx->codecData, sizeof(*params) + size);
    if (params == NULL) {
        pthread_mutex_unlock(&ctx->drv->objectCreationMutex);
        failVC1Picture(ctx, VA_STATUS_ERROR_ALLOCATION_FAILED);
        return;
    }
    ctx->codecData = params;
    memcpy(params->slices, buf->ptr, size);
    ctx->lastSliceParams = params->slices;
    ctx->lastSliceParamsCount = buf->elements;
    pthread_mutex_unlock(&ctx->drv->objectCreationMutex);
}

static void copyVC1SliceData(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    if (ctx->codecData == NULL) {
        failVC1Picture(ctx, VA_STATUS_ERROR_OPERATION_FAILED);
        return;
    }
    VC1Context *vc1 = ctx->codecData;
    if (!vc1->pictureParameters || ctx->lastSliceParams == NULL ||
        ctx->lastSliceParamsCount == 0 || buf->ptr == NULL || buf->size == 0) {
        failVC1Picture(ctx, VA_STATUS_ERROR_INVALID_BUFFER);
        return;
    }
    for (unsigned int i = 0; i < ctx->lastSliceParamsCount; i++) {
        const VASliceParameterBufferVC1 *slice = &((VASliceParameterBufferVC1 *) ctx->lastSliceParams)[i];
        if (slice->slice_data_size == 0 || slice->slice_data_offset > buf->size ||
            slice->slice_data_size > buf->size - slice->slice_data_offset ||
            (uint64_t) slice->macroblock_offset > (uint64_t) slice->slice_data_size * 8) {
            failVC1Picture(ctx, VA_STATUS_ERROR_INVALID_PARAMETER);
            return;
        }
        // Split slices are not advertised; never decode a partial slice as a picture.
        if (slice->slice_data_flag != VA_SLICE_DATA_FLAG_ALL) {
            failVC1Picture(ctx, VA_STATUS_ERROR_UNIMPLEMENTED);
            return;
        }
        const uint8_t *data = (const uint8_t *) buf->ptr + slice->slice_data_offset;
        const uint8_t marker = picParams->nNumSlices != 0 ? 0x0b :
            (picParams->field_pic_flag && picParams->second_field ? 0x0c : 0x0d);
        const bool advanced = ctx->profile == VAProfileVC1Advanced;
        const bool hasMarker = advanced && slice->slice_data_size >= 4 &&
            data[0] == 0 && data[1] == 0 && data[2] == 1;
        if (hasMarker && data[3] != marker) {
            failVC1Picture(ctx, VA_STATUS_ERROR_INVALID_PARAMETER);
            return;
        }
        const uint64_t prefix = advanced && !hasMarker ? 4 : 0;
        const uint64_t total = ctx->bitstreamBuffer.size + prefix + slice->slice_data_size;
        if (picParams->nNumSlices == UINT_MAX ||
            !reserveVC1Buffer(&ctx->bitstreamBuffer, total) ||
            !reserveVC1Buffer(&ctx->sliceOffsets, ctx->sliceOffsets.size + sizeof(uint32_t))) {
            failVC1Picture(ctx, VA_STATUS_ERROR_ALLOCATION_FAILED);
            return;
        }
        uint32_t offset = (uint32_t) ctx->bitstreamBuffer.size;
        memcpy((uint8_t *) ctx->sliceOffsets.buf + ctx->sliceOffsets.size, &offset, sizeof(offset));
        ctx->sliceOffsets.size += sizeof(offset);
        uint8_t *out = (uint8_t *) ctx->bitstreamBuffer.buf + offset;
        if (prefix != 0) {
            const uint8_t startCode[] = {0, 0, 1, marker};
            memcpy(out, startCode, sizeof(startCode));
        }
        memcpy(out + prefix, data, slice->slice_data_size);
        ctx->bitstreamBuffer.size = total;
        picParams->nBitstreamDataLen = (unsigned int) total;
        picParams->nNumSlices++;
    }
    ctx->lastSliceParams = NULL;
    ctx->lastSliceParamsCount = 0;
}

static void copyVC1BitPlane(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    // VA-API bitplanes are decoded per-macroblock flags, not compressed data.
    // NVDEC parses the original coded bitplanes from the submitted slice bytes.
    (void) ctx;
    (void) buf;
    (void) picParams;
}
static VAStatus renderVC1Picture(NVContext *ctx, VABufferID *buffers, int count) {
    if (ctx->codecData == NULL || ctx->codec == NULL) {
        abortVC1Picture(ctx);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (count < 0 || (count != 0 && buffers == NULL))
        failVC1Picture(ctx, VA_STATUS_ERROR_INVALID_PARAMETER);
    for (int i = 0; i < count; i++) {
        VC1Context *vc1 = ctx->codecData;
        if (vc1->pictureStatus != VA_STATUS_SUCCESS) break;
        NVBuffer *buffer = nvGetBuffer(ctx->drv, buffers[i]);
        if (buffer == NULL || buffer->ptr == NULL || (unsigned) buffer->bufferType >= VABufferTypeMax) {
            failVC1Picture(ctx, VA_STATUS_ERROR_INVALID_BUFFER);
            break;
        }
        HandlerFunc handler = ctx->codec->handlers[buffer->bufferType];
        if (handler == NULL) {
            failVC1Picture(ctx, VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
            break;
        }
        handler(ctx, buffer, &ctx->pPicParams);
    }
    VC1Context *vc1 = ctx->codecData;
    VAStatus status = vc1->pictureStatus;
    if (status != VA_STATUS_SUCCESS) abortVC1Picture(ctx);
    return status;
}

static cudaVideoCodec computeVC1CudaCodec(VAProfile profile) {
    if (profile == VAProfileVC1Advanced || profile == VAProfileVC1Main || profile == VAProfileVC1Simple) {
        return cudaVideoCodec_VC1;
    }

    return cudaVideoCodec_NONE;
}

static const VAProfile vc1SupportedProfiles[] = {
    VAProfileVC1Simple,
    VAProfileVC1Main,
    VAProfileVC1Advanced,
};

const DECLARE_CODEC(vc1Codec) = {
    .computeCudaCodec = computeVC1CudaCodec,
    .beginPicture = beginVC1Picture,
    .checkPicture = checkVC1Picture,
    .renderPicture = renderVC1Picture,
    .prepareDecode = prepareVC1Decode,
    .finishDecode = finishVC1Decode,
    .abortPicture = abortVC1Picture,
    .surfaceDestroyed = vc1SurfaceDestroyed,
    .handlers = {
        [VAPictureParameterBufferType] = copyVC1PicParam,
        [VASliceParameterBufferType] = copyVC1SliceParam,
        [VASliceDataBufferType] = copyVC1SliceData,
        [VABitPlaneBufferType] = copyVC1BitPlane,
    },
    .supportedProfileCount = ARRAY_SIZE(vc1SupportedProfiles),
    .supportedProfiles = vc1SupportedProfiles,
};
