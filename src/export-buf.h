#ifndef EXPORT_BUF_H
#define EXPORT_BUF_H

#include "vabackend.h"

int findGPUIndexFromFd(int displayType, int fd, int gpu, void **device);
bool initExporter(NVDriver *drv, void *device);
void releaseExporter(NVDriver *drv);
bool exportCudaPtr(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch);
void detachBackingImageFromSurface(NVDriver *drv, NVSurface *surface);
bool realiseSurface(NVDriver *drv, NVSurface *surface);

void destroyAllBackingImage(NVDriver *drv);

#endif
