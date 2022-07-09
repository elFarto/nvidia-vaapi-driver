#define _GNU_SOURCE 1

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>

#include <drm/drm_fourcc.h>

#include "nv-driver.h"
#include "../vabackend.h"

#include "../vabackend.h"

#include "../vabackend.h"

NvV32 nv_alloc_object(int fd, NvHandle hRoot, NvHandle hObjectParent, NvHandle* hObjectNew, NvV32 hClass, void* params) {
    NVOS64_PARAMETERS alloc = {
        .hRoot = hRoot,
        .hObjectParent = hObjectParent,
        .hObjectNew = *hObjectNew,
        .hClass = hClass,
        .pRightsRequested = NULL,
        .pAllocParms = params
    };

    int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_RM_ALLOC, sizeof(NVOS64_PARAMETERS)), &alloc);

    *hObjectNew = alloc.hObjectNew;

    return ret == 0 ? alloc.status : NV_ERR_GENERIC;
}

NvV32 nv_free_object(int fd, NvHandle hRoot, NvHandle hObject) {
    NVOS00_PARAMETERS freeParams = {
        .hRoot = hRoot,
        .hObjectParent = 0, //not actually used
        .hObjectOld = hObject
    };

    int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_RM_FREE, sizeof(NVOS00_PARAMETERS)), &freeParams);

    return ret == 0 ? freeParams.status : NV_ERR_GENERIC;
}

NvV32 nv_vid_heap_control(int fd, NVOS32_PARAMETERS* params) {
    int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_RM_VID_HEAP_CONTROL, sizeof(NVOS32_PARAMETERS)), params);

    return ret == 0 ? params->status : NV_ERR_GENERIC;
}

NvV32 nv_rm_control(int fd, NvHandle hClient, NvHandle hObject, NvV32 cmd, NvU32 flags, int paramSize, void* params) {
    NVOS54_PARAMETERS control = {
        .hClient = hClient,
        .hObject = hObject,
        .cmd = cmd,
        .flags = flags,
        .params = params,
        .paramsSize = paramSize
    };

    int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_RM_CONTROL, sizeof(NVOS54_PARAMETERS)), &control);

    return ret == 0 ? control.status : NV_ERR_GENERIC;
}

NvV32 nv_check_version(int fd, char *versionString) {
    nv_ioctl_rm_api_version_t obj = {
        .cmd = 0
    };

    strcpy(obj.versionString, versionString);

    int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_CHECK_VERSION_STR, sizeof(obj)), &obj);

    return ret == 0 && obj.reply == NV_RM_API_VERSION_REPLY_RECOGNIZED ? NV_OK : NV_ERR_GENERIC;
}

NvU64 nv_sys_params(int fd) {
    //read from /sys/devices/system/memory/block_size_bytes
    nv_ioctl_sys_params_t obj = { .memblock_size = 0x8000000 };

    int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_SYS_PARAMS, sizeof(obj)), &obj);

    return ret == 0 ? obj.memblock_size : 0;
}

NvV32 nv_card_info(int fd, nv_ioctl_card_info_t (*card_info)[32]) {
    int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_CARD_INFO, sizeof(nv_ioctl_card_info_t) * 32), card_info);

    return ret == 0 ? NV_OK : NV_ERR_GENERIC;
}

NvBool nv_attach_gpus(int fd, int gpu) {
    int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_ATTACH_GPUS_TO_FD, sizeof(gpu)), &gpu);

    return ret == 0;
}

NvV32 nv_export_object_to_fd(int fd, int export_fd, NvHandle hClient, NvHandle hDevice, NvHandle hParent, NvHandle hObject) {
    NV0000_CTRL_OS_UNIX_EXPORT_OBJECT_TO_FD_PARAMS params = {
        .fd = export_fd,
        .flags = 0,
        .object = {
            .type = NV0000_CTRL_OS_UNIX_EXPORT_OBJECT_TYPE_RM,
            .data.rmObject = {
                .hDevice = hDevice,
                .hParent = hParent,
                .hObject = hObject
            }
        }
    };

    return nv_rm_control(fd, hClient, hClient, NV0000_CTRL_CMD_OS_UNIX_EXPORT_OBJECT_TO_FD, 0, sizeof(params), &params);
}

void nv0_register_fd(int nv0_fd, int nvctl_fd) {
    ioctl(nv0_fd, _IOC(_IOC_READ|_IOC_WRITE, 0x46, 0xc9, 0x4), &nvctl_fd);
}

bool get_device_uuid(NVDriverContext *context, char uuid[16]) {
    NV0000_CTRL_GPU_GET_UUID_FROM_GPU_ID_PARAMS uuidParams = {
        .gpuId = context->devInfo.gpu_id,
        .flags = NV0000_CTRL_CMD_GPU_GET_UUID_FROM_GPU_ID_FLAGS_FORMAT_BINARY | NV0000_CTRL_CMD_GPU_GET_UUID_FROM_GPU_ID_FLAGS_TYPE_SHA1
    };
    int ret = nv_rm_control(context->nvctlFd, context->clientObject, context->clientObject, NV0000_CTRL_CMD_GPU_GET_UUID_FROM_GPU_ID, 0, sizeof(uuidParams), &uuidParams);
    if (ret) {
        return false;
    }

    for (int i = 0; i < 16; i++) {
        uuid[i] = uuidParams.gpuUuid[i];
    }

    return true;
}

bool init_nvdriver(NVDriverContext *context, int drmFd) {
    LOG("Initing nvdriver...");
    int drmret = ioctl(drmFd, DRM_IOCTL_NVIDIA_GET_DEV_INFO, &context->devInfo);
    if (drmret) {
        LOG("DRM_IOCTL_NVIDIA_GET_DEV_INFO failed: %d %d", drmret);
        return false;
    }

    LOG("Got dev info: %x", context->devInfo.generic_page_kind);

    int nvctlFd = open("/dev/nvidiactl", O_RDWR|O_CLOEXEC);
    int nv0Fd = open("/dev/nvidia0", O_RDWR|O_CLOEXEC);

    //nv_check_version(nvctl_fd, "515.48.07");
    //not sure why this is called.
    //printf("sys params: %llu\n", nv_sys_params(nvctl_fd));

    //allocate the root object
    NvV32 ret = nv_alloc_object(nvctlFd, 0, 0, &context->clientObject, NV01_ROOT_CLIENT, (void*)0);
    if (ret) {
        LOG("nv_alloc_object NV01_ROOT_CLIENT failed");
        goto err;
    }

    //attach the drm fd to this handle
    nv_attach_gpus(nvctlFd, context->devInfo.gpu_id);

    //allocate the parent memory object
    NV0080_ALLOC_PARAMETERS deviceParams = {
       .hClientShare = context->clientObject,
       .vaMode = 2
    };

    //allocate the device object
    ret = nv_alloc_object(nvctlFd, context->clientObject, context->clientObject, &context->deviceObject, NV01_DEVICE_0, &deviceParams);
    if (ret) {
        LOG("nv_alloc_object NV01_DEVICE_0 failed");
        goto err;
    }

    //allocate the subdevice object
    NV2080_ALLOC_PARAMETERS subdevice = { 0 };
    ret = nv_alloc_object(nvctlFd, context->clientObject, context->deviceObject, &context->subdeviceObject, NV20_SUBDEVICE_0, &subdevice);
    if (ret) {
        LOG("nv_alloc_object NV20_SUBDEVICE_0 failed");
        goto err;
    }

    //TODO honestly not sure if this is needed
    nv0_register_fd(nv0Fd, nvctlFd);

    context->drmFd = drmFd;
    context->nvctlFd = nvctlFd;
    context->nv0Fd = nv0Fd;

    return true;
err:

    LOG("Got error initing");
    close(nvctlFd);
    return false;
}

bool free_nvdriver(NVDriverContext *context) {
    NvV32 ret = nv_free_object(context->nvctlFd, context->clientObject, context->subdeviceObject);

    ret = nv_free_object(context->nvctlFd, context->clientObject, context->deviceObject);

    ret = nv_free_object(context->nvctlFd, context->clientObject, context->clientObject);

    close(context->nvctlFd);
    close(context->drmFd);
    close(context->nv0Fd);

    memset(context, 0, sizeof(NVDriverContext));
}

int alloc_memory(NVDriverContext *context, uint32_t size, uint32_t alignment, uint32_t bpc) {
    //allocate the buffer
    NvHandle bufferObject = 0;
    NV_MEMORY_ALLOCATION_PARAMS memParams = {
        .owner = context->clientObject,
        .type = NVOS32_TYPE_IMAGE,
        .flags = NVOS32_ALLOC_FLAGS_IGNORE_BANK_PLACEMENT |
                 NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE |
                 NVOS32_ALLOC_FLAGS_MAP_NOT_REQUIRED |
                 NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM,

        .attr = ((bpc == 8 ? DRF_DEF(OS32, _ATTR, _DEPTH, _8)
                           : DRF_DEF(OS32, _ATTR, _DEPTH, _16))) |
                DRF_DEF(OS32, _ATTR, _FORMAT, _BLOCK_LINEAR) |
                DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _HUGE) |
                DRF_DEF(OS32, _ATTR, _PHYSICALITY, _CONTIGUOUS),
        .format = 0xfe, //?
        .width = 0,
        .height = 0,
        .size = size,
        .alignment = alignment,
        .attr2 = DRF_DEF(OS32, _ATTR2, _ZBC, _PREFER_NO_ZBC) |
                 DRF_DEF(OS32, _ATTR2, _GPU_CACHEABLE, _YES) |
                 DRF_DEF(OS32, _ATTR2, _PAGE_SIZE_HUGE, _2MB)
    };
    LOG("Allocating object");
    NvV32 ret = nv_alloc_object(context->nvctlFd, context->clientObject, context->deviceObject, &bufferObject, NV01_MEMORY_LOCAL_USER, &memParams);

    //open a new handle to return
    int nvctFd2 = open("/dev/nvidiactl", O_RDWR|O_CLOEXEC);

    //attach the new fd to the correct gpus
    nv_attach_gpus(nvctFd2, context->devInfo.gpu_id);

    //actually export the object
    ret = nv_export_object_to_fd(context->nvctlFd, nvctFd2, context->clientObject, context->deviceObject, context->deviceObject, bufferObject);
    if (ret) {
        close(nvctFd2);
        return -1;
    }

    //ret = nv_free_object(context->nvctlFd, context->clientObject, bufferObject);

    return nvctFd2;
}

int alloc_image(NVDriverContext *context, uint32_t width, uint32_t height, uint8_t channels, uint8_t bitsPerChannel, NVDriverImage *image) {
    uint32_t depth = 1;
    uint32_t gobWidthInBytes = 64;
    uint32_t gobHeightInBytes = 8;
    uint32_t gobDepthInBytes = 1;

    uint32_t bytesPerChannel = bitsPerChannel/8;
    uint32_t bytesPerPixel = channels * bytesPerChannel;

    //first figure out the gob layout
    uint32_t log2GobsPerBlockX = 0; //TODO not sure if these are the correct numbers to start with, but they're the largest ones i've seen used
    uint32_t log2GobsPerBlockY = 4;
    uint32_t log2GobsPerBlockZ = 0;
    if (1) {
        while (log2GobsPerBlockX > 0 && (gobWidthInBytes << (log2GobsPerBlockX - 1)) >= width * bytesPerPixel)
            log2GobsPerBlockX--;
        while (log2GobsPerBlockY > 0 && (gobHeightInBytes << (log2GobsPerBlockY - 1)) >= height)
            log2GobsPerBlockY--;
        while (log2GobsPerBlockZ > 0 && (gobDepthInBytes << (log2GobsPerBlockZ - 1)) >= depth)
            log2GobsPerBlockZ--;
    }

    printf("aligned sizes: %dx%d\n", gobWidthInBytes << log2GobsPerBlockX, gobHeightInBytes << log2GobsPerBlockY );

    //These two seem to be correct, but it was discovered by trial and error so I'm not 100% sure
    uint32_t widthInBytes = ROUND_UP(width * bytesPerPixel, gobWidthInBytes << log2GobsPerBlockX);
    uint32_t alignedHeight = ROUND_UP(height, gobHeightInBytes << log2GobsPerBlockY);

    uint32_t granularity = 65536;
    uint32_t imageSizeInBytes = widthInBytes * alignedHeight;
    uint32_t size = ROUND_UP(imageSizeInBytes, granularity);
    uint32_t alignment = 0x200000;

    //this gets us some memory, and the fd to import into cuda
    int fd = alloc_memory(context, size, alignment, bitsPerChannel);

    //now export the dma-buf
    uint32_t pitchInBlocks = widthInBytes / (gobWidthInBytes << log2GobsPerBlockX);

    //printf("got gobsPerBlock: %ux%u %u %u %u %d\n", width, height, log2GobsPerBlockX, log2GobsPerBlockY, log2GobsPerBlockZ, pitchInBlocks);
    //duplicate the fd so we don't invalidate it by importing it
    int fd2 = dup(fd);

    struct NvKmsKapiPrivImportMemoryParams nvkmsParams = {
        .memFd = fd2,
        .surfaceParams = {
            .layout = NvKmsSurfaceMemoryLayoutBlockLinear,
            .blockLinear = {
                .genericMemory = 0,
                .pitchInBlocks = pitchInBlocks,
                .log2GobsPerBlock.x = log2GobsPerBlockX,
                .log2GobsPerBlock.y = log2GobsPerBlockY,
                .log2GobsPerBlock.z = log2GobsPerBlockZ,
            }
        }
    };

    struct drm_nvidia_gem_import_nvkms_memory_params params = {
        .mem_size = imageSizeInBytes,
        .nvkms_params_ptr = (uint64_t) &nvkmsParams,
        .nvkms_params_size = sizeof(nvkmsParams)
    };
    int drmret = ioctl(context->drmFd, DRM_IOCTL_NVIDIA_GEM_IMPORT_NVKMS_MEMORY, &params);

    //export dma-buf
    struct drm_prime_handle prime_handle = {
        .handle = params.handle
    };
    drmret = ioctl(context->drmFd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_handle);
    //printf("got DRM_IOCTL_PRIME_HANDLE_TO_FD: %d %d\n", drmret, prime_handle.fd); fflush(stdout);

    struct drm_gem_close gem_close = {
        .handle = params.handle
    };
    drmret = ioctl(context->drmFd, DRM_IOCTL_GEM_CLOSE, &gem_close);


    image->width = width;
    image->height = height;
    image->nvFd = fd;
    image->drmFd = prime_handle.fd;
    image->mods = DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, context->devInfo.sector_layout, context->devInfo.page_kind_generation, context->devInfo.generic_page_kind, log2GobsPerBlockY);
    image->offset = 0;
    image->pitch = widthInBytes;
    image->memorySize = imageSizeInBytes;
    if (channels == 1) {
        image->fourcc = bytesPerChannel == 1 ? DRM_FORMAT_R8 : DRM_FORMAT_R16;
    } else if (channels == 2) {
        image->fourcc = bytesPerChannel == 1 ? DRM_FORMAT_RG88 : DRM_FORMAT_RG1616;
    } else {
        printf("Unknown fourcc\n");
        return false;
    }

    LOG("created image: %dx%d %lx %d %x", width, height, image->mods, widthInBytes, imageSizeInBytes);

    return true;
}
