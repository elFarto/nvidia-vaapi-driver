#include <stdlib.h>
#include <string.h>

#include "encode_driver.h"
#include "encode_common.h"
#include "encode_pipeline.h"
#include "h264_encode.h"

static uint32_t nvenc_supported_rate_control_mask(void)
{
    return VA_RC_CQP | VA_RC_CBR | VA_RC_VBR;
}

static uint32_t nvenc_supported_rt_format_mask(void)
{
    return VA_RT_FORMAT_YUV420;
}

static bool nvenc_rate_control_is_valid(uint32_t rc_mode)
{
    uint32_t supported = nvenc_supported_rate_control_mask();
    uint32_t requested = rc_mode ? rc_mode : NVENC_DEFAULT_RATE_CONTROL;

    if ((requested & ~supported) != 0) {
        return false;
    }

    uint32_t base_modes = requested & supported;
    return base_modes == VA_RC_CQP || base_modes == VA_RC_CBR || base_modes == VA_RC_VBR;
}

bool nvenc_supports_profile(const NVDriver *drv, VAProfile profile)
{
    return drv && drv->nvencAvailable && isH264EncodeProfile(profile) &&
           nvenc_query_encode_caps((NVDriver*) drv);
}

void nvenc_driver_init(NVDriver *drv)
{
    if (!drv) {
        return;
    }

    drv->nvencAvailable = false;
    drv->nvencMaxVersion = 0;

    if (!drv->nvenc) {
        return;
    }

    uint32_t version = 0;
    if (drv->nvenc->NvEncodeAPIGetMaxSupportedVersion(&version) == NV_ENC_SUCCESS) {
        drv->nvencMaxVersion = version;
    }

    NV_ENCODE_API_FUNCTION_LIST fl = { 0 };
    fl.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (drv->nvenc->NvEncodeAPICreateInstance(&fl) == NV_ENC_SUCCESS) {
        drv->nvencFuncs = fl;
        drv->nvencAvailable = true;
    } else {
        LOG("NVENC functions unavailable");
    }
}

void nvenc_config_init_defaults(NVConfig *cfg,
                                VAProfile profile,
                                VAEntrypoint entrypoint,
                                cudaVideoCodec cuda_codec)
{
    cfg->profile = profile;
    cfg->entrypoint = entrypoint;
    cfg->cudaCodec = cuda_codec;
    cfg->chromaFormat = cudaVideoChromaFormat_420;
    cfg->surfaceFormat = cudaVideoSurfaceFormat_NV12;
    cfg->bitDepth = 8;
    cfg->rateControl = NVENC_DEFAULT_RATE_CONTROL;
}

VAStatus nvenc_config_apply_rt_format_attribs(NVConfig *cfg,
                                              const VAConfigAttrib *attrib_list,
                                              int num_attribs)
{
    if (!cfg) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (num_attribs > 0 && !attrib_list) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    for (int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type != VAConfigAttribRTFormat) {
            continue;
        }
        if (attrib_list[i].value != nvenc_supported_rt_format_mask()) {
            LOG("Unsupported encode RT format: 0x%x", attrib_list[i].value);
            return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        }
        cfg->surfaceFormat = cudaVideoSurfaceFormat_NV12;
        cfg->chromaFormat = cudaVideoChromaFormat_420;
        cfg->bitDepth = 8;
    }

    return VA_STATUS_SUCCESS;
}

VAStatus nvenc_config_apply_rate_control_attribs(NVConfig *cfg,
                                                 const VAConfigAttrib *attrib_list,
                                                 int num_attribs)
{
    if (!cfg) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (num_attribs > 0 && !attrib_list) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    for (int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == VAConfigAttribRateControl) {
            if (!nvenc_rate_control_is_valid(attrib_list[i].value)) {
                LOG("Unsupported encode rate-control mode: 0x%x", attrib_list[i].value);
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            }
            cfg->rateControl = attrib_list[i].value;
        }
    }

    return VA_STATUS_SUCCESS;
}

void nvenc_get_config_attributes(NVDriver *drv,
                                 VAConfigAttrib *attrib_list,
                                 int num_attribs)
{
    bool have_caps = nvenc_query_encode_caps(drv);
    uint32_t max_ref_l0 = (have_caps && drv->nvencCaps.supportMultipleRefFrames <= 0) ?
                          1u : NVENC_H264_MAX_REFS;
    bool supports_future_prediction = !have_caps || drv->nvencCaps.maxBFrames > 0;
    uint32_t max_ref_l1 = supports_future_prediction ? max_ref_l0 : 0u;

    for (int i = 0; i < num_attribs; i++) {
        switch (attrib_list[i].type) {
        case VAConfigAttribRTFormat:
            attrib_list[i].value = nvenc_supported_rt_format_mask();
            break;
        case VAConfigAttribRateControl:
            attrib_list[i].value = nvenc_supported_rate_control_mask();
            break;
        case VAConfigAttribEncMaxRefFrames:
            attrib_list[i].value = max_ref_l0 | (max_ref_l1 << 16);
            break;
        case VAConfigAttribPredictionDirection:
            attrib_list[i].value = VA_PREDICTION_DIRECTION_PREVIOUS;
            if (supports_future_prediction) {
                attrib_list[i].value |= VA_PREDICTION_DIRECTION_FUTURE;
            }
            break;
        case VAConfigAttribEncMaxSlices:
            if (have_caps && drv->nvencCaps.supportDynamicSliceMode > 0) {
                attrib_list[i].value = NVENC_H264_MAX_SLICES;
            } else {
                attrib_list[i].value = 1;
            }
            break;
        case VAConfigAttribEncSliceStructure:
            attrib_list[i].value = VA_ENC_SLICE_STRUCTURE_EQUAL_ROWS |
                                   VA_ENC_SLICE_STRUCTURE_EQUAL_MULTI_ROWS;
            break;
        case VAConfigAttribEncPackedHeaders:
            attrib_list[i].value = VA_ENC_PACKED_HEADER_SEQUENCE |
                                   VA_ENC_PACKED_HEADER_PICTURE |
                                   VA_ENC_PACKED_HEADER_SLICE |
                                   VA_ENC_PACKED_HEADER_MISC |
                                   VA_ENC_PACKED_HEADER_RAW_DATA;
            break;
#if VA_CHECK_VERSION(1, 20, 0)
        case VAConfigAttribEncIntraRefresh:
            if (have_caps && drv->nvencCaps.supportIntraRefresh > 0) {
                attrib_list[i].value = VA_ENC_INTRA_REFRESH_CYCLIC |
                                       VA_ENC_INTRA_REFRESH_P_FRAME;
            } else {
                attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            }
            break;
        case VAConfigAttribEncQuantization:
            attrib_list[i].value = VA_ENC_QUANTIZATION_NONE;
            break;
#endif
        case VAConfigAttribEncQualityRange:
            attrib_list[i].value = NVENC_H264_MAX_QUALITY_LEVEL;
            break;
        case VAConfigAttribMaxFrameSize:
            attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncROI:
            if (have_caps && drv->nvencCaps.supportEmphasisMap > 0) {
                VAConfigAttribValEncROI roi = { 0 };
                roi.bits.num_roi_regions = NVENC_H264_MAX_ROI_REGIONS;
                roi.bits.roi_rc_priority_support = 0;
                roi.bits.roi_rc_qp_delta_support = 1;
                attrib_list[i].value = roi.value;
            } else {
                attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            }
            break;
        default:
            attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        }
    }
}

void nvenc_query_config_attributes(const NVConfig *cfg,
                                   VAProfile *profile,
                                   VAEntrypoint *entrypoint,
                                   VAConfigAttrib *attrib_list,
                                   int *num_attribs)
{
    int i = 0;

    if (profile != NULL) {
        *profile = cfg->profile;
    }
    if (entrypoint != NULL) {
        *entrypoint = cfg->entrypoint;
    }

    if (attrib_list != NULL) {
        attrib_list[i].value = nvenc_supported_rt_format_mask();
        attrib_list[i].type = VAConfigAttribRTFormat;
        i++;

        attrib_list[i].value = cfg->rateControl;
        attrib_list[i].type = VAConfigAttribRateControl;
        i++;
    } else {
        i = 2;
    }

    if (num_attribs != NULL) {
        *num_attribs = i;
    }
}

VAStatus nvenc_context_create(NVDriver *drv,
                              const NVConfig *cfg,
                              int width,
                              int height,
                              NVEncodeContext **out_enc)
{
    NVEncodeContext *enc = NULL;

    if (!drv || !cfg || !out_enc) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (!drv->nvencAvailable || !drv->nvenc) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    enc = (NVEncodeContext*) calloc(1, sizeof(NVEncodeContext));
    if (!enc) {
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    nvenc_context_init_defaults(enc, drv, cfg->profile, cfg->entrypoint,
                                width, height,
                                cfg->rateControl ? cfg->rateControl : NVENC_DEFAULT_RATE_CONTROL,
                                &drv->nvencFuncs);
    nvenc_context_init_sync_primitives(enc);
    if (!nvenc_context_open_session(enc, drv->cudaContext)) {
        nvenc_context_destroy_sync_primitives(enc);
        free(enc);
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL))) {
        nvenc_context_release_encoder_resources(enc);
        nvenc_context_release_host_resources(enc);
        free(enc);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    *out_enc = enc;
    return VA_STATUS_SUCCESS;
}

void nvenc_context_init_defaults(NVEncodeContext *enc,
                                 NVDriver *drv,
                                 VAProfile profile,
                                 VAEntrypoint entrypoint,
                                 int width,
                                 int height,
                                 uint32_t rc_mode,
                                 NV_ENCODE_API_FUNCTION_LIST *funcs)
{
    enc->drv = drv;
    enc->profile = profile;
    enc->entrypoint = entrypoint;
    enc->width = width;
    enc->height = height;
    enc->funcs = funcs;
    enc->rcMode = rc_mode;
    enc->enablePTD = true;
    enc->forceConservativeInit = false;
    enc->inputSurface = VA_INVALID_ID;
    enc->codedBufId = VA_INVALID_ID;
    enc->patchBpSei = true;
    nvenc_reset_bp_state(enc);
    enc->bitstreamPoolMax = NVENC_DEFAULT_BITSTREAM_POOL_MAX;
}

void nvenc_context_init_sync_primitives(NVEncodeContext *enc)
{
    pthread_mutex_init(&enc->reorderMutex, NULL);
    pthread_cond_init(&enc->reorderCond, NULL);
    pthread_mutex_init(&enc->queueMutex, NULL);
    pthread_mutex_init(&enc->bitstreamPoolMutex, NULL);
}

void nvenc_context_destroy_sync_primitives(NVEncodeContext *enc)
{
    pthread_mutex_destroy(&enc->queueMutex);
    pthread_cond_destroy(&enc->reorderCond);
    pthread_mutex_destroy(&enc->reorderMutex);
    pthread_mutex_destroy(&enc->bitstreamPoolMutex);
}

bool nvenc_context_open_session(NVEncodeContext *enc, CUcontext cuda_context)
{
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams = { 0 };
    openParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    openParams.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    openParams.device = cuda_context;
    openParams.apiVersion = NVENCAPI_VERSION;
    NVENCSTATUS st = enc->funcs->nvEncOpenEncodeSessionEx(&openParams, &enc->encoder);
    if (st != NV_ENC_SUCCESS || !enc->encoder) {
        LOG("NvEncOpenEncodeSessionEx failed: %d", st);
        return false;
    }
    return true;
}

void nvenc_context_release_encoder_resources(NVEncodeContext *enc)
{
    if (!enc || !enc->encoder || !enc->funcs) {
        return;
    }

    nvenc_destroy_bitstream_pool(enc);
    ARRAY_FOR_EACH(NVEncSurfaceReg*, reg, &enc->registeredSurfaces)
        nvenc_unregister_surface(enc, reg);
        free(reg);
    END_FOR_EACH
    free(enc->registeredSurfaces.buf);
    enc->registeredSurfaces.buf = NULL;
    enc->registeredSurfaces.size = 0;
    enc->registeredSurfaces.capacity = 0;

    NVENCSTATUS st = enc->funcs->nvEncDestroyEncoder(enc->encoder);
    if (st != NV_ENC_SUCCESS) {
        LOG("NvEncDestroyEncoder failed: %d", st);
    }
    enc->encoder = NULL;
}

void nvenc_context_release_host_resources(NVEncodeContext *enc)
{
    if (!enc) {
        return;
    }

    nvenc_context_destroy_sync_primitives(enc);
    nvenc_release_roi_state(enc);
    nvenc_reset_appendable_buffer(&enc->packedHeaderBuf);
    nvenc_reset_appendable_buffer(&enc->packedSpsBuf);
    nvenc_reset_appendable_buffer(&enc->packedPpsBuf);
    memset(&enc->rewriteNativeH264Sps, 0, sizeof(enc->rewriteNativeH264Sps));
    memset(&enc->rewriteNativeH264Pps, 0, sizeof(enc->rewriteNativeH264Pps));
    nvenc_clear_h264_sei_payloads(&enc->packedSeiPayloads, &enc->packedSeiPayloadCount);
    enc->packedTimingSeiSeen = false;
    enc->havePackedHeaderParam = false;
    memset(&enc->packedHeaderParam, 0, sizeof(enc->packedHeaderParam));
}
