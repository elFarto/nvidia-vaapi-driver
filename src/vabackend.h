#ifndef VABACKEND_H
#define VABACKEND_H

#include <ffnvcodec/dynlink_loader.h>
#include <va/va_backend.h>
#include <va/va.h>
#include <va/va_enc_h264.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdbool.h>
#include <va/va_drmcommon.h>

#include <pthread.h>
#include "list.h"
#include "direct/nv-driver.h"
#include "common.h"

#define SURFACE_QUEUE_SIZE 16
#define MAX_IMAGE_COUNT 64
#define MAX_PROFILES 32

#ifndef VA_CHECK_VERSION
#define VA_CHECK_VERSION(major, minor, micro) 0
#endif

/* vaSyncBuffer landed in VA-API 1.9. Keep the entrypoint gated there,
 * but prefer symbol-based compat for related constants in case of backports.
 */
#if VA_CHECK_VERSION(1, 9, 0)
#define NVD_HAVE_VA_SYNCBUFFER 1
#else
#define NVD_HAVE_VA_SYNCBUFFER 0
#endif

#ifdef VA_TIMEOUT_INFINITE
#define NVD_VA_TIMEOUT_INFINITE VA_TIMEOUT_INFINITE
#else
#define NVD_VA_TIMEOUT_INFINITE ((uint64_t)-1)
#endif

#ifdef VA_STATUS_ERROR_TIMEDOUT
#define NVD_VA_STATUS_ERROR_TIMEDOUT VA_STATUS_ERROR_TIMEDOUT
#else
#define NVD_VA_STATUS_ERROR_TIMEDOUT ((VAStatus)0x00000026)
#endif

typedef struct {
    void        *buf;
    uint64_t    size;
    uint64_t    allocated;
} AppendableBuffer;

typedef struct {
    bool        valid;
    uint32_t    sps_id;
    uint32_t    log2_max_frame_num_minus4;
    uint32_t    pic_order_cnt_type;
    uint32_t    log2_max_pic_order_cnt_lsb_minus4;
    bool        delta_pic_order_always_zero_flag;
    bool        gaps_in_frame_num_value_allowed_flag;
    bool        frame_mbs_only_flag;
    bool        vui_parameters_present_flag;
    bool        pic_struct_present_flag;
} NVH264SpsLite;

typedef struct {
    bool        valid;
    uint32_t    pps_id;
    uint32_t    sps_id;
    bool        entropy_coding_mode_flag;
    bool        bottom_field_pic_order_in_frame_present_flag;
    uint32_t    num_slice_groups_minus1;
    bool        weighted_pred_flag;
    uint32_t    weighted_bipred_idc;
    bool        deblocking_filter_control_present_flag;
    bool        redundant_pic_cnt_present_flag;
} NVH264PpsLite;

struct _NVContext;
struct _NVEncodeContext;
struct _NVEncSurfaceReg;
struct _BackingImage;
struct _NVImportedObjectCacheEntry;

typedef enum
{
    OBJECT_TYPE_CONFIG,
    OBJECT_TYPE_CONTEXT,
    OBJECT_TYPE_SURFACE,
    OBJECT_TYPE_BUFFER,
    OBJECT_TYPE_IMAGE
} ObjectType;

typedef struct Object_t
{
    ObjectType      type;
    VAGenericID     id;
    void            *obj;
} *Object;

typedef struct
{
    unsigned int    elements;
    size_t          size;
    VABufferType    bufferType;
    void            *ptr;
    size_t          offset;
    // encode output (VAEncCodedBufferType)
    uint8_t         *codedBuf;
    size_t          codedSize;
    size_t          codedAllocated;
    bool            codedReady;
    VACodedBufferSegment codedSegment;
    struct _NVEncodeContext *encCtx;
    uint8_t         *packedHeader;
    size_t          packedHeaderSize;
    uint8_t         *packedSps;
    size_t          packedSpsSize;
    uint8_t         *packedPps;
    size_t          packedPpsSize;
    bool            packedTimingSeiSeen;
} NVBuffer;

typedef struct
{
    uint32_t                width;
    uint32_t                height;
    cudaVideoSurfaceFormat  format;
    cudaVideoChromaFormat   chromaFormat;
    int                     bitDepth;
    int                     pictureIdx;
    struct _NVContext       *context;
    int                     progressiveFrame;
    int                     topFieldFirst;
    int                     secondField;
    int                     order_hint; //needed for AV1
    struct _BackingImage    *backingImage;
    int                     resolving;
    pthread_mutex_t         mutex;
    pthread_cond_t          cond;
    bool                    decodeFailed;
    // encode input backing
    CUdeviceptr             encDevPtr;
    size_t                  encPitch;
    uint32_t                encAllocHeight;
    struct _NVContext       *encCtx;
    struct _NVEncSurfaceReg *encReg;
    uint64_t                encUploadSeq;
    uint64_t                encLastEncodedSeq;
    CUevent                 encCopyEvent;
    bool                    encCopyEventValid;
    bool                    encCopyEventPending;
    bool                    encDirectUploadLatest;
    bool                    encInputValid;
} NVSurface;

typedef enum
{
    NV_FORMAT_NONE,
    NV_FORMAT_NV12,
    NV_FORMAT_P010,
    NV_FORMAT_P012,
    NV_FORMAT_P016,
    NV_FORMAT_444P,
    NV_FORMAT_Q416,
    NV_FORMAT_COUNT
} NVFormat;

typedef struct
{
    uint32_t    width;
    uint32_t    height;
    NVFormat    format;
    NVBuffer    *imageBuffer;
    uint32_t    pitches[3];
    uint32_t    offsets[3];
} NVImage;

typedef struct {
    CUexternalMemory extMem;
    CUmipmappedArray mipmapArray;
} NVCudaImage;

typedef struct _BackingImage {
    NVSurface   *surface;
    EGLImage    image;
    CUarray     arrays[3];
    CUdeviceptr linearPtr;
    size_t      linearSize;
    void        *hostPtrs[4];
    size_t      hostMapSizes[4];
    bool        hostWritable[4];
    uint32_t    width;
    uint32_t    height;
    uint32_t    visibleWidth;
    uint32_t    visibleHeight;
    uint32_t    numObjects;
    uint32_t    numLayers;
    int         fourcc;
    int         fds[4];
    int         offsets[4];
    int         strides[4];
    uint64_t    mods[4];
    uint32_t    size[4];
    uint32_t    layerDrmFormats[4];
    uint32_t    layerNumPlanes[4];
    uint32_t    planeObjectIndices[4];
    uint32_t    planeLayerIndices[4];
    uint32_t    planeIndicesInLayer[4];
    struct _NVImportedObjectCacheEntry *hostObjects[4];
    bool        importedExternal;
    bool        hostMappedExternal;
    //direct backend only
    NVCudaImage cudaImages[3];
    NVFormat    format;
} BackingImage;

typedef struct _NVImportedObjectCacheEntry {
    uint64_t    device;
    uint64_t    inode;
    uint64_t    modifier;
    uint32_t    size;
    int         fd;
    void        *hostPtr;
    size_t      hostMapSize;
    bool        writable;
    uint32_t    refcount;
    uint64_t    lastUseSeq;
} NVImportedObjectCacheEntry;

struct _NVDriver;

typedef struct {
    const char *name;
    bool (*initExporter)(struct _NVDriver *drv);
    void (*releaseExporter)(struct _NVDriver *drv);
    bool (*exportCudaPtr)(struct _NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch);
    void (*detachBackingImageFromSurface)(struct _NVDriver *drv, NVSurface *surface);
    bool (*realiseSurface)(struct _NVDriver *drv, NVSurface *surface);
    bool (*fillExportDescriptor)(struct _NVDriver *drv, NVSurface *surface, VADRMPRIMESurfaceDescriptor *desc);
    void (*destroyAllBackingImage)(struct _NVDriver *drv);
} NVBackend;

uint32_t nvGetExportLayerDrmFormat(NVFormat format, uint32_t plane_idx, uint32_t fallback_fourcc);

typedef struct _NVDriver
{
    CudaFunctions           *cu;
    CuvidFunctions          *cv;
    NvencFunctions          *nvenc;
    NV_ENCODE_API_FUNCTION_LIST nvencFuncs;
    bool                    nvencAvailable;
    uint32_t                nvencMaxVersion;
    struct {
        bool valid;
        int supportIntraRefresh;
        int supportSingleSliceIntraRefresh;
        int supportDynamicSliceMode;
        int supportEmphasisMap;
        int supportCustomVbvBufSize;
        int supportLookahead;
        int supportTemporalAQ;
        int supportWeightedPrediction;
        int supportMultipleRefFrames;
        int supportBframeRefMode;
        int maxBFrames;
        int widthMax;
        int heightMax;
        int widthMin;
        int heightMin;
    } nvencCaps;
    pthread_mutex_t         nvencCapsMutex;
    CUcontext               cudaContext;
    CUvideoctxlock          vidLock;
    Array/*<Object>*/       objects;
    pthread_mutex_t         objectCreationMutex;
    VAGenericID             nextObjId;
    bool                    useCorrectNV12Format;
    bool                    supports16BitSurface;
    bool                    supports444Surface;
    int                     cudaGpuId;
    int                     drmFd;
    pthread_mutex_t         importedObjectCacheMutex;
    Array                   importedObjectCache;
    uint64_t                importedObjectCacheSeq;
    size_t                  importedObjectCacheBytes;
    pthread_mutex_t         exportMutex;
    pthread_mutex_t         imagesMutex;
    pthread_mutex_t         cudaMutex;
    Array/*<NVEGLImage>*/   images;
    const NVBackend         *backend;
    //fields for direct backend
    NVDriverContext         driverContext;
    //fields for egl backend
    EGLDeviceEXT            eglDevice;
    EGLDisplay              eglDisplay;
    EGLContext              eglContext;
    EGLStreamKHR            eglStream;
    CUeglStreamConnection   cuStreamConnection;
    bool                    eglExporterReady;
    int                     numFramesPresented;
    int                     profileCount;
    VAProfile               profiles[MAX_PROFILES];
} NVDriver;

struct _NVCodec;

typedef struct _NVContext
{
    NVDriver            *drv;
    VAProfile           profile;
    VAEntrypoint        entrypoint;
    uint64_t            ownerTid;
    uint32_t            width;
    uint32_t            height;
    CUvideodecoder      decoder;
    NVSurface           *renderTarget;
    void                *codecData;
    void                *lastSliceParams;
    unsigned int        lastSliceParamsCount;
    AppendableBuffer    bitstreamBuffer;
    AppendableBuffer    sliceOffsets;
    CUVIDPICPARAMS      pPicParams;
    const struct _NVCodec *codec;
    int                 currentPictureId;
    pthread_t           resolveThread;
    pthread_mutex_t     resolveMutex;
    pthread_cond_t      resolveCondition;
    NVSurface*          surfaceQueue[SURFACE_QUEUE_SIZE];
    int                 surfaceQueueReadIdx;
    int                 surfaceQueueWriteIdx;
    volatile bool       exiting;
    pthread_mutex_t     surfaceCreationMutex;
    int                 surfaceCount;
    bool                firstKeyframeValid;
    bool                isEncode;
    struct _NVEncodeContext *enc;
} NVContext;

typedef struct _NVEncSurfaceReg
{
    VASurfaceID         surfaceId;
    NV_ENC_REGISTERED_PTR registered;
    CUdeviceptr         inputPtr;
    size_t              inputPitch;
} NVEncSurfaceReg;

typedef struct _NVEncPendingOutput
{
    NVSurface   *surface;
    NVBuffer    *codedBuf;
    NV_ENC_OUTPUT_PTR bitstream;
    bool        encodeReady;
    bool        resolving;
    bool        detached;
    bool        releaseBitstreamOnCleanup;
    int8_t      *qpDeltaMap;
    uint32_t    qpDeltaMapSize;
} NVEncPendingOutput;

typedef struct _NVEncReorderEntry
{
    uint64_t    displayTs;
    NVBuffer    *codedBuf;
    uint32_t    reorderGroupId;
    bool        is_b;
} NVEncReorderEntry;

typedef struct _NVEncQueuedPic
{
    NVSurface   *surface;
    NVBuffer    *codedBuf;
    VASurfaceID surfaceId;
    uint32_t    displayPoc;
    uint64_t    displayTs;
    uint8_t     sliceType;
    bool        forceIdr;
    bool        refPicFlag;
    int8_t      *qpDeltaMap;
    uint32_t    qpDeltaMapSize;
    NV_ENC_QP_MAP_MODE qpMapMode;
    NV_ENC_SEI_PAYLOAD *seiPayloads;
    uint32_t    seiPayloadCount;
} NVEncQueuedPic;

typedef struct _NVEncSeqSignature
{
    uint32_t    intra_period;
    uint32_t    intra_idr_period;
    uint32_t    ip_period;
    uint32_t    bits_per_second;
    uint32_t    max_num_ref_frames;
    uint16_t    picture_width_in_mbs;
    uint16_t    picture_height_in_mbs;
    uint32_t    seq_fields_value;
    uint8_t     level_idc;
    uint8_t     bit_depth_luma_minus8;
    uint8_t     bit_depth_chroma_minus8;
    uint8_t     frame_cropping_flag;
    uint32_t    frame_crop_left_offset;
    uint32_t    frame_crop_right_offset;
    uint32_t    frame_crop_top_offset;
    uint32_t    frame_crop_bottom_offset;
} NVEncSeqSignature;

typedef struct _NVEncodeContext
{
    /* Driver and session identity */
    NVDriver            *drv;
    struct _NVContext   *context;
    VAProfile           profile;
    VAEntrypoint        entrypoint;
    uint32_t            width;
    uint32_t            height;
    void                *encoder;
    NV_ENCODE_API_FUNCTION_LIST *funcs;

    /* Runtime config toggles and per-picture presence flags */
    uint32_t            rcMode;
    bool                enablePTD;
    bool                initialized;
    bool                reconfigure;
    bool                haveSeq;
    bool                havePic;
    bool                haveSlice;
    bool                haveRc;
    bool                haveHrd;
    bool                haveFr;
    bool                haveQuality;
    bool                forceConservativeInit;
    bool                haveRir;
    bool                haveQpI;
    bool                haveQpP;
    bool                haveQpB;
    uint32_t            qpI;
    uint32_t            qpP;
    uint32_t            qpB;

    /* Codec-specific applied VA state and derived H.264 policy state */
    bool                haveSeqSignature;
    NVEncSeqSignature   seqSignature;
    bool                rirTriggered;
    VAEncSequenceParameterBufferH264 seqParams;
    VAEncPictureParameterBufferH264  picParams;
    VAEncSliceParameterBufferH264    sliceParams;
    VAEncMiscParameterRateControl    rcParams;
    VAEncMiscParameterHRD            hrdParams;
    VAEncMiscParameterFrameRate      frParams;
    VAEncMiscParameterBufferQualityLevel qualityParams;
    VAEncMiscParameterRIR            rirParams;
    int8_t              *roiMap;
    uint32_t            roiMapSize;
    NV_ENC_QP_MAP_MODE  roiMapMode;
    bool                roiMapValid;
    AppendableBuffer    packedHeaderBuf;
    AppendableBuffer    packedSpsBuf;
    AppendableBuffer    packedPpsBuf;
    NVH264SpsLite       rewriteNativeH264Sps;
    NVH264PpsLite       rewriteNativeH264Pps;
    NV_ENC_SEI_PAYLOAD *packedSeiPayloads;
    uint32_t            packedSeiPayloadCount;
    uint32_t            packedSeiStreamMask;
    bool                packedTimingSeiSeen;
    bool                havePackedHeaderParam;
    VAEncPackedHeaderParameterBuffer packedHeaderParam;
    uint32_t            sliceCount;
    uint32_t            sliceRows;
    bool                sliceRowsValid;
    uint32_t            sliceModeConfigured;
    uint32_t            sliceModeDataConfigured;
    bool                sliceModeConfiguredValid;

    /* Current picture/runtime mutable state */
    VASurfaceID         inputSurface;
    VABufferID          codedBufId;
    uint32_t            frameIdx;
    uint64_t            displayTsCounter;
    bool                recoveryIdrPending;
    uint32_t            framesSinceLastReconfigure;
    uint32_t            reconfigureReason;
    uint32_t            reconfigureBpsHint;

    /* Encode-owned resources and output queues */
    Array               registeredSurfaces;
    Array               pendingOutputs;
    Array               reorderEncode;
    Array               bitstreamPool;
    pthread_mutex_t     bitstreamPoolMutex;
    uint32_t            bitstreamPoolMax;
    Array               queuedPics;

    /* Sync and queue coordination */
    pthread_mutex_t     reorderMutex;
    pthread_cond_t      reorderCond;
    pthread_mutex_t     queueMutex;
    bool                haveNextDisplayPoc;
    uint32_t            nextDisplayPoc;
    bool                havePrevPoc;
    uint32_t            prevPocLsb;
    uint32_t            pocMsb;

    /* H.264 bitrate patch and SEI patch bookkeeping */
    bool                patchBpSei;
    bool                bpHaveParams;
    bool                bpHavePrev;
    uint8_t             bpDelayLen;
    uint8_t             bpCpbCntMinus1;
    uint32_t            bpPrevDelay;
    uint32_t            bpCpbTicks;
    uint64_t            bpBitsSince;
    uint32_t            bpSeen;

    /* Last applied NVENC snapshot for reconfigure decisions */
    NV_ENC_INITIALIZE_PARAMS initParams;
    NV_ENC_CONFIG        encConfig;
    bool                haveAppliedReconfigSnapshot;
    NV_ENC_CONFIG       appliedEncConfig;
    uint32_t            appliedFrameRateNum;
    uint32_t            appliedFrameRateDen;
    GUID                appliedPresetGUID;
    NV_ENC_TUNING_INFO  appliedTuningInfo;
} NVEncodeContext;

typedef struct
{
    VAProfile               profile;
    VAEntrypoint            entrypoint;
    cudaVideoSurfaceFormat  surfaceFormat;
    cudaVideoChromaFormat   chromaFormat;
    int                     bitDepth;
    cudaVideoCodec          cudaCodec;
    uint32_t                rateControl;
} NVConfig;

typedef void (*HandlerFunc)(NVContext*, NVBuffer* , CUVIDPICPARAMS*);
typedef cudaVideoCodec (*ComputeCudaCodec)(VAProfile);
typedef void (*CodecBeginPictureFunc)(NVContext*);

//padding/alignment is very important to this structure as it's placed in it's own section
//in the executable.
struct _NVCodec {
    ComputeCudaCodec    computeCudaCodec;
    HandlerFunc         handlers[VABufferTypeMax];
    int                 supportedProfileCount;
    const VAProfile     *supportedProfiles;
    CodecBeginPictureFunc beginPicture;
};

typedef struct _NVCodec NVCodec;

typedef struct
{
    uint32_t bppc; // bytes per pixel per channel
    uint32_t numPlanes;
    uint32_t fourcc;
    bool     is16bits;
    bool     isYuv444;
    NVFormatPlane plane[3];
    VAImageFormat vaFormat;
} NVFormatInfo;

extern const NVFormatInfo formatsInfo[NV_FORMAT_COUNT];
extern CudaFunctions *cu;

static inline uint32_t nv_div_round_up_u32(uint32_t value, uint32_t divisor)
{
    return divisor ? (value + divisor - 1u) / divisor : 0;
}

static inline uint32_t nv_format_plane_width(const NVFormatInfo *fmtInfo,
                                             uint32_t plane_idx,
                                             uint32_t width)
{
    const NVFormatPlane *plane = &fmtInfo->plane[plane_idx];
    return nv_div_round_up_u32(width, 1u << plane->ss.x);
}

static inline uint32_t nv_format_plane_height(const NVFormatInfo *fmtInfo,
                                              uint32_t plane_idx,
                                              uint32_t height)
{
    const NVFormatPlane *plane = &fmtInfo->plane[plane_idx];
    return nv_div_round_up_u32(height, 1u << plane->ss.y);
}

static inline size_t nv_format_plane_row_bytes(const NVFormatInfo *fmtInfo,
                                               uint32_t plane_idx,
                                               uint32_t width)
{
    const NVFormatPlane *plane = &fmtInfo->plane[plane_idx];
    return (size_t)nv_format_plane_width(fmtInfo, plane_idx, width) *
           fmtInfo->bppc *
           plane->channelCount;
}

static inline size_t nv_format_plane_size(const NVFormatInfo *fmtInfo,
                                          uint32_t plane_idx,
                                          uint32_t width,
                                          uint32_t height)
{
    return nv_format_plane_row_bytes(fmtInfo, plane_idx, width) *
           (size_t)nv_format_plane_height(fmtInfo, plane_idx, height);
}

void appendBuffer(AppendableBuffer *ab, const void *buf, uint64_t size);
int pictureIdxFromSurfaceId(NVDriver *ctx, VASurfaceID surf);
NVSurface* nvSurfaceFromSurfaceId(NVDriver *drv, VASurfaceID surf);
bool checkCudaErrors(CUresult err, const char *file, const char *function, const int line);
void logger(const char *filename, const char *function, int line, const char *msg, ...);
#define CHECK_CUDA_RESULT(err) checkCudaErrors(err, __FILE__, __func__, __LINE__)
#define CHECK_CUDA_RESULT_RETURN(err, ret) if (checkCudaErrors(err, __FILE__, __func__, __LINE__)) { return ret; }
#define cudaVideoCodec_NONE ((cudaVideoCodec) -1)
#define LOG(...) logger(__FILE__, __func__, __LINE__, __VA_ARGS__);
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PTROFF(base, bytes) ((void *)((unsigned char *)(base) + (bytes)))
#define DECLARE_CODEC(name) \
    __attribute__((used)) \
    __attribute__((retain)) \
    __attribute__((section("nvd_codecs"))) \
    __attribute__((aligned(__alignof__(NVCodec)))) \
    NVCodec name

#define DECLARE_DISABLED_CODEC(name) \
    __attribute__((section("nvd_disabled_codecs"))) \
    __attribute__((aligned(__alignof__(NVCodec)))) \
    NVCodec name

#endif // VABACKEND_H
