#ifndef NVD_EXTERNAL_SURFACE_H
#define NVD_EXTERNAL_SURFACE_H

#include "vabackend.h"

VAStatus nv_parse_external_import_descriptor(uint32_t mem_type,
                                             const void *external_desc,
                                             const VASurfaceAttribExternalBuffers **extbuf,
                                             const VADRMPRIMESurfaceDescriptor **prime2_desc);

VAStatus nv_validate_external_buffer_import(unsigned int format,
                                            const VASurfaceAttribExternalBuffers *extbuf,
                                            cudaVideoSurfaceFormat surface_format,
                                            int bitdepth);

VAStatus nv_validate_prime2_buffer_import(unsigned int format,
                                          const VADRMPRIMESurfaceDescriptor *desc,
                                          cudaVideoSurfaceFormat surface_format,
                                          int bitdepth);

BackingImage *nv_import_external_buffer(NVDriver *drv,
                                        NVSurface *surface,
                                        const VASurfaceAttribExternalBuffers *extbuf);

BackingImage *nv_import_external_prime2_buffer(NVDriver *drv,
                                               NVSurface *surface,
                                               const VADRMPRIMESurfaceDescriptor *desc);

bool nvBackingImageIsImportedExternal(const BackingImage *img);
bool nvBackingImageIsHostMappedExternal(const BackingImage *img);
bool nvSyncBackingImageHostAccess(const BackingImage *img, bool write, bool start);
bool nv_copy_device_to_imported_backing_image(NVDriver *drv,
                                              BackingImage *img,
                                              uint32_t width,
                                              uint32_t height,
                                              CUdeviceptr src,
                                              uint32_t src_pitch);
const void *nv_get_host_external_plane_ptr(const BackingImage *img, uint32_t plane_idx);
void *nv_get_host_external_plane_ptr_mut(BackingImage *img, uint32_t plane_idx);
bool nvFillImportedExportDescriptor(const BackingImage *img, VADRMPRIMESurfaceDescriptor *desc);
void nvDestroyImportedBackingImage(struct _NVDriver *drv, BackingImage *img);
void nv_destroy_imported_object_cache(NVDriver *drv);

#endif
