/*
 * test_encode_config.c — Config and capability tests.
 * Tests profile/entrypoint validation, config attributes, and error paths.
 *
 * Build: gcc -o test_encode_config tests/test_encode_config.c -lva -lva-drm
 * Run:   ./test_encode_config
 */

#include "test_common.h"

/* --- Profile/Entrypoint matrix --- */

typedef struct {
    VAProfile profile;
    const char *name;
    bool expect_encode;
    bool expect_decode;
} ProfileTest;

static const ProfileTest profile_tests[] = {
    { VAProfileH264ConstrainedBaseline, "H264 CB",   true,  true  },
    { VAProfileH264Main,               "H264 Main", true,  true  },
    { VAProfileH264High,               "H264 High", true,  true  },
    { VAProfileHEVCMain,               "HEVC Main", true,  true  },
    { VAProfileHEVCMain10,             "HEVC M10",  true,  true  },
    { VAProfileMPEG2Simple,            "MPEG2",     false, true  },
    { VAProfileVP9Profile0,            "VP9 P0",    false, false }, /* VP9 requires gstreamer-codecparsers */
    { VAProfileAV1Profile0,            "AV1 P0",    false, true  },
    { VAProfileJPEGBaseline,           "JPEG",      false, true  },
};
#define NUM_PROFILE_TESTS (sizeof(profile_tests) / sizeof(profile_tests[0]))

static void test_encode_entrypoints(void) {
    for (int i = 0; i < (int)NUM_PROFILE_TESTS; i++) {
        char name[64];
        snprintf(name, sizeof(name), "EncSlice for %-10s → %s",
                 profile_tests[i].name,
                 profile_tests[i].expect_encode ? "present" : "absent");
        TEST_START(name);

        bool has = test_has_entrypoint(g_dpy, profile_tests[i].profile,
                                        VAEntrypointEncSlice);
        if (has == profile_tests[i].expect_encode) {
            TEST_PASS();
        } else {
            TEST_FAIL(has ? "unexpected EncSlice" : "missing EncSlice");
        }
    }
}

static void test_decode_entrypoints(void) {
    for (int i = 0; i < (int)NUM_PROFILE_TESTS; i++) {
        char name[64];
        snprintf(name, sizeof(name), "VLD for %-10s → %s",
                 profile_tests[i].name,
                 profile_tests[i].expect_decode ? "present" : "absent");
        TEST_START(name);

        bool has = test_has_entrypoint(g_dpy, profile_tests[i].profile,
                                        VAEntrypointVLD);
        if (has == profile_tests[i].expect_decode) {
            TEST_PASS();
        } else {
            TEST_FAIL(has ? "unexpected VLD" : "missing VLD");
        }
    }
}

/* --- Config attribute validation --- */

static void test_config_rtformat(void) {
    TEST_START("H264 High RTFormat includes YUV420");
    VAConfigAttrib a = { .type = VAConfigAttribRTFormat };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, VAProfileH264High,
                                         VAEntrypointEncSlice, &a, 1));
    EXPECT_TRUE(a.value & VA_RT_FORMAT_YUV420, "no YUV420");
    TEST_PASS();
}

static void test_config_ratecontrol(void) {
    TEST_START("Rate control: CQP + CBR + VBR supported");
    VAConfigAttrib a = { .type = VAConfigAttribRateControl };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, VAProfileH264High,
                                         VAEntrypointEncSlice, &a, 1));
    EXPECT_TRUE(a.value & VA_RC_CQP, "no CQP");
    EXPECT_TRUE(a.value & VA_RC_CBR, "no CBR");
    EXPECT_TRUE(a.value & VA_RC_VBR, "no VBR");
    TEST_PASS();
}

static void test_config_packed_headers(void) {
    TEST_START("Packed headers: SEQ + PIC advertised");
    VAConfigAttrib a = { .type = VAConfigAttribEncPackedHeaders };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, VAProfileH264High,
                                         VAEntrypointEncSlice, &a, 1));
    EXPECT_TRUE(a.value & VA_ENC_PACKED_HEADER_SEQUENCE, "no SEQ");
    EXPECT_TRUE(a.value & VA_ENC_PACKED_HEADER_PICTURE, "no PIC");
    TEST_PASS();
}

static void test_config_max_ref_frames(void) {
    TEST_START("Max ref frames reported");
    VAConfigAttrib a = { .type = VAConfigAttribEncMaxRefFrames };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, VAProfileH264High,
                                         VAEntrypointEncSlice, &a, 1));
    EXPECT_TRUE(a.value != VA_ATTRIB_NOT_SUPPORTED, "not supported");
    EXPECT_TRUE((a.value & 0xffff) >= 1, "L0 refs < 1");
    TEST_PASS();
}

static void test_config_quality_range(void) {
    TEST_START("Quality range attribute reported");
    VAConfigAttrib a = { .type = VAConfigAttribEncQualityRange };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, VAProfileH264High,
                                         VAEntrypointEncSlice, &a, 1));
    EXPECT_TRUE(a.value != VA_ATTRIB_NOT_SUPPORTED, "not supported");
    EXPECT_TRUE(a.value >= 1, "quality range < 1");
    TEST_PASS();
}

/* --- Error path tests --- */

static void test_invalid_entrypoint(void) {
    TEST_START("vaCreateConfig with invalid entrypoint → error");
    VAConfigAttrib a = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    /* Use a valid profile but wrong entrypoint type (0xFF) */
    VAStatus st = vaCreateConfig(g_dpy, VAProfileH264High, (VAEntrypoint)0xFF,
                                  &a, 1, &config);
    EXPECT_TRUE(st != VA_STATUS_SUCCESS, "should fail for invalid entrypoint");
    TEST_PASS();
}

static void test_encode_on_decode_only_profile(void) {
    TEST_START("vaCreateConfig encode on MPEG2 (decode-only) → error");
    VAConfigAttrib a = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    VAStatus st = vaCreateConfig(g_dpy, VAProfileMPEG2Simple,
                                  VAEntrypointEncSlice, &a, 1, &config);
    EXPECT_TRUE(st != VA_STATUS_SUCCESS, "should fail for decode-only profile");
    TEST_PASS();
}

static void test_create_config_all_encode_profiles(void) {
    VAProfile profiles[] = {
        VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High,
        VAProfileHEVCMain, VAProfileHEVCMain10,
    };
    for (int i = 0; i < 5; i++) {
        char name[64];
        snprintf(name, sizeof(name), "vaCreateConfig for encode profile %d", profiles[i]);
        TEST_START(name);

        VAConfigAttrib a = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
        VAConfigID config;
        VAStatus st = vaCreateConfig(g_dpy, profiles[i], VAEntrypointEncSlice,
                                      &a, 1, &config);
        EXPECT_STATUS(st);
        st = vaDestroyConfig(g_dpy, config);
        EXPECT_STATUS(st);
        TEST_PASS();
    }
}

/* --- Surface creation tests --- */

static void test_surface_nv12(void) {
    TEST_START("Create NV12 surface 1920x1080");
    VASurfaceID surface;
    VAStatus st = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 1920, 1080,
                                    &surface, 1, NULL, 0);
    EXPECT_STATUS(st);
    vaDestroySurfaces(g_dpy, &surface, 1);
    TEST_PASS();
}

static void test_surface_p010(void) {
    TEST_START("Create P010 surface 1920x1080 (10-bit)");
    VASurfaceID surface;
    VAStatus st = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420_10, 1920, 1080,
                                    &surface, 1, NULL, 0);
    if (st != VA_STATUS_SUCCESS) {
        TEST_SKIP("10-bit surfaces not supported");
        return;
    }
    vaDestroySurfaces(g_dpy, &surface, 1);
    TEST_PASS();
}

static void test_surface_multiple(void) {
    TEST_START("Create 16 surfaces simultaneously");
    VASurfaceID surfaces[16];
    VAStatus st = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 640, 480,
                                    surfaces, 16, NULL, 0);
    EXPECT_STATUS(st);
    vaDestroySurfaces(g_dpy, surfaces, 16);
    TEST_PASS();
}

static void test_surface_small(void) {
    TEST_START("Create tiny surface 16x16");
    VASurfaceID surface;
    VAStatus st = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 16, 16,
                                    &surface, 1, NULL, 0);
    EXPECT_STATUS(st);
    vaDestroySurfaces(g_dpy, &surface, 1);
    TEST_PASS();
}

static void test_surface_4k(void) {
    TEST_START("Create 4K surface 3840x2160");
    VASurfaceID surface;
    VAStatus st = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 3840, 2160,
                                    &surface, 1, NULL, 0);
    EXPECT_STATUS(st);
    vaDestroySurfaces(g_dpy, &surface, 1);
    TEST_PASS();
}

/* --- Main --- */

int main(void)
{
    test_global_setup();

    printf("\n=== nvidia-vaapi-driver config & capability tests ===\n");
    printf("Driver: %s\n\n", vaQueryVendorString(g_dpy));

    printf("Encode entrypoints:\n");
    test_encode_entrypoints();

    printf("\nDecode entrypoints:\n");
    test_decode_entrypoints();

    printf("\nConfig attributes:\n");
    test_config_rtformat();
    test_config_ratecontrol();
    test_config_packed_headers();
    test_config_max_ref_frames();
    test_config_quality_range();

    printf("\nError paths:\n");
    test_invalid_entrypoint();
    test_encode_on_decode_only_profile();

    printf("\nConfig creation:\n");
    test_create_config_all_encode_profiles();

    printf("\nSurface creation:\n");
    test_surface_nv12();
    test_surface_p010();
    test_surface_multiple();
    test_surface_small();
    test_surface_4k();

    test_print_summary("Config tests");
    test_global_teardown();
    return g_fail > 0 ? 1 : 0;
}
