#ifndef VABACKEND_H
#define VABACKEND_H

#include <ffnvcodec/dynlink_loader.h>
#include <va/va_backend.h>
#include <va/va_vpp.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdbool.h>
#include <va/va_drmcommon.h>

#include <pthread.h>
#include "list.h"
#include "direct/nv-driver.h"
#include "common.h"

#ifndef VA_FOURCC_Q416
#define VA_FOURCC_Q416 0x36313451
#endif

// libva <= 2.14 lacks this enum name; value matches newer libva headers.
#define NV_VA_PROFILE_H264_HIGH10 ((VAProfile)36)

#define SURFACE_QUEUE_SIZE 16
#define MAX_IMAGE_COUNT 64
#define MAX_PROFILES 32

typedef struct {
    void        *buf;
    uint64_t    size;
    uint64_t    allocated;
} AppendableBuffer;

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
    void            *codedData;
    size_t          codedDataAllocated;
    VACodedBufferSegment codedSegment;
} NVBuffer;

struct _NVContext;
struct _BackingImage;

typedef struct
{
    uint32_t                width;
    uint32_t                height;
    cudaVideoSurfaceFormat  format;
    cudaVideoChromaFormat   chromaFormat;
    uint32_t                rtFormat;
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
} NVSurface;

typedef enum
{
    NV_FORMAT_NONE,
    NV_FORMAT_NV12,
    NV_FORMAT_NV16,
    NV_FORMAT_P010,
    NV_FORMAT_P210,
    NV_FORMAT_P012,
    NV_FORMAT_P016,
    NV_FORMAT_ARGB,
    NV_FORMAT_444P,
    NV_FORMAT_Q416
} NVFormat;

typedef struct
{
    uint32_t    width;
    uint32_t    height;
    NVFormat    format;
    NVBuffer    *imageBuffer;
} NVImage;

typedef struct {
    CUexternalMemory extMem;
    CUmipmappedArray mipmapArray;
    CUdeviceptr mappedBuffer;
    uint64_t mappedBufferSize;
    int importedFd;
} NVCudaImage;

typedef struct _BackingImage {
    NVSurface   *surface;
    EGLImage    image;
    CUarray     arrays[3];
    uint32_t    width;
    uint32_t    height;
    int         fourcc;
    int         fds[4];
    int         offsets[4];
    int         strides[4];
    uint64_t    mods[4];
    uint32_t    size[4];
    //direct backend only
    NVCudaImage cudaImages[3];
    NVCudaImage directEncodeWholeFrameCudaImage;
    CUarray     directEncodeWholeFrameArray;
    uint32_t    directEncodeWholeFramePitch;
    uint32_t    directEncodeWholeFrameRows;
    CUarray     directEncodeTightCudaArray;
    uint32_t    directEncodeTightCudaArrayPitch;
    uint32_t    directEncodeTightCudaArrayRows;
    CUdeviceptr directEncodeTightCudaBuffer;
    uint64_t    directEncodeTightCudaBufferSize;
    uint32_t    directEncodeTightCudaBufferPitch;
    uint32_t    directEncodeTightCudaBufferRows;
    NVFormat    format;
    uint64_t    resourceEpoch;
    bool        importedGpuCopy;
} BackingImage;

struct _NVDriver;

typedef struct {
    const char *name;
    bool (*initExporter)(struct _NVDriver *drv);
    void (*releaseExporter)(struct _NVDriver *drv);
    bool (*exportCudaPtr)(struct _NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch);
    bool (*importExternalSurface)(struct _NVDriver *drv, NVSurface *surface, const VADRMPRIMESurfaceDescriptor *desc);
    void (*detachBackingImageFromSurface)(struct _NVDriver *drv, NVSurface *surface);
    bool (*realiseSurface)(struct _NVDriver *drv, NVSurface *surface);
    bool (*fillExportDescriptor)(struct _NVDriver *drv, NVSurface *surface, VADRMPRIMESurfaceDescriptor *desc);
    void (*destroyAllBackingImage)(struct _NVDriver *drv);
} NVBackend;

bool directEnsureWholeFrameNv12CudaArray(struct _NVDriver *drv, BackingImage *img);
bool directEnsureWholeFrameNv12CudaBuffer(struct _NVDriver *drv, BackingImage *img);

typedef struct _NVDriver
{
    CudaFunctions           *cu;
    CuvidFunctions          *cv;
    NvencFunctions          *nv;
    CUcontext               cudaContext;
    bool                    usesPrimaryCudaContext;
    CUvideoctxlock          vidLock;
    Array/*<Object>*/       objects;
    pthread_mutex_t         objectCreationMutex;
    VAGenericID             nextObjId;
    bool                    useCorrectNV12Format;
    bool                    allowDirectDmabufCudaImport;
    bool                    preferExternalImportGpuCopy;
    bool                    forceGpuCopyRawDmabuf;
    bool                    forceGpuCopyNvKms;
    bool                    allowDmabufExportIoctl;
    bool                    supports16BitSurface;
    bool                    supports444Surface;
    int                     cudaGpuId;
    int                     drmFd;
    pthread_mutex_t         exportMutex;
    pthread_mutex_t         imagesMutex;
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
    int                     numFramesPresented;
    int                     profileCount;
    VAProfile               profiles[MAX_PROFILES];
    bool                    supportsEncodeH264;
    bool                    supportsEncodeH26410Bit;
    bool                    supportsEncodeH264444;
    bool                    supportsEncodeHEVC;
    bool                    supportsEncodeAV1;
    bool                    supportsEncodeAV110Bit;
    bool                    supportsEncodeHEVC10Bit;
    bool                    supportsEncodeHEVC422;
    bool                    supportsEncodeHEVC444;
    uint64_t                importSurfaceAttemptCount;
    uint64_t                importSurfaceSuccessCount;
    uint64_t                importSurfaceFailCount;
    uint64_t                importSurfaceLiveCount;
    uint64_t                importSurfacePeakLiveCount;
    uint64_t                importSurfaceLiveBytes;
    uint64_t                importSurfacePeakLiveBytes;
    uint32_t                importLastMemType;
    uint32_t                importLastSuccessWidth;
    uint32_t                importLastSuccessHeight;
    uint32_t                importLastFailWidth;
    uint32_t                importLastFailHeight;
    VAStatus                importLastFailStatus;
} NVDriver;

bool directResourcesIdle(void);
void directAdvanceResourceEpoch(void);
void directPurgeOldImportGpuCopyBackings(struct _NVDriver *drv);
void directReleaseOldImportGpuCopyBackingCudaViews(struct _NVDriver *drv);
void eglResetProcessTransientState(void);

struct _NVCodec;

typedef struct _NVContext
{
    NVDriver            *drv;
    VAGenericID         contextId;
    VAProfile           profile;
    VAEntrypoint        entrypoint;
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
    bool                resolveThreadStarted;
    bool                encodeSessionInitialized;
    void                *encoder;
    NV_ENCODE_API_FUNCTION_LIST encodeApi;
    uint32_t            encodeNvencApiVersion;
    CUdeviceptr         encodeInputBuffer;
    size_t              encodeInputBytes;
    size_t              encodeInputPitch;
    long long           encodeVisibleNetDeltaUsed;
    long long           encodeVisibleCumulativeDeltaUsed;
    long long           encodeVisiblePeakCumulativeDeltaUsed;
    uint32_t            encodeVisibleDeltaSamples;
    NV_ENC_REGISTERED_PTR encodeRegisteredInput;
    NV_ENC_OUTPUT_PTR   encodeBitstream;
    NV_ENC_BUFFER_FORMAT encodeInputFormat;
    uint64_t            encodeFrameIdx;
    VABufferID          encodeCodedBuffer;
    uint32_t            encodeBitrate;
    uint32_t            encodeFrameRateNum;
    uint32_t            encodeFrameRateDen;
    uint32_t            encodeIntraPeriod;
    uint32_t            encodeIntraIDRPeriod;
    uint32_t            encodeIPPeriod;
    uint32_t            encodeRateControl;
    uint32_t            encodeTargetPercentage;
    uint32_t            encodeInitialQp;
    uint32_t            encodeMinQp;
    uint32_t            encodeMaxQp;
    uint32_t            encodePicInitQp;
    bool                encodeInitialQpFromMisc;
    bool                encodeForceIDR;
    AppendableBuffer    encodePackedHeaders;
    uint32_t            encodePackedHeaderType;
    uint32_t            encodePackedHeaderBitLength;
    bool                encodePackedHeaderHasEmulationBytes;
    bool                encodePackedHeaderHasSequence;
    bool                encodePackedHeaderHasPicture;
    bool                vppPipelineSet;
    bool                vppSurfaceRegionSet;
    bool                vppOutputRegionSet;
    VARectangle         vppSurfaceRegion;
    VARectangle         vppOutputRegion;
    VAProcPipelineParameterBuffer vppPipeline;
} NVContext;

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

extern const NVFormatInfo formatsInfo[];

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
