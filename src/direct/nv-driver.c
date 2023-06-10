#define _GNU_SOURCE 1

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <drm_fourcc.h>

#include "nv-driver.h"
#include <nvidia.h>
#include "../vabackend.h"

static const NvHandle NULL_OBJECT;

static bool nv_alloc_object(int fd, int driverMajorVersion, NvHandle hRoot, NvHandle hObjectParent, NvHandle* hObjectNew,
                            NvV32 hClass, uint32_t paramSize, void* params) {
    NVOS64_PARAMETERS alloc = {
        .hRoot = hRoot,
        .hObjectParent = hObjectParent,
        .hObjectNew = *hObjectNew,
        .hClass = hClass,
        .pRightsRequested = (NvP64)NULL,
        .pAllocParms = (NvP64)params,
        .paramsSize = paramSize
    };

    //make sure we force this to 0 if the driver won't be using it
    //as we'll need to check it for the status on the way out
    if (driverMajorVersion < 535) {
        alloc.paramsSize = 0;
    }

    //v525 is the base and is 40 bytes large
    //v530 has an extra `flags` field, and is *still* 40 bytes large
    //v535 has `paramsSize` and `flags` fields, and is 48 bytes large
    int size = sizeof(NVOS64_PARAMETERS);
    if (driverMajorVersion < 535) {
        size -= 8;
    }

    int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_RM_ALLOC, size), &alloc);

    //this structure changed over the versions, make sure we read the status from the correct place
    //luckily the two new fields are the same width as the status field, so we can just read from that directly
    int status = 0;
    if (driverMajorVersion < 525) {
        status = alloc.paramsSize;
    } else if (driverMajorVersion < 535) {
        status = alloc.flags;
    } else {
        status = alloc.status;
    }

    if (ret != 0 || status != NV_OK) {
        LOG("nv_alloc_object failed: %d %X %d", ret, status, errno);
        return false;
    }

    *hObjectNew = alloc.hObjectNew;

    return true;
}

static bool nv_free_object(int fd, NvHandle hRoot, NvHandle hObject) {
    if (hObject == 0) {
        return true;
    }

    NVOS00_PARAMETERS freeParams = {
        .hRoot = hRoot,
        .hObjectParent = NULL_OBJECT,
        .hObjectOld = hObject
    };

    int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_RM_FREE, sizeof(NVOS00_PARAMETERS)), &freeParams);

    if (ret != 0 || freeParams.status != NV_OK) {
        LOG("nv_free_object failed: %d %X %d", ret, freeParams.status, errno);
        return false;
    }

    return true;
}

static bool nv_rm_control(int fd, NvHandle hClient, NvHandle hObject, NvV32 cmd, NvU32 flags, int paramSize, void* params) {
    NVOS54_PARAMETERS control = {
        .hClient = hClient,
        .hObject = hObject,
        .cmd = cmd,
        .flags = flags,
        .params = (NvP64)params,
        .paramsSize = paramSize
    };

    int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_RM_CONTROL, sizeof(NVOS54_PARAMETERS)), &control);

    if (ret != 0 || control.status != NV_OK) {
        LOG("nv_rm_control failed: %d %X %d", ret, control.status, errno);
        return false;
    }

    return true;
}

#if 0
static NvU64 nv_sys_params(int fd) {
    //read from /sys/devices/system/memory/block_size_bytes
    nv_ioctl_sys_params_t obj = { .memblock_size = 0x8000000 };

    int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_SYS_PARAMS, sizeof(obj)), &obj);

    if (ret != 0) {
        LOG("nv_sys_params failed: %d %d", ret, errno);
        return 0;
    }

    return obj.memblock_size;
}

static bool nv_card_info(int fd, nv_ioctl_card_info_t (*card_info)[32]) {
    int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_CARD_INFO, sizeof(nv_ioctl_card_info_t) * 32), card_info);

    if (ret != 0) {
        LOG("nv_card_info failed: %d %d", ret, errno);
        return false;
    }

    return ret == 0;
}
#endif

static bool nv_attach_gpus(int fd, int gpu) {
    int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_ATTACH_GPUS_TO_FD, sizeof(gpu)), &gpu);

    if (ret != 0) {
        LOG("nv_attach_gpus failed: %d %d", ret, errno);
        return false;
    }

    return ret == 0;
}

static bool nv_export_object_to_fd(int fd, int export_fd, NvHandle hClient, NvHandle hDevice, NvHandle hParent, NvHandle hObject) {
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

static bool nv_get_versions(int fd, char **versionString) {
    nv_ioctl_rm_api_version_t obj = {
        .cmd = '2' //query
    };

    int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_CHECK_VERSION_STR, sizeof(obj)), &obj);

    if (ret != 0) {
        LOG("nv_check_version failed: %d %d", ret, errno);
        return false;
    }

     *versionString = strdup(obj.versionString);

    return obj.reply == NV_RM_API_VERSION_REPLY_RECOGNIZED;
}

static bool nv0_register_fd(int nv0_fd, int nvctl_fd) {
    int ret = ioctl(nv0_fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_REGISTER_FD, sizeof(int)), &nvctl_fd);

    if (ret != 0) {
        LOG("nv0_register_fd failed: %d %d", ret, errno);
        return false;
    }

    return true;
}

static bool get_device_info(int fd, struct drm_nvidia_get_dev_info_params *devInfo) {
    int ret = ioctl(fd, DRM_IOCTL_NVIDIA_GET_DEV_INFO, devInfo);

    if (ret != 0) {
        LOG("get_device_info failed: %d %d", ret, errno);
        return false;
    }

    return true;
}

bool get_device_uuid(NVDriverContext *context, char uuid[16]) {
    NV0000_CTRL_GPU_GET_UUID_FROM_GPU_ID_PARAMS uuidParams = {
        .gpuId = context->devInfo.gpu_id,
        .flags = NV0000_CTRL_CMD_GPU_GET_UUID_FROM_GPU_ID_FLAGS_FORMAT_BINARY |
                 NV0000_CTRL_CMD_GPU_GET_UUID_FROM_GPU_ID_FLAGS_TYPE_SHA1
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
    if (!get_device_info(drmFd, &context->devInfo)) {
        return false;
    }

    LOG("Got dev info: %x %x %x %x", context->devInfo.gpu_id, context->devInfo.sector_layout, context->devInfo.page_kind_generation, context->devInfo.generic_page_kind);

    int nvctlFd = -1, nv0Fd = -1;

    nvctlFd = open("/dev/nvidiactl", O_RDWR|O_CLOEXEC);
    if (nvctlFd == -1) {
        goto err;
    }

    nv0Fd = open("/dev/nvidia0", O_RDWR|O_CLOEXEC);
    if (nv0Fd == -1) {
        goto err;
    }

    //query the version of the api
    char *ver = NULL;
    nv_get_versions(nvctlFd, &ver);
    context->driverMajorVersion = atoi(ver);
    LOG("NVIDIA kernel driver version: %s, major version: %d", ver, context->driverMajorVersion);
    free(ver);

    //allocate the root object
    bool ret = nv_alloc_object(nvctlFd, context->driverMajorVersion, NULL_OBJECT, NULL_OBJECT, &context->clientObject, NV01_ROOT_CLIENT, 0, (void*)0);
    if (!ret) {
        LOG("nv_alloc_object NV01_ROOT_CLIENT failed");
        goto err;
    }

    //attach the drm fd to this handle
    ret = nv_attach_gpus(nvctlFd, context->devInfo.gpu_id);
    if (!ret) {
        LOG("nv_attach_gpu failed");
        goto err;
    }

    //allocate the parent memory object
    NV0080_ALLOC_PARAMETERS deviceParams = {
       .hClientShare = context->clientObject
    };

    //allocate the device object
    ret = nv_alloc_object(nvctlFd, context->driverMajorVersion, context->clientObject, context->clientObject, &context->deviceObject, NV01_DEVICE_0, sizeof(deviceParams), &deviceParams);
    if (!ret) {
        LOG("nv_alloc_object NV01_DEVICE_0 failed");
        goto err;
    }

    //allocate the subdevice object
    NV2080_ALLOC_PARAMETERS subdevice = { 0 };
    ret = nv_alloc_object(nvctlFd, context->driverMajorVersion, context->clientObject, context->deviceObject, &context->subdeviceObject, NV20_SUBDEVICE_0, sizeof(subdevice), &subdevice);
    if (!ret) {
        LOG("nv_alloc_object NV20_SUBDEVICE_0 failed");
        goto err;
    }

    //TODO honestly not sure if this is needed
    ret = nv0_register_fd(nv0Fd, nvctlFd);
    if (!ret) {
        LOG("nv0_register_fd failed");
        goto err;
    }

    //figure out what page sizes are available
    //we don't actually need this at the moment
//    NV0080_CTRL_DMA_ADV_SCHED_GET_VA_CAPS_PARAMS vaParams = {0};
//    ret = nv_rm_control(nvctlFd, context->clientObject, context->deviceObject, NV0080_CTRL_CMD_DMA_ADV_SCHED_GET_VA_CAPS, 0, sizeof(vaParams), &vaParams);
//    if (!ret) {
//        LOG("NV0080_CTRL_CMD_DMA_ADV_SCHED_GET_VA_CAPS failed");
//        goto err;
//    }
//    LOG("Got big page size: %d, huge page size: %d", vaParams.bigPageSize, vaParams.hugePageSize);

    context->drmFd = drmFd;
    context->nvctlFd = nvctlFd;
    context->nv0Fd = nv0Fd;
    //context->hasHugePage = vaParams.hugePageSize != 0;

    return true;
err:

    LOG("Got error initing");
    if (nvctlFd != -1) {
        close(nvctlFd);
    }
    if (nv0Fd != -1) {
        close(nv0Fd);
    }
    return false;
}

bool free_nvdriver(NVDriverContext *context) {
    nv_free_object(context->nvctlFd, context->clientObject, context->subdeviceObject);
    nv_free_object(context->nvctlFd, context->clientObject, context->deviceObject);
    nv_free_object(context->nvctlFd, context->clientObject, context->clientObject);

    if (context->nvctlFd > 0) {
        close(context->nvctlFd);
    }
    if (context->drmFd > 0) {
        close(context->drmFd);
    }
    if (context->nv0Fd > 0) {
        close(context->nv0Fd);
    }

    memset(context, 0, sizeof(NVDriverContext));
    return true;
}

bool alloc_memory(NVDriverContext *context, uint32_t size, int *fd) {
    //allocate the buffer
    int nvctlFd2 = -1;
    NvHandle bufferObject = {0};

    //we don't have huge pages available on all hardware
    //turns out we don't need to know that anyway, although this will probably result is less optimal page size
    /*
    NvU32 pageSizeAttr = context->hasHugePage ? DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _HUGE)
                                              : DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _BIG);
    NvU32 pageSizeAttr2 = context->hasHugePage ? DRF_DEF(OS32, _ATTR2, _PAGE_SIZE_HUGE, _2MB)
                                               : 0;*/

    NV_MEMORY_ALLOCATION_PARAMS memParams = {
        .owner = context->clientObject,
        .type = NVOS32_TYPE_IMAGE,
        .flags = NVOS32_ALLOC_FLAGS_IGNORE_BANK_PLACEMENT |
                 //NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE | //this doesn't seem to be needed
                 NVOS32_ALLOC_FLAGS_MAP_NOT_REQUIRED |
                 NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM,

        .attr = //pageSizeAttr |
                DRF_DEF(OS32, _ATTR, _DEPTH, _UNKNOWN) |
                DRF_DEF(OS32, _ATTR, _FORMAT, _BLOCK_LINEAR) |
                DRF_DEF(OS32, _ATTR, _PHYSICALITY, _CONTIGUOUS),
        .format = 0,
        .width = 0,
        .height = 0,
        .size = size,
        .alignment = 0, //see flags above
        .attr2 = //pageSizeAttr2 |
                 DRF_DEF(OS32, _ATTR2, _ZBC, _PREFER_NO_ZBC) |
                 DRF_DEF(OS32, _ATTR2, _GPU_CACHEABLE, _YES)
    };
    bool ret = nv_alloc_object(context->nvctlFd, context->driverMajorVersion, context->clientObject, context->deviceObject, &bufferObject, NV01_MEMORY_LOCAL_USER, sizeof(memParams), &memParams);
    if (!ret) {
        LOG("nv_alloc_object NV01_MEMORY_LOCAL_USER failed");
        return false;
    }

    //open a new handle to return
    nvctlFd2 = open("/dev/nvidiactl", O_RDWR|O_CLOEXEC);
    if (nvctlFd2 == -1) {
        LOG("open /dev/nvidiactl failed");
        goto err;
    }

    //attach the new fd to the correct gpus
    ret = nv_attach_gpus(nvctlFd2, context->devInfo.gpu_id);
    if (!ret) {
        LOG("nv_attach_gpus failed");
        goto err;
    }

    //actually export the object
    ret = nv_export_object_to_fd(context->nvctlFd, nvctlFd2, context->clientObject, context->deviceObject, context->deviceObject, bufferObject);
    if (!ret) {
        LOG("nv_export_object_to_fd failed");
        goto err;
    }

    ret = nv_free_object(context->nvctlFd, context->clientObject, bufferObject);
    if (!ret) {
        LOG("nv_free_object failed");
        goto err;
    }

    *fd = nvctlFd2;
    return true;

 err:
    LOG("error");
    if (nvctlFd2 > 0) {
        close(nvctlFd2);
    }

    ret = nv_free_object(context->nvctlFd, context->clientObject, bufferObject);
    if (!ret) {
        LOG("nv_free_object failed");
    }

    return false;
}

bool alloc_image(NVDriverContext *context, uint32_t width, uint32_t height, uint8_t channels, uint8_t bitsPerChannel, uint32_t fourcc, NVDriverImage *image) {
    uint32_t gobWidthInBytes = 64;
    uint32_t gobHeightInBytes = 8;

    uint32_t bytesPerChannel = bitsPerChannel/8;
    uint32_t bytesPerPixel = channels * bytesPerChannel;

    //first figure out the gob layout
    uint32_t log2GobsPerBlockX = 0; //TODO not sure if these are the correct numbers to start with, but they're the largest ones i've seen used
    uint32_t log2GobsPerBlockY = height < 88 ? 3 : 4; //TODO 88 is a guess, 80px high needs 3, 112px needs 4, 96px needs 4, 88px needs 4
    uint32_t log2GobsPerBlockZ = 0;

    LOG("Calculated GOB size: %dx%d (%dx%d)", gobWidthInBytes << log2GobsPerBlockX, gobHeightInBytes << log2GobsPerBlockY, log2GobsPerBlockX, log2GobsPerBlockY);

    //These two seem to be correct, but it was discovered by trial and error so I'm not 100% sure
    uint32_t widthInBytes = ROUND_UP(width * bytesPerPixel, gobWidthInBytes << log2GobsPerBlockX);
    uint32_t alignedHeight = ROUND_UP(height, gobHeightInBytes << log2GobsPerBlockY);

    uint32_t imageSizeInBytes = widthInBytes * alignedHeight;
    uint32_t size = imageSizeInBytes;

    LOG("Aligned image size: %dx%d = %d", widthInBytes, alignedHeight, imageSizeInBytes);

    //this gets us some memory, and the fd to import into cuda
    int memFd = -1;
    bool ret = alloc_memory(context, size, &memFd);
    if (!ret) {
        LOG("alloc_memory failed");
        return false;
    }

    //now export the dma-buf
    uint32_t pitchInBlocks = widthInBytes / (gobWidthInBytes << log2GobsPerBlockX);

    //printf("got gobsPerBlock: %ux%u %u %u %u %d\n", width, height, log2GobsPerBlockX, log2GobsPerBlockY, log2GobsPerBlockZ, pitchInBlocks);
    //duplicate the fd so we don't invalidate it by importing it
    int memFd2 = dup(memFd);
    if (memFd2 == -1) {
        LOG("dup failed");
        goto err;
    }

    struct NvKmsKapiPrivImportMemoryParams nvkmsParams = {
        .memFd = memFd2,
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
        .nvkms_params_size = context->driverMajorVersion == 470 ? 0x20 : sizeof(nvkmsParams) //needs to be 0x20 in the 470 series driver
    };
    int drmret = ioctl(context->drmFd, DRM_IOCTL_NVIDIA_GEM_IMPORT_NVKMS_MEMORY, &params);
    if (drmret != 0) {
        LOG("DRM_IOCTL_NVIDIA_GEM_IMPORT_NVKMS_MEMORY failed: %d %d", drmret, errno);
        goto err;
    }

    //export dma-buf
    struct drm_prime_handle prime_handle = {
        .handle = params.handle
    };
    drmret = ioctl(context->drmFd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_handle);
    if (drmret != 0) {
        LOG("DRM_IOCTL_PRIME_HANDLE_TO_FD failed: %d %d", drmret, errno);
        goto err;
    }

    struct drm_gem_close gem_close = {
        .handle = params.handle
    };
    drmret = ioctl(context->drmFd, DRM_IOCTL_GEM_CLOSE, &gem_close);
    if (drmret != 0) {
        LOG("DRM_IOCTL_GEM_CLOSE failed: %d %d", drmret, errno);
        goto prime_err;
    }

    image->width = width;
    image->height = height;
    image->nvFd = memFd;
    image->nvFd2 = memFd2; //not sure why we can't close this one, we shouldn't need it after importing the image
    image->drmFd = prime_handle.fd;
    image->mods = DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, context->devInfo.sector_layout, context->devInfo.page_kind_generation, context->devInfo.generic_page_kind, log2GobsPerBlockY);
    image->offset = 0;
    image->pitch = widthInBytes;
    image->memorySize = imageSizeInBytes;
    image->fourcc = fourcc;

    LOG("created image: %dx%d %lx %d %x", width, height, image->mods, widthInBytes, imageSizeInBytes);

    return true;

prime_err:
    if (prime_handle.fd > 0) {
        close(prime_handle.fd);
    }

err:
    if (memFd > 0) {
        close(memFd);
    }

    return false;
}
