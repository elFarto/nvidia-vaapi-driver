#include "vabackend.h"
#include <string.h>

static const uint8_t ff_identity[64] = {
    0,   1,  2,  3,  4,  5,  6,  7,
    8,   9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55,
    56, 57, 58, 59, 60, 61, 62, 63
};

static const uint8_t ff_zigzag_direct[64] = {
    0,   1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

static const uint8_t ff_mpeg1_default_intra_matrix[64] = {
    8,  16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83
};

static const uint8_t ff_mpeg1_default_non_intra_matrix[64] = {
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16
};

static void copyMPEG2PicParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
{
    VAPictureParameterBufferMPEG2* buf = (VAPictureParameterBufferMPEG2*) buffer->ptr;

    picParams->PicWidthInMbs = (int) ( buf->horizontal_size + 15) / 16; //int
    picParams->FrameHeightInMbs = (int) ( buf->vertical_size + 15) / 16; //int

    LOG("buf->picture_coding_extension.bits.progressive_frame: %d",  buf->picture_coding_extension.bits.progressive_frame);
    ctx->renderTarget->progressiveFrame = buf->picture_coding_extension.bits.progressive_frame;
    picParams->field_pic_flag    = buf->picture_coding_extension.bits.picture_structure != 3;
    picParams->bottom_field_flag = buf->picture_coding_extension.bits.picture_structure == 2; //PICT_BOTTOM_FIELD
    picParams->second_field      = picParams->field_pic_flag && !buf->picture_coding_extension.bits.is_first_field;

    picParams->intra_pic_flag    = buf->picture_coding_type == 1; //Intra
    picParams->ref_pic_flag      = buf->picture_coding_type == 1 || //Intra
                                   buf->picture_coding_type == 2; //Predicted

    picParams->CodecSpecific.mpeg2.ForwardRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->forward_reference_picture);
    picParams->CodecSpecific.mpeg2.BackwardRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->backward_reference_picture);

    picParams->CodecSpecific.mpeg2.picture_coding_type = buf->picture_coding_type;
    picParams->CodecSpecific.mpeg2.full_pel_forward_vector = 0;
    picParams->CodecSpecific.mpeg2.full_pel_backward_vector = 0;
    picParams->CodecSpecific.mpeg2.f_code[0][0] = (buf->f_code >> 12) & 0xf;
    picParams->CodecSpecific.mpeg2.f_code[0][1] = (buf->f_code >>  8) & 0xf;
    picParams->CodecSpecific.mpeg2.f_code[1][0] = (buf->f_code >>  4) & 0xf;
    picParams->CodecSpecific.mpeg2.f_code[1][1] = buf->f_code & 0xf;
    picParams->CodecSpecific.mpeg2.intra_dc_precision = buf->picture_coding_extension.bits.intra_dc_precision;;
    picParams->CodecSpecific.mpeg2.frame_pred_frame_dct = buf->picture_coding_extension.bits.frame_pred_frame_dct;
    picParams->CodecSpecific.mpeg2.concealment_motion_vectors = buf->picture_coding_extension.bits.concealment_motion_vectors;
    picParams->CodecSpecific.mpeg2.q_scale_type = buf->picture_coding_extension.bits.q_scale_type;
    picParams->CodecSpecific.mpeg2.intra_vlc_format = buf->picture_coding_extension.bits.intra_vlc_format;
    picParams->CodecSpecific.mpeg2.alternate_scan = buf->picture_coding_extension.bits.alternate_scan;
    picParams->CodecSpecific.mpeg2.top_field_first = buf->picture_coding_extension.bits.top_field_first;
}

static void copyMPEG2SliceParam(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    ctx->lastSliceParams = buf->ptr;
    ctx->lastSliceParamsCount = buf->elements;

    picParams->nNumSlices += buf->elements;
}

static void copyMPEG2SliceData(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    for (unsigned int i = 0; i < ctx->lastSliceParamsCount; i++)
    {
        VASliceParameterBufferMPEG2 *sliceParams = &((VASliceParameterBufferMPEG2*) ctx->lastSliceParams)[i];
        uint32_t offset = (uint32_t) ctx->bitstreamBuffer.size;
        appendBuffer(&ctx->sliceOffsets, &offset, sizeof(offset));
        appendBuffer(&ctx->bitstreamBuffer, PTROFF(buf->ptr, sliceParams->slice_data_offset), sliceParams->slice_data_size);
        picParams->nBitstreamDataLen += sliceParams->slice_data_size;
    }
}

static void copyMPEG2IQMatrix(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    VAIQMatrixBufferMPEG2 *iq = (VAIQMatrixBufferMPEG2*) buf->ptr;

    const uint8_t *intra_matrix, *intra_matrix_lookup;
    const uint8_t *inter_matrix, *inter_matrix_lookup;

    if (iq->load_intra_quantiser_matrix) {
        intra_matrix = iq->intra_quantiser_matrix;
        intra_matrix_lookup = ff_zigzag_direct;
    }
    else {
        intra_matrix = ff_mpeg1_default_intra_matrix;
        intra_matrix_lookup = ff_identity;
    }
    if (iq->load_non_intra_quantiser_matrix) {
        inter_matrix = iq->non_intra_quantiser_matrix;
        inter_matrix_lookup = ff_zigzag_direct;
    }
    else {
        inter_matrix = ff_mpeg1_default_non_intra_matrix;
        inter_matrix_lookup = ff_identity;
    }
    for (int i = 0; i < 64; i++) {
        //Quantization matrices (raster order)
        picParams->CodecSpecific.mpeg2.QuantMatrixIntra[intra_matrix_lookup[i]] = intra_matrix[i];
        picParams->CodecSpecific.mpeg2.QuantMatrixInter[inter_matrix_lookup[i]] = inter_matrix[i];
    }
}

static cudaVideoCodec computeMPEG2CudaCodec(VAProfile profile) {
    switch (profile) {
        case VAProfileMPEG2Main:
        case VAProfileMPEG2Simple:
            return cudaVideoCodec_MPEG2;
        default:
            return cudaVideoCodec_NONE;
    }
}

static const VAProfile mpeg2SupportedProfiles[] = {
    VAProfileMPEG2Main,
    VAProfileMPEG2Simple,
};

const DECLARE_CODEC(mpeg2Codec) = {
    .computeCudaCodec = computeMPEG2CudaCodec,
    .handlers = {
        [VAPictureParameterBufferType] = copyMPEG2PicParam,
        [VAIQMatrixBufferType] = copyMPEG2IQMatrix,
        [VASliceParameterBufferType] = copyMPEG2SliceParam,
        [VASliceDataBufferType] = copyMPEG2SliceData,
    },
    .supportedProfileCount = ARRAY_SIZE(mpeg2SupportedProfiles),
    .supportedProfiles = mpeg2SupportedProfiles,
};
