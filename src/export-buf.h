#ifndef EXPORT_BUF_H
#define EXPORT_BUF_H

#include "vabackend.h"
#include <va/va_drmcommon.h>

bool initExporter(NVDriver *drv);
void releaseExporter(NVDriver *drv);
bool exportCudaPtr(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch);
void detachBackingImageFromSurface(NVDriver *drv, NVSurface *surface);
bool realiseSurface(NVDriver *drv, NVSurface *surface);
bool fillExportDescriptor(NVDriver *drv, NVSurface *surface, VADRMPRIMESurfaceDescriptor *desc);
void destroyAllBackingImage(NVDriver *drv);

#endif
