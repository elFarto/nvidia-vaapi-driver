#include "encode_loader.h"
#include "encode_common.h"

void nvenc_loader_init(NvencFunctions **nvenc_out)
{
    if (!nvenc_out) {
        return;
    }
    *nvenc_out = NULL;

    int ret = nvenc_load_functions(nvenc_out, NULL);
    if (ret != 0 || !*nvenc_out) {
        *nvenc_out = NULL;
        LOG("Failed to load NVENC functions");
        return;
    }

    uint32_t version = 0;
    if ((*nvenc_out)->NvEncodeAPIGetMaxSupportedVersion(&version) != NV_ENC_SUCCESS) {
        LOG("NVENC API version query failed");
        nvenc_free_functions(nvenc_out);
        *nvenc_out = NULL;
        return;
    }

    uint32_t major = 0, minor = 0;
    nvenc_decode_api_version(version, &major, &minor);
    if (major < NVENCAPI_MAJOR_VERSION ||
        (major == NVENCAPI_MAJOR_VERSION && minor < NVENCAPI_MINOR_VERSION)) {
        LOG("NVENC API version unsupported (max %u.%u, required %u.%u)",
            major, minor, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
        nvenc_free_functions(nvenc_out);
        *nvenc_out = NULL;
    }
}

void nvenc_loader_cleanup(NvencFunctions **nvenc_io)
{
    if (nvenc_io && *nvenc_io) {
        nvenc_free_functions(nvenc_io);
        *nvenc_io = NULL;
    }
}
