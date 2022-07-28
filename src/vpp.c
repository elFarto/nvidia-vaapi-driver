#include "vabackend.h"

static void copyPipeline(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams) {
    VAProcFilterParameterBuffer *buf = (VAProcFilterParameterBuffer*) buffer->ptr;

    LOG("got pipeline: %p\n", buf);
}

static cudaVideoCodec computeVPPCudaCodec(VAProfile profile) {
    return cudaVideoCodec_NONE;
}

static const VAProfile vppSupportedProfiles[] = {
    VAProfileNone,
};

const DECLARE_CODEC(vppCodec) = {
    .computeCudaCodec = computeVPPCudaCodec,
    .handlers = {
        [VAProcPipelineParameterBufferType] = copyPipeline,
    },
    .entrypoint = VAEntrypointVideoProc,
    .supportedProfileCount = ARRAY_SIZE(vppSupportedProfiles),
    .supportedProfiles = vppSupportedProfiles,
};
