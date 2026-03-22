#ifndef NVD_ENCODE_BUFFER_H
#define NVD_ENCODE_BUFFER_H

#include "../vabackend.h"

VAStatus nvenc_alloc_coded_buffer(NVBuffer *buf, size_t size, const void *data);
void nvenc_release_coded_buffer(NVBuffer *buf);
void nvenc_map_coded_buffer(NVBuffer *buf, void **pbuf);
VAStatus nvenc_sync_buffer(NVDriver *drv, NVBuffer *buf, uint64_t timeout_ns);

#endif
