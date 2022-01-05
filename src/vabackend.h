#ifndef VABACKEND_H
#define VABACKEND_H

#include "cuviddec.h"
#include <va/va_backend.h>
#include <EGL/egl.h>
#include <cudaEGL.h>

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
} NVSurface;

typedef struct
{
    int         width;
    int         height;
    uint32_t    format;
    NVBuffer    *imageBuffer;
} NVImage;

typedef struct
{
    CUcontext               cudaContext;
    Object                  objRoot;
    VAGenericID             nextObjId;
    EGLDisplay              eglDisplay;
    EGLContext              eglContext;
    EGLStreamKHR            eglStream;
    CUeglStreamConnection   cuStreamConnection;
    int                     numFramesPresented;
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
} NVContext;

typedef struct
{
    VAProfile               profile;
    VAEntrypoint            entrypoint;
    VAConfigAttrib          *attributes;
    int                     numAttribs;
    cudaVideoSurfaceFormat  surfaceFormat;
    cudaVideoChromaFormat   chromaFormat;
    int                     bitDepth;
    cudaVideoCodec          cudaCodec;
} NVConfig;

typedef void (*HandlerFunc)(NVContext*, NVBuffer* , CUVIDPICPARAMS*);
typedef cudaVideoCodec (*ComputeCudaCodec)(VAProfile);

typedef struct _NVCodec
{
    ComputeCudaCodec    computeCudaCodec;
    HandlerFunc         handlers[VABufferTypeMax];
    int                 supportedProfileCount;
    const VAProfile     *supportedProfiles;
} NVCodec;

void appendBuffer(AppendableBuffer *ab, const void *buf, uint64_t size);
int pictureIdxFromSurfaceId(NVDriver *ctx, VASurfaceID surf);
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
