// Exercise private validation and descriptor ownership without a GPU. As in
// test-direct-import.c, section GC drops the unrelated driver entrypoints.
#include "../src/vabackend.c"

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static bool failExport;
static int exportedFd;
static unsigned copyCalls;
static unsigned paddingCalls;
static size_t expectedPitch;
static size_t expectedWidth;

void backendCloseFd(int fd, const char *reason) {
    (void)reason;
    REQUIRE(close(fd) == 0);
}

static bool mockExport(NVDriver *drv, NVSurface *surface, VADRMPRIMESurfaceDescriptor *desc) {
    (void)drv;
    REQUIRE(surface->width == 640 && surface->height == 368);
    REQUIRE(surface->colorStandard == VAProcColorStandardBT709 && surface->colorRangeFull);
    exportedFd = dup(surface->backingImage->fds[0]);
    REQUIRE(exportedFd >= 0);
    desc->num_objects = 1;
    desc->objects[0].fd = exportedFd;
    return !failExport;
}

static void testDescriptorOwnership(void) {
    int source = memfd_create("own-buffer", MFD_CLOEXEC);
    int foreign = memfd_create("foreign-buffer", MFD_CLOEXEC);
    REQUIRE(source >= 0 && foreign >= 0);
    BackingImage img = {
        .width = 640, .height = 368, .fds = {source, -1, -1, -1},
        .colorStandard = VAProcColorStandardBT709, .colorRangeFull = true,
    };
    cacheBackingImageFdStat(&img, 0);
    void *images[] = {&img};
    const NVBackend backend = {.fillExportDescriptor = mockExport};
    NVDriver drv = {
        .backend = &backend, .imagesMutex = PTHREAD_MUTEX_INITIALIZER,
        .images = {.buf = images, .size = 1, .capacity = 1},
    };
    VADRMPRIMESurfaceDescriptor desc = {0};
    REQUIRE(describeOwnExportedBuffer(&drv, source, &desc) == VA_STATUS_SUCCESS);
    REQUIRE(exportedFd != source && fcntl(exportedFd, F_GETFD) >= 0);
    closeOwnExportDescriptor(&desc);
    REQUIRE(fcntl(exportedFd, F_GETFD) == -1 && errno == EBADF);
    REQUIRE(fcntl(source, F_GETFD) >= 0);
    REQUIRE(describeOwnExportedBuffer(&drv, foreign, &desc) == VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
    img.isExternalBuffer = true;
    REQUIRE(describeOwnExportedBuffer(&drv, source, &desc) == VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
    img.isExternalBuffer = false;
    img.borrowedCudaResources = true;
    REQUIRE(describeOwnExportedBuffer(&drv, source, &desc) == VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
    img.borrowedCudaResources = false;
    failExport = true;
    REQUIRE(describeOwnExportedBuffer(&drv, source, &desc) == VA_STATUS_ERROR_ALLOCATION_FAILED);
    REQUIRE(desc.num_objects == 0);
    REQUIRE(fcntl(exportedFd, F_GETFD) == -1 && errno == EBADF);
    REQUIRE(fcntl(source, F_GETFD) >= 0);
    REQUIRE(pthread_mutex_trylock(&drv.imagesMutex) == 0);
    REQUIRE(pthread_mutex_unlock(&drv.imagesMutex) == 0);
    REQUIRE(pthread_mutex_destroy(&drv.imagesMutex) == 0);
    REQUIRE(close(source) == 0 && close(foreign) == 0);
}

static void testCropping(void) {
    NVContext context = {.width = 1920, .height = 1088};
    VAEncSequenceParameterBufferH264 seq = {0};
    seq.picture_width_in_mbs = 120;
    seq.picture_height_in_mbs = 68;
    seq.seq_fields.bits.chroma_format_idc = 1;
    seq.seq_fields.bits.frame_mbs_only_flag = 1;
    seq.frame_cropping_flag = 1;
    seq.frame_crop_bottom_offset = 4;
    REQUIRE(applyH264EncodeCropping(&context, &seq) == VA_STATUS_SUCCESS);
    REQUIRE(context.encodeCropWidth == 1920 && context.encodeCropHeight == 1080);
    seq.frame_crop_right_offset = UINT32_MAX;
    REQUIRE(applyH264EncodeCropping(&context, &seq) == VA_STATUS_ERROR_INVALID_PARAMETER);
    seq.frame_crop_right_offset = 0;
    seq.frame_crop_bottom_offset = 544;
    REQUIRE(applyH264EncodeCropping(&context, &seq) == VA_STATUS_ERROR_INVALID_PARAMETER);
    seq.frame_crop_bottom_offset = 4;
    seq.frame_crop_top_offset = 1;
    REQUIRE(applyH264EncodeCropping(&context, &seq) == VA_STATUS_ERROR_INVALID_PARAMETER);
    seq.frame_crop_top_offset = 0;
    seq.frame_crop_left_offset = 1;
    REQUIRE(applyH264EncodeCropping(&context, &seq) == VA_STATUS_ERROR_INVALID_PARAMETER);
    seq.frame_crop_left_offset = 0;
    seq.seq_fields.bits.frame_mbs_only_flag = 0;
    REQUIRE(applyH264EncodeCropping(&context, &seq) == VA_STATUS_ERROR_INVALID_PARAMETER);
    seq.seq_fields.bits.frame_mbs_only_flag = 1;
    seq.picture_width_in_mbs = 0;
    REQUIRE(applyH264EncodeCropping(&context, &seq) == VA_STATUS_ERROR_INVALID_PARAMETER);
    seq.picture_width_in_mbs = 120;
    context.width = 1280;
    REQUIRE(applyH264EncodeCropping(&context, &seq) == VA_STATUS_ERROR_INVALID_PARAMETER);
    context.width = 1920;
    context.height = 1080;
    context.encodeSessionInitialized = true;
    REQUIRE(applyH264EncodeCropping(&context, &seq) == VA_STATUS_SUCCESS);
    seq.frame_crop_bottom_offset = 5;
    REQUIRE(applyH264EncodeCropping(&context, &seq) == VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
    seq.frame_cropping_flag = 0;
    REQUIRE(applyH264EncodeCropping(&context, &seq) == VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
    context.encodeSessionInitialized = false;
    REQUIRE(applyH264EncodeCropping(&context, &seq) == VA_STATUS_SUCCESS);
    REQUIRE(context.encodeCropWidth == 0 && context.encodeCropHeight == 0);
    const unsigned dimensions[][2] = {{1920, 1080}, {1600, 900}, {640, 360}, {638, 358}};
    for (unsigned i = 0; i < ARRAY_SIZE(dimensions); i++) {
        const unsigned width = dimensions[i][0], height = dimensions[i][1];
        context.width = ROUND_UP(width, 16);
        context.height = ROUND_UP(height, 16);
        seq.picture_width_in_mbs = context.width / 16;
        seq.picture_height_in_mbs = context.height / 16;
        seq.frame_cropping_flag = 1;
        seq.frame_crop_right_offset = (context.width - width) / 2;
        seq.frame_crop_bottom_offset = (context.height - height) / 2;
        REQUIRE(applyH264EncodeCropping(&context, &seq) == VA_STATUS_SUCCESS);
        REQUIRE(context.encodeCropWidth == width && context.encodeCropHeight == height);
    }
}

static CUresult CUDAAPI mockCopy(const CUDA_MEMCPY2D *copy) {
    REQUIRE(copy->WidthInBytes == expectedWidth && copy->dstPitch == expectedPitch);
    REQUIRE(copy->Height == (copyCalls == 0 ? 1080u : 540u));
    REQUIRE(copy->dstDevice == (copyCalls == 0 ? 0x1000u : 0x1000u + expectedPitch * 1080));
    copyCalls++;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI mockPadding(CUdeviceptr destination, const void *source, size_t bytes) {
    (void)source;
    REQUIRE(destination >= 0x1000u && destination + bytes <= 0x1000u + expectedPitch * 1620);
    REQUIRE((destination - 0x1000u) % expectedPitch == expectedWidth);
    REQUIRE(bytes == expectedPitch - expectedWidth);
    paddingCalls++;
    return CUDA_SUCCESS;
}

static void testStagingCopy(void) {
    CudaFunctions functions = {.cuMemcpy2D = mockCopy, .cuMemcpyHtoD = mockPadding};
    NVDriver drv = {.cu = &functions};
    NVContext context = {
        .drv = &drv, .width = 1920, .height = 1080,
        .encodeCropWidth = 1920, .encodeCropHeight = 1080,
        .encodeInputBuffer = 0x1000, .encodeInputFormat = NV_ENC_BUFFER_FORMAT_NV12,
    };
    BackingImage image = {.arrays = {(CUarray)0x10, (CUarray)0x20}};
    NVSurface surface = {
        .width = 1920, .height = 1088,
        .format = cudaVideoSurfaceFormat_NV12, .backingImage = &image,
    };
    CudaFunctions *savedCuda = cu;
    cu = &functions;
    expectedWidth = expectedPitch = context.encodeInputPitch = 1920;
    REQUIRE(copySurfaceToEncodeInputBuffer(&context, &surface));
    REQUIRE(copyCalls == 2 && paddingCalls == 0);
    copyCalls = 0;
    context.encodeCropWidth = context.encodeCropHeight = 0;
    REQUIRE(!copySurfaceToEncodeInputBuffer(&context, &surface));
    REQUIRE(copyCalls == 0);
    context.encodeCropWidth = 1920;
    context.encodeCropHeight = 1080;
    surface.height = 1078;
    REQUIRE(!copySurfaceToEncodeInputBuffer(&context, &surface));
    REQUIRE(copyCalls == 0);
    surface.height = 1088;
    surface.format = cudaVideoSurfaceFormat_P016;
    context.encodeInputFormat = NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
    expectedPitch = context.encodeInputPitch = 4096;
    expectedWidth = 3840;
    REQUIRE(copySurfaceToEncodeInputBuffer(&context, &surface));
    REQUIRE(copyCalls == 2 && paddingCalls == 1620);
    cu = savedCuda;
}

int main(void) {
    testDescriptorOwnership();
    testCropping();
    testStagingCopy();
    puts("own-buffer ownership, H.264 crop validation and staging copy bounds passed");
    return 0;
}
