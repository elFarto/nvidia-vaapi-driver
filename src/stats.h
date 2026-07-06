#ifndef STATS_H
#define STATS_H

typedef enum {
    NV_STAT_DECODER_CREATES,
    NV_STAT_DECODE_PICTURES,
    NV_STAT_RESOLVE_FRAMES,
    NV_STAT_EXPORT_COPIES,
    NV_STAT_EXPORT_HOST_COPIES,
    NV_STAT_EXPORT_DESCRIPTORS,
    NV_STAT_EXPORT_DESCRIPTORS_SINGLE,
    NV_STAT_EXPORT_DESCRIPTORS_MULTI,
    NV_STAT_VIDEOPROC_REQUESTS,
    NV_STAT_VIDEOPROC_CUDA,
    NV_STAT_VIDEOPROC_CUDA_FAILURES,
    NV_STAT_VIDEOPROC_CPU_FALLBACK,
    NV_STAT_COUNT
} NVStatCounter;

struct _NVDriver;

// Reads NVD_STATS (and its optional interval) and enables periodic + final
// statistics logging on the driver.
void nvStatsInit(struct _NVDriver *drv);

// Atomically increments a counter. On NV_STAT_DECODE_PICTURES ticks it may emit
// a periodic dump once statsLogInterval pictures have been decoded.
void nvStatsIncrement(struct _NVDriver *drv, NVStatCounter counter);

// Dumps the current counters together with live backing-image accounting to the
// stats log stream. No-op unless stats are enabled.
void nvStatsLog(struct _NVDriver *drv, const char *reason);

#endif
