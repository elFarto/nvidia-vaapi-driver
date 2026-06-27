#ifndef NVENC_H
#define NVENC_H

#include <ffnvcodec/nvEncodeAPI.h>
#include <ffnvcodec/dynlink_loader.h>
#include <va/va.h>
#include <stdbool.h>
#include <stdint.h>
#include "vabackend.h"

// Encode-specific context, stored in NVContext->encodeData
// when created with VAEntrypointEncSlice.

typedef struct {
    NV_ENC_OUTPUT_PTR       bitstreamBuffer;
    bool                    allocated;
    void                   *lockedPtr;      //locked bitstream pointer
    uint32_t                lockedSize;
    bool                    locked;
} NVENCOutputBuffer;

typedef struct {
    void                           *encoder;        //NVENC session handle
    NV_ENCODE_API_FUNCTION_LIST     funcs;
    bool                            initialized;
    GUID                            codecGuid;
    GUID                            profileGuid;
    NV_ENC_CONFIG                   encodeConfig;
    NV_ENC_INITIALIZE_PARAMS        initParams;
    uint32_t                        width;
    uint32_t                        height;
    NV_ENC_BUFFER_FORMAT            inputFormat;
    bool                            seqParamSet;
    uint32_t                        rcMode;         //VA-API rate control mode
    uint32_t                        bitrate;        //bits/sec
    uint32_t                        maxBitrate;
    uint32_t                        frameRateNum;
    uint32_t                        frameRateDen;
    uint32_t                        intraPeriod;    //GOP length
    uint32_t                        ipPeriod;
    uint32_t                        vbvBufferSize;  //HRD buffer size (bits)
    uint32_t                        vbvInitialDelay; //HRD initial fullness (bits)
    uint64_t                        frameCount;
    NVENCOutputBuffer               outputBuffer;
    VABufferID                      currentCodedBufId;
    bool                            forceIDR;       //from idr_pic_flag
    NV_ENC_PIC_TYPE                 picType;        //from slice params
    bool                            useIPC;         //encode via 64-bit helper
    int                             ipcFd;          //socket fd, -1 if not connected
    void                           *shmPtr;         //mmap'd shared memory for frame data
    uint32_t                        shmSize;
    int                             shmFd;
} NVENCContext;

// Wraps VACodedBufferSegment with NVENC bitstream storage
typedef struct {
    VACodedBufferSegment    segment;
    void                   *bitstreamData;
    uint32_t                bitstreamSize;
    uint32_t                bitstreamAlloc;
    bool                    hasData;
} NVCodedBuffer;

bool nvenc_load(NvencFunctions **nvenc_dl);
void nvenc_unload(NvencFunctions **nvenc_dl);

bool nvenc_open_session(NVENCContext *nvencCtx, NvencFunctions *nvenc_dl, CUcontext cudaCtx);
void nvenc_close_session(NVENCContext *nvencCtx);

bool nvenc_init_encoder(NVENCContext *nvencCtx, uint32_t width, uint32_t height,
                        GUID codecGuid, GUID profileGuid, GUID presetGuid,
                        NV_ENC_TUNING_INFO tuningInfo);

bool nvenc_alloc_output_buffer(NVENCContext *nvencCtx);
void nvenc_free_output_buffer(NVENCContext *nvencCtx);

bool nvenc_register_cuda_resource(NVENCContext *nvencCtx, CUdeviceptr devPtr,
                                  uint32_t width, uint32_t height, uint32_t pitch,
                                  NV_ENC_BUFFER_FORMAT format,
                                  NV_ENC_REGISTERED_PTR *outRegistered);
bool nvenc_map_resource(NVENCContext *nvencCtx, NV_ENC_REGISTERED_PTR registered,
                        NV_ENC_INPUT_PTR *outMapped, NV_ENC_BUFFER_FORMAT *outFmt);
bool nvenc_unmap_resource(NVENCContext *nvencCtx, NV_ENC_INPUT_PTR mapped);
bool nvenc_unregister_resource(NVENCContext *nvencCtx, NV_ENC_REGISTERED_PTR registered);

int nvenc_encode_frame(NVENCContext *nvencCtx, NV_ENC_INPUT_PTR inputBuffer,
                       NV_ENC_BUFFER_FORMAT bufferFmt,
                       uint32_t inputWidth, uint32_t inputHeight, uint32_t inputPitch,
                       NV_ENC_PIC_TYPE picType, uint32_t picFlags);

bool nvenc_lock_bitstream(NVENCContext *nvencCtx, void **outPtr, uint32_t *outSize);
bool nvenc_unlock_bitstream(NVENCContext *nvencCtx);

bool nvenc_is_encode_profile(VAProfile profile);
GUID nvenc_va_profile_to_codec_guid(VAProfile profile);
GUID nvenc_va_profile_to_profile_guid(VAProfile profile);
NV_ENC_BUFFER_FORMAT nvenc_surface_format(VAProfile profile);

void h264enc_handle_sequence_params(NVENCContext *ctx, NVBuffer *buf);
void h264enc_handle_picture_params(NVENCContext *ctx, NVBuffer *buf);
void h264enc_handle_slice_params(NVENCContext *ctx, NVBuffer *buf);
void h264enc_handle_misc_params(NVENCContext *ctx, NVBuffer *buf);
void hevcenc_handle_sequence_params(NVENCContext *ctx, NVBuffer *buf);
void hevcenc_handle_picture_params(NVENCContext *ctx, NVBuffer *buf);
void hevcenc_handle_slice_params(NVENCContext *ctx, NVBuffer *buf);
void hevcenc_handle_misc_params(NVENCContext *ctx, NVBuffer *buf);

#endif // NVENC_H
