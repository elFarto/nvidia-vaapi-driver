#define _GNU_SOURCE

#include "encode_surface.h"
#include "encode_common.h"
#include "h264_encode.h"

#if __has_include(<pthread_np.h>)
#include <pthread_np.h>
#define gettid pthread_getthreadid_np
#define HAVE_GETTID 1
#endif

#ifndef HAVE_GETTID
#include <sys/syscall.h>
/* Bionic and glibc >= 2.30 declare gettid() in unistd.h. */
#ifdef __BIONIC__
#define HAVE_GETTID 1
#elif !defined(__GLIBC_PREREQ)
#define HAVE_GETTID 0
#elif !__GLIBC_PREREQ(2,30)
#define HAVE_GETTID 0
#else
#define HAVE_GETTID 1
#endif
#endif

#include <unistd.h>

typedef struct
{
    VASurfaceAttribType type;
    unsigned int flags;
    VAGenericValueType value_type;
    int value;
} NVEncSurfaceAttribInit;

typedef struct
{
    cudaVideoSurfaceFormat format;
    cudaVideoChromaFormat chromaFormat;
    int bitDepth;
    uint32_t width;
    uint32_t height;
} NVEncSurfaceInfo;

static bool nvenc_get_surface_info(NVDriver *drv,
                                   VASurfaceID id,
                                   NVEncSurfaceInfo *info)
{
    if (!drv || !info) {
        return false;
    }

    pthread_mutex_lock(&drv->objectCreationMutex);
    ARRAY_FOR_EACH(Object, o, &drv->objects)
        if (!o || o->type != OBJECT_TYPE_SURFACE || o->id != id || !o->obj) {
            continue;
        }

        NVSurface *surface = (NVSurface *)o->obj;
        info->format = surface->format;
        info->chromaFormat = surface->chromaFormat;
        info->bitDepth = surface->bitDepth;
        info->width = surface->width;
        info->height = surface->height;
        pthread_mutex_unlock(&drv->objectCreationMutex);
        return true;
    END_FOR_EACH
    pthread_mutex_unlock(&drv->objectCreationMutex);

    return false;
}

static uint64_t nvenc_gettid(void)
{
#if HAVE_GETTID
    return (uint64_t)gettid();
#else
    return (uint64_t)syscall(__NR_gettid);
#endif
}

static bool nvenc_surface_dim_matches_encode(uint32_t surface_dim,
                                             uint32_t visible_dim,
                                             uint32_t coded_dim,
                                             bool allow_pre_seq_cropped)
{
    if (surface_dim == visible_dim || surface_dim == coded_dim) {
        return true;
    }
    if (!allow_pre_seq_cropped || visible_dim != coded_dim || surface_dim >= coded_dim) {
        return false;
    }
    return ROUND_UP(surface_dim, 16) == coded_dim;
}

uint32_t nvenc_profile_coded_dim(VAProfile profile, uint32_t visible_dim)
{
    if (visible_dim == 0) {
        return 0;
    }
    if (isH264EncodeProfile(profile)) {
        return ROUND_UP(visible_dim, 16);
    }
    return visible_dim;
}

uint32_t nvenc_context_coded_width(const NVContext *nvCtx)
{
    if (!nvCtx) {
        return 0;
    }
    if (nvCtx->enc && nvCtx->enc->haveSeq && nvCtx->enc->seqParams.picture_width_in_mbs > 0) {
        return (uint32_t)nvCtx->enc->seqParams.picture_width_in_mbs * 16u;
    }
    return nvenc_profile_coded_dim(nvCtx->profile, (uint32_t)nvCtx->width);
}

uint32_t nvenc_context_coded_height(const NVContext *nvCtx)
{
    if (!nvCtx) {
        return 0;
    }
    if (nvCtx->enc && nvCtx->enc->haveSeq && nvCtx->enc->seqParams.picture_height_in_mbs > 0) {
        return (uint32_t)nvCtx->enc->seqParams.picture_height_in_mbs * 16u;
    }
    return nvenc_profile_coded_dim(nvCtx->profile, (uint32_t)nvCtx->height);
}

VAStatus nvenc_validate_render_targets(NVDriver *drv,
                                       uint32_t visible_width,
                                       uint32_t visible_height,
                                       uint32_t coded_width,
                                       uint32_t coded_height,
                                       VASurfaceID *render_targets,
                                       int num_render_targets,
                                       bool allow_pre_seq_cropped)
{
    if (num_render_targets <= 0) {
        return VA_STATUS_SUCCESS;
    }
    if (!drv || !render_targets) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    for (int i = 0; i < num_render_targets; i++) {
        NVEncSurfaceInfo surface_info;
        if (!nvenc_get_surface_info(drv, render_targets[i], &surface_info)) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (surface_info.format != cudaVideoSurfaceFormat_NV12 ||
            surface_info.chromaFormat != cudaVideoChromaFormat_420 ||
            surface_info.bitDepth != 8) {
            LOG("Encode render target %d format mismatch: fmt=%d chroma=%d bitDepth=%d",
                render_targets[i], surface_info.format, surface_info.chromaFormat, surface_info.bitDepth);
            return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        }
        if ((surface_info.width & 1u) != 0 || (surface_info.height & 1u) != 0) {
            LOG("Encode render target %d is not 4:2:0 aligned: %ux%u",
                render_targets[i], surface_info.width, surface_info.height);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (surface_info.width > coded_width || surface_info.height > coded_height ||
            !nvenc_surface_dim_matches_encode(surface_info.width,
                                              visible_width,
                                              coded_width,
                                              allow_pre_seq_cropped) ||
            !nvenc_surface_dim_matches_encode(surface_info.height,
                                              visible_height,
                                              coded_height,
                                              allow_pre_seq_cropped)) {
            LOG("Encode render target %d size mismatch: surface %ux%u visible %ux%u coded %ux%u pre_seq_crop=%d",
                render_targets[i],
                surface_info.width,
                surface_info.height,
                visible_width,
                visible_height,
                coded_width,
                coded_height,
                allow_pre_seq_cropped ? 1 : 0);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
    }

    return VA_STATUS_SUCCESS;
}

static bool nvenc_surface_matches_encode_context(const NVSurface *surface,
                                                 const NVContext *nvCtx)
{
    if (!surface || !nvCtx || !nvCtx->isEncode || !nvCtx->enc) {
        return false;
    }

    if (surface->format != cudaVideoSurfaceFormat_NV12 ||
        surface->chromaFormat != cudaVideoChromaFormat_420 ||
        surface->bitDepth != 8 ||
        (surface->width & 1u) != 0 ||
        (surface->height & 1u) != 0) {
        return false;
    }

    uint32_t visible_width = (uint32_t)nvCtx->width;
    uint32_t visible_height = (uint32_t)nvCtx->height;
    uint32_t coded_width = nvenc_context_coded_width(nvCtx);
    uint32_t coded_height = nvenc_context_coded_height(nvCtx);
    bool allow_pre_seq_cropped = !nvCtx->enc->haveSeq;

    if (coded_width == 0 || coded_height == 0 ||
        surface->width > coded_width ||
        surface->height > coded_height) {
        return false;
    }

    return nvenc_surface_dim_matches_encode(surface->width,
                                            visible_width,
                                            coded_width,
                                            allow_pre_seq_cropped) &&
           nvenc_surface_dim_matches_encode(surface->height,
                                            visible_height,
                                            coded_height,
                                            allow_pre_seq_cropped);
}

bool nvenc_try_bind_surface_to_encode_context(NVDriver *drv,
                                              NVSurface *surface)
{
    if (!drv || !surface || surface->context || surface->encCtx) {
        return false;
    }

    uint64_t current_tid = nvenc_gettid();
    NVContext *best_same_ctx = NULL;
    VAGenericID best_same_id = VA_INVALID_ID;
    NVContext *best_any_ctx = NULL;
    VAGenericID best_any_id = VA_INVALID_ID;
    uint32_t total_matches = 0;

    pthread_mutex_lock(&drv->objectCreationMutex);
    ARRAY_FOR_EACH(Object, o, &drv->objects)
        if (!o || o->type != OBJECT_TYPE_CONTEXT || !o->obj) {
            continue;
        }

        NVContext *nvCtx = (NVContext *)o->obj;
        if (!nvenc_surface_matches_encode_context(surface, nvCtx)) {
            continue;
        }

        total_matches++;
        if (best_any_id == VA_INVALID_ID || o->id > best_any_id) {
            best_any_ctx = nvCtx;
            best_any_id = o->id;
        }

        if (nvCtx->ownerTid == current_tid) {
            if (best_same_id == VA_INVALID_ID || o->id > best_same_id) {
                best_same_ctx = nvCtx;
                best_same_id = o->id;
            }
        }
    END_FOR_EACH

    NVContext *bound_ctx = NULL;
    if (best_same_ctx) {
        bound_ctx = best_same_ctx;
    } else if (total_matches == 1) {
        bound_ctx = best_any_ctx;
    }

    if (bound_ctx) {
        surface->context = bound_ctx;
    }
    pthread_mutex_unlock(&drv->objectCreationMutex);

    return bound_ctx != NULL;
}

bool nvenc_surface_has_encode_context(const NVSurface *surface)
{
    return surface && surface->encCtx && surface->encCtx->isEncode;
}

bool nvenc_surface_uses_encode_path(const NVSurface *surface)
{
    NVContext *surface_ctx = NULL;

    if (!surface) {
        return false;
    }

    surface_ctx = (NVContext *)surface->context;
    return nvenc_surface_has_encode_context(surface) ||
           (surface_ctx && surface_ctx->isEncode) ||
           (!surface_ctx && surface->encDevPtr != 0);
}

VAStatus nvenc_query_surface_attributes(NVDriver *drv,
                                        VASurfaceAttrib *attrib_list,
                                        unsigned int *num_attribs)
{
    if (!drv) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (!nvenc_query_encode_caps(drv) ||
        drv->nvencCaps.widthMax <= 0 ||
        drv->nvencCaps.heightMax <= 0) {
        LOG("NVENC caps missing encode surface limits; refusing encode attributes");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (num_attribs == NULL) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    uint32_t external_mem_types = VA_SURFACE_ATTRIB_MEM_TYPE_VA |
                                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME |
                                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    const NVEncSurfaceAttribInit attrs[] = {
        { VASurfaceAttribMinWidth, VA_SURFACE_ATTRIB_GETTABLE, VAGenericValueTypeInteger, drv->nvencCaps.widthMin },
        { VASurfaceAttribMinHeight, VA_SURFACE_ATTRIB_GETTABLE, VAGenericValueTypeInteger, drv->nvencCaps.heightMin },
        { VASurfaceAttribMaxWidth, VA_SURFACE_ATTRIB_GETTABLE, VAGenericValueTypeInteger, drv->nvencCaps.widthMax },
        { VASurfaceAttribMaxHeight, VA_SURFACE_ATTRIB_GETTABLE, VAGenericValueTypeInteger, drv->nvencCaps.heightMax },
        { VASurfaceAttribPixelFormat, VA_SURFACE_ATTRIB_GETTABLE, VAGenericValueTypeInteger, VA_FOURCC_NV12 },
        { VASurfaceAttribMemoryType, VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE,
          VAGenericValueTypeInteger, external_mem_types },
        { VASurfaceAttribExternalBufferDescriptor, VA_SURFACE_ATTRIB_SETTABLE, VAGenericValueTypePointer, 0 },
    };
    const unsigned int required = ARRAY_SIZE(attrs);
    if (attrib_list == NULL) {
        *num_attribs = required;
        return VA_STATUS_SUCCESS;
    }
    if (*num_attribs < required) {
        *num_attribs = required;
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    *num_attribs = required;

    for (unsigned int i = 0; i < required; i++) {
        attrib_list[i].type = attrs[i].type;
        attrib_list[i].flags = attrs[i].flags;
        attrib_list[i].value.type = attrs[i].value_type;
        if (attrs[i].value_type == VAGenericValueTypePointer) {
            attrib_list[i].value.value.p = NULL;
        } else {
            attrib_list[i].value.value.i = attrs[i].value;
        }
    }

    return VA_STATUS_SUCCESS;
}
