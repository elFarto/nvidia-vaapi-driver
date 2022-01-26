#ifndef EXPORT_BUF_H
#define EXPORT_BUF_H

#include "vabackend.h"

bool initExporter(NVDriver *drv);
void releaseExporter(NVDriver *drv);
bool exportCudaPtr(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch, int *fourcc, int *fds, int *offsets, int *strides, uint64_t *mods, int *bppOut);
bool freeSurface(NVDriver *drv, NVSurface *surface);

#endif
