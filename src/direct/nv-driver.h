#ifndef NV_DRIVER_H
#define NV_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

#include "../common.h"
#include "nvidia-drm-ioctl.h"

#define ROUND_UP(N, S) ((((N) + (S) - 1) / (S)) * (S))

typedef struct {
    int nvctlFd;
    int nv0Fd;
    int drmFd;
    uint32_t clientObject;
    uint32_t deviceObject;
    uint32_t subdeviceObject;
    uint32_t driverMajorVersion;
    uint32_t driverMinorVersion;
    //bool hasHugePage;
    uint32_t gpu_id;
    uint32_t generic_page_kind;
    uint32_t page_kind_generation;
    uint32_t sector_layout;
} NVDriverContext;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint64_t mods;
    uint32_t memorySize;
    uint32_t offset;
    uint32_t pitch;
    uint32_t fourcc;
    uint32_t log2GobsPerBlockX;
    uint32_t log2GobsPerBlockY;
    uint32_t log2GobsPerBlockZ;
} NVDriverImage;

bool init_nvdriver(NVDriverContext *context, int drmFd);
bool free_nvdriver(NVDriverContext *context);
bool get_device_uuid(const NVDriverContext *context, uint8_t uuid[16]);
bool alloc_memory(const NVDriverContext *context, uint32_t size, int *fd);
bool alloc_buffer(NVDriverContext *context, uint32_t size, const NVDriverImage images[], int *fd1, int *fd2, int *drmFd);
uint32_t calculate_image_size(const NVDriverContext *context, NVDriverImage images[], uint32_t width, uint32_t height, uint32_t bppc, uint32_t numPlanes, const NVFormatPlane planes[]);
#endif
