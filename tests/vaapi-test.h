#ifndef VAAPI_TEST_H
#define VAAPI_TEST_H

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static void checkVa(VAStatus status, const char *operation) {
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "%s: %s\n", operation, vaErrorStr(status));
        exit(1);
    }
}
#define VA_CHECK(expression) checkVa((expression), #expression)

static VADisplay openDisplay(int *fd) {
    const char *device = getenv("VAAPI_DEVICE");
    *fd = open(device != NULL ? device : "/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (*fd < 0) {
        perror("open VA-API device");
        exit(77);
    }
    VADisplay display = vaGetDisplayDRM(*fd);
    REQUIRE(display != NULL);
    int major, minor;
    VA_CHECK(vaInitialize(display, &major, &minor));
    const char *vendor = vaQueryVendorString(display);
    REQUIRE(vendor != NULL);
    if (strstr(vendor, "NVDEC/NVENC") == NULL) {
        VA_CHECK(vaTerminate(display));
        close(*fd);
        fprintf(stderr, "test requires the NVIDIA NVDEC/NVENC VA-API driver\n");
        exit(77);
    }
    return display;
}

#endif
