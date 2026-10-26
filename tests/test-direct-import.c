// Exercise the private helper with CUDA failures injected below. Section GC
// keeps the unrelated exporter paths out of this GPU-independent executable.
#include "../src/direct/direct-export-buf.c"

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

typedef struct {
    int fd;
} MockMemory;

static unsigned importCalls;
static unsigned mapCalls;
static unsigned failedMaps;
static bool failFirstImport;
static bool reuseFd;
static int reusedFd;
static struct stat sourceStat;

bool checkCudaErrors(CUresult result, const char *file, const char *function, int line) {
    (void)file;
    (void)function;
    (void)line;
    return result != CUDA_SUCCESS;
}

void logger(const char *file, const char *function, int line, const char *format, ...) {
    (void)file;
    (void)function;
    (void)line;
    (void)format;
}

void backendCloseFd(int fd, const char *reason) {
    (void)reason;
    REQUIRE(close(fd) == 0);
}

void backendReleaseTrackedNvKmsFd(int fd, const char *reason) {
    (void)fd;
    (void)reason;
}

static CUresult CUDAAPI mockImport(CUexternalMemory *out,
                                  const CUDA_EXTERNAL_MEMORY_HANDLE_DESC *desc) {
    struct stat current;
    REQUIRE(fstat(desc->handle.fd, &current) == 0);
    REQUIRE(current.st_dev == sourceStat.st_dev && current.st_ino == sourceStat.st_ino);
    importCalls++;
    if (failFirstImport && importCalls == 1) {
        REQUIRE(close(desc->handle.fd) == 0);
        return CUDA_ERROR_UNKNOWN;
    }
    MockMemory *memory = malloc(sizeof(*memory));
    REQUIRE(memory != NULL);
    memory->fd = desc->handle.fd;
    *out = (CUexternalMemory)memory;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI mockMap(CUdeviceptr *out, CUexternalMemory memory,
                               const CUDA_EXTERNAL_MEMORY_BUFFER_DESC *desc) {
    (void)memory;
    (void)desc;
    if (++mapCalls <= failedMaps) {
        return CUDA_ERROR_UNKNOWN;
    }
    *out = 0x1000;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI mockError(CUresult result, const char **message) {
    (void)result;
    *message = "injected error";
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI mockDestroy(CUexternalMemory external) {
    MockMemory *memory = (MockMemory *)external;
    REQUIRE(close(memory->fd) == 0);
    free(memory);
    if (reuseFd && reusedFd < 0) {
        reusedFd = open("/dev/zero", O_RDONLY);
        REQUIRE(reusedFd >= 0);
    }
    return CUDA_SUCCESS;
}

static void runCase(bool dmaBuf, unsigned failMaps, bool failImport,
                    bool reuse, bool expectSuccess) {
    importCalls = mapCalls = 0;
    failedMaps = failMaps;
    failFirstImport = failImport;
    reuseFd = reuse;
    reusedFd = -1;
    const int source = open("/dev/null", O_RDONLY);
    REQUIRE(source > 0);
    REQUIRE(fstat(source, &sourceStat) == 0);

    CudaFunctions functions = {
        .cuImportExternalMemory = mockImport,
        .cuExternalMemoryGetMappedBuffer = mockMap,
        .cuGetErrorString = mockError,
        .cuDestroyExternalMemory = mockDestroy,
    };
    NVDriver driver = {.cu = &functions};
    NVDriverImage image = {
        .nvFd = source, .nvFd2 = source,
        .useDmaBufHandle = dmaBuf,
        .memorySize = 4096, .width = 64, .height = 64, .pitch = 64,
    };
    NVCudaImage imported = {.importedFd = -1};
    REQUIRE(import_to_cuda_buffer(&driver, &image, &imported) == expectSuccess);
    REQUIRE(importCalls == (dmaBuf && (failMaps || failImport) ? 2u : 1u));
    if (expectSuccess) {
        REQUIRE(image.nvFd == 0 && image.nvFd2 == 0);
        REQUIRE(fcntl(source, F_GETFD) == -1 && errno == EBADF);
        REQUIRE(imported.mappedBuffer == 0x1000 && imported.mappedBufferSize == 4096);
        reuseFd = false;
        REQUIRE(mockDestroy(imported.extMem) == CUDA_SUCCESS);
    } else {
        struct stat remaining;
        REQUIRE(image.nvFd == source && image.nvFd2 == source);
        REQUIRE(fstat(source, &remaining) == 0);
        REQUIRE(remaining.st_dev == sourceStat.st_dev && remaining.st_ino == sourceStat.st_ino);
        REQUIRE(imported.extMem == NULL && imported.mappedBuffer == 0);
        REQUIRE(close(source) == 0);
    }
    if (reusedFd >= 0) {
        REQUIRE(close(reusedFd) == 0);
    }
}

int main(void) {
    runCase(true, 0, false, false, true);
    runCase(true, 1, false, false, true);
    runCase(true, 1, false, true, true);
    runCase(true, 2, false, true, false);
    runCase(true, 0, true, false, true);
    runCase(false, 1, false, false, false);
    puts("CUDA buffer import ownership: 6 cases passed");
    return 0;
}
