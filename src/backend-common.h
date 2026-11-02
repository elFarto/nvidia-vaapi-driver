#ifndef BACKENDCOMMON_H
#define BACKENDCOMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

bool checkModesetParameterFromFd(int fd);
bool isNvidiaDrmFd(int fd, bool log);
bool isCudaDmabufSupported(struct _NVDriver *drv, int gpuId);
void backendWatchCallerOwnedFd(int fd, const char *reason);
void backendUnwatchCallerOwnedFd(int fd, const char *reason);
void backendCloseFd(int fd, const char *reason);
void backendTrackNvKmsFdDup(int srcFd, int dupFd, const char *reason);
void backendCloseTrackedNvKmsFd(int fd, const char *reason);
void backendReleaseTrackedNvKmsFd(int fd, const char *reason);
int convertDmabufFdToNvFd(
    int dmaBufFd,
    int drmFd,
    int nvctlFallbackFd,
    uint32_t driverMajorVersion
);
bool copy_external_plane_to_cuda_array(
    struct _NVDriver *drv,
    int importFd,
    bool useDmaBufHandle,
    uint32_t objectSize,
    uint32_t planeOffset,
    uint32_t planePitch,
    uint32_t copyWidthBytes,
    uint32_t planeHeight,
    CUarray dstArray
);

static inline bool isTruthyEnv(const char *envValue) {
    return envValue != NULL && strcmp(envValue, "0") != 0;
}

#endif // BACKENDCOMMON_H
