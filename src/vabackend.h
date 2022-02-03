#ifndef VABACKEND_H
#define VABACKEND_H

#include <ffnvcodec/dynlink_loader.h>
#include <va/va_backend.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdbool.h>

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
    struct Object_t *next;
} *Object;

typedef struct
{
    int             elements;
    int             size;
    VABufferType    bufferType;
    void            *ptr;
    int             offset;
} NVBuffer;

typedef struct
{
    int                     width;
    int                     height;
    cudaVideoSurfaceFormat  format;
    int                     bitDepth;
    int                     pictureIdx;
    VAContextID             contextId;
    int                     progressiveFrame;
    int                     topFieldFirst;
    int                     secondField;
    CUarray                 cuImages[2];
    EGLImage                eglImage;
    int                     fourcc;
    int                     fds[4];
    int                     offsets[4];
    int                     strides[4];
    uint64_t                mods[4];
    int                     order_hint; //needed for av1?
} NVSurface;

typedef struct
{
    int         width;
    int         height;
    uint32_t    format;
    NVBuffer    *imageBuffer;
} NVImage;

typedef struct _NVEGLImage {
    EGLImage image;
    struct _NVEGLImage *next;
} NVEGLImage;

typedef struct
{
    CudaFunctions           *cu;
    CuvidFunctions          *cv;
    CUcontext               cudaContext;
    Object                  objRoot;
    VAGenericID             nextObjId;
    EGLDisplay              eglDisplay;
    EGLContext              eglContext;
    EGLStreamKHR            eglStream;
    CUeglStreamConnection   cuStreamConnection;
    int                     numFramesPresented;
    bool                    useCorrectNV12Format;
    bool                    supports16BitSurface;
    NVEGLImage              *allocatedEGLImages;
    int                     surfaceCount;
} NVDriver;

struct _NVCodec;

typedef struct
{
    NVDriver            *drv;
    VAProfile           profile;
    VAEntrypoint        entrypoint;
    int                 width;
    int                 height;
    CUvideodecoder      decoder;
    NVSurface           *renderTargets;
    void                *lastSliceParams;
    unsigned int        lastSliceParamsCount;
    AppendableBuffer    buf;
    AppendableBuffer    sliceOffsets;
    CUVIDPICPARAMS      pPicParams;
    const struct _NVCodec *codec;
    int                 currentPictureId;
} NVContext;

typedef struct
{
    VAProfile               profile;
    VAEntrypoint            entrypoint;
    cudaVideoSurfaceFormat  surfaceFormat;
    cudaVideoChromaFormat   chromaFormat;
    int                     bitDepth;
    cudaVideoCodec          cudaCodec;
} NVConfig;

typedef void (*HandlerFunc)(NVContext*, NVBuffer* , CUVIDPICPARAMS*);
typedef cudaVideoCodec (*ComputeCudaCodec)(VAProfile);

//padding/alignment is very important to this structure as it's placed in it's own section
//in the executable.
typedef struct _NVCodec
{
    ComputeCudaCodec    computeCudaCodec;
    HandlerFunc         handlers[VABufferTypeMax];
    int                 supportedProfileCount;
    const VAProfile     *supportedProfiles;
} NVCodec;

void appendBuffer(AppendableBuffer *ab, const void *buf, uint64_t size);
int pictureIdxFromSurfaceId(NVDriver *ctx, VASurfaceID surf);
NVSurface* nvSurfaceFromSurfaceId(NVDriver *drv, VASurfaceID surf);
void checkCudaErrors(CUresult err, const char *file, const char *function, const int line);
void logger(const char *filename, const char *function, int line, const char *msg, ...);
#define CHECK_CUDA_RESULT(err) checkCudaErrors(err, __FILE__, __func__, __LINE__)
#define cudaVideoCodec_NONE ((cudaVideoCodec) -1)
#define LOG(...) logger(__FILE__, __func__, __LINE__, __VA_ARGS__);
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PTROFF(base, bytes) ((void *)((unsigned char *)(base) + (bytes)))
#define DECLARE_CODEC(name) \
    __attribute__((section("nvd_codecs"), used)) \
    NVCodec name

#endif // VABACKEND_H
