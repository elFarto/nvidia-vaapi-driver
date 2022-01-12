#define _GNU_SOURCE

#include "vabackend.h"
#include "export-buf.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <va/va_backend.h>
#include <va/va_drmcommon.h>

#ifdef __has_include
#  if __has_include(<libdrm/drm_fourcc.h>)
#    include <libdrm/drm_fourcc.h>
#  else
#    include <drm/drm_fourcc.h>
#  endif
#else
#  include <drm/drm_fourcc.h>
#endif

#include <unistd.h>
#include <sys/types.h>
#include <stdarg.h>

extern const NVCodec __start_nvd_codecs[];
extern const NVCodec __stop_nvd_codecs[];

static FILE *LOG_OUTPUT;

__attribute__ ((constructor))
static void init() {
    LOG_OUTPUT = 0;

    char *nvdLog = getenv("NVD_LOG");
    if (nvdLog != NULL) {
        if (strcmp(nvdLog, "1") == 0) {
            LOG_OUTPUT = stdout;
        } else {
            LOG_OUTPUT = fopen(nvdLog, "a");
            if (LOG_OUTPUT == NULL) {
                LOG_OUTPUT = stdout;
            }
        }
    }
}

void logger(const char *filename, const char *function, int line, const char *msg, ...) {
    if (LOG_OUTPUT == 0) {
        return;
    }

    va_list argList;
    char formattedMessage[1024];

    va_start(argList, msg);
    vsnprintf(formattedMessage, 1024, msg, argList);
    va_end(argList);

    fprintf(LOG_OUTPUT, "[%d-%d] %s:%4d %24s %s\n", getpid(), gettid(), filename, line, function, formattedMessage);
}

void checkCudaErrors(NVDriver *drv, CUresult err, const char *file, const char *function, const int line)
{
    if (CUDA_SUCCESS != err)
    {
        const char *errStr = NULL;
        drv->cu->cuGetErrorString(err, &errStr);
        logger(file, function, line, "cuda error '%s' (%d)\n", errStr, err);
        exit(EXIT_FAILURE);
    }
}

void appendBuffer(AppendableBuffer *ab, const void *buf, uint64_t size)
{
  if (ab->buf == NULL) {
      ab->allocated = size*2;
      ab->buf = memalign(16, ab->allocated);
      ab->size = 0;
  } else if (ab->size + size > ab->allocated) {
      while (ab->size + size > ab->allocated) {
        ab->allocated += ab->allocated >> 1;
      }
      void *nb = memalign(16, ab->allocated);
      memcpy(nb, ab->buf, ab->size);
      free(ab->buf);
      ab->buf = nb;
  }
  memcpy(PTROFF(ab->buf, ab->size), buf, size);
  ab->size += size;
}

void freeBuffer(AppendableBuffer *ab) {
  if (ab->buf != NULL) {
      free(ab->buf);
      ab->buf = NULL;
      ab->size = 0;
      ab->allocated = 0;
  }
}

static Object allocateObject(NVDriver *drv, ObjectType type, int allocatePtrSize)
{
    Object newObj = (Object) calloc(1, sizeof(struct Object_t));

    newObj->type = type;
    newObj->id = (++drv->nextObjId);

    if (allocatePtrSize > 0) {
        newObj->obj = calloc(1, allocatePtrSize);
    }

    if (drv->objRoot == NULL) {
        drv->objRoot = newObj;
    } else {
        Object o = drv->objRoot;
        while (o->next != NULL) {
            o = (Object) (o->next);
        }

        o->next = (struct Object_t *) newObj;
    }

    return newObj;
}

static void* getObjectPtr(NVDriver *drv, VAGenericID id) {
    if (id != VA_INVALID_ID) {
        for (Object o = drv->objRoot; o != NULL; o = o->next) {
            if (o->id == id) {
                return o->obj;
            }
        }
    }
    return NULL;
}

static Object getObject(NVDriver *drv, VAGenericID id) {
    if (id != VA_INVALID_ID) {
        for (Object o = drv->objRoot; o != NULL; o = o->next) {
            if (o->id == id) {
                return o;
            }
        }
    }
    return NULL;
}

static Object getObjectByPtr(NVDriver *drv, void *ptr) {
    if (ptr != NULL) {
        for (Object o = drv->objRoot; o != NULL; o = o->next) {
            if (o->obj == ptr) {
                return o;
            }
        }
    }
    return NULL;
}

static void deleteObject(NVDriver *drv, VAGenericID id) {
    if (drv->objRoot == NULL || id == VA_INVALID_ID) {
        return;
    } else if (drv->objRoot->id == id) {
        Object o = drv->objRoot;
        drv->objRoot = (Object) drv->objRoot->next;
        if (o->obj != NULL) {
            free(o->obj);
        }
        free(o);
    } else {
        Object last = NULL;
        Object o = drv->objRoot;
        while (o != NULL && o->id != id) {
            last = o;
            o = (Object) o->next;
        }

        if (o != NULL) {
            last->next = o->next;
            if (o->obj != NULL) {
                free(o->obj);
            }
            free(o);
        }
    }
}

int pictureIdxFromSurfaceId(NVDriver *drv, VASurfaceID surf) {
    Object obj = getObject(drv, surf);
    if (obj != NULL && obj->type == OBJECT_TYPE_SURFACE) {
        NVSurface *suf = (NVSurface*) obj->obj;
        return suf->pictureIdx;
    }
    return -1;
}

static cudaVideoCodec vaToCuCodec(VAProfile profile)
{
    for (const NVCodec *c = __start_nvd_codecs; c < __stop_nvd_codecs; c++) {
        cudaVideoCodec cvc = c->computeCudaCodec(profile);
        if (cvc != cudaVideoCodec_NONE) {
            return cvc;
        }
    }

    //LOG("vaToCuCodec: Unknown codec: %d", profile);
    return cudaVideoCodec_NONE;
}

static int doesGPUSupportCodec(NVDriver *drv, cudaVideoCodec codec, int bitDepth, cudaVideoChromaFormat chromaFormat, int *width, int *height)
{
    CUVIDDECODECAPS videoDecodeCaps;
    memset(&videoDecodeCaps, 0, sizeof(CUVIDDECODECAPS));
    videoDecodeCaps.eCodecType      = codec;
    videoDecodeCaps.eChromaFormat   = chromaFormat;
    videoDecodeCaps.nBitDepthMinus8 = bitDepth - 8;

    CHECK_CUDA_RESULT(drv->cv->cuvidGetDecoderCaps(&videoDecodeCaps));
    if (width != NULL) {
        *width = videoDecodeCaps.nMaxWidth;
    }
    if (height != NULL) {
        *height = videoDecodeCaps.nMaxHeight;
    }
    return videoDecodeCaps.bIsSupported;
}

#define MAX_PROFILES 32
static VAStatus nvQueryConfigProfiles(
        VADriverContextP ctx,
        VAProfile *profile_list,	/* out */
        int *num_profiles			/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    drv->cu->cuCtxPushCurrent(drv->cudaContext);

    int profiles = 0;
    if (doesGPUSupportCodec(drv, cudaVideoCodec_MPEG2, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileMPEG2Simple;
        profile_list[profiles++] = VAProfileMPEG2Main;
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_MPEG4, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileMPEG4Simple;
        profile_list[profiles++] = VAProfileMPEG4AdvancedSimple;
        profile_list[profiles++] = VAProfileMPEG4Main;
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_VC1, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVC1Simple;
        profile_list[profiles++] = VAProfileVC1Main;
        profile_list[profiles++] = VAProfileVC1Advanced;
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_H264, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileH264Baseline;
        profile_list[profiles++] = VAProfileH264Main;
        profile_list[profiles++] = VAProfileH264High;
        profile_list[profiles++] = VAProfileH264ConstrainedBaseline;
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_JPEG, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileJPEGBaseline;
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_H264_SVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileH264StereoHigh;
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_H264_MVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileH264MultiviewHigh;
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_HEVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain;
    }
//    if (doesGPUSupportCodec(drv, cudaVideoCodec_HEVC, 10, cudaVideoChromaFormat_420, NULL, NULL)) {
//        profile_list[profiles++] = VAProfileHEVCMain10;
//    }
//    if (doesGPUSupportCodec(drv, cudaVideoCodec_HEVC, 10, cudaVideoChromaFormat_420, NULL, NULL)) {
//        profile_list[profiles++] = VAProfileHEVCMain422_10;
//    }
//    if (doesGPUSupportCodec(drv, cudaVideoCodec_HEVC, 12, cudaVideoChromaFormat_420, NULL, NULL)) {
//        profile_list[profiles++] = VAProfileHEVCMain422_12;
//    }
//    if (doesGPUSupportCodec(drv, cudaVideoCodec_HEVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
//        profile_list[profiles++] = VAProfileHEVCMain444;
//    }
//    if (doesGPUSupportCodec(drv, cudaVideoCodec_HEVC, 10, cudaVideoChromaFormat_420, NULL, NULL)) {
//        profile_list[profiles++] = VAProfileHEVCMain444_10;
//    }
//    if (doesGPUSupportCodec(drv, cudaVideoCodec_HEVC, 12, cudaVideoChromaFormat_420, NULL, NULL)) {
//        profile_list[profiles++] = VAProfileHEVCMain444_12;
//    }
//    if (doesGPUSupportCodec(drv, cudaVideoCodec_HEVC, 12, cudaVideoChromaFormat_420, NULL, NULL)) {
//        profile_list[profiles++] = VAProfileHEVCMain12;
//    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_VP8, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVP8Version0_3;
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_VP9, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVP9Profile0; //color depth: 8 bit, 4:2:0
    }
    if (doesGPUSupportCodec(drv, cudaVideoCodec_VP9, 8, cudaVideoChromaFormat_444, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVP9Profile1; //color depth: 8 bit, 4:2:2, 4:4:0, 4:4:4
    }
//    if (doesGPUSupportCodec(drv, cudaVideoCodec_VP9, 10, cudaVideoChromaFormat_420, NULL, NULL)) {
//        profile_list[profiles++] = VAProfileVP9Profile2; //color depth: 10–12 bit, 4:2:0
//    }
//    if (doesGPUSupportCodec(drv, cudaVideoCodec_VP9, 10, cudaVideoChromaFormat_444, NULL, NULL)) {
//        profile_list[profiles++] = VAProfileVP9Profile3; //color depth: 10–12 bit, 4:2:2, 4:4:0, 4:4:4
//    }
//    if (doesGPUSupportCodec(drv, cudaVideoCodec_AV1, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
//        profile_list[profiles++] = VAProfileAV1Profile0;
//    }
//    if (doesGPUSupportCodec(drv, cudaVideoCodec_AV1, 8, cudaVideoChromaFormat_444, NULL, NULL)) {
//        profile_list[profiles++] = VAProfileAV1Profile1;
//    }

    //now filter out the codecs we don't support
    for (int i = 0; i < profiles; i++) {
        if (vaToCuCodec(profile_list[i]) == cudaVideoCodec_NONE) {
            //LOG("Removing profile: %d", profile_list[i])
            for (int x = i; x < profiles-1; x++) {
                profile_list[x] = profile_list[x+1];
            }
            profiles--;
            i--;
        }
    }

    *num_profiles = profiles;

    drv->cu->cuCtxPopCurrent(NULL);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvQueryConfigEntrypoints(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint  *entrypoint_list,	/* out */
        int *num_entrypoints			/* out */
    )
{
    entrypoint_list[0] = VAEntrypointVLD;
    *num_entrypoints = 1;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvGetConfigAttributes(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint entrypoint,
        VAConfigAttrib *attrib_list,	/* in/out */
        int num_attribs
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    if (vaToCuCodec(profile) == cudaVideoCodec_NONE) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    for (int i = 0; i < num_attribs; i++)
    {
        if (attrib_list[i].type == VAConfigAttribRTFormat)
        {
            attrib_list[i].value = VA_RT_FORMAT_YUV420;

            if (profile == VAProfileHEVCMain10) {
                attrib_list[i].value |= VA_RT_FORMAT_YUV420_10;
            }
        }
        else if (attrib_list[i].type == VAConfigAttribMaxPictureWidth)
        {
            doesGPUSupportCodec(drv, vaToCuCodec(profile), 8, cudaVideoChromaFormat_420, &attrib_list[i].value, NULL);
        }
        else if (attrib_list[i].type == VAConfigAttribMaxPictureHeight)
        {
            doesGPUSupportCodec(drv, vaToCuCodec(profile), 8, cudaVideoChromaFormat_420, NULL, &attrib_list[i].value);
        }
        else
        {
            LOG("unhandled config attribute: %d", attrib_list[i].type);
        }
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateConfig(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint entrypoint,
        VAConfigAttrib *attrib_list,
        int num_attribs,
        VAConfigID *config_id		/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("In %s with profile: %d with %d attributes", __func__, profile, num_attribs);

    Object obj = allocateObject(drv, OBJECT_TYPE_CONFIG, sizeof(NVConfig));
    NVConfig *cfg = (NVConfig*) obj->obj;
    cfg->profile = profile;
    cfg->entrypoint = entrypoint;
    cfg->attributes = attrib_list; //TODO might need to make a copy of this
    cfg->numAttribs = num_attribs;

    for (int i = 0; i < num_attribs; i++)
    {
      LOG("got config attrib: %d %d %d", i, attrib_list[i].type, attrib_list[i].value);
    }

    cfg->cudaCodec = vaToCuCodec(cfg->profile);

    if (profile == VAProfileHEVCMain10) {
        cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
        cfg->chromaFormat = cudaVideoChromaFormat_420;
        cfg->bitDepth = 10;
    } else {
        cfg->surfaceFormat = cudaVideoSurfaceFormat_NV12;
        cfg->chromaFormat = cudaVideoChromaFormat_420;
        cfg->bitDepth = 8;
    }

    *config_id = obj->id;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvDestroyConfig(
        VADriverContextP ctx,
        VAConfigID config_id
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    deleteObject(drv, config_id);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvQueryConfigAttributes(
        VADriverContextP ctx,
        VAConfigID config_id,
        VAProfile *profile,		/* out */
        VAEntrypoint *entrypoint, 	/* out */
        VAConfigAttrib *attrib_list,	/* out */
        int *num_attribs		/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    Object obj = getObject(drv, config_id);

    if (obj != NULL)
    {
        NVConfig *cfg = (NVConfig*) obj->obj;
        *profile = cfg->profile;
        *entrypoint = cfg->entrypoint;
        //*attrib_list = cfg->attrib_list; //TODO is that the right thing/type?
        *num_attribs = cfg->numAttribs;
        return VA_STATUS_SUCCESS;
    }

    return VA_STATUS_ERROR_INVALID_CONFIG;
}

static VAStatus nvCreateSurfaces2(
            VADriverContextP    ctx,
            unsigned int        format,
            unsigned int        width,
            unsigned int        height,
            VASurfaceID        *surfaces,
            unsigned int        num_surfaces,
            VASurfaceAttrib    *attrib_list,
            unsigned int        num_attribs
        )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("creating %d surface(s) %dx%d, format %X", num_surfaces, width, height, format);

    cudaVideoSurfaceFormat nvFormat;
    int bitdepth;

    if (format == VA_RT_FORMAT_YUV420) {
        nvFormat = cudaVideoSurfaceFormat_NV12;
        bitdepth = 8;
    } else if (format == VA_RT_FORMAT_YUV420_10) {
        nvFormat = cudaVideoSurfaceFormat_P016;
        bitdepth = 10;
    } else if (format == VA_RT_FORMAT_YUV420_12) {
        nvFormat = cudaVideoSurfaceFormat_P016;
        bitdepth = 12;
    } else {
        LOG("Unknown format: %X", format);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    for (int i = 0; i < num_surfaces; i++)
    {
        Object surfaceObject = allocateObject(drv, OBJECT_TYPE_SURFACE, sizeof(NVSurface));
        surfaces[i] = surfaceObject->id;
        NVSurface *suf = (NVSurface*) surfaceObject->obj;
        suf->width = width;
        suf->height = height;
        suf->format = nvFormat;
        suf->pictureIdx = -1;
        suf->bitDepth = bitdepth;
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateSurfaces(
        VADriverContextP ctx,
        int width,
        int height,
        int format,
        int num_surfaces,
        VASurfaceID *surfaces		/* out */
    )
{
    return nvCreateSurfaces2(ctx, format, width, height, surfaces, num_surfaces, NULL, 0);
}


static VAStatus nvDestroySurfaces(
        VADriverContextP ctx,
        VASurfaceID *surface_list,
        int num_surfaces
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    for (int i = 0; i < num_surfaces; i++)
    {
        deleteObject(drv, surface_list[i]);
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateContext(
        VADriverContextP ctx,
        VAConfigID config_id,
        int picture_width,
        int picture_height,
        int flag,
        VASurfaceID *render_targets,
        int num_render_targets,
        VAContextID *context		/* out */
    )
{
    LOG("with %d render targets", num_render_targets);

    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVConfig *cfg = (NVConfig*) getObject(drv, config_id)->obj;

    CUvideodecoder decoder;
    CUVIDDECODECREATEINFO vdci;
    memset(&vdci, 0, sizeof(CUVIDDECODECREATEINFO));
    vdci.ulWidth = picture_width;
    vdci.ulHeight = picture_height;
    vdci.ulNumDecodeSurfaces = num_render_targets; //TODO is this correct? probably not, but the amount of decode surfaces needed is determined by codec, i think
    vdci.CodecType = cfg->cudaCodec;
    vdci.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    vdci.ulIntraDecodeOnly = 0; //TODO (flag & VA_PROGRESSIVE) != 0
    vdci.display_area.right = picture_width;
    vdci.display_area.bottom = picture_height;

    vdci.ChromaFormat = cfg->chromaFormat;
    vdci.OutputFormat = cfg->surfaceFormat;
    vdci.bitDepthMinus8 = cfg->bitDepth - 8;

    vdci.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    vdci.ulTargetWidth = picture_width;
    vdci.ulTargetHeight = picture_height;
    vdci.ulNumOutputSurfaces = num_render_targets;

    drv->cv->cuvidCtxLockCreate(&vdci.vidLock, drv->cudaContext);

    CUresult result = drv->cv->cuvidCreateDecoder(&decoder, &vdci);

    if (result != CUDA_SUCCESS)
    {
        LOG("cuvidCreateDecoder failed: %d", result);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    Object contextObj = allocateObject(drv, OBJECT_TYPE_CONTEXT, sizeof(NVContext));
    *context = contextObj->id;

    NVContext *nvCtx = (NVContext*) contextObj->obj;
    nvCtx->drv = drv;
    nvCtx->decoder = decoder;
    nvCtx->profile = cfg->profile;
    nvCtx->entrypoint = cfg->entrypoint;
    nvCtx->width = picture_width;
    nvCtx->height = picture_height;

    for (const NVCodec *c = __start_nvd_codecs; c < __stop_nvd_codecs; c++) {
        for (int i = 0; i < c->supportedProfileCount; i++) {
            if (c->supportedProfiles[i] == cfg->profile) {
                nvCtx->codec = c;
                break;
            }
        }
    }

    if (nvCtx->codec == NULL) {
        LOG("Unable to find codec for profile: %d", cfg->profile);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE; //TODO not sure this is the correct error
    }

    //assign all the render targets unique ids up-front
    //this seems to be a simplier way to manage them
    for (int i = 0; i < num_render_targets; i++) {
        Object obj = getObject(drv, render_targets[i]);
        NVSurface *suf = obj->obj;
        //LOG("assigning surface id %d to picture index %d", obj->id, i);
        suf->pictureIdx = i;
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvDestroyContext(
        VADriverContextP ctx,
        VAContextID context)
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("In %s", __func__);

    NVContext *nvCtx = (NVContext*) getObject(drv, context)->obj;

    if (nvCtx != NULL)
    {
      CUvideodecoder decoder = nvCtx->decoder;
      nvCtx->decoder = NULL;
      freeBuffer(&nvCtx->sliceOffsets);
      freeBuffer(&nvCtx->buf);

      //delete the NVContext object before we try to free the decoder
      //so if that fails we don't leak
      deleteObject(drv, context);

      if (decoder != NULL)
      {
        CUresult result = drv->cv->cuvidDestroyDecoder(decoder);
        if (result != CUDA_SUCCESS)
        {
            LOG("cuvidDestroyDecoder failed: %d", result);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
      }
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateBuffer(
        VADriverContextP ctx,
        VAContextID context,		/* in */
        VABufferType type,		/* in */
        unsigned int size,		/* in */
        unsigned int num_elements,	/* in */
        void *data,			/* in */
        VABufferID *buf_id
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    //TODO should pool these as most of the time these should be the same size
    Object bufferObject = allocateObject(drv, OBJECT_TYPE_BUFFER, sizeof(NVBuffer));
    *buf_id = bufferObject->id;

    NVBuffer *buf = (NVBuffer*) bufferObject->obj;
    buf->bufferType = type;
    buf->elements = num_elements;
    buf->size = num_elements * size;
    buf->ptr = memalign(16, buf->size);

    if (buf->ptr == NULL) {
        LOG("Unable to allocate buffer of %d bytes", buf->size);
        exit(EXIT_FAILURE);
    }

    if (data != NULL)
    {
        memcpy(buf->ptr, data, buf->size);
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvBufferSetNumElements(
        VADriverContextP ctx,
        VABufferID buf_id,	/* in */
        unsigned int num_elements	/* in */
    )
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvMapBuffer(
        VADriverContextP ctx,
        VABufferID buf_id,	/* in */
        void **pbuf         /* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    Object obj = getObject(drv, buf_id);
    NVBuffer *buf = (NVBuffer*) obj->obj;
    *pbuf = buf->ptr;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvUnmapBuffer(
        VADriverContextP ctx,
        VABufferID buf_id	/* in */
    )
{
    return VA_STATUS_SUCCESS;
}

static VAStatus nvDestroyBuffer(
        VADriverContextP ctx,
        VABufferID buffer_id
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    Object obj = getObject(drv, buffer_id);

    if (obj->obj != NULL) {
        NVBuffer *buf = (NVBuffer*) obj->obj;
        if (buf->ptr != NULL) {
            free(buf->ptr);
        }
    }

    deleteObject(drv, buffer_id);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvBeginPicture(
        VADriverContextP ctx,
        VAContextID context,
        VASurfaceID render_target
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    NVContext *nvCtx = (NVContext*) getObject(drv, context)->obj;
    memset(&nvCtx->pPicParams, 0, sizeof(CUVIDPICPARAMS));
    nvCtx->renderTargets = (NVSurface*) getObject(drv, render_target)->obj;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvRenderPicture(
        VADriverContextP ctx,
        VAContextID context,
        VABufferID *buffers,
        int num_buffers
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    NVContext *nvCtx = (NVContext*) getObject(drv, context)->obj;
    CUVIDPICPARAMS *picParams = &nvCtx->pPicParams;
    VAProfile prof = nvCtx->profile;
    cudaVideoCodec codec = vaToCuCodec(prof);

    for (int i = 0; i < num_buffers; i++)
    {
        NVBuffer *buf = (NVBuffer*) getObject(drv, buffers[i])->obj;
        VABufferType bt = buf->bufferType;
        HandlerFunc func = nvCtx->codec->handlers[bt];
        if (func != NULL) {
            func(nvCtx, buf, picParams);
        } else {
            LOG("Unhandled buffer type: %d", bt);
        }
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus nvEndPicture(
        VADriverContextP ctx,
        VAContextID context
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVContext *nvCtx = (NVContext*) getObject(drv, context)->obj;

    CUVIDPICPARAMS *picParams = &nvCtx->pPicParams;

    picParams->pBitstreamData = nvCtx->buf.buf;
    picParams->pSliceDataOffsets = nvCtx->sliceOffsets.buf;
    nvCtx->buf.size = 0;
    nvCtx->sliceOffsets.size = 0;

    picParams->CurrPicIdx = nvCtx->renderTargets->pictureIdx;

    CUresult result = drv->cv->cuvidDecodePicture(nvCtx->decoder, picParams);

    if (result != CUDA_SUCCESS)
    {
        LOG("cuvidDecodePicture failed: %d", result);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    LOG("cuvid decoded successful to idx: %d", picParams->CurrPicIdx);
    nvCtx->renderTargets->contextId = context;
    nvCtx->renderTargets->progressiveFrame = !picParams->field_pic_flag;
    nvCtx->renderTargets->topFieldFirst = !picParams->bottom_field_flag;
    nvCtx->renderTargets->secondField = picParams->second_field;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvSyncSurface(
        VADriverContextP ctx,
        VASurfaceID render_target
    )
{
    return VA_STATUS_SUCCESS;
}

static VAStatus nvQuerySurfaceStatus(
        VADriverContextP ctx,
        VASurfaceID render_target,
        VASurfaceStatus *status	/* out */
    )
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQuerySurfaceError(
        VADriverContextP ctx,
        VASurfaceID render_target,
        VAStatus error_status,
        void **error_info /*out*/
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvPutSurface(
        VADriverContextP ctx,
        VASurfaceID surface,
        void* draw, /* Drawable of window system */
        short srcx,
        short srcy,
        unsigned short srcw,
        unsigned short srch,
        short destx,
        short desty,
        unsigned short destw,
        unsigned short desth,
        VARectangle *cliprects, /* client supplied clip list */
        unsigned int number_cliprects, /* number of clip rects in the clip list */
        unsigned int flags /* de-interlacing flags */
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQueryImageFormats(
        VADriverContextP ctx,
        VAImageFormat *format_list,        /* out */
        int *num_formats           /* out */
    )
{
    LOG("In %s", __func__);

    int i = 0;

    format_list[i].fourcc = VA_FOURCC_NV12;
    format_list[i].byte_order = VA_LSB_FIRST;
    format_list[i++].bits_per_pixel = 12;

//    format_list[i].fourcc = VA_FOURCC_P010;
//    format_list[i].byte_order = VA_LSB_FIRST;
//    format_list[i++].bits_per_pixel = 24;

//    format_list[i].fourcc = VA_FOURCC_P012;
//    format_list[i].byte_order = VA_LSB_FIRST;
//    format_list[i++].bits_per_pixel = 24;

    *num_formats = i;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateImage(
        VADriverContextP ctx,
        VAImageFormat *format,
        int width,
        int height,
        VAImage *image     /* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    Object imageObj = allocateObject(drv, OBJECT_TYPE_IMAGE, sizeof(NVImage));
    image->image_id = imageObj->id;

    LOG("created image id: %d", imageObj->id);

    NVImage *img = (NVImage*) imageObj->obj;
    img->width = width;
    img->height = height;
    img->format = format->fourcc;

    int bytesPerPixel = 1;
    if (format->fourcc == VA_FOURCC_P010 || format->fourcc == VA_FOURCC_P012) {
        bytesPerPixel = 2;
    }

    //allocate buffer to hold image when we copy down from the GPU
    //TODO could probably put these in a pool, they appear to be allocated, used, then freed
    Object imageBufferObject = allocateObject(drv, OBJECT_TYPE_BUFFER, sizeof(NVBuffer));
    NVBuffer *imageBuffer = (NVBuffer*) imageBufferObject->obj;
    imageBuffer->bufferType = VAImageBufferType;
    imageBuffer->size = (width * height + (width * height / 2)) * bytesPerPixel;
    imageBuffer->elements = 1;
    imageBuffer->ptr = memalign(16, imageBuffer->size);

    img->imageBuffer = imageBuffer;

    memcpy(&image->format, format, sizeof(VAImageFormat));
    image->buf = imageBufferObject->id;	/* image data buffer */
    /*
     * Image data will be stored in a buffer of type VAImageBufferType to facilitate
     * data store on the server side for optimal performance. The buffer will be
     * created by the CreateImage function, and proper storage allocated based on the image
     * size and format. This buffer is managed by the library implementation, and
     * accessed by the client through the buffer Map/Unmap functions.
     */
    image->width = width;
    image->height = height;
    image->data_size = imageBuffer->size;
    image->num_planes = 2;	/* can not be greater than 3 */
    /*
     * An array indicating the scanline pitch in bytes for each plane.
     * Each plane may have a different pitch. Maximum 3 planes for planar formats
     */
    image->pitches[0] = width * bytesPerPixel;
    image->pitches[1] = width * bytesPerPixel;
    /*
     * An array indicating the byte offset from the beginning of the image data
     * to the start of each plane.
     */
    image->offsets[0] = 0;
    image->offsets[1] = width * height * bytesPerPixel;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvDeriveImage(
        VADriverContextP ctx,
        VASurfaceID surface,
        VAImage *image     /* out */
    )
{
    LOG("In %s", __func__);
    //FAILED because we don't support it yet
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

static VAStatus nvDestroyImage(
        VADriverContextP ctx,
        VAImageID image
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVImage *img = getObject(drv, image)->obj;

    Object imageBufferObj = getObjectByPtr(drv, img->imageBuffer);
    if (imageBufferObj != NULL) {
        NVBuffer *imageBuffer = (NVBuffer*) imageBufferObj->obj;
        if (imageBuffer != NULL){
            if (imageBuffer->ptr != NULL) {
                free(imageBuffer->ptr);
            }
        }

        deleteObject(drv, imageBufferObj->id);
    }

    deleteObject(drv, image);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvSetImagePalette(
            VADriverContextP ctx,
            VAImageID image,
            /*
                 * pointer to an array holding the palette data.  The size of the array is
                 * num_palette_entries * entry_bytes in size.  The order of the components
                 * in the palette is described by the component_order in VAImage struct
                 */
                unsigned char *palette
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvGetImage(
        VADriverContextP ctx,
        VASurfaceID surface,
        int x,     /* coordinates of the upper left source pixel */
        int y,
        unsigned int width, /* width and height of the region */
        unsigned int height,
        VAImageID image
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    NVSurface *surfaceObj = (NVSurface*) getObject(drv, surface)->obj;
    NVImage *imageObj = (NVImage*) getObject(drv, image)->obj;

    if (getObject(drv, surfaceObj->contextId) == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    NVContext *context = (NVContext*) getObject(drv, surfaceObj->contextId)->obj;

    int bytesPerPixel = 1;
    if (imageObj->format == VA_FOURCC_P010 || imageObj->format == VA_FOURCC_P012) {
        bytesPerPixel = 2;
    }

    CUVIDPROCPARAMS procParams = {0};
    procParams.progressive_frame = surfaceObj->progressiveFrame;
    procParams.top_field_first = surfaceObj->topFieldFirst;
    procParams.second_field = surfaceObj->secondField;

    CUdeviceptr deviceMemory = (CUdeviceptr) NULL;
    unsigned int pitch;

    CUresult result = drv->cv->cuvidMapVideoFrame(context->decoder, surfaceObj->pictureIdx, &deviceMemory, &pitch, &procParams);
    LOG("got address %X for surface %d", deviceMemory, getObject(drv, surface)->id);

    if (result != CUDA_SUCCESS)
    {
            LOG("cuvidMapVideoFrame failed: %d", result);
            return VA_STATUS_ERROR_DECODING_ERROR;
    }

    CUDA_MEMCPY2D memcpy2d = {
      .srcXInBytes = 0, .srcY = 0,
      .srcMemoryType = CU_MEMORYTYPE_DEVICE,
      .srcDevice = deviceMemory,
      .srcPitch = pitch,

      .dstXInBytes = 0, .dstY = 0,
      .dstMemoryType = CU_MEMORYTYPE_HOST,
      .dstHost = imageObj->imageBuffer->ptr,
      .dstPitch = width * bytesPerPixel,

      .WidthInBytes = width * bytesPerPixel,
      .Height = height + (height>>1) //luma and chroma
    };

    result = drv->cu->cuMemcpy2D(&memcpy2d);
    if (result != CUDA_SUCCESS)
    {
            LOG("cuMemcpy2D failed: %d", result);
            return VA_STATUS_ERROR_DECODING_ERROR;
    }

    drv->cv->cuvidUnmapVideoFrame(context->decoder, deviceMemory);

//    static int counter = 0;
//    char filename[64];
//    int size = (pitch * surfaceObj->height) + (pitch * surfaceObj->height>>1);
//    char *buf = malloc(size);
//    snprintf(filename, 64, "/tmp/frame-%03d.data\0", counter++);
//    LOG("writing %d to %s", surfaceObj->pictureIdx, filename);
//    int fd = open(filename, O_RDWR | O_TRUNC | O_CREAT, 0644);
//    cuMemcpyDtoH(buf, deviceMemory, size);
//    write(fd, buf, size);
//    free(buf);
//    close(fd);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvPutImage(
        VADriverContextP ctx,
        VASurfaceID surface,
        VAImageID image,
        int src_x,
        int src_y,
        unsigned int src_width,
        unsigned int src_height,
        int dest_x,
        int dest_y,
        unsigned int dest_width,
        unsigned int dest_height
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_SUCCESS;
}

static VAStatus nvQuerySubpictureFormats(
        VADriverContextP ctx,
        VAImageFormat *format_list,        /* out */
        unsigned int *flags,       /* out */
        unsigned int *num_formats  /* out */
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvCreateSubpicture(
        VADriverContextP ctx,
        VAImageID image,
        VASubpictureID *subpicture   /* out */
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvDestroySubpicture(
        VADriverContextP ctx,
        VASubpictureID subpicture
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetSubpictureImage(
                VADriverContextP ctx,
                VASubpictureID subpicture,
                VAImageID image
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetSubpictureChromakey(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        unsigned int chromakey_min,
        unsigned int chromakey_max,
        unsigned int chromakey_mask
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetSubpictureGlobalAlpha(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        float global_alpha
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvAssociateSubpicture(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        VASurfaceID *target_surfaces,
        int num_surfaces,
        short src_x, /* upper left offset in subpicture */
        short src_y,
        unsigned short src_width,
        unsigned short src_height,
        short dest_x, /* upper left offset in surface */
        short dest_y,
        unsigned short dest_width,
        unsigned short dest_height,
        /*
         * whether to enable chroma-keying or global-alpha
         * see VA_SUBPICTURE_XXX values
         */
        unsigned int flags
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvDeassociateSubpicture(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        VASurfaceID *target_surfaces,
        int num_surfaces
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQueryDisplayAttributes(
        VADriverContextP ctx,
        VADisplayAttribute *attr_list,	/* out */
        int *num_attributes		/* out */
        )
{
    LOG("In %s", __func__);
    *num_attributes = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus nvGetDisplayAttributes(
        VADriverContextP ctx,
        VADisplayAttribute *attr_list,	/* in/out */
        int num_attributes
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetDisplayAttributes(
        VADriverContextP ctx,
                VADisplayAttribute *attr_list,
                int num_attributes
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQuerySurfaceAttributes(
        VADriverContextP    ctx,
	    VAConfigID          config,
	    VASurfaceAttrib    *attrib_list,
	    unsigned int       *num_attribs
	)
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVConfig *cfg = (NVConfig*) getObject(drv, config)->obj;

    LOG("with %d %p %d", cfg->cudaCodec, attrib_list, *num_attribs);

    if (attrib_list == NULL) {
            *num_attribs = 5;
    } else {
        CUVIDDECODECAPS videoDecodeCaps = {
            .eCodecType      = cfg->cudaCodec,
            .eChromaFormat   = cfg->chromaFormat,
            .nBitDepthMinus8 = cfg->bitDepth - 8
        };

        CHECK_CUDA_RESULT(drv->cu->cuCtxPushCurrent(drv->cudaContext));
        CUresult result = drv->cv->cuvidGetDecoderCaps(&videoDecodeCaps);
        drv->cu->cuCtxPopCurrent(NULL);
        if (result != CUDA_SUCCESS) {
            CHECK_CUDA_RESULT(result);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        attrib_list[0].type = VASurfaceAttribPixelFormat;
        attrib_list[0].flags = 0;
        attrib_list[0].value.type = VAGenericValueTypeInteger;

        if (cfg->chromaFormat == cudaVideoChromaFormat_420) {
            if (cfg->bitDepth == 10 && (videoDecodeCaps.nOutputFormatMask & 2)) {
                attrib_list[0].value.value.i = VA_FOURCC_P010;
            } else if (cfg->bitDepth == 12 && (videoDecodeCaps.nOutputFormatMask & 2)) {
                attrib_list[0].value.value.i = VA_FOURCC_P012;
            } else if (videoDecodeCaps.nOutputFormatMask & 1) {
                attrib_list[0].value.value.i = VA_FOURCC_NV12;
            }
        } else {
            //TODO not sure what pixel formats are needed for 422 and 444 formats
            LOG("Unknown chrome format: %d", cfg->chromaFormat);
            return VA_STATUS_ERROR_INVALID_CONFIG;
        }

        attrib_list[1].type = VASurfaceAttribMinWidth;
        attrib_list[1].flags = 0;
        attrib_list[1].value.type = VAGenericValueTypeInteger;
        attrib_list[1].value.value.i = videoDecodeCaps.nMinWidth;

        attrib_list[2].type = VASurfaceAttribMinHeight;
        attrib_list[2].flags = 0;
        attrib_list[2].value.type = VAGenericValueTypeInteger;
        attrib_list[2].value.value.i = videoDecodeCaps.nMinHeight;

        attrib_list[3].type = VASurfaceAttribMaxWidth;
        attrib_list[3].flags = 0;
        attrib_list[3].value.type = VAGenericValueTypeInteger;
        attrib_list[3].value.value.i = videoDecodeCaps.nMaxWidth;

        attrib_list[4].type = VASurfaceAttribMaxHeight;
        attrib_list[4].flags = 0;
        attrib_list[4].value.type = VAGenericValueTypeInteger;
        attrib_list[4].value.value.i = videoDecodeCaps.nMaxHeight;
    }

    return VA_STATUS_SUCCESS;
}

/* used by va trace */
static VAStatus nvBufferInfo(
           VADriverContextP ctx,      /* in */
           VABufferID buf_id,         /* in */
           VABufferType *type,        /* out */
           unsigned int *size,        /* out */
           unsigned int *num_elements /* out */
)
{
    LOG("In %s", __func__);
    *size=0;
    *num_elements=0;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvAcquireBufferHandle(
            VADriverContextP    ctx,
            VABufferID          buf_id,         /* in */
            VABufferInfo *      buf_info        /* in/out */
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvReleaseBufferHandle(
            VADriverContextP    ctx,
            VABufferID          buf_id          /* in */
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

//        /* lock/unlock surface for external access */
static VAStatus nvLockSurface(
        VADriverContextP ctx,
        VASurfaceID surface,
        unsigned int *fourcc, /* out  for follow argument */
        unsigned int *luma_stride,
        unsigned int *chroma_u_stride,
        unsigned int *chroma_v_stride,
        unsigned int *luma_offset,
        unsigned int *chroma_u_offset,
        unsigned int *chroma_v_offset,
        unsigned int *buffer_name, /* if it is not NULL, assign the low lever
                                    * surface buffer name
                                    */
        void **buffer /* if it is not NULL, map the surface buffer for
                       * CPU access
                       */
)
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvUnlockSurface(
        VADriverContextP ctx,
                VASurfaceID surface
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvCreateMFContext(
            VADriverContextP ctx,
            VAMFContextID *mfe_context    /* out */
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvMFAddContext(
            VADriverContextP ctx,
            VAMFContextID mf_context,
            VAContextID context
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvMFReleaseContext(
            VADriverContextP ctx,
            VAMFContextID mf_context,
            VAContextID context
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvMFSubmit(
            VADriverContextP ctx,
            VAMFContextID mf_context,
            VAContextID *contexts,
            int num_contexts
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
static VAStatus nvCreateBuffer2(
            VADriverContextP ctx,
            VAContextID context,                /* in */
            VABufferType type,                  /* in */
            unsigned int width,                 /* in */
            unsigned int height,                /* in */
            unsigned int *unit_size,            /* out */
            unsigned int *pitch,                /* out */
            VABufferID *buf_id                  /* out */
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQueryProcessingRate(
            VADriverContextP ctx,               /* in */
            VAConfigID config_id,               /* in */
            VAProcessingRateParameter *proc_buf,/* in */
            unsigned int *processing_rate	/* out */
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvExportSurfaceHandle(
            VADriverContextP    ctx,
            VASurfaceID         surface_id,     /* in */
            uint32_t            mem_type,       /* in */
            uint32_t            flags,          /* in */
            void               *descriptor      /* out */
)
{
    //TODO check mem_type
    //TODO deal with flags

    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("got %p", drv);

    drv->cu->cuCtxPushCurrent(drv->cudaContext);

    NVSurface *surfaceObj = (NVSurface*) getObjectPtr(drv, surface_id);
    //This will be NULL for surfaces that haven't been end
    NVContext *context = (NVContext*) getObjectPtr(drv, surfaceObj->contextId);

    if (surfaceObj->pictureIdx != -1 && context == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    CUdeviceptr deviceMemory = (CUdeviceptr) NULL;
    unsigned int pitch = 0;

    if (surfaceObj->pictureIdx != -1) {
        CUVIDPROCPARAMS procParams = {0};
        procParams.progressive_frame = surfaceObj->progressiveFrame;
        procParams.top_field_first = surfaceObj->topFieldFirst;
        procParams.second_field = surfaceObj->secondField;

        CHECK_CUDA_RESULT(drv->cv->cuvidMapVideoFrame(context->decoder, surfaceObj->pictureIdx, &deviceMemory, &pitch, &procParams));
        LOG("got address %llX (%d) for surface %d (picIdx: %d)", deviceMemory, pitch, surface_id, surfaceObj->pictureIdx);
    } else {
        pitch = surfaceObj->width;
    }

    int fourcc, bpp, fds[4] = {0, 0, 0, 0}, strides[4] = {0, 0, 0, 0}, offsets[4] = {0, 0, 0, 0};
    uint64_t mods[4] = {0, 0, 0, 0};
    exportCudaPtr(drv, deviceMemory, surfaceObj, pitch, &fourcc, fds, offsets, strides, mods, &bpp);

    //since we have to make a copy of the data anyway, we can unmap here
    if (surfaceObj->pictureIdx != -1) {
        drv->cv->cuvidUnmapVideoFrame(context->decoder, deviceMemory);
    }

    //TODO only support 420 images (either NV12, P010 or P012)
    VADRMPRIMESurfaceDescriptor *ptr = (VADRMPRIMESurfaceDescriptor*) descriptor;
    ptr->fourcc = fourcc;
    ptr->width = surfaceObj->width;
    ptr->height = surfaceObj->height;
    ptr->num_layers = 2;
    ptr->num_objects = 2;

    ptr->objects[0].fd = fds[0];
    ptr->objects[0].size = surfaceObj->width * surfaceObj->height * bpp;
    ptr->objects[0].drm_format_modifier = mods[0];

    ptr->objects[1].fd = fds[1];
    ptr->objects[1].size = surfaceObj->width * (surfaceObj->height >> 1) * bpp;
    ptr->objects[1].drm_format_modifier = mods[1];

    ptr->layers[0].drm_format = fourcc == DRM_FORMAT_NV12 ? DRM_FORMAT_R8 : DRM_FORMAT_R16;
    ptr->layers[0].num_planes = 1;
    ptr->layers[0].object_index[0] = 0;
    ptr->layers[0].offset[0] = offsets[0];
    ptr->layers[0].pitch[0] = strides[0];

    ptr->layers[1].drm_format = fourcc == DRM_FORMAT_NV12 ? DRM_FORMAT_RG88 : DRM_FORMAT_RG1616;
    ptr->layers[1].num_planes = 1;
    ptr->layers[1].object_index[0] = 1;
    ptr->layers[1].offset[0] = offsets[1];
    ptr->layers[1].pitch[0] = strides[1];

    drv->cu->cuCtxPopCurrent(NULL);

    return VA_STATUS_SUCCESS;
}


static VAStatus nvTerminate( VADriverContextP ctx )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("In %s", __func__);

    releaseExporter(drv);

    drv->cu->cuCtxDestroy(drv->cudaContext);
    cuvid_free_functions(&drv->cv);
    cuda_free_functions(&drv->cu);

    return VA_STATUS_SUCCESS;
}

__attribute__((visibility("default")))
VAStatus __vaDriverInit_1_0(VADriverContextP ctx)
{
    LOG("Initing NVIDIA VA-API Driver");
    NVDriver *drv = (NVDriver*) calloc(1, sizeof(NVDriver));
    ctx->pDriverData = drv;

    int ret;

    ret = cuda_load_functions(&drv->cu, NULL);
    if (ret != 0) {
        LOG("Failed to load CUDA functions");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    ret = cuvid_load_functions(&drv->cv, NULL);
    if (ret != 0) {
        LOG("Failed to load NVDEC functions");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    CHECK_CUDA_RESULT(drv->cu->cuInit(0));
    CHECK_CUDA_RESULT(drv->cu->cuCtxCreate(&drv->cudaContext, CU_CTX_SCHED_BLOCKING_SYNC, 0));

    ctx->max_profiles = MAX_PROFILES;
    ctx->max_entrypoints = 1;
    ctx->max_attributes = 1;
    ctx->max_display_attributes = 1;
    ctx->max_image_formats = 3;
    ctx->max_subpic_formats = 1;

    ctx->str_vendor = "VA-API NVDEC driver";

    initExporter(drv);

#define VTABLE(ctx, func) ctx->vtable->va ## func = nv ## func

    VTABLE(ctx, Terminate);
    VTABLE(ctx, QueryConfigProfiles);
    VTABLE(ctx, QueryConfigEntrypoints);
    VTABLE(ctx, QueryConfigAttributes);
    VTABLE(ctx, CreateConfig);
    VTABLE(ctx, DestroyConfig);
    VTABLE(ctx, GetConfigAttributes);
    VTABLE(ctx, CreateSurfaces);
    VTABLE(ctx, CreateSurfaces2);
    VTABLE(ctx, DestroySurfaces);
    VTABLE(ctx, CreateContext);
    VTABLE(ctx, DestroyContext);
    VTABLE(ctx, CreateBuffer);
    VTABLE(ctx, BufferSetNumElements);
    VTABLE(ctx, MapBuffer);
    VTABLE(ctx, UnmapBuffer);
    VTABLE(ctx, DestroyBuffer);
    VTABLE(ctx, BeginPicture);
    VTABLE(ctx, RenderPicture);
    VTABLE(ctx, EndPicture);
    VTABLE(ctx, SyncSurface);
    VTABLE(ctx, QuerySurfaceStatus);
    VTABLE(ctx, PutSurface);
    VTABLE(ctx, QueryImageFormats);
    VTABLE(ctx, CreateImage);
    VTABLE(ctx, DeriveImage);
    VTABLE(ctx, DestroyImage);
    VTABLE(ctx, SetImagePalette);
    VTABLE(ctx, GetImage);
    VTABLE(ctx, PutImage);
    VTABLE(ctx, QuerySubpictureFormats);
    VTABLE(ctx, CreateSubpicture);
    VTABLE(ctx, DestroySubpicture);
    VTABLE(ctx, SetSubpictureImage);
    VTABLE(ctx, SetSubpictureChromakey);
    VTABLE(ctx, SetSubpictureGlobalAlpha);
    VTABLE(ctx, AssociateSubpicture);
    VTABLE(ctx, DeassociateSubpicture);
    VTABLE(ctx, QueryDisplayAttributes);
    VTABLE(ctx, GetDisplayAttributes);
    VTABLE(ctx, SetDisplayAttributes);
    VTABLE(ctx, QuerySurfaceAttributes);
    VTABLE(ctx, BufferInfo);
    VTABLE(ctx, AcquireBufferHandle);
    VTABLE(ctx, ReleaseBufferHandle);
    VTABLE(ctx, LockSurface);
    VTABLE(ctx, UnlockSurface);
    VTABLE(ctx, CreateMFContext);
    VTABLE(ctx, MFAddContext);
    VTABLE(ctx, MFReleaseContext);
    VTABLE(ctx, MFSubmit);
    VTABLE(ctx, CreateBuffer2);
    VTABLE(ctx, QueryProcessingRate);
    VTABLE(ctx, ExportSurfaceHandle);

    return VA_STATUS_SUCCESS;
}
