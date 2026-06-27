#define _GNU_SOURCE
#include "nvenc-ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>

/* Reliable send: loop until all bytes sent */
static bool send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

/* Reliable recv: loop until all bytes received */
static bool recv_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

bool nvenc_ipc_get_socket_path(char *buf, size_t bufsize)
{
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir == NULL) {
        runtime_dir = "/tmp";
    }
    int ret = snprintf(buf, bufsize, "%s/%s", runtime_dir, NVENC_IPC_SOCK_NAME);
    return ret > 0 && (size_t)ret < bufsize;
}

int nvenc_ipc_connect(void)
{
    char path[256];
    if (!nvenc_ipc_get_socket_path(path, sizeof(path))) {
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

int nvenc_ipc_connect_or_start(const char *helper_path)
{
    /* Try connecting first */
    int fd = nvenc_ipc_connect();
    if (fd >= 0) {
        return fd;
    }

    /* Helper not running — start it */
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        /* Child: exec the helper.
         * Detach from parent's session so it survives parent exit. */
        setsid();

        /* Close inherited fds */
        for (int i = 3; i < 1024; i++) {
            close(i);
        }

        /* Redirect stdout/stderr to /dev/null unless NVD_LOG is set */
        if (getenv("NVD_LOG") == NULL) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }

        execl(helper_path, helper_path, NULL);
        _exit(127);
    }

    /* Parent: wait for the helper to create the socket */
    for (int attempt = 0; attempt < 50; attempt++) {
        usleep(100000); /* 100ms */
        fd = nvenc_ipc_connect();
        if (fd >= 0) {
            return fd;
        }
    }

    /* Timed out — kill the child */
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    return -1;
}

/* Receive a single fd via SCM_RIGHTS */
static int recv_fd(int sock, void *buf, size_t len)
{
    struct iovec iov = { .iov_base = buf, .iov_len = len };
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_buf;
    memset(&cmsg_buf, 0, sizeof(cmsg_buf));

    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg_buf.buf,
        .msg_controllen = sizeof(cmsg_buf.buf),
    };

    ssize_t n = recvmsg(sock, &msg, 0);
    if (n != (ssize_t)len) return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        int received_fd = -1;
        memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));
        return received_fd;
    }
    return -1;
}

int nvenc_ipc_init(int fd, const NVEncIPCInitParams *params,
                   int *shm_fd_out, uint32_t *shm_size_out)
{
    NVEncIPCMsgHeader hdr = {
        .cmd = NVENC_IPC_CMD_INIT,
        .payload_size = sizeof(*params)
    };

    if (!send_all(fd, &hdr, sizeof(hdr))) return -1;
    if (!send_all(fd, params, sizeof(*params))) return -1;

    /* Response includes shm fd via SCM_RIGHTS + NVEncIPCInitResponse payload */
    NVEncIPCRespHeader resp;
    NVEncIPCInitResponse init_resp = {0};

    int shm_fd = recv_fd(fd, &resp, sizeof(resp));

    if (resp.status != 0) {
        if (shm_fd >= 0) close(shm_fd);
        return resp.status;
    }

    if (resp.payload_size >= sizeof(init_resp)) {
        if (!recv_all(fd, &init_resp, sizeof(init_resp))) {
            if (shm_fd >= 0) close(shm_fd);
            return -1;
        }
    }

    if (shm_fd_out) {
        *shm_fd_out = shm_fd;
    } else if (shm_fd >= 0) {
        close(shm_fd);
    }
    if (shm_size_out) *shm_size_out = init_resp.shm_size;

    return 0;
}

int nvenc_ipc_encode(int fd, const void *frame_data,
                     uint32_t width, uint32_t height, uint32_t frame_size,
                     uint32_t force_idr,
                     void **bitstream_out, uint32_t *bitstream_size_out)
{
    NVEncIPCEncodeParams enc_params = {
        .width = width,
        .height = height,
        .frame_size = frame_size,
        .force_idr = force_idr,
    };

    NVEncIPCMsgHeader hdr = {
        .cmd = NVENC_IPC_CMD_ENCODE,
        .payload_size = sizeof(enc_params) + frame_size
    };

    if (!send_all(fd, &hdr, sizeof(hdr))) return -1;
    if (!send_all(fd, &enc_params, sizeof(enc_params))) return -1;
    if (!send_all(fd, frame_data, frame_size)) return -1;

    NVEncIPCRespHeader resp;
    if (!recv_all(fd, &resp, sizeof(resp))) return -1;

    if (resp.status != 0) {
        *bitstream_out = NULL;
        *bitstream_size_out = 0;
        return resp.status;
    }

    if (resp.payload_size > 0) {
        void *data = malloc(resp.payload_size);
        if (data == NULL) return -1;
        if (!recv_all(fd, data, resp.payload_size)) {
            free(data);
            return -1;
        }
        *bitstream_out = data;
        *bitstream_size_out = resp.payload_size;
    } else {
        *bitstream_out = NULL;
        *bitstream_size_out = 0;
    }

    return 0;
}

/* Send multiple DMA-BUF fds via SCM_RIGHTS ancillary data */
static bool send_fds(int sock, const int *fds, int num_fds, const void *data, size_t len)
{
    struct iovec iov = { .iov_base = (void *)data, .iov_len = len };
    union {
        char buf[CMSG_SPACE(sizeof(int) * 4)]; /* up to 4 fds */
        struct cmsghdr align;
    } cmsg_buf;
    memset(&cmsg_buf, 0, sizeof(cmsg_buf));

    size_t fd_size = sizeof(int) * (size_t)num_fds;
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg_buf.buf,
        .msg_controllen = CMSG_SPACE(fd_size),
    };

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(fd_size);
    memcpy(CMSG_DATA(cmsg), fds, fd_size);

    ssize_t n = sendmsg(sock, &msg, MSG_NOSIGNAL);
    return n == (ssize_t)len;
}

int nvenc_ipc_encode_dmabuf(int fd, const int *dmabuf_fds, int num_fds,
                            const NVEncIPCEncodeDmaBufParams *params,
                            void **bitstream_out, uint32_t *bitstream_size_out)
{
    NVEncIPCMsgHeader hdr = {
        .cmd = NVENC_IPC_CMD_ENCODE_DMABUF,
        .payload_size = sizeof(*params)
    };

    /* Send the header normally */
    if (!send_all(fd, &hdr, sizeof(hdr))) return -1;

    /* Send the params WITH the fds attached via SCM_RIGHTS */
    if (!send_fds(fd, dmabuf_fds, num_fds, params, sizeof(*params))) return -1;

    /* Receive response */
    NVEncIPCRespHeader resp;
    if (!recv_all(fd, &resp, sizeof(resp))) return -1;

    if (resp.status != 0) {
        *bitstream_out = NULL;
        *bitstream_size_out = 0;
        return resp.status;
    }

    if (resp.payload_size > 0) {
        void *data = malloc(resp.payload_size);
        if (data == NULL) return -1;
        if (!recv_all(fd, data, resp.payload_size)) {
            free(data);
            return -1;
        }
        *bitstream_out = data;
        *bitstream_size_out = resp.payload_size;
    } else {
        *bitstream_out = NULL;
        *bitstream_size_out = 0;
    }

    return 0;
}

int nvenc_ipc_encode_shm(int fd, uint32_t width, uint32_t height,
                         uint32_t frame_size, uint32_t force_idr,
                         void **bitstream_out, uint32_t *bitstream_size_out)
{
    NVEncIPCEncodeShmParams sp = {
        .width = width,
        .height = height,
        .frame_size = frame_size,
        .force_idr = force_idr,
    };

    NVEncIPCMsgHeader hdr = {
        .cmd = NVENC_IPC_CMD_ENCODE_SHM,
        .payload_size = sizeof(sp)
    };

    /* Only send the small header + params — pixel data is already in shm */
    if (!send_all(fd, &hdr, sizeof(hdr))) return -1;
    if (!send_all(fd, &sp, sizeof(sp))) return -1;

    NVEncIPCRespHeader resp;
    if (!recv_all(fd, &resp, sizeof(resp))) return -1;

    if (resp.status != 0) {
        *bitstream_out = NULL;
        *bitstream_size_out = 0;
        return resp.status;
    }

    if (resp.payload_size > 0) {
        void *data = malloc(resp.payload_size);
        if (data == NULL) return -1;
        if (!recv_all(fd, data, resp.payload_size)) {
            free(data);
            return -1;
        }
        *bitstream_out = data;
        *bitstream_size_out = resp.payload_size;
    } else {
        *bitstream_out = NULL;
        *bitstream_size_out = 0;
    }

    return 0;
}

void nvenc_ipc_close(int fd)
{
    NVEncIPCMsgHeader hdr = {
        .cmd = NVENC_IPC_CMD_CLOSE,
        .payload_size = 0
    };
    /* Best-effort send; ignore errors since we're closing anyway */
    send_all(fd, &hdr, sizeof(hdr));

    NVEncIPCRespHeader resp;
    recv_all(fd, &resp, sizeof(resp));

    close(fd);
}
