#include "encode_buffer.h"
#include "encode_common.h"
#include "encode_pipeline.h"

#include <string.h>

extern CudaFunctions *cu;

static uint64_t nvenc_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

VAStatus nvenc_alloc_coded_buffer(NVBuffer *buf, size_t size, const void *data)
{
    size_t copy_size = 0;

    if (!buf) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    buf->codedAllocated = size ? size : 4096;
    buf->codedBuf = malloc(buf->codedAllocated);
    buf->codedSize = 0;
    buf->codedReady = false;
    buf->ptr = NULL;
    buf->encCtx = NULL;
    buf->packedHeader = NULL;
    buf->packedHeaderSize = 0;
    buf->packedSps = NULL;
    buf->packedSpsSize = 0;
    buf->packedPps = NULL;
    buf->packedPpsSize = 0;
    memset(&buf->codedSegment, 0, sizeof(buf->codedSegment));
    if (!buf->codedBuf) {
        LOG("Unable to allocate coded buffer of %zu bytes", buf->codedAllocated);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    if (!data) {
        return VA_STATUS_SUCCESS;
    }

    copy_size = size < buf->codedAllocated ? size : buf->codedAllocated;
    memcpy(buf->codedBuf, data, copy_size);
    buf->codedSize = copy_size;
    return VA_STATUS_SUCCESS;
}

void nvenc_release_coded_buffer(NVBuffer *buf)
{
    if (!buf) {
        return;
    }

    if (buf->codedBuf != NULL) {
        free(buf->codedBuf);
        buf->codedBuf = NULL;
    }
    if (buf->packedHeader != NULL) {
        free(buf->packedHeader);
        buf->packedHeader = NULL;
    }
    if (buf->packedSps != NULL) {
        free(buf->packedSps);
        buf->packedSps = NULL;
    }
    if (buf->packedPps != NULL) {
        free(buf->packedPps);
        buf->packedPps = NULL;
    }
    buf->codedSize = 0;
    buf->codedAllocated = 0;
    buf->packedHeaderSize = 0;
    buf->packedSpsSize = 0;
    buf->packedPpsSize = 0;
    buf->codedReady = false;
    buf->encCtx = NULL;
    memset(&buf->codedSegment, 0, sizeof(buf->codedSegment));
}

void nvenc_map_coded_buffer(NVBuffer *buf, void **pbuf)
{
    if (!buf || !pbuf) {
        return;
    }

    buf->codedSegment.size = (uint32_t)buf->codedSize;
    buf->codedSegment.bit_offset = 0;
    buf->codedSegment.status = 0;
    buf->codedSegment.reserved = 0;
    buf->codedSegment.buf = buf->codedBuf;
    buf->codedSegment.next = NULL;
    *pbuf = &buf->codedSegment;
}

static bool nvenc_buffer_in_queued_pics(NVEncodeContext *enc, NVBuffer *buf)
{
    if (!enc || !buf) {
        return false;
    }
    bool found = false;
    pthread_mutex_lock(&enc->queueMutex);
    for (uint32_t i = 0; i < enc->queuedPics.size; i++) {
        NVEncQueuedPic *queued = (NVEncQueuedPic*) get_element_at(&enc->queuedPics, i);
        if (queued && queued->codedBuf == buf) {
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&enc->queueMutex);
    return found;
}

static bool nvenc_buffer_in_reorder_encode(NVEncodeContext *enc, NVBuffer *buf)
{
    if (!enc || !buf) {
        return false;
    }
    pthread_mutex_lock(&enc->reorderMutex);
    for (uint32_t i = 0; i < enc->reorderEncode.size; i++) {
        NVEncReorderEntry *entry = (NVEncReorderEntry*) get_element_at(&enc->reorderEncode, i);
        if (entry && entry->codedBuf == buf) {
            pthread_mutex_unlock(&enc->reorderMutex);
            return true;
        }
    }
    pthread_mutex_unlock(&enc->reorderMutex);
    return false;
}

VAStatus nvenc_sync_buffer(NVDriver *drv, NVBuffer *buf, uint64_t timeout_ns)
{
    if (!drv || !buf) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (buf->bufferType == VAEncCodedBufferType && buf->encCtx) {
        NVEncodeContext *enc = buf->encCtx;
        if (enc->context) {
            const bool immediate = (timeout_ns == 0);
            const bool finite_timeout = !immediate && timeout_ns != NVD_VA_TIMEOUT_INFINITE;
            uint64_t deadline_ns = 0;
            if (finite_timeout) {
                deadline_ns = nvenc_now_ns() + timeout_ns;
            }
            CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
            VAStatus st = VA_STATUS_SUCCESS;
            bool in_flight = nvenc_coded_buffer_has_pending_output(enc, buf) ||
                             nvenc_buffer_in_queued_pics(enc, buf) ||
                             nvenc_buffer_in_reorder_encode(enc, buf);
            while (!buf->codedReady && in_flight) {
                if (nvenc_use_internal_reorder(enc)) {
                    nvenc_process_reorder_queue(drv, enc->context, false);
                }
                bool resolved_any = false;
                while (true) {
                    int32_t pending_idx = nvenc_find_pending_output_index_for_coded_buffer(enc, buf);
                    if (pending_idx < 0) {
                        break;
                    }
                    bool busy = false;
                    st = nvenc_resolve_pending_output_ex(enc, (uint32_t)pending_idx, true, &busy);
                    if (st != VA_STATUS_SUCCESS) {
                        break;
                    }
                    if (busy) {
                        break;
                    }
                    resolved_any = true;
                }
                if (st != VA_STATUS_SUCCESS) {
                    break;
                }
                if (!resolved_any) {
                    if (immediate) {
                        st = NVD_VA_STATUS_ERROR_TIMEDOUT;
                        break;
                    }
                    if (finite_timeout && nvenc_now_ns() >= deadline_ns) {
                        st = NVD_VA_STATUS_ERROR_TIMEDOUT;
                        break;
                    }
                    struct timespec ts = { 0, 1000000 };
                    nanosleep(&ts, NULL);
                }
                in_flight = nvenc_coded_buffer_has_pending_output(enc, buf) ||
                            nvenc_buffer_in_queued_pics(enc, buf) ||
                            nvenc_buffer_in_reorder_encode(enc, buf);
            }
            CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
            if (st == VA_STATUS_SUCCESS && buf->codedReady) {
                return VA_STATUS_SUCCESS;
            }
            if (st == VA_STATUS_SUCCESS && in_flight) {
                return NVD_VA_STATUS_ERROR_TIMEDOUT;
            }
            return st == VA_STATUS_SUCCESS ? VA_STATUS_ERROR_OPERATION_FAILED : st;
        }
    }
    return VA_STATUS_SUCCESS;
}
