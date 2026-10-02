#include "vabackend.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* JPEG Decode Implementation
 *
 * VA-API supplies JPEG data as separate buffers (picture params, IQ tables,
 * Huffman tables, slice data). NVDEC expects a complete JPEG bitstream.
 * This codec reconstructs a minimal JFIF-compliant JPEG from VA buffers and
 * feeds it to NVDEC.
 */

// JPEG marker bytes (JPEG spec: ISO/IEC 10918-1)
#define JPEG_SOI        0xD8    // Start of Image
#define JPEG_EOI        0xD9    // End of Image
#define JPEG_APP0       0xE0    // JFIF application marker
#define JPEG_DQT        0xDB    // Define Quantization Table
#define JPEG_SOF0       0xC0    // Start of Frame (Baseline DCT)
#define JPEG_DHT        0xC4    // Define Huffman Table
#define JPEG_DRI        0xDD    // Define Restart Interval
#define JPEG_SOS        0xDA    // Start of Scan
#define JPEG_MARKER     0xFF    // Marker prefix byte

#define JPEG_MAX_COMPONENTS 4U

typedef struct {
    VAPictureParameterBufferJPEGBaseline picParams;
    VAIQMatrixBufferJPEGBaseline         iqMatrix;
    VAHuffmanTableBufferJPEGBaseline     huffmanTable;
    bool                                 hasPicParams;
    bool                                 hasIQMatrix;
    bool                                 hasHuffmanTable;
    uint8_t                              validQuantTablesMask;
    uint8_t                              validHuffmanDcMask;
    uint8_t                              validHuffmanAcMask;
} JPEGContext;

// Minimal APP0/JFIF header
static const uint8_t jfifHeader[] = {
    JPEG_MARKER, JPEG_SOI,           // Start of Image
    JPEG_MARKER, JPEG_APP0,          // APP0 marker
    0x00, 0x10,                      // Length (16 bytes)
    0x4A, 0x46, 0x49, 0x46, 0x00,    // "JFIF\0"
    0x01, 0x01,                      // Version 1.1
    0x00,                            // Units (0 = none)
    0x00, 0x01,                      // X density
    0x00, 0x01,                      // Y density
    0x00, 0x00                       // No thumbnail
};

// Standard Huffman tables for baseline JPEG (YCbCr)
static const uint8_t dcLuminanceBits[]   = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t dcLuminanceVals[]   = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t dcChrominanceBits[] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t dcChrominanceVals[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

static const uint8_t acLuminanceBits[] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t acLuminanceVals[] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};

static const uint8_t acChrominanceBits[] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const uint8_t acChrominanceVals[] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};

// Write 16-bit big-endian value
static void write16be(uint8_t *ptr, uint16_t value) {
    ptr[0] = (uint8_t)((value >> 8) & 0xFFU);
    ptr[1] = (uint8_t)(value & 0xFFU);
}

static JPEGContext *getJPEGContext(NVContext *ctx, bool createIfMissing) {
    if (ctx->codecData == NULL) {
        if (!createIfMissing) {
            return NULL;
        }

        ctx->codecData = calloc(1, sizeof(JPEGContext));
        if (ctx->codecData == NULL) {
            LOG("JPEG: Failed to allocate codec context");
            return NULL;
        }
    }

    return (JPEGContext *)ctx->codecData;
}

static void resetJPEGPictureState(NVContext *ctx) {
    JPEGContext *jpegCtx = getJPEGContext(ctx, false);
    if (jpegCtx != NULL) {
        // VA-API allows IQ/Huffman tables to be reused across pictures, so only
        // clear the per-picture frame header state here.
        memset(&jpegCtx->picParams, 0, sizeof(jpegCtx->picParams));
        jpegCtx->hasPicParams = false;
    }

    ctx->lastSliceParams = NULL;
    ctx->lastSliceParamsCount = 0U;
}

static bool componentInFrame(const VAPictureParameterBufferJPEGBaseline *pic, uint8_t componentId) {
    for (uint32_t i = 0; i < pic->num_components; i++) {
        if (pic->components[i].component_id == componentId) {
            return true;
        }
    }

    return false;
}

static bool getUsedQuantTablesMask(const VAPictureParameterBufferJPEGBaseline *pic, uint8_t *outMask) {
    uint8_t usedMask = 0;

    for (uint32_t i = 0; i < pic->num_components; i++) {
        uint8_t tableId = pic->components[i].quantiser_table_selector;
        if (tableId >= 4U) {
            LOG("JPEG: Invalid quantiser table selector %u", tableId);
            return false;
        }

        usedMask |= (uint8_t)(1U << tableId);
    }

    *outMask = usedMask;
    return true;
}

// Write DQT (Define Quantization Table) segments for all tables used by this frame.
static bool writeDQT(uint8_t **pptr,
                     const VAIQMatrixBufferJPEGBaseline *iq,
                     const VAPictureParameterBufferJPEGBaseline *pic,
                     uint8_t validMask) {
    uint8_t usedMask = 0;
    uint8_t *ptr = *pptr;

    if (!getUsedQuantTablesMask(pic, &usedMask)) {
        return false;
    }

    if ((validMask & usedMask) != usedMask) {
        LOG("JPEG: Missing quant tables (used=0x%02x, valid=0x%02x)", usedMask, validMask);
        return false;
    }

    for (uint32_t table = 0; table < 4U; table++) {
        uint8_t bit = (uint8_t)(1U << table);
        if ((usedMask & bit) == 0) {
            continue;
        }

        uint32_t sum = 0;
        for (uint32_t j = 0; j < 64U; j++) {
            sum += iq->quantiser_table[table][j];
        }
        if (sum == 0U) {
            LOG("JPEG: Quant table %u is empty", table);
            return false;
        }

        *ptr++ = JPEG_MARKER;
        *ptr++ = JPEG_DQT;
        write16be(ptr, 67U); // Length (2 + 1 + 64)
        ptr += 2;
        *ptr++ = (uint8_t)table;
        memcpy(ptr, iq->quantiser_table[table], 64U);
        ptr += 64;
    }

    *pptr = ptr;
    return true;
}

// Write DRI (Define Restart Interval) segment
static uint8_t *writeDRI(uint8_t *ptr, uint16_t restartInterval) {
    *ptr++ = JPEG_MARKER;
    *ptr++ = JPEG_DRI;
    write16be(ptr, 4U); // Length (2 + 2 bytes interval)
    ptr += 2;
    write16be(ptr, restartInterval);
    ptr += 2;
    return ptr;
}

// Write SOF0 (Start of Frame) segment
static uint8_t *writeSOF0(uint8_t *ptr, const VAPictureParameterBufferJPEGBaseline *pic) {
    *ptr++ = JPEG_MARKER;
    *ptr++ = JPEG_SOF0;

    uint16_t length = (uint16_t)(2U + 1U + 2U + 2U + 1U + ((uint16_t)pic->num_components * 3U));
    write16be(ptr, length);
    ptr += 2;

    *ptr++ = 8U; // Precision (8 bits)
    write16be(ptr, pic->picture_height);
    ptr += 2;
    write16be(ptr, pic->picture_width);
    ptr += 2;
    *ptr++ = pic->num_components;

    for (uint32_t i = 0; i < pic->num_components; i++) {
        *ptr++ = pic->components[i].component_id;
        *ptr++ = (uint8_t)((pic->components[i].h_sampling_factor << 4) |
                           pic->components[i].v_sampling_factor);
        *ptr++ = pic->components[i].quantiser_table_selector;
    }

    return ptr;
}

// Write DHT (Define Huffman Table) segment
static uint8_t *writeDHT(uint8_t *ptr,
                         const uint8_t *bits,
                         const uint8_t *vals,
                         uint8_t tableClass,
                         uint8_t tableId,
                         uint32_t numVals) {
    *ptr++ = JPEG_MARKER;
    *ptr++ = JPEG_DHT;

    uint16_t length = (uint16_t)(2U + 1U + 16U + numVals);
    write16be(ptr, length);
    ptr += 2;

    *ptr++ = (uint8_t)((tableClass << 4) | tableId);
    memcpy(ptr, bits, 16U);
    ptr += 16;
    memcpy(ptr, vals, numVals);
    ptr += numVals;

    return ptr;
}

// Write standard Huffman tables
static uint8_t *writeStandardHuffmanTables(uint8_t *ptr) {
    ptr = writeDHT(ptr, dcLuminanceBits, dcLuminanceVals, 0U, 0U, 12U);
    ptr = writeDHT(ptr, dcChrominanceBits, dcChrominanceVals, 0U, 1U, 12U);
    ptr = writeDHT(ptr, acLuminanceBits, acLuminanceVals, 1U, 0U, 162U);
    ptr = writeDHT(ptr, acChrominanceBits, acChrominanceVals, 1U, 1U, 162U);
    return ptr;
}

static bool countHuffValues(const uint8_t codes[16], uint32_t maxVals, uint32_t *outCount) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 16U; i++) {
        sum += codes[i];
        if (sum > maxVals) {
            return false;
        }
    }

    *outCount = sum;
    return true;
}

// Write Huffman tables from VA-API. Returns false if tables are invalid/out-of-range.
static bool writeVAHuffmanTables(uint8_t **pptr,
                                 const VAHuffmanTableBufferJPEGBaseline *huffman,
                                 uint8_t requiredDcMask,
                                 uint8_t requiredAcMask,
                                 uint8_t validDcMask,
                                 uint8_t validAcMask) {
    uint8_t *ptr = *pptr;

    for (uint32_t tableIdx = 0; tableIdx < 2U; tableIdx++) {
        uint8_t bit = (uint8_t)(1U << tableIdx);

        if ((requiredDcMask & bit) != 0U) {
            if ((validDcMask & bit) == 0U) {
                LOG("JPEG: Missing required DC Huffman table %u", tableIdx);
                return false;
            }

            uint32_t numDcValues = 0;
            if (!countHuffValues(huffman->huffman_table[tableIdx].num_dc_codes, 12U, &numDcValues) ||
                numDcValues == 0U) {
                LOG("JPEG: Invalid DC Huffman table %u (count=%u)", tableIdx, numDcValues);
                return false;
            }

            ptr = writeDHT(ptr,
                           huffman->huffman_table[tableIdx].num_dc_codes,
                           huffman->huffman_table[tableIdx].dc_values,
                           0U,
                           (uint8_t)tableIdx,
                           numDcValues);
        }

        if ((requiredAcMask & bit) != 0U) {
            if ((validAcMask & bit) == 0U) {
                LOG("JPEG: Missing required AC Huffman table %u", tableIdx);
                return false;
            }

            uint32_t numAcValues = 0;
            if (!countHuffValues(huffman->huffman_table[tableIdx].num_ac_codes, 162U, &numAcValues) ||
                numAcValues == 0U) {
                LOG("JPEG: Invalid AC Huffman table %u (count=%u)", tableIdx, numAcValues);
                return false;
            }

            ptr = writeDHT(ptr,
                           huffman->huffman_table[tableIdx].num_ac_codes,
                           huffman->huffman_table[tableIdx].ac_values,
                           1U,
                           (uint8_t)tableIdx,
                           numAcValues);
        }
    }

    *pptr = ptr;
    return true;
}

static bool validateSliceAndCollectHuffmanUsage(const VAPictureParameterBufferJPEGBaseline *pic,
                                                const VASliceParameterBufferJPEGBaseline *slice,
                                                uint8_t *requiredDcMask,
                                                uint8_t *requiredAcMask) {
    if (slice->num_components == 0U || slice->num_components > JPEG_MAX_COMPONENTS) {
        LOG("JPEG: Unsupported scan component count: %u", slice->num_components);
        return false;
    }

    for (uint32_t i = 0; i < slice->num_components; i++) {
        uint8_t componentSelector = slice->components[i].component_selector;
        uint8_t dcSelector = slice->components[i].dc_table_selector;
        uint8_t acSelector = slice->components[i].ac_table_selector;

        if (!componentInFrame(pic, componentSelector)) {
            LOG("JPEG: Scan references unknown frame component id %u", componentSelector);
            return false;
        }

        if (dcSelector > 1U || acSelector > 1U) {
            LOG("JPEG: Huffman selector out of range (dc=%u ac=%u)", dcSelector, acSelector);
            return false;
        }

        *requiredDcMask |= (uint8_t)(1U << dcSelector);
        *requiredAcMask |= (uint8_t)(1U << acSelector);
    }

    return true;
}

// Write SOS (Start of Scan) segment
static uint8_t *writeSOS(uint8_t *ptr, const VASliceParameterBufferJPEGBaseline *slice) {
    *ptr++ = JPEG_MARKER;
    *ptr++ = JPEG_SOS;

    uint16_t length = (uint16_t)(2U + 1U + ((uint16_t)slice->num_components * 2U) + 3U);
    write16be(ptr, length);
    ptr += 2;

    *ptr++ = slice->num_components;

    for (uint32_t i = 0; i < slice->num_components; i++) {
        *ptr++ = slice->components[i].component_selector;
        *ptr++ = (uint8_t)((slice->components[i].dc_table_selector << 4) |
                           slice->components[i].ac_table_selector);
    }

    *ptr++ = 0U;   // Ss (start of spectral selection)
    *ptr++ = 63U;  // Se (end of spectral selection)
    *ptr++ = 0U;   // Ah/Al (successive approximation)

    return ptr;
}

// Reconstruct complete JPEG frame
static uint8_t *reconstructJPEG(const JPEGContext *jpegCtx,
                                const VASliceParameterBufferJPEGBaseline *slices,
                                uint32_t sliceCount,
                                const uint8_t *sliceData,
                                uint32_t sliceDataSize,
                                uint32_t *outSize) {
    if (!jpegCtx->hasPicParams || !jpegCtx->hasIQMatrix) {
        LOG("JPEG: Missing picture params or IQ matrix");
        return NULL;
    }

    if (jpegCtx->picParams.picture_width == 0U || jpegCtx->picParams.picture_height == 0U) {
        LOG("JPEG: Invalid dimensions: %ux%u",
            jpegCtx->picParams.picture_width,
            jpegCtx->picParams.picture_height);
        return NULL;
    }

    if (jpegCtx->picParams.num_components == 0U || jpegCtx->picParams.num_components > JPEG_MAX_COMPONENTS) {
        LOG("JPEG: Unsupported frame component count: %u", jpegCtx->picParams.num_components);
        return NULL;
    }

    uint8_t usedQuantMask = 0;
    if (!getUsedQuantTablesMask(&jpegCtx->picParams, &usedQuantMask)) {
        return NULL;
    }

    if ((jpegCtx->validQuantTablesMask & usedQuantMask) != usedQuantMask) {
        LOG("JPEG: Missing required quant tables (used=0x%02x valid=0x%02x)",
            usedQuantMask,
            jpegCtx->validQuantTablesMask);
        return NULL;
    }

    if (sliceCount == 0U) {
        LOG("JPEG: No slice parameters");
        return NULL;
    }

    uint64_t totalEcsSize = 0;
    uint8_t requiredDcMask = 0;
    uint8_t requiredAcMask = 0;

    for (uint32_t i = 0; i < sliceCount; i++) {
        const VASliceParameterBufferJPEGBaseline *slice = &slices[i];

        if (slice->slice_data_flag != VA_SLICE_DATA_FLAG_ALL) {
            LOG("JPEG: slice_data_flag=%u not supported (expected ALL)", slice->slice_data_flag);
            return NULL;
        }

        if (!validateSliceAndCollectHuffmanUsage(&jpegCtx->picParams,
                                                 slice,
                                                 &requiredDcMask,
                                                 &requiredAcMask)) {
            return NULL;
        }

        if (slice->slice_data_offset > sliceDataSize) {
            LOG("JPEG: Invalid slice_data_offset (%u) exceeds buffer size (%u)",
                slice->slice_data_offset,
                sliceDataSize);
            return NULL;
        }

        uint32_t availableData = sliceDataSize - slice->slice_data_offset;
        if (slice->slice_data_size > availableData) {
            LOG("JPEG: Invalid slice_data_size (%u) exceeds available data (%u)",
                slice->slice_data_size,
                availableData);
            return NULL;
        }

        if (UINT64_MAX - totalEcsSize < slice->slice_data_size) {
            LOG("JPEG: Total ECS size overflow");
            return NULL;
        }
        totalEcsSize += slice->slice_data_size;
    }

    const VASliceParameterBufferJPEGBaseline *slice0 = &slices[0];

    bool allSameSOSHeader = true;
    bool allSameRestartInterval = true;

    for (uint32_t i = 1; i < sliceCount; i++) {
        const VASliceParameterBufferJPEGBaseline *slice = &slices[i];

        if (slice->restart_interval != slice0->restart_interval) {
            allSameRestartInterval = false;
        }

        if (slice->num_components != slice0->num_components) {
            allSameSOSHeader = false;
            continue;
        }

        for (uint32_t c = 0; c < slice->num_components; c++) {
            if (slice->components[c].component_selector != slice0->components[c].component_selector ||
                slice->components[c].dc_table_selector != slice0->components[c].dc_table_selector ||
                slice->components[c].ac_table_selector != slice0->components[c].ac_table_selector) {
                allSameSOSHeader = false;
                break;
            }
        }
    }

    // Worst-case buffer size calculation (overestimate for safety)
    const uint64_t dqtSize = 4U * (2U + 2U + 1U + 64U);
    const uint64_t sof0Size = 2U + 2U + 1U + 2U + 2U + 1U +
                              ((uint64_t)jpegCtx->picParams.num_components * 3U);
    const uint64_t dhtSize = 2U * ((2U + 2U + 1U + 16U + 12U) +
                                   (2U + 2U + 1U + 16U + 162U));
    const uint64_t driSize = (uint64_t)sliceCount * (2U + 2U + 2U);
    const uint64_t sosSize = (uint64_t)sliceCount * (2U + 2U + 1U + 4U * 2U + 3U);

    uint64_t maxSize64 = sizeof(jfifHeader) + dqtSize + sof0Size + dhtSize +
                         driSize + sosSize + totalEcsSize + 2U;

    if (maxSize64 > SIZE_MAX) {
        LOG("JPEG: Frame size too large to allocate (%llu bytes)", (unsigned long long)maxSize64);
        return NULL;
    }

    uint8_t *frame = (uint8_t *)malloc((size_t)maxSize64);
    if (frame == NULL) {
        LOG("JPEG: Failed to allocate frame buffer");
        return NULL;
    }

    uint8_t *ptr = frame;

    // 1. SOI + JFIF header
    memcpy(ptr, jfifHeader, sizeof(jfifHeader));
    ptr += sizeof(jfifHeader);

    // 2. DQT
    if (!writeDQT(&ptr, &jpegCtx->iqMatrix, &jpegCtx->picParams, jpegCtx->validQuantTablesMask)) {
        free(frame);
        return NULL;
    }

    // 3. SOF0
    ptr = writeSOF0(ptr, &jpegCtx->picParams);

    // 4. DHT (VA tables if complete and valid, else standard)
    bool useVAHuffman = jpegCtx->hasHuffmanTable &&
                        ((jpegCtx->validHuffmanDcMask & requiredDcMask) == requiredDcMask) &&
                        ((jpegCtx->validHuffmanAcMask & requiredAcMask) == requiredAcMask);

    if (useVAHuffman) {
        uint8_t *tmp = ptr;
        if (writeVAHuffmanTables(&tmp,
                                 &jpegCtx->huffmanTable,
                                 requiredDcMask,
                                 requiredAcMask,
                                 jpegCtx->validHuffmanDcMask,
                                 jpegCtx->validHuffmanAcMask)) {
            ptr = tmp;
        } else {
            ptr = writeStandardHuffmanTables(ptr);
        }
    } else {
        ptr = writeStandardHuffmanTables(ptr);
    }

    // 4b. DRI (Restart interval) once if consistent across slices
    if (allSameRestartInterval && slice0->restart_interval != 0U) {
        ptr = writeDRI(ptr, slice0->restart_interval);
    }

    // 5/6. Scan(s)
    if (allSameSOSHeader) {
        ptr = writeSOS(ptr, slice0);
        for (uint32_t i = 0; i < sliceCount; i++) {
            const VASliceParameterBufferJPEGBaseline *slice = &slices[i];
            memcpy(ptr, sliceData + slice->slice_data_offset, slice->slice_data_size);
            ptr += slice->slice_data_size;
        }
    } else {
        for (uint32_t i = 0; i < sliceCount; i++) {
            const VASliceParameterBufferJPEGBaseline *slice = &slices[i];

            // If restart_interval wasn't consistent globally, emit per-scan DRI.
            if (!allSameRestartInterval && slice->restart_interval != 0U) {
                ptr = writeDRI(ptr, slice->restart_interval);
            }

            ptr = writeSOS(ptr, slice);
            memcpy(ptr, sliceData + slice->slice_data_offset, slice->slice_data_size);
            ptr += slice->slice_data_size;
        }
    }

    // 7. EOI (avoid duplicating if client already included it)
    if (!(ptr - frame >= 2 && ptr[-2] == JPEG_MARKER && ptr[-1] == JPEG_EOI)) {
        *ptr++ = JPEG_MARKER;
        *ptr++ = JPEG_EOI;
    }

    uint64_t frameSize64 = (uint64_t)(ptr - frame);
    if (frameSize64 > UINT32_MAX) {
        LOG("JPEG: Reconstructed frame too large (%llu bytes)", (unsigned long long)frameSize64);
        free(frame);
        return NULL;
    }

    *outSize = (uint32_t)frameSize64;
    return frame;
}

static void copyJPEGPicParam(NVContext *ctx, NVBuffer *buffer, CUVIDPICPARAMS *picParams)
{
    VAPictureParameterBufferJPEGBaseline *buf = (VAPictureParameterBufferJPEGBaseline *)buffer->ptr;
    JPEGContext *jpegCtx = getJPEGContext(ctx, true);

    if (jpegCtx != NULL) {
        memcpy(&jpegCtx->picParams, buf, sizeof(VAPictureParameterBufferJPEGBaseline));
        jpegCtx->hasPicParams = buf->picture_width != 0U &&
                                buf->picture_height != 0U &&
                                buf->num_components > 0U &&
                                buf->num_components <= JPEG_MAX_COMPONENTS;
    }

    picParams->PicWidthInMbs = (int)((buf->picture_width + 15U) / 16U);
    picParams->FrameHeightInMbs = (int)((buf->picture_height + 15U) / 16U);
    picParams->field_pic_flag = 0;
    picParams->bottom_field_flag = 0;
    picParams->second_field = 0;
    picParams->intra_pic_flag = 1;
    picParams->ref_pic_flag = 0;
}

static void copyJPEGIQMatrix(NVContext *ctx, NVBuffer *buffer, CUVIDPICPARAMS *picParams)
{
    VAIQMatrixBufferJPEGBaseline *buf = (VAIQMatrixBufferJPEGBaseline *)buffer->ptr;
    JPEGContext *jpegCtx = getJPEGContext(ctx, true);

    if (jpegCtx == NULL) {
        return;
    }

    for (uint32_t table = 0; table < 4U; table++) {
        if (buf->load_quantiser_table[table] != 0U) {
            memcpy(jpegCtx->iqMatrix.quantiser_table[table],
                   buf->quantiser_table[table],
                   sizeof(jpegCtx->iqMatrix.quantiser_table[table]));
            jpegCtx->validQuantTablesMask |= (uint8_t)(1U << table);
        }
    }

    jpegCtx->hasIQMatrix = jpegCtx->validQuantTablesMask != 0U;
    (void)picParams;
}

static void copyJPEGHuffmanTable(NVContext *ctx, NVBuffer *buffer, CUVIDPICPARAMS *picParams)
{
    VAHuffmanTableBufferJPEGBaseline *buf = (VAHuffmanTableBufferJPEGBaseline *)buffer->ptr;
    JPEGContext *jpegCtx = getJPEGContext(ctx, true);

    if (jpegCtx == NULL) {
        return;
    }

    for (uint32_t table = 0; table < 2U; table++) {
        if (buf->load_huffman_table[table] != 0U) {
            memcpy(jpegCtx->huffmanTable.huffman_table[table].num_dc_codes,
                   buf->huffman_table[table].num_dc_codes,
                   sizeof(jpegCtx->huffmanTable.huffman_table[table].num_dc_codes));
            memcpy(jpegCtx->huffmanTable.huffman_table[table].dc_values,
                   buf->huffman_table[table].dc_values,
                   sizeof(jpegCtx->huffmanTable.huffman_table[table].dc_values));
            memcpy(jpegCtx->huffmanTable.huffman_table[table].num_ac_codes,
                   buf->huffman_table[table].num_ac_codes,
                   sizeof(jpegCtx->huffmanTable.huffman_table[table].num_ac_codes));
            memcpy(jpegCtx->huffmanTable.huffman_table[table].ac_values,
                   buf->huffman_table[table].ac_values,
                   sizeof(jpegCtx->huffmanTable.huffman_table[table].ac_values));

            jpegCtx->validHuffmanDcMask |= (uint8_t)(1U << table);
            jpegCtx->validHuffmanAcMask |= (uint8_t)(1U << table);
        }
    }

    jpegCtx->hasHuffmanTable = (jpegCtx->validHuffmanDcMask | jpegCtx->validHuffmanAcMask) != 0U;
    (void)picParams;
}

static void copyJPEGSliceParam(NVContext *ctx, NVBuffer *buf, CUVIDPICPARAMS *picParams)
{
    ctx->lastSliceParams = buf->ptr;
    ctx->lastSliceParamsCount = buf->elements;
    (void)picParams;
}

static void copyJPEGSliceData(NVContext *ctx, NVBuffer *buf, CUVIDPICPARAMS *picParams)
{
    JPEGContext *jpegCtx = getJPEGContext(ctx, false);
    if (jpegCtx == NULL) {
        LOG("JPEG: No codec context available");
        return;
    }

    if (ctx->lastSliceParams == NULL || ctx->lastSliceParamsCount == 0U) {
        LOG("JPEG: No slice parameters available");
        return;
    }

    const VASliceParameterBufferJPEGBaseline *slices =
        (const VASliceParameterBufferJPEGBaseline *)ctx->lastSliceParams;

    LOG("JPEG: Processing %u slice(s), input size %zu bytes",
        ctx->lastSliceParamsCount,
        buf->size);

    if (buf->size > UINT32_MAX) {
        LOG("JPEG: Slice data too large (%zu bytes)", buf->size);
        return;
    }

    uint32_t frameSize = 0;
    uint8_t *frame = reconstructJPEG(jpegCtx,
                                     slices,
                                     ctx->lastSliceParamsCount,
                                     (const uint8_t *)buf->ptr,
                                     (uint32_t)buf->size,
                                     &frameSize);
    if (frame == NULL) {
        LOG("JPEG: Failed to reconstruct JPEG frame");
        return;
    }

    if (ctx->bitstreamBuffer.size > UINT32_MAX - frameSize) {
        LOG("JPEG: Reconstructed bitstream would overflow CUVID limit");
        free(frame);
        return;
    }

    // NVDEC can consume a full JPEG as a single "slice" (same approach as FFmpeg's mjpeg_nvdec)
    picParams->nNumSlices = 1U;

    uint32_t offset = (uint32_t)ctx->bitstreamBuffer.size;
    appendBuffer(&ctx->sliceOffsets, &offset, sizeof(offset));
    appendBuffer(&ctx->bitstreamBuffer, frame, frameSize);
    picParams->nBitstreamDataLen = (uint32_t)ctx->bitstreamBuffer.size;

    LOG("JPEG: Reconstructed %u bytes for NVDEC", frameSize);

    free(frame);
}

static cudaVideoCodec computeJPEGCudaCodec(VAProfile profile) {
    switch (profile) {
        case VAProfileJPEGBaseline:
            return cudaVideoCodec_JPEG;
        default:
            return cudaVideoCodec_NONE;
    }
}

static const VAProfile jpegSupportedProfiles[] = {
    VAProfileJPEGBaseline,
};

static void jpegBeginPicture(NVContext *ctx, VASurfaceID renderTarget) {
    (void) renderTarget;
    resetJPEGPictureState(ctx);
}

const DECLARE_CODEC(jpegCodec) = {
    .computeCudaCodec = computeJPEGCudaCodec,
    .handlers = {
        [VAPictureParameterBufferType] = copyJPEGPicParam,
        [VAIQMatrixBufferType] = copyJPEGIQMatrix,
        [VAHuffmanTableBufferType] = copyJPEGHuffmanTable,
        [VASliceParameterBufferType] = copyJPEGSliceParam,
        [VASliceDataBufferType] = copyJPEGSliceData,
    },
    .supportedProfileCount = ARRAY_SIZE(jpegSupportedProfiles),
    .supportedProfiles = jpegSupportedProfiles,
    .beginPicture = jpegBeginPicture,
};
