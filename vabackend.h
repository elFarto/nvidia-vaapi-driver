#ifndef VABACKEND_H
#define VABACKEND_H

#include <cuviddec.h>
#include <va/va_backend.h>
#include <EGL/egl.h>
#include <cudaEGL.h>

typedef struct {
  void *buf;
  uint64_t size;
  uint64_t allocated;
} AppendableBuffer;

enum ObjectType_e
{
    OBJECT_TYPE_CONFIG,
    OBJECT_TYPE_CONTEXT,
    OBJECT_TYPE_SURFACE,
    OBJECT_TYPE_BUFFER,
    OBJECT_TYPE_IMAGE
};

typedef enum ObjectType_e ObjectType;

struct Object_t
{
    ObjectType type;
    VAGenericID id;
    void *obj;
    struct Object_t *next;
};

typedef struct Object_t* Object;

typedef struct
{
    int elements;
    int size;
    VABufferType bufferType;
    void *ptr;
} NVBuffer;


typedef struct
{
    int width;
    int height;
    cudaVideoSurfaceFormat format;
    int bitdepth;
    int pictureIdx;
    CUvideodecoder decoder;
    int progressive_frame;
    int top_field_first;
    int second_field;
} NVSurface;

typedef struct
{
    int width;
    int height;
    uint32_t format;
    NVBuffer *imageBuffer;
} NVImage;

struct _NVContext;

typedef struct
{
    CUcontext          g_oContext;
    CUdevice           g_oDevice;
    Object             objRoot;
    VAGenericID        nextObjId;
    EGLDisplay         eglDisplay;
    EGLStreamKHR       eglStream;
    CUeglStreamConnection cuStreamConnection;
} NVDriver;

struct _NVCodec;

typedef struct
{
    NVDriver *drv;
    VAProfile profile;
    VAEntrypoint entrypoint;
    int width;
    int height;
    CUvideodecoder decoder;
    NVSurface *render_target;
    void *last_slice_params;
    unsigned int last_slice_params_count;
    AppendableBuffer buf;
    AppendableBuffer slice_offsets;
    CUVIDPICPARAMS pPicParams;
    struct _NVCodec *codec;
} NVContext;

typedef struct
{
    VAProfile profile;
    VAEntrypoint entrypoint;
    VAConfigAttrib *attrib_list;
    int num_attribs;
    cudaVideoSurfaceFormat surfaceFormat;
    cudaVideoChromaFormat chromaFormat;
    int bitDepth;
    cudaVideoCodec cudaCodec;
} NVConfig;

typedef void (*HandlerFunc)(NVContext*, NVBuffer* , CUVIDPICPARAMS*);
typedef cudaVideoCodec (*ComputeCudaCodec)(VAProfile);

typedef struct _NVCodec
{
    ComputeCudaCodec computeCudaCodec;
    HandlerFunc handlers[VABufferTypeMax];
    int supportedProfileCount;
    VAProfile supportedProfiles[];
} NVCodec;

typedef struct _NVCodecHolder
{
    NVCodec *codec;
    struct _NVCodecHolder *next;
} NVCodecHolder;


void appendBuffer(AppendableBuffer *ab, void *buf, uint64_t size);
int pictureIdxFromSurfaceId(NVDriver *ctx, VASurfaceID surf);
void registerCodec(NVCodec *codec);
void __checkCudaErrors(CUresult err, const char *file, const int line);
#define checkCudaErrors(err)  __checkCudaErrors(err, __FILE__, __LINE__)
#define cudaVideoCodec_NONE ((cudaVideoCodec) -1)
#define LOG(msg, ...) printf(__FILE__ ":%4d %24s " msg, __LINE__, __FUNCTION__ __VA_OPT__(,) __VA_ARGS__);
#define DEFINE_CODEC(c) __attribute__((constructor)) void reg_ ## c() { registerCodec(&c); }



#endif // VABACKEND_H
