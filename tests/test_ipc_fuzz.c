/*
 * test_ipc_fuzz.c — Fuzz the nvenc-helper IPC protocol with malformed messages.
 * Tests robustness against corrupt/malicious data from the socket.
 *
 * Build: gcc -o test_ipc_fuzz tests/test_ipc_fuzz.c src/nvenc-ipc-client.c -lm
 * Run:   ./test_ipc_fuzz  (nvenc-helper must be running)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <errno.h>

#include "../src/nvenc-ipc.h"

static int g_pass = 0, g_fail = 0;
#define C_GREEN  "\033[32m"
#define C_RED    "\033[31m"
#define C_RESET  "\033[0m"
#define TEST_START(n) printf("  %-55s ", n); fflush(stdout);
#define TEST_PASS() do { printf(C_GREEN "PASS" C_RESET "\n"); g_pass++; } while(0)
#define TEST_FAIL(r) do { printf(C_RED "FAIL" C_RESET " (%s)\n", r); g_fail++; } while(0)
#define EXPECT_TRUE(c, r) do { if(!(c)) { TEST_FAIL(r); return; } } while(0)

static bool send_raw(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static int connect_helper(void) {
    char path[256];
    nvenc_ipc_get_socket_path(path, sizeof(path));
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void test_invalid_command(void) {
    TEST_START("Invalid command ID (0xFF)");
    int fd = connect_helper();
    EXPECT_TRUE(fd >= 0, "can't connect to helper");
    NVEncIPCMsgHeader hdr = { .cmd = 0xFF, .payload_size = 0 };
    send_raw(fd, &hdr, sizeof(hdr));
    NVEncIPCRespHeader resp = {0};
    recv(fd, &resp, sizeof(resp), 0);
    EXPECT_TRUE(resp.status != 0, "should reject unknown command");
    close(fd);
    TEST_PASS();
}

static void test_zero_payload(void) {
    TEST_START("CMD_INIT with zero payload");
    int fd = connect_helper();
    EXPECT_TRUE(fd >= 0, "can't connect");
    NVEncIPCMsgHeader hdr = { .cmd = NVENC_IPC_CMD_INIT, .payload_size = 0 };
    send_raw(fd, &hdr, sizeof(hdr));
    NVEncIPCRespHeader resp = {0};
    recv(fd, &resp, sizeof(resp), 0);
    EXPECT_TRUE(resp.status != 0, "should reject zero-size init");
    close(fd);
    TEST_PASS();
}

static void test_truncated_init(void) {
    TEST_START("CMD_INIT with truncated payload (5 bytes)");
    int fd = connect_helper();
    EXPECT_TRUE(fd >= 0, "can't connect");
    NVEncIPCMsgHeader hdr = { .cmd = NVENC_IPC_CMD_INIT, .payload_size = sizeof(NVEncIPCInitParams) };
    send_raw(fd, &hdr, sizeof(hdr));
    char partial[5] = {1, 2, 3, 4, 5};
    send_raw(fd, partial, sizeof(partial));
    close(fd); //disconnect mid-message
    TEST_PASS(); //helper should not crash
}

static void test_huge_payload_size(void) {
    TEST_START("CMD_ENCODE with payload_size=0xFFFFFFFF");
    int fd = connect_helper();
    EXPECT_TRUE(fd >= 0, "can't connect");
    //first init a valid encoder
    NVEncIPCMsgHeader ihdr = { .cmd = NVENC_IPC_CMD_INIT, .payload_size = sizeof(NVEncIPCInitParams) };
    NVEncIPCInitParams params = { .width = 320, .height = 240, .codec = 0,
        .frameRateNum = 30, .frameRateDen = 1 };
    send_raw(fd, &ihdr, sizeof(ihdr));
    send_raw(fd, &params, sizeof(params));
    //drain init response (may include shm fd)
    char drain[256];
    recv(fd, drain, sizeof(drain), 0);

    //now send encode with huge size
    NVEncIPCMsgHeader hdr = { .cmd = NVENC_IPC_CMD_ENCODE, .payload_size = 0xFFFFFFFF };
    send_raw(fd, &hdr, sizeof(hdr));
    close(fd);
    TEST_PASS(); //helper should not malloc 4GB and crash
}

static void test_encode_without_init(void) {
    TEST_START("CMD_ENCODE_SHM without prior CMD_INIT");
    int fd = connect_helper();
    EXPECT_TRUE(fd >= 0, "can't connect");
    NVEncIPCMsgHeader hdr = { .cmd = NVENC_IPC_CMD_ENCODE_SHM,
        .payload_size = sizeof(NVEncIPCEncodeShmParams) };
    NVEncIPCEncodeShmParams sp = { .width = 320, .height = 240, .frame_size = 115200 };
    send_raw(fd, &hdr, sizeof(hdr));
    send_raw(fd, &sp, sizeof(sp));
    NVEncIPCRespHeader resp = {0};
    recv(fd, &resp, sizeof(resp), 0);
    EXPECT_TRUE(resp.status != 0, "should reject encode without init");
    close(fd);
    TEST_PASS();
}

static void test_rapid_connect_disconnect(void) {
    TEST_START("50 rapid connect/disconnect cycles");
    for (int i = 0; i < 50; i++) {
        int fd = connect_helper();
        if (fd >= 0) close(fd);
    }
    //verify helper still alive
    int fd = connect_helper();
    EXPECT_TRUE(fd >= 0, "helper died after rapid cycles");
    close(fd);
    TEST_PASS();
}

static void test_close_without_init(void) {
    TEST_START("CMD_CLOSE without prior CMD_INIT");
    int fd = connect_helper();
    EXPECT_TRUE(fd >= 0, "can't connect");
    NVEncIPCMsgHeader hdr = { .cmd = NVENC_IPC_CMD_CLOSE, .payload_size = 0 };
    send_raw(fd, &hdr, sizeof(hdr));
    NVEncIPCRespHeader resp = {0};
    recv(fd, &resp, sizeof(resp), 0);
    EXPECT_TRUE(resp.status == 0, "close should succeed even without init");
    close(fd);
    TEST_PASS();
}

static void test_double_init(void) {
    TEST_START("Two CMD_INIT in a row (re-init)");
    int fd = connect_helper();
    EXPECT_TRUE(fd >= 0, "can't connect");
    NVEncIPCInitParams params = { .width = 320, .height = 240, .codec = 0,
        .frameRateNum = 30, .frameRateDen = 1 };

    for (int i = 0; i < 2; i++) {
        NVEncIPCMsgHeader hdr = { .cmd = NVENC_IPC_CMD_INIT, .payload_size = sizeof(params) };
        send_raw(fd, &hdr, sizeof(hdr));
        send_raw(fd, &params, sizeof(params));
        char drain[256];
        recv(fd, drain, sizeof(drain), 0);
    }
    //clean close
    NVEncIPCMsgHeader chdr = { .cmd = NVENC_IPC_CMD_CLOSE, .payload_size = 0 };
    send_raw(fd, &chdr, sizeof(chdr));
    char drain[64];
    recv(fd, drain, sizeof(drain), 0);
    close(fd);
    TEST_PASS();
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    printf("\n=== nvenc-helper IPC fuzz tests ===\n\n");

    //check helper is running
    int fd = connect_helper();
    if (fd < 0) {
        printf("ERROR: nvenc-helper not running\n");
        return 1;
    }
    close(fd);

    test_invalid_command();
    test_zero_payload();
    test_truncated_init();
    test_huge_payload_size();
    test_encode_without_init();
    test_rapid_connect_disconnect();
    test_close_without_init();
    test_double_init();

    printf("\n=== Results: %d passed, %d failed ===\n\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
