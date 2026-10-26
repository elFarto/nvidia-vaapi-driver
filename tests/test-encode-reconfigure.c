#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>

#define WIDTH 640
#define HEIGHT 384
#define PHASE_FRAMES 90

static void check(VAStatus status, const char *operation) {
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "%s: %s\n", operation, vaErrorStr(status));
        exit(1);
    }
}

#define VA_CHECK(expression) check((expression), #expression)

static VABufferID parameter(VADisplay display, VAContextID context,
                            VABufferType type, size_t size, void *data) {
    VABufferID buffer;
    VA_CHECK(vaCreateBuffer(display, context, type, (unsigned int)size, 1, data, &buffer));
    return buffer;
}

static VABufferID rateControl(VADisplay display, VAContextID context, uint32_t bitrate) {
    VABufferID buffer;
    const size_t size = sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl);
    VA_CHECK(vaCreateBuffer(display, context, VAEncMiscParameterBufferType,
                            (unsigned int)size, 1, NULL, &buffer));
    VAEncMiscParameterBuffer *misc;
    VA_CHECK(vaMapBuffer(display, buffer, (void **)&misc));
    memset(misc, 0, size);
    misc->type = VAEncMiscParameterTypeRateControl;
    VAEncMiscParameterRateControl *rc = (VAEncMiscParameterRateControl *)misc->data;
    rc->bits_per_second = bitrate;
    rc->target_percentage = 100;
    VA_CHECK(vaUnmapBuffer(display, buffer));
    return buffer;
}

static VABufferID frameRate(VADisplay display, VAContextID context, uint32_t fps) {
    VABufferID buffer;
    const size_t size = sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterFrameRate);
    VA_CHECK(vaCreateBuffer(display, context, VAEncMiscParameterBufferType,
                            (unsigned int)size, 1, NULL, &buffer));
    VAEncMiscParameterBuffer *misc;
    VA_CHECK(vaMapBuffer(display, buffer, (void **)&misc));
    memset(misc, 0, size);
    misc->type = VAEncMiscParameterTypeFrameRate;
    ((VAEncMiscParameterFrameRate *)misc->data)->framerate = fps;
    VA_CHECK(vaUnmapBuffer(display, buffer));
    return buffer;
}

static void uploadNoise(VADisplay display, VAImage *image, VASurfaceID surface,
                        uint32_t *seed) {
    uint8_t *bytes;
    VA_CHECK(vaMapBuffer(display, image->buf, (void **)&bytes));
    for (unsigned y = 0; y < HEIGHT; y++) {
        for (unsigned x = 0; x < WIDTH; x++) {
            *seed = *seed * 1664525u + 1013904223u;
            bytes[image->offsets[0] + y * image->pitches[0] + x] =
                (uint8_t)(16 + ((*seed >> 16) % 220));
        }
    }
    for (unsigned y = 0; y < HEIGHT / 2; y++) {
        memset(bytes + image->offsets[1] + y * image->pitches[1], 128, WIDTH);
    }
    VA_CHECK(vaUnmapBuffer(display, image->buf));
    VA_CHECK(vaPutImage(display, surface, image->image_id,
                        0, 0, WIDTH, HEIGHT, 0, 0, WIDTH, HEIGHT));
}

static void checkPackedHeaders(VADisplay display, VAProfile profile, bool disabled) {
    VAConfigAttrib packed = {.type = VAConfigAttribEncPackedHeaders};
    VA_CHECK(vaGetConfigAttributes(display, profile, VAEntrypointEncSlice, &packed, 1));
    const uint32_t expected = disabled ? VA_ENC_PACKED_HEADER_NONE :
        VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_PICTURE | VA_ENC_PACKED_HEADER_RAW_DATA;
    if (packed.value != expected) {
        fprintf(stderr, "unexpected packed header support: 0x%x\n", packed.value);
        exit(1);
    }
    packed.value = disabled ? VA_ENC_PACKED_HEADER_RAW_DATA : VA_ENC_PACKED_HEADER_SLICE;
    VAConfigID rejected;
    VAStatus status = vaCreateConfig(display, profile, VAEntrypointEncSlice, &packed, 1, &rejected);
    if (status == VA_STATUS_SUCCESS) {
        VA_CHECK(vaDestroyConfig(display, rejected));
        fprintf(stderr, "unsupported packed headers were accepted\n");
        exit(1);
    }
    if (status != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED) {
        check(status, "reject unsupported packed headers");
    }
}

int main(int argc, char **argv) {
    const bool hevc = argc > 1 && strcmp(argv[1], "hevc") == 0;
    const bool reconfigureOnly = argc > 2 && strcmp(argv[2], "--reconfigure-only") == 0;
    const bool headersDisabled = argc > 2 && strcmp(argv[2], "--packed-headers-disabled") == 0;
    const VAProfile profile = hevc ? VAProfileHEVCMain : VAProfileH264Main;
    const char *device = getenv("VAAPI_DEVICE");
    if (device == NULL) device = "/dev/dri/renderD128";
    int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror(device);
        return 77;
    }
    VADisplay display = vaGetDisplayDRM(fd);
    int major, minor;
    VA_CHECK(vaInitialize(display, &major, &minor));
    if (strstr(vaQueryVendorString(display), "NVDEC/NVENC") == NULL) {
        fprintf(stderr, "test requires the NVIDIA NVDEC/NVENC VA-API driver\n");
        VA_CHECK(vaTerminate(display));
        close(fd);
        return 77;
    }
    VAEntrypoint entrypoints[16];
    int count = 0;
    VAStatus status = vaQueryConfigEntrypoints(display, profile, entrypoints, &count);
    bool supported = false;
    if (status == VA_STATUS_SUCCESS) {
        for (int i = 0; i < count; i++) supported |= entrypoints[i] == VAEntrypointEncSlice;
    }
    if (!supported) {
        fprintf(stderr, "%s encoding is unavailable\n", hevc ? "HEVC" : "H.264");
        VA_CHECK(vaTerminate(display));
        close(fd);
        return 77;
    }
    if (!reconfigureOnly) checkPackedHeaders(display, profile, headersDisabled);
    if (headersDisabled) {
        VA_CHECK(vaTerminate(display));
        close(fd);
        puts("disabled packed-header capabilities and rejection verified");
        return 0;
    }
    VAConfigAttrib attributes[] = {
        {.type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420},
        {.type = VAConfigAttribRateControl, .value = VA_RC_CBR},
        {.type = VAConfigAttribEncPackedHeaders,
         .value = reconfigureOnly ? VA_ENC_PACKED_HEADER_NONE : VA_ENC_PACKED_HEADER_RAW_DATA},
    };
    VAConfigID config;
    VA_CHECK(vaCreateConfig(display, profile, VAEntrypointEncSlice, attributes, 3, &config));
    VAConfigAttrib configured[16];
    VAProfile configuredProfile;
    VAEntrypoint configuredEntrypoint;
    int configuredCount = 0;
    VA_CHECK(vaQueryConfigAttributes(display, config, &configuredProfile,
                                     &configuredEntrypoint, configured, &configuredCount));
    if (!reconfigureOnly) {
        bool found = false;
        for (int i = 0; i < configuredCount; i++) {
            if (configured[i].type == VAConfigAttribEncPackedHeaders) {
                found = configured[i].value == VA_ENC_PACKED_HEADER_RAW_DATA;
            }
        }
        if (!found) {
            fprintf(stderr, "config did not retain the negotiated packed header mask\n");
            return 1;
        }
    }
    VASurfaceID surface;
    VA_CHECK(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, WIDTH, HEIGHT, &surface, 1, NULL, 0));
    VAContextID context;
    VA_CHECK(vaCreateContext(display, config, WIDTH, HEIGHT, VA_PROGRESSIVE, &surface, 1, &context));
    VAImageFormat format = {.fourcc = VA_FOURCC_NV12};
    VAImage image;
    VA_CHECK(vaCreateImage(display, &format, WIDTH, HEIGHT, &image));
    VABufferID coded;
    VA_CHECK(vaCreateBuffer(display, context, VAEncCodedBufferType,
                            WIDTH * HEIGHT * 4, 1, NULL, &coded));

    uint32_t seed = 1;
    uint64_t totals[3] = {0};
    const uint32_t bitrates[] = {2000000, 250000, 2000000};
    const uint32_t rates[] = {30, 15, 30};
    for (unsigned phase = 0; phase < 3; phase++) {
        for (unsigned frame = 0; frame < PHASE_FRAMES; frame++) {
            uploadNoise(display, &image, surface, &seed);
            VABufferID buffers[8];
            unsigned n = 0;
            if (phase == 0 && frame == 0) {
                if (hevc) {
                    VAEncSequenceParameterBufferHEVC seq = {0};
                    seq.general_level_idc = 120;
                    seq.intra_period = seq.intra_idr_period = 30;
                    seq.ip_period = 1;
                    seq.bits_per_second = bitrates[phase];
                    seq.pic_width_in_luma_samples = WIDTH;
                    seq.pic_height_in_luma_samples = HEIGHT;
                    seq.seq_fields.bits.chroma_format_idc = 1;
                    seq.log2_diff_max_min_luma_coding_block_size = 2;
                    seq.log2_diff_max_min_transform_block_size = 3;
                    buffers[n++] = parameter(display, context, VAEncSequenceParameterBufferType,
                                              sizeof(seq), &seq);
                } else {
                    VAEncSequenceParameterBufferH264 seq = {0};
                    seq.level_idc = 41;
                    seq.intra_period = seq.intra_idr_period = 30;
                    seq.ip_period = 1;
                    seq.bits_per_second = bitrates[phase];
                    seq.picture_width_in_mbs = WIDTH / 16;
                    seq.picture_height_in_mbs = HEIGHT / 16;
                    seq.seq_fields.bits.chroma_format_idc = 1;
                    seq.seq_fields.bits.frame_mbs_only_flag = 1;
                    buffers[n++] = parameter(display, context, VAEncSequenceParameterBufferType,
                                              sizeof(seq), &seq);
                }
            }
            if (frame == 0) {
                buffers[n++] = rateControl(display, context, bitrates[phase]);
                buffers[n++] = frameRate(display, context, rates[phase]);
            }
            static const char marker[] = "nvidia-vaapi-header-test";
            if (phase == 0 && frame == 0 && !reconfigureOnly) {
                uint8_t sei[80] = {0, 0, 0, 1};
                size_t length = 4;
                sei[length++] = hevc ? 0x4e : 0x06;
                if (hevc) sei[length++] = 0x01;
                sei[length++] = 5;
                sei[length++] = 16 + sizeof(marker) - 1;
                for (unsigned i = 0; i < 16; i++) sei[length++] = (uint8_t)(i + 1);
                memcpy(sei + length, marker, sizeof(marker) - 1);
                length += sizeof(marker) - 1;
                sei[length++] = 0x80;
                VAEncPackedHeaderParameterBuffer packed = {
                    .type = VAEncPackedHeaderRawData,
                    .bit_length = (uint32_t)(length * 8),
                    .has_emulation_bytes = 1,
                };
                buffers[n++] = parameter(display, context, VAEncPackedHeaderParameterBufferType,
                                          sizeof(packed), &packed);
                buffers[n++] = parameter(display, context, VAEncPackedHeaderDataBufferType,
                                          length, sei);
            }
            if (hevc) {
                VAEncPictureParameterBufferHEVC pic = {0};
                pic.coded_buf = coded;
                pic.pic_fields.bits.idr_pic_flag = frame == 0;
                buffers[n++] = parameter(display, context, VAEncPictureParameterBufferType,
                                          sizeof(pic), &pic);
            } else {
                VAEncPictureParameterBufferH264 pic = {0};
                pic.coded_buf = coded;
                pic.frame_num = phase * PHASE_FRAMES + frame;
                pic.pic_fields.bits.idr_pic_flag = frame == 0;
                buffers[n++] = parameter(display, context, VAEncPictureParameterBufferType,
                                          sizeof(pic), &pic);
            }
            VA_CHECK(vaBeginPicture(display, context, surface));
            VA_CHECK(vaRenderPicture(display, context, buffers, (int)n));
            VA_CHECK(vaEndPicture(display, context));
            VA_CHECK(vaSyncSurface(display, surface));
            VACodedBufferSegment *segment;
            VA_CHECK(vaMapBuffer(display, coded, (void **)&segment));
            uint64_t bytes = 0;
            bool foundMarker = false;
            for (; segment != NULL; segment = segment->next) {
                bytes += segment->size;
                foundMarker |= memmem(segment->buf, segment->size, marker, sizeof(marker) - 1) != NULL;
            }
            if (phase == 0 && frame == 0 && !reconfigureOnly && !foundMarker) {
                fprintf(stderr, "client packed SEI was lost\n");
                return 1;
            }
            if (bytes == 0) {
                fprintf(stderr, "empty coded frame\n");
                return 1;
            }
            if (frame >= 30) totals[phase] += bytes;
            VA_CHECK(vaUnmapBuffer(display, coded));
            for (unsigned i = 0; i < n; i++) VA_CHECK(vaDestroyBuffer(display, buffers[i]));
        }
        printf("%s phase %u: target=%u fps=%u sampled_bytes=%llu\n",
               hevc ? "HEVC" : "H.264", phase, bitrates[phase], rates[phase],
               (unsigned long long)totals[phase]);
    }
    VA_CHECK(vaDestroyBuffer(display, coded));
    VA_CHECK(vaDestroyImage(display, image.image_id));
    VA_CHECK(vaDestroyContext(display, context));
    VA_CHECK(vaDestroySurfaces(display, &surface, 1));
    VA_CHECK(vaDestroyConfig(display, config));
    VA_CHECK(vaTerminate(display));
    close(fd);
    if (totals[0] < totals[1] * 2 || totals[2] < totals[1] * 2) {
        fprintf(stderr, "output did not follow bitrate changes in the same encode session\n");
        return 1;
    }
    return 0;
}
