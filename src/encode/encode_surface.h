#ifndef NVD_ENCODE_SURFACE_H
#define NVD_ENCODE_SURFACE_H

#include "../vabackend.h"

uint32_t nvenc_profile_coded_dim(VAProfile profile, uint32_t visible_dim);
uint32_t nvenc_context_coded_width(const NVContext *nvCtx);
uint32_t nvenc_context_coded_height(const NVContext *nvCtx);
bool nvenc_surface_has_encode_context(const NVSurface *surface);
bool nvenc_surface_uses_encode_path(const NVSurface *surface);
VAStatus nvenc_validate_render_targets(NVDriver *drv,
                                       uint32_t visible_width,
                                       uint32_t visible_height,
                                       uint32_t coded_width,
                                       uint32_t coded_height,
                                       VASurfaceID *render_targets,
                                       int num_render_targets,
                                       bool allow_pre_seq_cropped);
bool nvenc_try_bind_surface_to_encode_context(NVDriver *drv,
                                              NVSurface *surface);

VAStatus nvenc_query_surface_attributes(NVDriver *drv,
                                        VASurfaceAttrib *attrib_list,
                                        unsigned int *num_attribs);

#endif
