#ifndef NVENC_IPC_H
#define NVENC_IPC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * IPC protocol between the VA-API driver and the 64-bit NVENC helper.
 *
 * When CUDA is unavailable (e.g. 32-bit process on Blackwell GPUs where
 * cuInit fails), the driver delegates encoding to a 64-bit helper process
 * via a Unix domain socket. On systems where CUDA works, the driver uses
 * NVENC directly without the helper.
 *
 * Socket path: /run/user/<uid>/nvenc-helper.sock
 *
 * All integers are in host byte order (both processes are on the same machine).
 * Messages are: header + payload. Responses are: header + payload.
 */

#define NVENC_IPC_SOCK_NAME "nvenc-helper.sock"

/* Maximum frame size we'll accept over the socket (64MB, enough for 8K NV12) */
#define NVENC_IPC_MAX_FRAME_SIZE (64 * 1024 * 1024)

/* Commands */
#define NVENC_IPC_CMD_INIT    1  /* Initialize encoder */
#define NVENC_IPC_CMD_ENCODE  2  /* Encode a frame (host pixel data) */
#define NVENC_IPC_CMD_CLOSE   3  /* Close encoder and disconnect */
#define NVENC_IPC_CMD_ENCODE_DMABUF 4  /* Encode from DMA-BUF fd (GPU zero-copy) */
#define NVENC_IPC_CMD_ENCODE_SHM   5  /* Encode from shared memory (zero-copy host) */

/* Message header (client → helper) */
typedef struct {
    uint32_t cmd;
    uint32_t payload_size;
} NVEncIPCMsgHeader;

/* Response header (helper → client) */
typedef struct {
    int32_t  status;        /* 0 = success, <0 = error code */
    uint32_t payload_size;  /* size of following data */
} NVEncIPCRespHeader;

/* CMD_INIT payload */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t codec;         /* 0 = H.264, 1 = HEVC */
    uint32_t profile;       /* VA-API profile value */
    uint32_t frameRateNum;
    uint32_t frameRateDen;
    uint32_t bitrate;
    uint32_t maxBitrate;
    uint32_t gopLength;
    uint32_t is10bit;       /* 0 = 8-bit NV12, 1 = 10-bit P010 */
} NVEncIPCInitParams;

/* CMD_ENCODE payload header (followed by frame_size bytes of NV12/P010 data) */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t frame_size;    /* total bytes of pixel data */
    uint32_t force_idr;     /* 1 = force IDR keyframe */
} NVEncIPCEncodeParams;

/* CMD_ENCODE_DMABUF payload.
 * Multiple DMA-BUF fds (one per plane) sent via SCM_RIGHTS ancillary data.
 * For NV12: 2 fds (Y plane, UV plane). */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitches[4];     /* stride per plane */
    uint32_t offsets[4];     /* offset per plane */
    uint32_t sizes[4];       /* memory size per plane */
    uint32_t num_planes;
    uint32_t bppc;           /* bytes per pixel per channel */
    uint32_t is10bit;
} NVEncIPCEncodeDmaBufParams;

/* CMD_INIT response includes a shm fd via SCM_RIGHTS.
 * The shm region is large enough for one NV12/P010 frame. */
typedef struct {
    uint32_t shm_size;          /* size of the shared memory region */
} NVEncIPCInitResponse;

/* CMD_ENCODE_SHM payload (frame data is already in shared memory) */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t frame_size;
    uint32_t force_idr;
} NVEncIPCEncodeShmParams;

/* IPC client functions (used by the driver when CUDA is unavailable) */

/* Get the socket path for this user */
bool nvenc_ipc_get_socket_path(char *buf, size_t bufsize);

/* Try to connect to the helper. Returns socket fd or -1. */
int nvenc_ipc_connect(void);

/* Start the helper if not running, then connect. Returns socket fd or -1. */
int nvenc_ipc_connect_or_start(const char *helper_path);

/* Send init command. Returns 0 on success.
 * If shm_fd_out is non-NULL, receives the shared memory fd from the helper.
 * If shm_size_out is non-NULL, receives the shm region size. */
int nvenc_ipc_init(int fd, const NVEncIPCInitParams *params,
                   int *shm_fd_out, uint32_t *shm_size_out);

/* Send frame data and receive encoded bitstream.
 * bitstream_out is malloc'd by this function, caller must free.
 * Returns 0 on success. */
int nvenc_ipc_encode(int fd, const void *frame_data,
                     uint32_t width, uint32_t height, uint32_t frame_size,
                     uint32_t force_idr,
                     void **bitstream_out, uint32_t *bitstream_size_out);

/* Send DMA-BUF fd and receive encoded bitstream (GPU zero-copy path).
 * The fd is sent via SCM_RIGHTS ancillary data.
 * bitstream_out is malloc'd by this function, caller must free.
 * Returns 0 on success. */
int nvenc_ipc_encode_dmabuf(int fd, const int *dmabuf_fds, int num_fds,
                            const NVEncIPCEncodeDmaBufParams *params,
                            void **bitstream_out, uint32_t *bitstream_size_out);

/* Encode from shared memory — frame data already written to shm.
 * Only sends a small header, no pixel data over the socket.
 * Returns 0 on success. */
int nvenc_ipc_encode_shm(int fd, uint32_t width, uint32_t height,
                         uint32_t frame_size, uint32_t force_idr,
                         void **bitstream_out, uint32_t *bitstream_size_out);

/* Send close command and close the socket. */
void nvenc_ipc_close(int fd);

#endif /* NVENC_IPC_H */
