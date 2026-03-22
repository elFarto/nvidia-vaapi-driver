#ifndef NVD_ENCODE_DRIVER_H
#define NVD_ENCODE_DRIVER_H

#include "../vabackend.h"

bool nvenc_supports_profile(const NVDriver *drv, VAProfile profile);
void nvenc_driver_init(NVDriver *drv);
void nvenc_config_init_defaults(NVConfig *cfg,
                                VAProfile profile,
                                VAEntrypoint entrypoint,
                                cudaVideoCodec cuda_codec);
VAStatus nvenc_config_apply_rt_format_attribs(NVConfig *cfg,
                                              const VAConfigAttrib *attrib_list,
                                              int num_attribs);
VAStatus nvenc_config_apply_rate_control_attribs(NVConfig *cfg,
                                                 const VAConfigAttrib *attrib_list,
                                                 int num_attribs);
void nvenc_get_config_attributes(NVDriver *drv,
                                 VAConfigAttrib *attrib_list,
                                 int num_attribs);
void nvenc_query_config_attributes(const NVConfig *cfg,
                                   VAProfile *profile,
                                   VAEntrypoint *entrypoint,
                                   VAConfigAttrib *attrib_list,
                                   int *num_attribs);
VAStatus nvenc_context_create(NVDriver *drv,
                              const NVConfig *cfg,
                              int width,
                              int height,
                              NVEncodeContext **out_enc);
void nvenc_context_init_defaults(NVEncodeContext *enc,
                                 NVDriver *drv,
                                 VAProfile profile,
                                 VAEntrypoint entrypoint,
                                 int width,
                                 int height,
                                 uint32_t rc_mode,
                                 NV_ENCODE_API_FUNCTION_LIST *funcs);
void nvenc_context_init_sync_primitives(NVEncodeContext *enc);
void nvenc_context_destroy_sync_primitives(NVEncodeContext *enc);
bool nvenc_context_open_session(NVEncodeContext *enc, CUcontext cuda_context);
void nvenc_context_release_encoder_resources(NVEncodeContext *enc);
void nvenc_context_release_host_resources(NVEncodeContext *enc);

#endif
