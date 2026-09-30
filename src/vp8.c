#include "vabackend.h"

/* Size of the VP8 "uncompressed data chunk" that the client keeps out of the
 * slice data buffer and instead describes through the picture parameter
 * buffer:
 *   key frame   : frame tag (3 bytes) + key frame header (start code, width,
 *                 height) = 10 bytes
 *   inter frame : frame tag (3 bytes)
 * NVDEC expects the complete frame, so the driver has to rebuild that header
 * from the VA-API parameters before handing the partitions to the decoder. */
#define VP8_UNCOMPRESSED_CHUNK_KEYFRAME   10
#define VP8_UNCOMPRESSED_CHUNK_INTERFRAME  3
#define VP8_KEYFRAME_STARTCODE0 0x9d
#define VP8_KEYFRAME_STARTCODE1 0x01
#define VP8_KEYFRAME_STARTCODE2 0x2a

static void copyVP8PicParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
{
    VAPictureParameterBufferVP8* buf = (VAPictureParameterBufferVP8*) buffer->ptr;

    picParams->PicWidthInMbs    = (buf->frame_width + 15) / 16;
    picParams->FrameHeightInMbs = (buf->frame_height + 15) / 16;

    picParams->CodecSpecific.vp8.width = buf->frame_width;
    picParams->CodecSpecific.vp8.height = buf->frame_height;

    picParams->CodecSpecific.vp8.LastRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->last_ref_frame);
    picParams->CodecSpecific.vp8.GoldenRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->golden_ref_frame);
    picParams->CodecSpecific.vp8.AltRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->alt_ref_frame);

    picParams->CodecSpecific.vp8.vp8_frame_tag.frame_type = buf->pic_fields.bits.key_frame;
    picParams->CodecSpecific.vp8.vp8_frame_tag.version = buf->pic_fields.bits.version;
    /* VA-API has no show_frame bit, and it is not part of the slice data the
     * client submits. Decoded frames are displayable frames unless the client
     * uses the frame purely as a reference, so report it as shown. Reading bit
     * 4 of the first partition byte (what this used to do) yields a garbage
     * value because that byte is token data, not a frame tag. */
    picParams->CodecSpecific.vp8.vp8_frame_tag.show_frame = 1;
    picParams->CodecSpecific.vp8.vp8_frame_tag.update_mb_segmentation_data = buf->pic_fields.bits.segmentation_enabled ? buf->pic_fields.bits.update_segment_feature_data : 0;
}

static void copyVP8SliceParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
{
    VASliceParameterBufferVP8* buf = (VASliceParameterBufferVP8*) buffer->ptr;

    // VA-API reports the first partition size excluding the uncompressed data
    // chunk, which is exactly the first_part_size field of the VP8 frame tag.
    picParams->CodecSpecific.vp8.first_partition_size = buf->partition_size[0] + ((buf->macroblock_offset + 7) / 8);

    ctx->lastSliceParams = buffer->ptr;
    ctx->lastSliceParamsCount = buffer->elements;

    picParams->nNumSlices += buffer->elements;
}

static void copyVP8SliceData(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    if (ctx->lastSliceParamsCount == 0) {
        LOG("VP8 slice data without slice parameters");
        return;
    }

    bool isKeyFrame = (picParams->CodecSpecific.vp8.vp8_frame_tag.frame_type == 0);
    size_t headerSize = isKeyFrame ? VP8_UNCOMPRESSED_CHUNK_KEYFRAME : VP8_UNCOMPRESSED_CHUNK_INTERFRAME;

    // Rebuild the frame tag: frame_type, version, show_frame and the 19 bit
    // first_part_size (least significant bit first, see RFC 6386 9.1).
    uint32_t firstPartitionSize = picParams->CodecSpecific.vp8.first_partition_size;
    uint8_t frameTag[VP8_UNCOMPRESSED_CHUNK_KEYFRAME] = {0};

    frameTag[0] = (uint8_t) ((isKeyFrame ? 0 : 0x01) |
                             (picParams->CodecSpecific.vp8.vp8_frame_tag.version << 1) |
                             (picParams->CodecSpecific.vp8.vp8_frame_tag.show_frame << 4) |
                             ((firstPartitionSize & 0x7) << 5));
    frameTag[1] = (uint8_t) ((firstPartitionSize >> 3) & 0xff);
    frameTag[2] = (uint8_t) ((firstPartitionSize >> 11) & 0xff);

    if (isKeyFrame) {
        frameTag[3] = VP8_KEYFRAME_STARTCODE0;
        frameTag[4] = VP8_KEYFRAME_STARTCODE1;
        frameTag[5] = VP8_KEYFRAME_STARTCODE2;
        uint32_t width = (uint32_t) picParams->CodecSpecific.vp8.width & 0x7fff;
        uint32_t height = (uint32_t) picParams->CodecSpecific.vp8.height & 0x7fff;
        frameTag[6] = (uint8_t) (width & 0xff);
        frameTag[7] = (uint8_t) ((width >> 8) & 0xff);
        frameTag[8] = (uint8_t) (height & 0xff);
        frameTag[9] = (uint8_t) ((height >> 8) & 0xff);
    }

    for (unsigned int i = 0; i < ctx->lastSliceParamsCount; i++)
    {
        VASliceParameterBufferVP8 *sliceParams = &((VASliceParameterBufferVP8*) ctx->lastSliceParams)[i];

        // Only ever read inside the buffer the client gave us: reading before
        // the buffer (which this driver used to do to recover the frame header)
        // returned unrelated memory and made every frame decode to garbage.
        if ((uint64_t) sliceParams->slice_data_offset + sliceParams->slice_data_size > buf->size) {
            LOG("VP8 slice %u out of bounds (offset %u + size %u > buffer size %zu)",
                i, sliceParams->slice_data_offset, sliceParams->slice_data_size, buf->size);
            picParams->nBitstreamDataLen = 0;
            ctx->bitstreamBuffer.size = 0;
            ctx->sliceOffsets.size = 0;
            return;
        }

        uint32_t offset = (uint32_t) ctx->bitstreamBuffer.size;
        appendBuffer(&ctx->sliceOffsets, &offset, sizeof(offset));
        appendBuffer(&ctx->bitstreamBuffer, frameTag, headerSize);
        appendBuffer(&ctx->bitstreamBuffer, PTROFF(buf->ptr, sliceParams->slice_data_offset),
                     sliceParams->slice_data_size);
        picParams->nBitstreamDataLen += headerSize + sliceParams->slice_data_size;
    }
}

static void ignoreVP8Buffer(NVContext *ctx, NVBuffer *buffer, CUVIDPICPARAMS *picParams)
{
    // Intentionally do nothing
    (void)ctx;
    (void)buffer;
    (void)picParams;
}

static cudaVideoCodec computeVP8CudaCodec(VAProfile profile) {
    if (profile == VAProfileVP8Version0_3) {
        return cudaVideoCodec_VP8;
    }

    return cudaVideoCodec_NONE;
}

static const VAProfile vp8SupportedProfiles[] = {
    VAProfileVP8Version0_3,
};

const DECLARE_CODEC(vp8Codec) = {
    .computeCudaCodec = computeVP8CudaCodec,
    .handlers = {
        [VAPictureParameterBufferType] = copyVP8PicParam,
        [VASliceParameterBufferType] = copyVP8SliceParam,
        [VASliceDataBufferType] = copyVP8SliceData,
        [VAIQMatrixBufferType]         = ignoreVP8Buffer,
        [VAProbabilityBufferType]      = ignoreVP8Buffer,
    },
    .supportedProfileCount = ARRAY_SIZE(vp8SupportedProfiles),
    .supportedProfiles = vp8SupportedProfiles,
};
