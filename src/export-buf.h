#ifndef EXPORT_BUF_H
#define EXPORT_BUF_H

#include "vabackend.h"
#include <cuda.h>

void initExporter(NVDriver *drv);
void releaseExporter(NVDriver *drv);
int exportCudaPtr(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch, int *fourcc, int *fds, int *offsets, int *strides, uint64_t *mods, int *bppOut);

#endif
