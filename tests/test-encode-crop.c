#include "vaapi-test.h"
#include <stdbool.h>
#include <va/va_enc_h264.h>

static void encodeCase(VADisplay display, VAConfigID config, unsigned width,
                       unsigned height, const char *directory, int invalid) {
    const unsigned codedWidth = (width + 15) & ~15u;
    const unsigned codedHeight = (height + 15) & ~15u;
    VASurfaceID surface;
    VA_CHECK(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, codedWidth, codedHeight,
                              &surface, 1, NULL, 0));
    VAContextID context;
    VA_CHECK(vaCreateContext(display, config, (int)codedWidth, (int)codedHeight,
                             VA_PROGRESSIVE, &surface, 1, &context));
    VAEncSequenceParameterBufferH264 seq = {0};
    seq.level_idc = 41;
    seq.intra_period = seq.intra_idr_period = 30;
    seq.ip_period = 1;
    seq.bits_per_second = 4000000;
    seq.picture_width_in_mbs = codedWidth / 16;
    seq.picture_height_in_mbs = codedHeight / 16;
    seq.seq_fields.bits.chroma_format_idc = 1;
    seq.seq_fields.bits.frame_mbs_only_flag = 1;
    seq.frame_cropping_flag = width != codedWidth || height != codedHeight;
    seq.frame_crop_right_offset = (codedWidth - width) / 2;
    seq.frame_crop_bottom_offset = (codedHeight - height) / 2;
    if (invalid) {
        seq.frame_cropping_flag = 1;
        switch (invalid) {
        case 1: seq.frame_crop_right_offset = UINT32_MAX; break;
        case 2: seq.frame_crop_bottom_offset = codedHeight / 2; break;
        case 3: seq.frame_crop_left_offset = 1; break;
        case 4: seq.frame_crop_top_offset = 1; break;
        case 5: seq.picture_width_in_mbs = 0; break;
        }
    }
    VABufferID sequence;
    VA_CHECK(vaCreateBuffer(display, context, VAEncSequenceParameterBufferType,
                            sizeof(seq), 1, &seq, &sequence));
    if (invalid) {
        VA_CHECK(vaBeginPicture(display, context, surface));
        REQUIRE(vaRenderPicture(display, context, &sequence, 1) == VA_STATUS_ERROR_INVALID_PARAMETER);
        VA_CHECK(vaDestroyBuffer(display, sequence));
        VA_CHECK(vaDestroyContext(display, context));
        VA_CHECK(vaDestroySurfaces(display, &surface, 1));
        return;
    }

    VAImageFormat format = {.fourcc = VA_FOURCC_NV12};
    VAImage image;
    VA_CHECK(vaCreateImage(display, &format, codedWidth, codedHeight, &image));
    uint8_t *pixels;
    VA_CHECK(vaMapBuffer(display, image.buf, (void **)&pixels));
    for (unsigned y = 0; y < codedHeight; y++) {
        uint8_t *row = pixels + image.offsets[0] + y * image.pitches[0];
        memset(row, 235, codedWidth);
        if (y < height) memset(row, 64, width);
    }
    for (unsigned y = 0; y < codedHeight / 2; y++) {
        memset(pixels + image.offsets[1] + y * image.pitches[1], 128, codedWidth);
    }
    VA_CHECK(vaUnmapBuffer(display, image.buf));
    VA_CHECK(vaPutImage(display, surface, image.image_id, 0, 0, codedWidth, codedHeight,
                         0, 0, codedWidth, codedHeight));
    VABufferID coded;
    VA_CHECK(vaCreateBuffer(display, context, VAEncCodedBufferType,
                            codedWidth * codedHeight * 4, 1, NULL, &coded));
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/%ux%u.h264", directory, width, height);
    REQUIRE(length > 0 && (size_t)length < sizeof(path));
    FILE *output = fopen(path, "wb");
    REQUIRE(output != NULL);
    for (unsigned frame = 0; frame < 3; frame++) {
        VAEncPictureParameterBufferH264 pic = {0};
        pic.coded_buf = coded;
        pic.frame_num = frame;
        pic.pic_fields.bits.idr_pic_flag = frame == 0;
        VABufferID picture;
        VA_CHECK(vaCreateBuffer(display, context, VAEncPictureParameterBufferType,
                                sizeof(pic), 1, &pic, &picture));
        VABufferID buffers[] = {sequence, picture};
        VA_CHECK(vaBeginPicture(display, context, surface));
        // Resubmit the same crop after initialization, as clients do at keyframes.
        VA_CHECK(vaRenderPicture(display, context, buffers, 2));
        VA_CHECK(vaEndPicture(display, context));
        VA_CHECK(vaSyncSurface(display, surface));
        VACodedBufferSegment *segment;
        VA_CHECK(vaMapBuffer(display, coded, (void **)&segment));
        for (; segment != NULL; segment = segment->next) {
            REQUIRE(segment->size != 0);
            REQUIRE(fwrite(segment->buf, 1, segment->size, output) == segment->size);
        }
        VA_CHECK(vaUnmapBuffer(display, coded));
        VA_CHECK(vaDestroyBuffer(display, picture));
    }
    REQUIRE(fclose(output) == 0);
    // Dynamic resolution changes need a new context, not a silently stale SPS.
    seq.frame_cropping_flag = 1;
    seq.frame_crop_bottom_offset++;
    void *data;
    VA_CHECK(vaMapBuffer(display, sequence, &data));
    memcpy(data, &seq, sizeof(seq));
    VA_CHECK(vaUnmapBuffer(display, sequence));
    VA_CHECK(vaBeginPicture(display, context, surface));
    REQUIRE(vaRenderPicture(display, context, &sequence, 1) == VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
    VA_CHECK(vaDestroyBuffer(display, sequence));
    VA_CHECK(vaDestroyBuffer(display, coded));
    VA_CHECK(vaDestroyImage(display, image.image_id));
    VA_CHECK(vaDestroyContext(display, context));
    VA_CHECK(vaDestroySurfaces(display, &surface, 1));
    printf("encoded cropped surface %ux%u as %ux%u\n", codedWidth, codedHeight, width, height);
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2 || argc == 3);
    int fd;
    VADisplay display = openDisplay(&fd);
    VAConfigAttrib attributes[] = {
        {.type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420},
        {.type = VAConfigAttribRateControl, .value = VA_RC_CBR},
        {.type = VAConfigAttribEncPackedHeaders, .value = VA_ENC_PACKED_HEADER_NONE},
    };
    VAConfigID config;
    VAStatus status = vaCreateConfig(display, VAProfileH264Main, VAEntrypointEncSlice,
                                     attributes, 3, &config);
    if (status == VA_STATUS_ERROR_UNSUPPORTED_PROFILE || status == VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT) {
        VA_CHECK(vaTerminate(display));
        close(fd);
        return 77;
    }
    checkVa(status, "create H.264 encode config");
    if (argc == 3 && strcmp(argv[2], "--invalid-only") == 0) {
        for (int invalid = 1; invalid <= 5; invalid++) {
            encodeCase(display, config, 640, 360, argv[1], invalid);
        }
    } else {
        const unsigned dimensions[][2] = {{1920, 1080}, {1600, 900}, {640, 360}, {638, 358}, {640, 384}};
        for (unsigned i = 0; i < sizeof(dimensions) / sizeof(dimensions[0]); i++) {
            encodeCase(display, config, dimensions[i][0], dimensions[i][1], argv[1], 0);
        }
    }
    VA_CHECK(vaDestroyConfig(display, config));
    VA_CHECK(vaTerminate(display));
    close(fd);
    return 0;
}
