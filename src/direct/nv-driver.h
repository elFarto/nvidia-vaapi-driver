#ifndef NV_DRIVER_H
#define NV_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

#include "nvidia-drm-ioctl.h"

#define ROUND_UP(N, S) ((((N) + (S) - 1) / (S)) * (S))

typedef struct {
    int nvctlFd;
    int nv0Fd;
    int drmFd;
    struct drm_nvidia_get_dev_info_params devInfo;
    uint32_t clientObject;
    uint32_t deviceObject;
    uint32_t subdeviceObject;
    uint32_t driverMajorVersion;
    //bool hasHugePage;
} NVDriverContext;

typedef struct {
    int nvFd;
    int nvFd2;
    int drmFd;
    uint32_t width;
    uint32_t height;
    uint64_t mods;
    uint32_t memorySize;
    uint32_t offset;
    uint32_t pitch;
    uint32_t fourcc;
} NVDriverImage;

bool init_nvdriver(NVDriverContext *context, int drmFd);
bool free_nvdriver(NVDriverContext *context);
bool get_device_uuid(NVDriverContext *context, char uuid[16]);
bool alloc_memory(NVDriverContext *context, uint32_t size, int *fd);
bool alloc_image(NVDriverContext *context, uint32_t width, uint32_t height, uint8_t channels, uint8_t bytesPerChannel, uint32_t fourcc, NVDriverImage *image);

#endif
