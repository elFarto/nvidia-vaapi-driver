/*
 * test_common.h — Shared test utilities for nvidia-vaapi-driver tests.
 * Inspired by Intel's i965 test infrastructure.
 */

#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>

#define DRM_DEVICE "/dev/dri/renderD128"

/* Test counters */
static int g_pass = 0;
static int g_fail = 0;
static int g_skip = 0;

/* Colors */
#define C_GREEN  "\033[32m"
#define C_RED    "\033[31m"
#define C_YELLOW "\033[33m"
#define C_RESET  "\033[0m"

/* Test macros */
#define TEST_START(name) \
    printf("  %-55s ", name); fflush(stdout);

#define TEST_PASS() do { \
    printf(C_GREEN "PASS" C_RESET "\n"); g_pass++; \
} while (0)

#define TEST_FAIL(reason) do { \
    printf(C_RED "FAIL" C_RESET " (%s)\n", reason); g_fail++; \
} while (0)

#define TEST_SKIP(reason) do { \
    printf(C_YELLOW "SKIP" C_RESET " (%s)\n", reason); g_skip++; \
} while (0)

/* Assert that aborts current test function on failure */
#define EXPECT_STATUS(st) do { \
    if ((st) != VA_STATUS_SUCCESS) { \
        char _msg[64]; snprintf(_msg, sizeof(_msg), "VA status %d", (st)); \
        TEST_FAIL(_msg); return; \
    } \
} while (0)

#define EXPECT_STATUS_EQ(expect, st) do { \
    VAStatus _s = (st); \
    if (_s != (expect)) { \
        char _msg[64]; snprintf(_msg, sizeof(_msg), \
            "expected status %d, got %d", (expect), _s); \
        TEST_FAIL(_msg); return; \
    } \
} while (0)

#define EXPECT_TRUE(cond, reason) do { \
    if (!(cond)) { TEST_FAIL(reason); return; } \
} while (0)

#define EXPECT_NOT_NULL(ptr, reason) do { \
    if ((ptr) == NULL) { TEST_FAIL(reason); return; } \
} while (0)

/* Timer for performance measurement */
typedef struct {
    struct timespec start;
    struct timespec end;
} TestTimer;

static inline void timer_start(TestTimer *t) {
    clock_gettime(CLOCK_MONOTONIC, &t->start);
}

static inline double timer_stop_ms(TestTimer *t) {
    clock_gettime(CLOCK_MONOTONIC, &t->end);
    return (t->end.tv_sec - t->start.tv_sec) * 1000.0
         + (t->end.tv_nsec - t->start.tv_nsec) / 1000000.0;
}

/* Global VA display setup */
static VADisplay g_dpy;
static int g_drm_fd;

static void test_global_setup(void) {
    g_drm_fd = open(DRM_DEVICE, O_RDWR);
    if (g_drm_fd < 0) {
        fprintf(stderr, "Cannot open %s\n", DRM_DEVICE);
        exit(1);
    }
    g_dpy = vaGetDisplayDRM(g_drm_fd);
    if (!g_dpy) {
        fprintf(stderr, "vaGetDisplayDRM failed\n");
        exit(1);
    }
    int major, minor;
    VAStatus st = vaInitialize(g_dpy, &major, &minor);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaInitialize failed: %d\n", st);
        exit(1);
    }
}

static void test_global_teardown(void) {
    vaTerminate(g_dpy);
    close(g_drm_fd);
}

static void test_print_summary(const char *suite_name) {
    printf("\n=== %s: %d passed, %d failed, %d skipped ===\n\n",
           suite_name, g_pass, g_fail, g_skip);
}

/* Check if a profile+entrypoint combination is supported */
static bool test_has_entrypoint(VADisplay dpy, VAProfile profile, VAEntrypoint ep) {
    int ne = vaMaxNumEntrypoints(dpy);
    VAEntrypoint *eps = calloc(ne, sizeof(VAEntrypoint));
    int n = 0;
    vaQueryConfigEntrypoints(dpy, profile, eps, &n);
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (eps[i] == ep) { found = true; break; }
    }
    free(eps);
    return found;
}

#endif /* TEST_COMMON_H */
