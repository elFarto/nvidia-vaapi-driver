#include "vaapi-test.h"
#include <dirent.h>
#include <stdbool.h>
#include <va/va_drmcommon.h>

#define WIDTH 640
#define HEIGHT 368

static unsigned fdCount(void) {
    DIR *dir = opendir("/proc/self/fd");
    REQUIRE(dir != NULL);
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != '.') count++;
    }
    REQUIRE(closedir(dir) == 0);
    return count;
}

static VAStatus importSurface(VADisplay display, const VADRMPRIMESurfaceDescriptor *desc,
                              int fd, VASurfaceID *surface) {
    uintptr_t buffer = (uintptr_t)fd;
    VASurfaceAttribExternalBuffers legacy = {
        .pixel_format = desc->fourcc, .width = desc->width, .height = desc->height,
        .num_planes = 2, .num_buffers = 1, .buffers = &buffer,
        .data_size = desc->objects[0].size,
    };
    for (unsigned i = 0; i < 2; i++) {
        legacy.pitches[i] = desc->layers[i].pitch[0];
        legacy.offsets[i] = desc->layers[i].offset[0];
    }
    VASurfaceAttrib attributes[] = {
        {.type = VASurfaceAttribMemoryType, .flags = VA_SURFACE_ATTRIB_SETTABLE,
         .value = {.type = VAGenericValueTypeInteger, .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME}},
        {.type = VASurfaceAttribExternalBufferDescriptor, .flags = VA_SURFACE_ATTRIB_SETTABLE,
         .value = {.type = VAGenericValueTypePointer, .value.p = &legacy}},
    };
    return vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, WIDTH, HEIGHT, surface, 1, attributes, 2);
}

static void checkPixels(VADisplay display, VAImage *image, VASurfaceID surface) {
    VA_CHECK(vaGetImage(display, surface, 0, 0, WIDTH, HEIGHT, image->image_id));
    uint8_t *pixels;
    VA_CHECK(vaMapBuffer(display, image->buf, (void **)&pixels));
    for (unsigned y = 0; y < HEIGHT; y++) {
        for (unsigned x = 0; x < WIDTH; x++) {
            REQUIRE(pixels[image->offsets[0] + y * image->pitches[0] + x] == 64);
        }
    }
    for (unsigned y = 0; y < HEIGHT / 2; y++) {
        for (unsigned x = 0; x < WIDTH; x++) {
            REQUIRE(pixels[image->offsets[1] + y * image->pitches[1] + x] == (x % 2 ? 192 : 96));
        }
    }
    VA_CHECK(vaUnmapBuffer(display, image->buf));
}

int main(void) {
    int fd;
    VADisplay display = openDisplay(&fd);
    VAConfigID config;
    VA_CHECK(vaCreateConfig(display, VAProfileH264Main, VAEntrypointVLD, NULL, 0, &config));
    VASurfaceID original;
    VA_CHECK(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, WIDTH, HEIGHT, &original, 1, NULL, 0));
    VAContextID context;
    VA_CHECK(vaCreateContext(display, config, WIDTH, HEIGHT, VA_PROGRESSIVE, &original, 1, &context));
    VAImageFormat format = {.fourcc = VA_FOURCC_NV12};
    VAImage image;
    VA_CHECK(vaCreateImage(display, &format, WIDTH, HEIGHT, &image));
    uint8_t *pixels;
    VA_CHECK(vaMapBuffer(display, image.buf, (void **)&pixels));
    for (unsigned y = 0; y < HEIGHT; y++) {
        memset(pixels + image.offsets[0] + y * image.pitches[0], 64, WIDTH);
    }
    for (unsigned y = 0; y < HEIGHT / 2; y++) {
        for (unsigned x = 0; x < WIDTH; x++) {
            pixels[image.offsets[1] + y * image.pitches[1] + x] = x % 2 ? 192 : 96;
        }
    }
    VA_CHECK(vaUnmapBuffer(display, image.buf));
    VA_CHECK(vaPutImage(display, original, image.image_id, 0, 0, WIDTH, HEIGHT, 0, 0, WIDTH, HEIGHT));
    VADRMPRIMESurfaceDescriptor desc = {0};
    VA_CHECK(vaExportSurfaceHandle(display, original, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc));
    REQUIRE(desc.num_objects > 0 && desc.num_layers == 2);
    if (getenv("NVD_SINGLE_BUFFER") != NULL) REQUIRE(desc.num_objects == 1);
    VA_CHECK(vaDestroyContext(display, context));

    // A foreign FD and, for multi-object exports, a chroma-only FD are not enough
    // to reconstruct a surface's missing modifier and plane metadata.
    VASurfaceID imported;
    int unknown = open("/dev/zero", O_RDONLY | O_CLOEXEC);
    REQUIRE(unknown >= 0);
    REQUIRE(importSurface(display, &desc, unknown, &imported) == VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
    REQUIRE(fcntl(unknown, F_GETFD) >= 0);
    REQUIRE(close(unknown) == 0);
    if (desc.num_objects > 1) {
        REQUIRE(importSurface(display, &desc, desc.objects[1].fd, &imported) == VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
    }

    unsigned stableCount = 0;
    for (unsigned iteration = 0; iteration < 21; iteration++) {
        VA_CHECK(importSurface(display, &desc, desc.objects[0].fd, &imported));
        for (unsigned i = 0; i < desc.num_objects; i++) REQUIRE(fcntl(desc.objects[i].fd, F_GETFD) >= 0);
        VA_CHECK(vaCreateContext(display, config, WIDTH, HEIGHT, VA_PROGRESSIVE, &imported, 1, &context));
        checkPixels(display, &image, imported);
        VA_CHECK(vaDestroyContext(display, context));
        VA_CHECK(vaDestroySurfaces(display, &imported, 1));
        if (iteration == 0) stableCount = fdCount();
        else REQUIRE(fdCount() == stableCount);
    }
    for (unsigned i = 0; i < desc.num_objects; i++) REQUIRE(close(desc.objects[i].fd) == 0);
    VA_CHECK(vaDestroyImage(display, image.image_id));
    VA_CHECK(vaDestroySurfaces(display, &original, 1));
    VA_CHECK(vaDestroyConfig(display, config));
    VA_CHECK(vaTerminate(display));
    close(fd);
    puts("legacy own-buffer reimport: planes, context recreation, FD ownership and rejection verified");
    return 0;
}
