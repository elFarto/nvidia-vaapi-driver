#ifndef VABACKEND_H
#define VABACKEND_H

#include <ffnvcodec/dynlink_loader.h>
#include <va/va_backend.h>
#include <va/va_vpp.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>

#include <stdio.h>
#include <pthread.h>
#include "list.h"
#include "direct/nv-driver.h"
#include "common.h"
#include "stats.h"

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
    uint64_t                pictureIdxLastUsed; // drv->pictureIdxUseCounter value when last decoded into or referenced
    VAContextID             contextId; // last context to use this target; remains valid as an ID after destruction
    int                     progressiveFrame;
    int                     topFieldFirst;
    int                     secondField;
    int                     order_hint; //needed for AV1
    uint32_t                av1FrameWidth;  //AV1 frame size last decoded into this surface,
    uint32_t                av1FrameHeight; //which can be smaller than the surface
    VAProcColorStandardType colorStandard;
    bool                    colorRangeFull;
    struct _BackingImage    *backingImage;
    int                     resolving;
    // Number of in-flight vaRenderPicture() blits currently reading this
    // surface as a source or writing it as the render target. vaDestroySurfaces()
    // waits for this to drain before detaching the backing image, so a client
    // that destroys a surface while a VideoProc blit is still using it cannot
    // pull the memory out from under the copy.
    atomic_uint             videoProcReads;
    // Protected by drv->objectCreationMutex. Once set, VideoProc calls may
    // no longer take a new read reference to this surface.
    bool                    destroying;
    int                     fourcc;
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
    dev_t       st_dev[4];
    ino_t       st_ino[4];
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
    VAProcColorStandardType colorStandard;
    bool        colorRangeFull;
    uint32_t    totalSize;
    CUexternalMemory extMem;
    bool        isSingleBuffer;
    bool        isExternalBuffer;
    bool        borrowedCudaResources;
    struct _BackingImage *borrowedBackingImage;
    atomic_uint borrowCount;
    bool        syncInitialized;
    bool        resolving;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    void        *externalMapping;
    uint32_t    externalMappingSize;
    CUdeviceptr externalDevicePtr;
    uint32_t    externalDeviceSize;
    uint64_t    detachedSerial;
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
    uint64_t                pictureIdxUseCounter; // updated atomically
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
    int                     decodeProfileCount;
    VAProfile               decodeProfiles[MAX_PROFILES];
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
    CUmodule                videoProcModule;
    CUfunction              nv12ToArgbKernel;
    CUfunction              p010ToArgbKernel;
    CUmodule                videoProcModuleP010;
    bool                    videoProcKernelP010Failed;
    bool                    videoProcKernelFailed;
    CUdeviceptr             videoProcYBuffer;
    CUdeviceptr             videoProcUVBuffer;
    CUdeviceptr             videoProcArgbBuffer;
    size_t                  videoProcYBufferSize;
    size_t                  videoProcUVBufferSize;
    size_t                  videoProcArgbBufferSize;
    void                    *cpuVideoProcYBuffer;
    void                    *cpuVideoProcUVBuffer;
    void                    *cpuVideoProcArgbBuffer;
    size_t                  cpuVideoProcYBufferSize;
    size_t                  cpuVideoProcUVBufferSize;
    size_t                  cpuVideoProcArgbBufferSize;
    bool                    statsEnabled;
    uint64_t                statsLogInterval;
    atomic_uint_fast64_t    stats[NV_STAT_COUNT];
    uint64_t                maxDetachedBackingImageBytes;
    uint32_t                maxDetachedBackingImages;
    uint64_t                detachedBackingImageSerial;
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
    NVSurface           *displayTarget;
    void                *codecData;
    void                *lastSliceParams;
    unsigned int        lastSliceParamsCount;
    AppendableBuffer    bitstreamBuffer;
    AppendableBuffer    sliceOffsets;
    bool                av1SequenceEnableRestoration;
    uint32_t            av1TileOffsetsSeen;
    uint32_t            av1TileMinOffset;
    uint32_t            av1TileMaxEnd;
    bool                av1BitstreamCompacted;
    /* AV1 frames may be coded smaller than the sequence maximum the context was
     * created with (frame_size_override_flag). NVDEC scales the decoded frame
     * to fill the decoder's display area, so the display area has to follow
     * the frame size: requested is what the current picture needs, applied is
     * what the decoder was last configured with (0 = the context size). */
    uint32_t            requestedDisplayWidth;
    uint32_t            requestedDisplayHeight;
    uint32_t            appliedDisplayWidth;
    uint32_t            appliedDisplayHeight;
    bool                decoderHasDecoded;
    NVSurface           *lastQueuedSurface; //most recent surface handed to the resolve thread
    CUVIDPICPARAMS      pPicParams;
    const struct _NVCodec *codec;
    cudaVideoCodec      cudaCodec;
    cudaVideoSurfaceFormat decoderSurfaceFormat;
    cudaVideoChromaFormat decoderChromaFormat;
    int                 decoderBitDepth;
    // NVDEC can address at most 32 decode surfaces, but VA-API lets a client
    // render into as many surfaces as it likes. pictureIdxOwners maps each
    // index to the surface currently holding it.
    NVSurface          *pictureIdxOwners[32]; // protected by drv->objectCreationMutex
    bool                pictureIdxAssigned; // an index has been handed out, so the decoder's format is fixed
    pthread_t           resolveThread;
    bool                resolveThreadStarted;
    bool                resolveThreadFailed; // protected by resolveMutex
    pthread_mutex_t     resolveMutex;
    pthread_cond_t      resolveCondition;
    pthread_cond_t      videoProcCondition; // protected by drv->objectCreationMutex
    unsigned int        activeVideoProcCalls;
    unsigned int        activeVideoProcRenders;
    bool                videoProcDestroying;
    unsigned int        activeDecodeCalls; // protected by drv->objectCreationMutex
    bool                decodeDestroying;
    NVSurface**         surfaceQueue; // protected by resolveMutex
    size_t              surfaceQueueCapacity;
    size_t              surfaceQueueReadIdx;
    size_t              surfaceQueueWriteIdx;
    volatile bool       exiting;
    pthread_mutex_t     surfaceCreationMutex;
    int                 surfaceCount;
    bool                firstKeyframeValid;
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
typedef enum {
    NV_PICTURE_BEGIN,
    NV_PICTURE_RENDER,
    NV_PICTURE_END,
} NVPictureOperation;
typedef void (*CodecBeginPictureFunc)(NVContext*, VASurfaceID);

// Internals exposed for the stats subsystem (src/stats.c).
pid_t nv_gettid(void);
FILE *nvStatsOutput(void);

//padding/alignment is very important to this structure as it's placed in it's own section
//in the executable.
struct _NVCodec {
    ComputeCudaCodec    computeCudaCodec;
    HandlerFunc         handlers[VABufferTypeMax];
    int                 supportedProfileCount;
    const VAProfile     *supportedProfiles;
    CodecBeginPictureFunc beginPicture;
    // Optional lifecycle hooks. checkPicture and surfaceDestroyed run with
    // objectCreationMutex held; the other hooks run during an active decode call.
    // checkPicture may reject a call before the backend changes its target.
    VAStatus (*checkPicture)(NVContext*, NVPictureOperation);
    // Override buffer dispatch when a codec needs submission error reporting.
    VAStatus (*renderPicture)(NVContext*, VABufferID*, int);
    // Validate the assembled picture before CUDA submission; release any
    // codec-owned pending output on failure before returning the error.
    VAStatus (*prepareDecode)(NVContext*);
    // Called after decode with default field order set. May adjust metadata;
    // return false to defer resolution or release an incomplete failed output.
    bool (*finishDecode)(NVContext*, VAStatus);
    // Release codec-owned pending output when CUDA context push/pop fails.
    void (*abortPicture)(NVContext*);
    void (*surfaceDestroyed)(NVContext*, VASurfaceID, NVSurface*);
};

typedef struct _NVCodec NVCodec;

// Codec helpers. Surface lookup must stay under objectCreationMutex while the
// returned pointer is used; buffer lookup has the same lifetime as RenderPicture.
NVSurface *nvGetSurface(NVDriver *drv, VASurfaceID id);
NVBuffer *nvGetBuffer(NVDriver *drv, VABufferID id);
void nvSetSurfaceResolving(NVSurface *surface, bool resolving);

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
const char *nvColorStandardName(VAProcColorStandardType colorStandard);
VAProcColorStandardType nvColorStandardFromMatrixCoefficients(uint8_t matrixCoefficients);
void nvSurfaceResetColorMetadata(NVSurface *surface);
void nvSurfaceSetColorMetadata(NVSurface *surface, VAProcColorStandardType colorStandard, bool colorRangeFull);
void nvSurfaceCopyColorMetadata(NVSurface *dst, const NVSurface *src);
void nvSurfaceCopyColorMetadataFromBackingImage(NVSurface *surface, const BackingImage *img);
void nvBackingImageStoreSurfaceColorMetadata(BackingImage *img, const NVSurface *surface);
void nvBackingImageCopyColorMetadata(BackingImage *dst, const BackingImage *src);
bool checkCudaErrors(CUresult err, const char *file, const char *function, const int line);
void logger(const char *filename, const char *function, int line, const char *msg, ...);
bool nvdLogDebugEnabled(void);
bool nvdSingleBufferForced(void);
#define CHECK_CUDA_RESULT(err) checkCudaErrors(err, __FILE__, __func__, __LINE__)
#define CHECK_CUDA_RESULT_RETURN(err, ret) if (checkCudaErrors(err, __FILE__, __func__, __LINE__)) { return ret; }
#define cudaVideoCodec_NONE ((cudaVideoCodec) -1)
#define LOG(...) logger(__FILE__, __func__, __LINE__, __VA_ARGS__);
#define LOG_DEBUG(...) do { if (nvdLogDebugEnabled()) { logger(__FILE__, __func__, __LINE__, __VA_ARGS__); } } while (0)
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
