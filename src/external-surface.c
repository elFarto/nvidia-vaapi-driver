#define _GNU_SOURCE

#include "external-surface.h"
#include "encode/encode_common.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drm_fourcc.h>

#define NVD_IMPORTED_OBJECT_CACHE_MAX_OBJECTS 32
#define NVD_IMPORTED_OBJECT_CACHE_MAX_BYTES (128u * 1024u * 1024u)

typedef struct {
    uint32_t objectIndex;
    uint32_t layerIndex;
    uint32_t planeIndexInLayer;
    uint32_t offset;
    uint32_t pitch;
} NVExternalPlaneBinding;

static NVFormat nv_external_format_from_va_fourcc(uint32_t fourcc)
{
    for (uint32_t i = NV_FORMAT_NONE + 1; i < ARRAY_SIZE(formatsInfo); i++) {
        if (formatsInfo[i].vaFormat.fourcc == fourcc) {
            return i;
        }
    }
    return NV_FORMAT_NONE;
}

static NVFormat nv_external_format_from_any_fourcc(uint32_t fourcc)
{
    for (uint32_t i = NV_FORMAT_NONE + 1; i < ARRAY_SIZE(formatsInfo); i++) {
        if (formatsInfo[i].vaFormat.fourcc == fourcc || formatsInfo[i].fourcc == fourcc) {
            return i;
        }
    }
    return NV_FORMAT_NONE;
}

static uint32_t nv_external_buffer_size(const VASurfaceAttribExternalBuffers *extbuf,
                                        const NVFormatInfo *fmtInfo,
                                        uint32_t width,
                                        uint32_t height)
{
    if (extbuf->data_size > 0) {
        return extbuf->data_size;
    }

    uint32_t size = 0;

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *p = &fmtInfo->plane[i];
        uint32_t plane_h = height >> p->ss.y;
        uint32_t plane_w_bytes = (width >> p->ss.x) * fmtInfo->bppc * p->channelCount;
        uint32_t pitch = extbuf->pitches[i] ? extbuf->pitches[i] : plane_w_bytes;
        uint32_t end = extbuf->offsets[i] + pitch * plane_h;
        if (end > size) {
            size = end;
        }
    }

    return size;
}

static uint32_t nv_external_buffer_object_count(const VASurfaceAttribExternalBuffers *extbuf,
                                                const NVFormatInfo *fmtInfo)
{
    (void)fmtInfo;
    if (!extbuf) {
        return 0;
    }
    return extbuf->num_buffers == 1 ? 1u : 0u;
}

static uint32_t nv_external_buffer_object_size(const VASurfaceAttribExternalBuffers *extbuf,
                                               const NVFormatInfo *fmtInfo,
                                               uint32_t width,
                                               uint32_t height,
                                               uint32_t object_idx)
{
    uint32_t size = 0;

    if (!extbuf || !fmtInfo) {
        return 0;
    }

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *p = &fmtInfo->plane[i];
        uint32_t plane_h = height >> p->ss.y;
        uint32_t plane_w_bytes = (width >> p->ss.x) * fmtInfo->bppc * p->channelCount;
        uint32_t pitch = extbuf->pitches[i] ? extbuf->pitches[i] : plane_w_bytes;
        uint32_t end = extbuf->offsets[i] + pitch * plane_h;
        if (end > size) {
            size = end;
        }
    }

    if (object_idx == 0 && extbuf->data_size > 0 && size < extbuf->data_size) {
        size = extbuf->data_size;
    }

    return size;
}

static bool nv_modifier_is_linear_or_implicit(uint64_t modifier)
{
    return modifier == DRM_FORMAT_MOD_LINEAR ||
           modifier == DRM_FORMAT_MOD_INVALID;
}

static void nv_init_legacy_imported_plane_layout(BackingImage *img,
                                                 const NVFormatInfo *fmtInfo)
{
    if (!img || !fmtInfo) {
        return;
    }

    img->numLayers = fmtInfo->numPlanes;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        img->layerDrmFormats[i] = nvGetExportLayerDrmFormat(img->format, i, fmtInfo->plane[i].fourcc);
        img->layerNumPlanes[i] = 1;
        img->planeObjectIndices[i] = 0;
        img->planeLayerIndices[i] = i;
        img->planeIndicesInLayer[i] = 0;
    }
}

static int nv_guess_prime2_plane_index(const VADRMPRIMESurfaceDescriptor *desc,
                                       uint32_t layer_idx,
                                       uint32_t plane_idx_in_layer)
{
    uint32_t layer_format;

    if (!desc || layer_idx >= desc->num_layers) {
        return -1;
    }

    layer_format = desc->layers[layer_idx].drm_format;

    if (desc->num_layers == 1 && desc->layers[layer_idx].num_planes == 2) {
        if (layer_format != DRM_FORMAT_NV12) {
            return -1;
        }
        return plane_idx_in_layer < 2 ? (int)plane_idx_in_layer : -1;
    }

    if (desc->layers[layer_idx].num_planes != 1) {
        return -1;
    }

    switch (layer_format) {
    case DRM_FORMAT_R8:
        return 0;
    case DRM_FORMAT_GR88:
    case DRM_FORMAT_RG88:
        return 1;
    default:
        if (desc->num_layers == 2) {
            return layer_idx < 2 ? (int)layer_idx : -1;
        }
        return -1;
    }
}

static bool nv_parse_prime2_plane_bindings(const VADRMPRIMESurfaceDescriptor *desc,
                                           const NVFormatInfo *fmtInfo,
                                           NVExternalPlaneBinding *bindings)
{
    bool seen_planes[ARRAY_SIZE(fmtInfo->plane)] = { false };

    if (!desc || !fmtInfo || !bindings) {
        return false;
    }

    if (fmtInfo->numPlanes != 2 || desc->num_objects == 0 || desc->num_layers == 0) {
        return false;
    }

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        bindings[i].objectIndex = UINT32_MAX;
    }

    for (uint32_t layer_idx = 0; layer_idx < desc->num_layers; layer_idx++) {
        const uint32_t num_planes = desc->layers[layer_idx].num_planes;
        if (num_planes == 0 || num_planes > ARRAY_SIZE(desc->layers[layer_idx].object_index)) {
            return false;
        }

        for (uint32_t plane_in_layer = 0; plane_in_layer < num_planes; plane_in_layer++) {
            int plane_idx = nv_guess_prime2_plane_index(desc, layer_idx, plane_in_layer);
            if (plane_idx < 0 || (uint32_t)plane_idx >= fmtInfo->numPlanes) {
                return false;
            }
            if (seen_planes[plane_idx]) {
                return false;
            }

            bindings[plane_idx].objectIndex = desc->layers[layer_idx].object_index[plane_in_layer];
            bindings[plane_idx].layerIndex = layer_idx;
            bindings[plane_idx].planeIndexInLayer = plane_in_layer;
            bindings[plane_idx].offset = desc->layers[layer_idx].offset[plane_in_layer];
            bindings[plane_idx].pitch = desc->layers[layer_idx].pitch[plane_in_layer];
            seen_planes[plane_idx] = true;
        }
    }

    for (uint32_t plane_idx = 0; plane_idx < fmtInfo->numPlanes; plane_idx++) {
        if (!seen_planes[plane_idx] || bindings[plane_idx].objectIndex >= desc->num_objects ||
            bindings[plane_idx].pitch == 0) {
            return false;
        }
    }

    return true;
}

static uint32_t nv_prime2_required_object_size(const VADRMPRIMESurfaceDescriptor *desc,
                                               const NVFormatInfo *fmtInfo,
                                               const NVExternalPlaneBinding *bindings,
                                               uint32_t width,
                                               uint32_t height,
                                               uint32_t object_idx)
{
    uint32_t required = 0;

    (void)desc;
    for (uint32_t plane_idx = 0; plane_idx < fmtInfo->numPlanes; plane_idx++) {
        if (bindings[plane_idx].objectIndex != object_idx) {
            continue;
        }

        const NVFormatPlane *plane = &fmtInfo->plane[plane_idx];
        uint32_t plane_h = height >> plane->ss.y;
        uint32_t plane_w_bytes = (width >> plane->ss.x) * fmtInfo->bppc * plane->channelCount;
        uint32_t pitch = bindings[plane_idx].pitch ? bindings[plane_idx].pitch : plane_w_bytes;
        uint32_t end = bindings[plane_idx].offset + pitch * plane_h;
        if (end > required) {
            required = end;
        }
    }

    return required;
}

static bool nv_import_external_plane_array(NVDriver *drv,
                                           int import_fd,
                                           uint32_t object_size,
                                           uint32_t plane_offset,
                                           uint32_t plane_width,
                                           uint32_t plane_height,
                                           int bits_per_component,
                                           int channels,
                                           NVCudaImage *cudaImage,
                                           CUexternalMemory shared_extmem,
                                           bool owns_extmem,
                                           CUarray *array);

static void nv_release_imported_cuda_arrays(NVDriver *drv, BackingImage *img)
{
    if (!drv || !img) {
        return;
    }

    for (uint32_t i = 0; i < ARRAY_SIZE(img->cudaImages); i++) {
        if (img->cudaImages[i].mipmapArray) {
            CHECK_CUDA_RESULT(drv->cu->cuMipmappedArrayDestroy(img->cudaImages[i].mipmapArray));
            img->cudaImages[i].mipmapArray = NULL;
        }
        img->arrays[i] = NULL;
    }
}

static void nv_release_imported_cuda_views(NVDriver *drv, BackingImage *img)
{
    if (!drv || !img) {
        return;
    }

    nv_release_imported_cuda_arrays(drv, img);

    if (img->linearPtr) {
        CHECK_CUDA_RESULT(drv->cu->cuMemFree(img->linearPtr));
        img->linearPtr = 0;
    }

    for (uint32_t i = 0; i < ARRAY_SIZE(img->cudaImages); i++) {
        if (img->cudaImages[i].extMem) {
            CHECK_CUDA_RESULT(drv->cu->cuDestroyExternalMemory(img->cudaImages[i].extMem));
            img->cudaImages[i].extMem = NULL;
        }
    }
}

static void nv_free_imported_object_cache_entry(NVImportedObjectCacheEntry *entry)
{
    if (!entry) {
        return;
    }

    if (entry->hostPtr && entry->hostMapSize > 0) {
        munmap(entry->hostPtr, entry->hostMapSize);
    }
    if (entry->fd >= 0) {
        close(entry->fd);
    }
    free(entry);
}

static bool nv_get_imported_object_cache_key(int fd, uint64_t *device, uint64_t *inode)
{
    struct stat st;

    if (fd < 0 || !device || !inode) {
        return false;
    }
    if (fstat(fd, &st) != 0) {
        return false;
    }

    *device = (uint64_t)st.st_dev;
    *inode = (uint64_t)st.st_ino;
    return true;
}

static void *nv_map_host_external_object(int fd, size_t size, bool *writable)
{
    void *host_ptr;
    bool mapped_writable = true;

    if (writable) {
        *writable = false;
    }

    host_ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (host_ptr == MAP_FAILED) {
        if (errno != EACCES && errno != EPERM) {
            return MAP_FAILED;
        }

        host_ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
        if (host_ptr == MAP_FAILED) {
            return MAP_FAILED;
        }
        mapped_writable = false;
    }

    if (writable) {
        *writable = mapped_writable;
    }
    return host_ptr;
}

static NVImportedObjectCacheEntry *nv_find_imported_object_cache_locked(NVDriver *drv,
                                                                        uint64_t device,
                                                                        uint64_t inode,
                                                                        uint64_t modifier,
                                                                        uint32_t size,
                                                                        bool writable_only)
{
    NVImportedObjectCacheEntry *fallback = NULL;

    ARRAY_FOR_EACH(NVImportedObjectCacheEntry*, entry, &drv->importedObjectCache)
        if (entry->device == device && entry->inode == inode &&
            entry->modifier == modifier && entry->size == size) {
            if (entry->writable) {
                return entry;
            }
            if (!writable_only && !fallback) {
                fallback = entry;
            }
        }
    END_FOR_EACH

    return writable_only ? NULL : fallback;
}

static void nv_destroy_imported_object_cache_entry_locked(NVDriver *drv, uint32_t index)
{
    NVImportedObjectCacheEntry *entry = (NVImportedObjectCacheEntry *)get_element_at(&drv->importedObjectCache, index);
    if (!entry) {
        return;
    }

    if (drv->importedObjectCacheBytes >= entry->hostMapSize) {
        drv->importedObjectCacheBytes -= entry->hostMapSize;
    } else {
        drv->importedObjectCacheBytes = 0;
    }

    remove_element_at(&drv->importedObjectCache, index);
    nv_free_imported_object_cache_entry(entry);
}

static void nv_trim_imported_object_cache_locked(NVDriver *drv)
{
    while (get_size(&drv->importedObjectCache) > NVD_IMPORTED_OBJECT_CACHE_MAX_OBJECTS ||
           drv->importedObjectCacheBytes > NVD_IMPORTED_OBJECT_CACHE_MAX_BYTES) {
        uint32_t victim_idx = UINT32_MAX;
        uint64_t oldest = UINT64_MAX;

        ARRAY_FOR_EACH(NVImportedObjectCacheEntry*, entry, &drv->importedObjectCache)
            if (entry->refcount == 0 && entry->lastUseSeq <= oldest) {
                oldest = entry->lastUseSeq;
                victim_idx = entry_idx;
            }
        END_FOR_EACH

        if (victim_idx == UINT32_MAX) {
            break;
        }

        nv_destroy_imported_object_cache_entry_locked(drv, victim_idx);
    }
}

static NVImportedObjectCacheEntry *nv_acquire_imported_object_cache_entry(NVDriver *drv,
                                                                          int fd,
                                                                          uint64_t modifier,
                                                                          uint32_t size)
{
    NVImportedObjectCacheEntry *entry = NULL;
    uint64_t device;
    uint64_t inode;
    int cache_fd = -1;
    void *host_ptr = MAP_FAILED;
    bool writable = false;

    if (!drv || fd < 0 || size == 0) {
        return NULL;
    }
    if (!nv_get_imported_object_cache_key(fd, &device, &inode)) {
        return NULL;
    }

    pthread_mutex_lock(&drv->importedObjectCacheMutex);
    entry = nv_find_imported_object_cache_locked(drv, device, inode, modifier, size, false);
    if (entry && entry->writable) {
        entry->refcount++;
        entry->lastUseSeq = ++drv->importedObjectCacheSeq;
        pthread_mutex_unlock(&drv->importedObjectCacheMutex);
        return entry;
    }
    pthread_mutex_unlock(&drv->importedObjectCacheMutex);

    cache_fd = dup(fd);
    if (cache_fd < 0) {
        return NULL;
    }

    host_ptr = nv_map_host_external_object(cache_fd, size, &writable);
    if (host_ptr == MAP_FAILED) {
        pthread_mutex_lock(&drv->importedObjectCacheMutex);
        entry = nv_find_imported_object_cache_locked(drv, device, inode, modifier, size, false);
        if (entry) {
            entry->refcount++;
            entry->lastUseSeq = ++drv->importedObjectCacheSeq;
            pthread_mutex_unlock(&drv->importedObjectCacheMutex);
            close(cache_fd);
            return entry;
        }
        pthread_mutex_unlock(&drv->importedObjectCacheMutex);
        close(cache_fd);
        return NULL;
    }

    entry = calloc(1, sizeof(*entry));
    if (!entry) {
        munmap(host_ptr, size);
        close(cache_fd);
        return NULL;
    }

    entry->device = device;
    entry->inode = inode;
    entry->modifier = modifier;
    entry->size = size;
    entry->fd = cache_fd;
    entry->hostPtr = host_ptr;
    entry->hostMapSize = size;
    entry->writable = writable;
    entry->refcount = 1;

    pthread_mutex_lock(&drv->importedObjectCacheMutex);
    NVImportedObjectCacheEntry *existing =
        nv_find_imported_object_cache_locked(drv, device, inode, modifier, size, writable);
    if (existing) {
        existing->refcount++;
        existing->lastUseSeq = ++drv->importedObjectCacheSeq;
        pthread_mutex_unlock(&drv->importedObjectCacheMutex);
        nv_free_imported_object_cache_entry(entry);
        return existing;
    }

    entry->lastUseSeq = ++drv->importedObjectCacheSeq;
    add_element(&drv->importedObjectCache, entry);
    drv->importedObjectCacheBytes += entry->hostMapSize;
    nv_trim_imported_object_cache_locked(drv);
    pthread_mutex_unlock(&drv->importedObjectCacheMutex);

    return entry;
}

static void nv_release_imported_object_cache_entry(NVDriver *drv, NVImportedObjectCacheEntry *entry)
{
    if (!drv || !entry) {
        return;
    }

    pthread_mutex_lock(&drv->importedObjectCacheMutex);
    if (entry->refcount > 0) {
        entry->refcount--;
    }
    entry->lastUseSeq = ++drv->importedObjectCacheSeq;
    nv_trim_imported_object_cache_locked(drv);
    pthread_mutex_unlock(&drv->importedObjectCacheMutex);
}

void nv_destroy_imported_object_cache(NVDriver *drv)
{
    if (!drv) {
        return;
    }

    pthread_mutex_lock(&drv->importedObjectCacheMutex);
    while (get_size(&drv->importedObjectCache) > 0) {
        nv_destroy_imported_object_cache_entry_locked(drv, 0);
    }
    pthread_mutex_unlock(&drv->importedObjectCacheMutex);
}

static void nv_unmap_host_external_objects(NVDriver *drv, BackingImage *img)
{
    if (!img) {
        return;
    }

    for (uint32_t i = 0; i < ARRAY_SIZE(img->hostPtrs); i++) {
        if (img->hostObjects[i]) {
            nv_release_imported_object_cache_entry(drv, img->hostObjects[i]);
            img->hostObjects[i] = NULL;
        } else if (img->hostPtrs[i] && img->hostMapSizes[i] > 0) {
            munmap(img->hostPtrs[i], img->hostMapSizes[i]);
        }
        img->hostPtrs[i] = NULL;
        img->hostMapSizes[i] = 0;
        img->hostWritable[i] = false;
    }
    img->hostMappedExternal = false;
}

const void *nv_get_host_external_plane_ptr(const BackingImage *img, uint32_t plane_idx)
{
    uint32_t object_idx;

    if (!img || plane_idx >= ARRAY_SIZE(img->planeObjectIndices)) {
        return NULL;
    }

    object_idx = img->planeObjectIndices[plane_idx];
    if (object_idx >= img->numObjects || !img->hostPtrs[object_idx] ||
        img->hostMapSizes[object_idx] == 0 || img->offsets[plane_idx] < 0) {
        return NULL;
    }

    if ((size_t)img->offsets[plane_idx] > img->hostMapSizes[object_idx]) {
        return NULL;
    }

    return (const char *)img->hostPtrs[object_idx] + img->offsets[plane_idx];
}

void *nv_get_host_external_plane_ptr_mut(BackingImage *img, uint32_t plane_idx)
{
    uint32_t object_idx;

    if (!img || plane_idx >= ARRAY_SIZE(img->planeObjectIndices)) {
        return NULL;
    }

    object_idx = img->planeObjectIndices[plane_idx];
    if (object_idx >= img->numObjects || !img->hostWritable[object_idx]) {
        return NULL;
    }

    return (void *)nv_get_host_external_plane_ptr(img, plane_idx);
}

static bool nv_host_mapped_object_is_used(const BackingImage *img, uint32_t object_idx)
{
    uint32_t num_planes;

    if (!img || object_idx >= img->numObjects) {
        return false;
    }
    if (img->format <= NV_FORMAT_NONE || img->format >= ARRAY_SIZE(formatsInfo)) {
        return true;
    }

    num_planes = formatsInfo[img->format].numPlanes;
    for (uint32_t plane_idx = 0; plane_idx < num_planes; plane_idx++) {
        if (img->planeObjectIndices[plane_idx] == object_idx) {
            return true;
        }
    }

    return false;
}

static bool nv_validate_host_mapped_external_layout(const BackingImage *img,
                                                    const NVFormatInfo *fmtInfo)
{
    if (!img || !fmtInfo || !img->hostMappedExternal) {
        return false;
    }

    if (img->format != NV_FORMAT_NV12 || fmtInfo->numPlanes != 2 || img->numObjects == 0) {
        return false;
    }

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *p = &fmtInfo->plane[i];
        uint32_t object_idx = img->planeObjectIndices[i];
        uint32_t plane_h = img->height >> p->ss.y;
        uint32_t plane_w_bytes = (img->width >> p->ss.x) * fmtInfo->bppc * p->channelCount;
        size_t pitch;
        size_t offset;
        size_t plane_size;

        if (object_idx >= img->numObjects ||
            !nv_modifier_is_linear_or_implicit(img->mods[object_idx]) ||
            !img->hostPtrs[object_idx] || img->hostMapSizes[object_idx] == 0 ||
            img->offsets[i] < 0) {
            return false;
        }

        pitch = img->strides[i] > 0 ? (size_t)img->strides[i] : (size_t)plane_w_bytes;
        offset = (size_t)img->offsets[i];
        plane_size = pitch * (size_t)plane_h;

        if (pitch < plane_w_bytes || offset > img->hostMapSizes[object_idx]) {
            return false;
        }
        if (plane_size > img->hostMapSizes[object_idx] - offset) {
            return false;
        }
    }

    return true;
}

static bool nv_try_host_map_external_buffer(NVDriver *drv,
                                            BackingImage *img,
                                            const NVFormatInfo *fmtInfo)
{
    if (!img || !fmtInfo || img->numObjects == 0) {
        return false;
    }

    if (img->format != NV_FORMAT_NV12 || fmtInfo->numPlanes != 2) {
        return false;
    }

    for (uint32_t object_idx = 0; object_idx < img->numObjects; object_idx++) {
        size_t map_size;
        void *host_ptr;
        NVImportedObjectCacheEntry *cache_entry;
        bool writable = false;

        if (img->fds[object_idx] < 0 || !nv_modifier_is_linear_or_implicit(img->mods[object_idx])) {
            nv_unmap_host_external_objects(drv, img);
            return false;
        }

        map_size = img->size[object_idx] ? (size_t)img->size[object_idx] : img->linearSize;
        if (map_size == 0) {
            nv_unmap_host_external_objects(drv, img);
            return false;
        }

        cache_entry = nv_acquire_imported_object_cache_entry(drv, img->fds[object_idx],
                                                             img->mods[object_idx],
                                                             (uint32_t)map_size);
        if (cache_entry) {
            img->hostObjects[object_idx] = cache_entry;
            img->hostPtrs[object_idx] = cache_entry->hostPtr;
            img->hostMapSizes[object_idx] = cache_entry->hostMapSize;
            img->hostWritable[object_idx] = cache_entry->writable;
            continue;
        }

        host_ptr = nv_map_host_external_object(img->fds[object_idx], map_size, &writable);
        if (host_ptr == MAP_FAILED) {
            nv_unmap_host_external_objects(drv, img);
            return false;
        }

        img->hostPtrs[object_idx] = host_ptr;
        img->hostMapSizes[object_idx] = map_size;
        img->hostWritable[object_idx] = writable;
    }
    img->hostMappedExternal = true;

    if (!nv_validate_host_mapped_external_layout(img, fmtInfo)) {
        nv_unmap_host_external_objects(drv, img);
        return false;
    }

    return true;
}

static bool nv_try_import_external_cuda_buffer(NVDriver *drv,
                                               BackingImage *img,
                                               const NVFormatInfo *fmtInfo)
{
    bool have_linear = false;

    if (!drv || !img || !fmtInfo || img->numObjects != 1 || img->fds[0] < 0 ||
        img->size[0] == 0) {
        return false;
    }

    int import_fd = dup(img->fds[0]);
    if (import_fd < 0) {
        LOG("dup failed for external buffer import fd");
        return false;
    }

    CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
        .type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD,
        .handle.fd = import_fd,
        .flags = 0,
        .size = img->size[0]
    };
    if (CHECK_CUDA_RESULT(drv->cu->cuImportExternalMemory(&img->cudaImages[0].extMem, &extMemDesc))) {
        close(import_fd);
        return false;
    }

    CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufDesc = {
        .offset = 0,
        .size = img->linearSize
    };
    if (img->linearSize > 0 &&
        !CHECK_CUDA_RESULT(drv->cu->cuExternalMemoryGetMappedBuffer(&img->linearPtr,
                                                                    img->cudaImages[0].extMem,
                                                                    &bufDesc))) {
        have_linear = true;
    }

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *p = &fmtInfo->plane[i];
        if (img->offsets[i] < 0) {
            nv_release_imported_cuda_views(drv, img);
            return false;
        }
        uint32_t plane_w = img->width >> p->ss.x;
        uint32_t plane_h = img->height >> p->ss.y;
        int bpc = 8 * fmtInfo->bppc;
        if (!nv_import_external_plane_array(drv, -1, img->size[0], img->offsets[i],
                                            plane_w, plane_h, bpc, p->channelCount,
                                            &img->cudaImages[i], img->cudaImages[0].extMem,
                                            false, &img->arrays[i])) {
            if (have_linear) {
                LOG("External plane-array import failed, retrying with host-mapped fallback");
            }
            nv_release_imported_cuda_views(drv, img);
            return false;
        }
    }

    return have_linear || img->arrays[0] != NULL;
}

static bool nv_try_import_external_cuda_arrays(NVDriver *drv,
                                               BackingImage *img,
                                               const NVFormatInfo *fmtInfo)
{
    if (!drv || !img || !fmtInfo || img->format != NV_FORMAT_NV12 || fmtInfo->numPlanes != 2 ||
        img->numObjects == 0) {
        return false;
    }

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *p = &fmtInfo->plane[i];
        if (img->offsets[i] < 0) {
            nv_release_imported_cuda_views(drv, img);
            return false;
        }
        uint32_t object_idx = img->planeObjectIndices[i];
        uint32_t plane_w = img->width >> p->ss.x;
        uint32_t plane_h = img->height >> p->ss.y;
        int bpc = 8 * fmtInfo->bppc;
        CUexternalMemory shared_extmem = NULL;
        bool owns_extmem = true;
        int import_fd = -1;

        if (object_idx >= img->numObjects || img->fds[object_idx] < 0 || img->size[object_idx] == 0 ||
            !nv_modifier_is_linear_or_implicit(img->mods[object_idx])) {
            nv_release_imported_cuda_views(drv, img);
            return false;
        }

        for (uint32_t prev = 0; prev < i; prev++) {
            if (img->planeObjectIndices[prev] == object_idx && img->cudaImages[prev].extMem) {
                shared_extmem = img->cudaImages[prev].extMem;
                owns_extmem = false;
                break;
            }
        }

        if (!shared_extmem) {
            import_fd = dup(img->fds[object_idx]);
            if (import_fd < 0) {
                LOG("dup failed for external buffer import fd");
                nv_release_imported_cuda_views(drv, img);
                return false;
            }
        }

        if (!nv_import_external_plane_array(drv,
                                            import_fd,
                                            img->size[object_idx],
                                            (uint32_t)img->offsets[i],
                                            plane_w,
                                            plane_h,
                                            bpc,
                                            p->channelCount,
                                            &img->cudaImages[i],
                                            shared_extmem,
                                            owns_extmem,
                                            &img->arrays[i])) {
            nv_release_imported_cuda_views(drv, img);
            return false;
        }
    }

    return true;
}

static bool nv_try_import_external_prime2_cuda_buffer(NVDriver *drv,
                                                      BackingImage *img,
                                                      const NVFormatInfo *fmtInfo)
{
    if (!drv || !img || !fmtInfo) {
        return false;
    }

    if (img->numObjects == 1) {
        img->linearSize = img->size[0];
        return nv_try_import_external_cuda_buffer(drv, img, fmtInfo);
    }

    return nv_try_import_external_cuda_arrays(drv, img, fmtInfo);
}

static bool nv_import_external_plane_array(NVDriver *drv,
                                           int import_fd,
                                           uint32_t object_size,
                                           uint32_t plane_offset,
                                           uint32_t plane_width,
                                           uint32_t plane_height,
                                           int bits_per_component,
                                           int channels,
                                           NVCudaImage *cudaImage,
                                           CUexternalMemory shared_extmem,
                                           bool owns_extmem,
                                           CUarray *array)
{
    if (!drv || !cudaImage || !array || plane_width == 0 || plane_height == 0) {
        return false;
    }

    CUexternalMemory extMem = shared_extmem;
    if (!extMem) {
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
            .type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD,
            .handle.fd = import_fd,
            .flags = 0,
            .size = object_size
        };
        if (CHECK_CUDA_RESULT(drv->cu->cuImportExternalMemory(&extMem, &extMemDesc))) {
            if (import_fd >= 0) {
                close(import_fd);
            }
            return false;
        }
    }

    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC mipmapArrayDesc = {
        .arrayDesc = {
            .Width = plane_width,
            .Height = plane_height,
            .Depth = 0,
            .Format = bits_per_component == 8 ? CU_AD_FORMAT_UNSIGNED_INT8 : CU_AD_FORMAT_UNSIGNED_INT16,
            .NumChannels = channels,
            .Flags = 0
        },
        .numLevels = 1,
        .offset = plane_offset
    };

    if (CHECK_CUDA_RESULT(drv->cu->cuExternalMemoryGetMappedMipmappedArray(&cudaImage->mipmapArray,
                                                                           extMem,
                                                                           &mipmapArrayDesc))) {
        if (owns_extmem && extMem) {
            drv->cu->cuDestroyExternalMemory(extMem);
        }
        return false;
    }
    if (CHECK_CUDA_RESULT(drv->cu->cuMipmappedArrayGetLevel(array, cudaImage->mipmapArray, 0))) {
        drv->cu->cuMipmappedArrayDestroy(cudaImage->mipmapArray);
        cudaImage->mipmapArray = NULL;
        if (owns_extmem && extMem) {
            drv->cu->cuDestroyExternalMemory(extMem);
        }
        return false;
    }

    if (owns_extmem) {
        cudaImage->extMem = extMem;
    }
    return true;
}

bool nvBackingImageIsImportedExternal(const BackingImage *img)
{
    return img && img->importedExternal;
}

bool nvBackingImageIsHostMappedExternal(const BackingImage *img)
{
    if (!img || !img->importedExternal || !img->hostMappedExternal || img->numObjects == 0) {
        return false;
    }

    for (uint32_t i = 0; i < img->numObjects; i++) {
        if (img->fds[i] < 0 || !img->hostPtrs[i] || img->hostMapSizes[i] == 0) {
            return false;
        }
    }
    return true;
}

bool nvSyncBackingImageHostAccess(const BackingImage *img, bool write, bool start)
{
    if (!nvBackingImageIsHostMappedExternal(img)) {
        return true;
    }

    struct dma_buf_sync sync = {
        .flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) |
                 (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ)
    };

    for (uint32_t i = 0; i < img->numObjects; i++) {
        if (!nv_host_mapped_object_is_used(img, i)) {
            continue;
        }
        if (write && !img->hostWritable[i]) {
            LOG("Imported backing image object %u is host-mapped read-only", i);
            return false;
        }
        if (ioctl(img->fds[i], DMA_BUF_IOCTL_SYNC, &sync) != 0) {
            LOG("DMA_BUF_IOCTL_SYNC %s/%s failed on imported backing image object %u: %s",
                start ? "start" : "end",
                write ? "write" : "read",
                i,
                strerror(errno));
            return false;
        }
    }

    return true;
}

bool nv_copy_device_to_imported_backing_image(NVDriver *drv,
                                              BackingImage *img,
                                              uint32_t width,
                                              uint32_t height,
                                              CUdeviceptr src,
                                              uint32_t src_pitch)
{
    bool use_host_map;
    uint32_t src_y = 0;

    (void)drv;

    if (!drv || !img || !img->importedExternal || width == 0 || height == 0 ||
        width > img->width || height > img->height || src == 0 || src_pitch == 0 ||
        img->format <= NV_FORMAT_NONE || img->format >= ARRAY_SIZE(formatsInfo)) {
        return false;
    }

    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];
    use_host_map = nvBackingImageIsHostMappedExternal(img);
    if (use_host_map && !nvSyncBackingImageHostAccess(img, true, true)) {
        return false;
    }

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *p = &fmtInfo->plane[i];
        size_t plane_width = (size_t)(width >> p->ss.x) * fmtInfo->bppc * p->channelCount;
        uint32_t plane_height = height >> p->ss.y;
        size_t dst_pitch = img->strides[i] > 0 ? (size_t)img->strides[i] : plane_width;
        CUDA_MEMCPY2D memcpy2d = {
            .srcXInBytes = 0,
            .srcY = src_y,
            .srcMemoryType = CU_MEMORYTYPE_DEVICE,
            .srcDevice = src,
            .srcPitch = src_pitch,
            .dstXInBytes = 0,
            .dstY = 0,
            .WidthInBytes = plane_width,
            .Height = plane_height
        };

        if (img->arrays[i]) {
            memcpy2d.dstMemoryType = CU_MEMORYTYPE_ARRAY;
            memcpy2d.dstArray = img->arrays[i];
        } else if (img->linearPtr != 0) {
            if (img->offsets[i] < 0) {
                goto fail;
            }
            memcpy2d.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            memcpy2d.dstDevice = img->linearPtr + (size_t)img->offsets[i];
            memcpy2d.dstPitch = dst_pitch;
        } else if (use_host_map) {
            void *dst_base = nv_get_host_external_plane_ptr_mut(img, i);
            if (!dst_base) {
                goto fail;
            }
            memcpy2d.dstMemoryType = CU_MEMORYTYPE_HOST;
            memcpy2d.dstHost = dst_base;
            memcpy2d.dstPitch = dst_pitch;
        } else {
            goto fail;
        }

        if (CHECK_CUDA_RESULT(cu->cuMemcpy2D(&memcpy2d))) {
            goto fail;
        }

        src_y += plane_height;
    }

    if (use_host_map && !nvSyncBackingImageHostAccess(img, true, false)) {
        return false;
    }
    return true;

fail:
    if (use_host_map) {
        (void)nvSyncBackingImageHostAccess(img, true, false);
    }
    return false;
}

bool nvFillImportedExportDescriptor(const BackingImage *img, VADRMPRIMESurfaceDescriptor *desc)
{
    if (!img || !desc) {
        return false;
    }

    uint32_t num_objects = img->numObjects ? img->numObjects : 1;
    uint32_t num_layers = img->numLayers ? img->numLayers : 1;
    if (img->fds[0] < 0 || num_objects > ARRAY_SIZE(desc->objects) ||
        num_layers > ARRAY_SIZE(desc->layers)) {
        return false;
    }

    memset(desc, 0, sizeof(*desc));
    desc->fourcc = img->fourcc;
    desc->width = img->visibleWidth ? img->visibleWidth : img->width;
    desc->height = img->visibleHeight ? img->visibleHeight : img->height;
    desc->num_objects = num_objects;
    desc->num_layers = num_layers;

    for (uint32_t i = 0; i < num_objects; i++) {
        desc->objects[i].fd = dup(img->fds[i]);
        if (desc->objects[i].fd < 0) {
            for (uint32_t j = 0; j < i; j++) {
                close(desc->objects[j].fd);
                desc->objects[j].fd = -1;
            }
            return false;
        }
        desc->objects[i].size = img->size[i];
        desc->objects[i].drm_format_modifier = img->mods[i];
    }

    for (uint32_t layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        desc->layers[layer_idx].drm_format = img->layerDrmFormats[layer_idx];
        desc->layers[layer_idx].num_planes = img->layerNumPlanes[layer_idx];
    }

    for (uint32_t plane_idx = 0; plane_idx < ARRAY_SIZE(img->planeObjectIndices); plane_idx++) {
        uint32_t layer_idx = img->planeLayerIndices[plane_idx];
        uint32_t plane_in_layer = img->planeIndicesInLayer[plane_idx];

        if (plane_idx >= formatsInfo[img->format].numPlanes ||
            layer_idx >= num_layers ||
            plane_in_layer >= ARRAY_SIZE(desc->layers[layer_idx].object_index)) {
            continue;
        }

        desc->layers[layer_idx].object_index[plane_in_layer] = img->planeObjectIndices[plane_idx];
        desc->layers[layer_idx].offset[plane_in_layer] = img->offsets[plane_idx];
        desc->layers[layer_idx].pitch[plane_in_layer] = img->strides[plane_idx];
    }

    return true;
}

void nvDestroyImportedBackingImage(NVDriver *drv, BackingImage *img)
{
    if (!drv || !img) {
        return;
    }

    if (img->surface) {
        img->surface->backingImage = NULL;
    }

    nv_unmap_host_external_objects(drv, img);

    for (int i = 0; i < 4; i++) {
        if (img->fds[i] >= 0) {
            close(img->fds[i]);
            img->fds[i] = -1;
        }
    }

    nv_release_imported_cuda_views(drv, img);

    free(img);
}

BackingImage *nv_import_external_buffer(NVDriver *drv,
                                        NVSurface *surface,
                                        const VASurfaceAttribExternalBuffers *extbuf)
{
    if (!drv || !surface || !extbuf || !extbuf->buffers) {
        return NULL;
    }

    NVFormat fmt = nv_external_format_from_va_fourcc(extbuf->pixel_format);
    if (fmt == NV_FORMAT_NONE) {
        LOG("Unsupported external buffer format: 0x%x", extbuf->pixel_format);
        return NULL;
    }
    const NVFormatInfo *fmtInfo = &formatsInfo[fmt];
    if (extbuf->num_planes < fmtInfo->numPlanes) {
        LOG("External buffer planes mismatch: got %u need %u",
            extbuf->num_planes, fmtInfo->numPlanes);
        return NULL;
    }
    uint32_t object_count = nv_external_buffer_object_count(extbuf, fmtInfo);
    if (extbuf->num_buffers < 1 || object_count == 0) {
        LOG("External buffer missing fds");
        return NULL;
    }

    BackingImage *img = calloc(1, sizeof(BackingImage));
    if (!img) {
        return NULL;
    }
    for (uint32_t i = 0; i < ARRAY_SIZE(img->fds); i++) {
        img->fds[i] = -1;
    }

    uint32_t ext_width = extbuf->width ? extbuf->width : surface->width;
    uint32_t ext_height = extbuf->height ? extbuf->height : surface->height;
    uint32_t aligned_width = ext_width;
    uint32_t aligned_height = ext_height;
    switch (surface->chromaFormat) {
    case cudaVideoChromaFormat_422:
        aligned_width = ROUND_UP(aligned_width, 2);
        break;
    case cudaVideoChromaFormat_420:
        aligned_width = ROUND_UP(aligned_width, 2);
        aligned_height = ROUND_UP(aligned_height, 2);
        break;
    default:
        break;
    }

    if (aligned_width != surface->width || aligned_height != surface->height) {
        LOG("External buffer size mismatch: surface %ux%u ext %ux%u",
            surface->width, surface->height, ext_width, ext_height);
        free(img);
        return NULL;
    }

    img->format = fmt;
    img->importedExternal = true;
    img->width = surface->width;
    img->height = surface->height;
    img->visibleWidth = ext_width;
    img->visibleHeight = ext_height;
    img->numObjects = object_count;
    img->fourcc = extbuf->pixel_format;
    nv_init_legacy_imported_plane_layout(img, fmtInfo);
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *p = &fmtInfo->plane[i];
        uint32_t plane_w_bytes = (aligned_width >> p->ss.x) * fmtInfo->bppc * p->channelCount;
        img->strides[i] = (int)(extbuf->pitches[i] ? extbuf->pitches[i] : plane_w_bytes);
        img->offsets[i] = extbuf->offsets[i];
    }
    for (uint32_t object_idx = 0; object_idx < object_count; object_idx++) {
        int export_fd = dup((int)extbuf->buffers[object_idx]);
        if (export_fd < 0) {
            LOG("dup failed for external buffer fd");
            nvDestroyImportedBackingImage(drv, img);
            return NULL;
        }
        img->fds[object_idx] = export_fd;
        img->mods[object_idx] = DRM_FORMAT_MOD_INVALID;
        img->size[object_idx] = nv_external_buffer_object_size(extbuf, fmtInfo,
                                                               aligned_width, aligned_height,
                                                               object_idx);
    }

    if (object_count == 1) {
        img->linearSize = nv_external_buffer_size(extbuf, fmtInfo, aligned_width, aligned_height);
        if (!nv_try_import_external_cuda_buffer(drv, img, fmtInfo)) {
            if (!nv_try_host_map_external_buffer(drv, img, fmtInfo)) {
                nvDestroyImportedBackingImage(drv, img);
                return NULL;
            }
            LOG("Using host-mapped fallback for imported DRM_PRIME buffer");
        }
    }

    return img;
}

VAStatus nv_validate_prime2_buffer_import(unsigned int format,
                                          const VADRMPRIMESurfaceDescriptor *desc,
                                          cudaVideoSurfaceFormat surface_format,
                                          int bitdepth)
{
    NVExternalPlaneBinding bindings[3];
    NVFormat import_fmt;
    NVFormat expected_fmt;
    uint32_t aligned_width;
    uint32_t aligned_height;

    if (!desc) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    import_fmt = nv_external_format_from_any_fourcc(desc->fourcc);
    expected_fmt = nvenc_surface_format_to_nv_format(surface_format, bitdepth);
    if (import_fmt != NV_FORMAT_NV12 || expected_fmt != NV_FORMAT_NV12) {
        LOG("Unsupported DRM_PRIME_2 encode format: desc=0x%x expected_rt=0x%x",
            desc->fourcc, format);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    if (desc->num_objects == 0 || desc->num_layers == 0 ||
        desc->num_objects > ARRAY_SIZE(desc->objects) ||
        desc->num_layers > ARRAY_SIZE(desc->layers)) {
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    if (!nv_parse_prime2_plane_bindings(desc, &formatsInfo[import_fmt], bindings)) {
        LOG("Unsupported DRM_PRIME_2 descriptor layout");
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    aligned_width = ROUND_UP(desc->width, 2);
    aligned_height = ROUND_UP(desc->height, 2);
    for (uint32_t object_idx = 0; object_idx < desc->num_objects; object_idx++) {
        uint32_t required_size = nv_prime2_required_object_size(desc, &formatsInfo[import_fmt], bindings,
                                                                aligned_width, aligned_height, object_idx);

        if (desc->objects[object_idx].fd < 0 ||
            !nv_modifier_is_linear_or_implicit(desc->objects[object_idx].drm_format_modifier)) {
            LOG("Unsupported DRM_PRIME_2 object %u modifier=%" PRIx64,
                object_idx, desc->objects[object_idx].drm_format_modifier);
            return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        }
        if (required_size == 0) {
            return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        }
        if (desc->objects[object_idx].size != 0 && desc->objects[object_idx].size < required_size) {
            LOG("DRM_PRIME_2 object %u too small: size=%u required=%u",
                object_idx, desc->objects[object_idx].size, required_size);
            return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        }
    }

    return VA_STATUS_SUCCESS;
}

BackingImage *nv_import_external_prime2_buffer(NVDriver *drv,
                                               NVSurface *surface,
                                               const VADRMPRIMESurfaceDescriptor *desc)
{
    NVExternalPlaneBinding bindings[3];
    NVFormat fmt;
    const NVFormatInfo *fmtInfo;
    BackingImage *img;
    uint32_t ext_width;
    uint32_t ext_height;
    uint32_t aligned_width;
    uint32_t aligned_height;

    if (!drv || !surface || !desc) {
        return NULL;
    }

    fmt = nv_external_format_from_any_fourcc(desc->fourcc);
    if (fmt != NV_FORMAT_NV12) {
        LOG("Unsupported DRM_PRIME_2 fourcc: 0x%x", desc->fourcc);
        return NULL;
    }
    fmtInfo = &formatsInfo[fmt];
    if (!nv_parse_prime2_plane_bindings(desc, fmtInfo, bindings)) {
        LOG("Unable to parse DRM_PRIME_2 descriptor");
        return NULL;
    }

    ext_width = desc->width ? desc->width : surface->width;
    ext_height = desc->height ? desc->height : surface->height;
    aligned_width = ROUND_UP(ext_width, 2);
    aligned_height = ROUND_UP(ext_height, 2);
    if (aligned_width != surface->width || aligned_height != surface->height) {
        LOG("DRM_PRIME_2 size mismatch: surface %ux%u desc %ux%u",
            surface->width, surface->height, ext_width, ext_height);
        return NULL;
    }

    img = calloc(1, sizeof(BackingImage));
    if (!img) {
        return NULL;
    }
    for (uint32_t i = 0; i < ARRAY_SIZE(img->fds); i++) {
        img->fds[i] = -1;
    }

    img->format = fmt;
    img->fourcc = desc->fourcc;
    img->importedExternal = true;
    img->width = surface->width;
    img->height = surface->height;
    img->visibleWidth = ext_width;
    img->visibleHeight = ext_height;
    img->numObjects = desc->num_objects;
    img->numLayers = desc->num_layers;

    for (uint32_t layer_idx = 0; layer_idx < img->numLayers; layer_idx++) {
        img->layerDrmFormats[layer_idx] = desc->layers[layer_idx].drm_format;
        img->layerNumPlanes[layer_idx] = desc->layers[layer_idx].num_planes;
    }

    for (uint32_t plane_idx = 0; plane_idx < fmtInfo->numPlanes; plane_idx++) {
        img->planeObjectIndices[plane_idx] = bindings[plane_idx].objectIndex;
        img->planeLayerIndices[plane_idx] = bindings[plane_idx].layerIndex;
        img->planeIndicesInLayer[plane_idx] = bindings[plane_idx].planeIndexInLayer;
        img->offsets[plane_idx] = (int)bindings[plane_idx].offset;
        img->strides[plane_idx] = (int)bindings[plane_idx].pitch;
    }

    for (uint32_t object_idx = 0; object_idx < img->numObjects; object_idx++) {
        uint32_t required_size = nv_prime2_required_object_size(desc, fmtInfo, bindings,
                                                                aligned_width, aligned_height, object_idx);
        int export_fd = dup(desc->objects[object_idx].fd);
        if (export_fd < 0) {
            LOG("dup failed for DRM_PRIME_2 object fd");
            nvDestroyImportedBackingImage(drv, img);
            return NULL;
        }

        img->fds[object_idx] = export_fd;
        img->mods[object_idx] = desc->objects[object_idx].drm_format_modifier;
        img->size[object_idx] = desc->objects[object_idx].size ?
                                desc->objects[object_idx].size : required_size;
    }

    if (!nv_try_import_external_prime2_cuda_buffer(drv, img, fmtInfo) &&
        !nv_try_host_map_external_buffer(drv, img, fmtInfo)) {
        LOG("Unable to host-map DRM_PRIME_2 import");
        nvDestroyImportedBackingImage(drv, img);
        return NULL;
    }
    if (img->arrays[0] == NULL && img->linearPtr == 0) {
        LOG("Using host-mapped fallback for imported DRM_PRIME_2 buffer");
    }

    return img;
}

VAStatus nv_validate_external_buffer_import(unsigned int format,
                                            const VASurfaceAttribExternalBuffers *extbuf,
                                            cudaVideoSurfaceFormat surface_format,
                                            int bitdepth)
{
    if (!extbuf) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (extbuf->flags != 0) {
        LOG("External buffer flags 0x%x are not supported for DRM_PRIME import", extbuf->flags);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    NVFormat import_fmt = nv_external_format_from_va_fourcc(extbuf->pixel_format);
    NVFormat expected_fmt = nvenc_surface_format_to_nv_format(surface_format, bitdepth);
    if (import_fmt == NV_FORMAT_NONE || expected_fmt == NV_FORMAT_NONE) {
        LOG("Unable to validate external buffer format: ext=0x%x rt=0x%x",
            extbuf->pixel_format, format);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    if (import_fmt != expected_fmt) {
        LOG("External buffer format mismatch: ext=0x%x expected=0x%x for rt=0x%x",
            extbuf->pixel_format, formatsInfo[expected_fmt].vaFormat.fourcc, format);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    if (nv_external_buffer_object_count(extbuf, &formatsInfo[import_fmt]) == 0) {
        LOG("Unsupported DRM_PRIME external buffer layout: num_buffers=%u planes=%u. "
            "Use DRM_PRIME_2 for multi-object imports.",
            extbuf->num_buffers, extbuf->num_planes);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    return VA_STATUS_SUCCESS;
}

VAStatus nv_parse_external_import_descriptor(uint32_t mem_type,
                                             const void *external_desc,
                                             const VASurfaceAttribExternalBuffers **extbuf,
                                             const VADRMPRIMESurfaceDescriptor **prime2_desc)
{
    const uint32_t supported_mask = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME |
                                    VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    const uint32_t allowed_mask = VA_SURFACE_ATTRIB_MEM_TYPE_VA | supported_mask;
    uint32_t external_mask;
    uint32_t unsupported_mask;

    if (extbuf) {
        *extbuf = NULL;
    }
    if (prime2_desc) {
        *prime2_desc = NULL;
    }
    if (!external_desc) {
        return VA_STATUS_SUCCESS;
    }

    unsupported_mask = mem_type & ~allowed_mask;
    if (unsupported_mask != 0) {
        LOG("Unsupported external import memory type bits: 0x%x", unsupported_mask);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    external_mask = mem_type & supported_mask;
    switch (external_mask) {
    case 0:
        LOG("External buffer descriptor provided without an external import memory type");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    case VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME:
        if (extbuf) {
            *extbuf = (const VASurfaceAttribExternalBuffers *)external_desc;
        }
        return VA_STATUS_SUCCESS;
    case VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2:
        if (prime2_desc) {
            *prime2_desc = (const VADRMPRIMESurfaceDescriptor *)external_desc;
        }
        return VA_STATUS_SUCCESS;
    case VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2: {
        uint32_t prefix[4];
        uint64_t min_surface_bytes;

        memcpy(prefix, external_desc, sizeof(prefix));
        if (prefix[3] == 0 || prefix[3] > 4) {
            if (extbuf) {
                *extbuf = (const VASurfaceAttribExternalBuffers *)external_desc;
            }
            return VA_STATUS_SUCCESS;
        }

        min_surface_bytes = (uint64_t)prefix[1] * prefix[2];
        if (min_surface_bytes == 0 || (uint64_t)prefix[3] < min_surface_bytes) {
            if (prime2_desc) {
                *prime2_desc = (const VADRMPRIMESurfaceDescriptor *)external_desc;
            }
            return VA_STATUS_SUCCESS;
        }

        LOG("Ambiguous external import memory type mask 0x%x for %ux%u descriptor",
            mem_type, prefix[1], prefix[2]);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    default:
        LOG("Unsupported external import memory type: 0x%x", mem_type);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
}
