#define _GNU_SOURCE 1

#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <drm_fourcc.h>

#include "nv-driver.h"
#include <nvidia.h>

#include "../vabackend.h"

#if !defined(_IOC_READ) && defined(IOC_OUT)
#define _IOC_READ IOC_OUT
#endif

#if !defined(_IOC_WRITE) && defined(IOC_IN)
#define _IOC_WRITE IOC_IN
#endif

//Technically these can vary per architecture, but all the ones we support have the same values
#define GOB_WIDTH_IN_BYTES  64
#define GOB_HEIGHT_IN_BYTES 8

static const NvHandle NULL_OBJECT;

static bool nv_alloc_object(const int fd, const uint32_t driverMajorVersion, const NvHandle hRoot, const NvHandle hObjectParent,
                            NvHandle* hObjectNew,const NvV32 hClass, const uint32_t paramSize, void* params) {
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

    const int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_RM_ALLOC, size), &alloc);

    //this structure changed over the versions, make sure we read the status from the correct place
    //luckily the two new fields are the same width as the status field, so we can just read from that directly
    int status = 0;
    if (driverMajorVersion < 525) {
        status = (int) alloc.paramsSize;
    } else if (driverMajorVersion < 535) {
        status = (int) alloc.flags;
    } else {
        status = (int) alloc.status;
    }

    if (ret != 0 || status != NV_OK) {
        LOG("nv_alloc_object failed: %d %X %d", ret, status, errno)
        return false;
    }

    *hObjectNew = alloc.hObjectNew;

    return true;
}

static bool nv_free_object(const int fd, const NvHandle hRoot, const NvHandle hObject) {
    if (hObject == 0) {
        return true;
    }

    NVOS00_PARAMETERS freeParams = {
        .hRoot = hRoot,
        .hObjectParent = NULL_OBJECT,
        .hObjectOld = hObject
    };

    const int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_RM_FREE, sizeof(NVOS00_PARAMETERS)), &freeParams);

    if (ret != 0 || freeParams.status != NV_OK) {
        LOG("nv_free_object failed: %d %X %d", ret, freeParams.status, errno)
        return false;
    }

    return true;
}

static bool nv_rm_control(const int fd, const NvHandle hClient, const NvHandle hObject, const NvV32 cmd,
                          const NvU32 flags, const int paramSize, void* params) {
    NVOS54_PARAMETERS control = {
        .hClient = hClient,
        .hObject = hObject,
        .cmd = cmd,
        .flags = flags,
        .params = (NvP64)params,
        .paramsSize = paramSize
    };

    const int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_RM_CONTROL, sizeof(NVOS54_PARAMETERS)), &control);

    if (ret != 0 || control.status != NV_OK) {
        LOG("nv_rm_control failed: %d %X %d", ret, control.status, errno)
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

static bool nv_attach_gpus(const int fd, uint32_t gpu) {
    const int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_ATTACH_GPUS_TO_FD, sizeof(gpu)), &gpu);

    if (ret != 0) {
        LOG("nv_attach_gpus failed: %d %d", ret, errno)
        return false;
    }

    return true;
}

static bool nv_export_object_to_fd(const int fd, const int export_fd, const NvHandle hClient, const NvHandle hDevice,
                                   const NvHandle hParent,const  NvHandle hObject) {
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

static bool nv_get_versions(const int fd, char **versionString) {
    nv_ioctl_rm_api_version_t obj = {
        .cmd = '2' //query
    };

    const int ret = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_CHECK_VERSION_STR, sizeof(obj)), &obj);

    if (ret != 0) {
        LOG("nv_check_version failed: %d %d", ret, errno)
        return false;
    }

    if (strlen(obj.versionString) == 0) {
        //Fall back to reading a file from /proc.
        int procFd = open("/proc/driver/nvidia/version", O_RDONLY);
        bool successful = false;
        if (procFd > 0) {
            char buf[257];
            ssize_t readBytes = read(procFd, buf, 256);
            close(procFd);

            //The first line should look something like this. We just need to extract the version, which seems to be surrounded by 2 spaces
            //NVRM version: NVIDIA UNIX x86_64 Kernel Module  560.31.02  Tue Jul 30 21:02:43 UTC 2024
            if (readBytes > 0) {
                buf[readBytes] = '\0';
                char *versionStart = strstr(buf, "  ");
                if (versionStart != NULL) {
                    versionStart += 2;
                    char *versionEnd = strstr(versionStart, "  ");
                    if (versionEnd != NULL) {
                        *versionEnd = '\0';
                        *versionString = strdup(versionStart);
                        successful = true;
                    }
                }
            }
        }

        if (!successful) {
            //if the fallback wasn't successful, just return a fixed string.
            //the newer 470 series of drivers don't actually return the version number, so just substitute in a dummy one
            *versionString = strdup("470.123.45");
        }
    } else {
        *versionString = strdup(obj.versionString);
    }

    return obj.reply == NV_RM_API_VERSION_REPLY_RECOGNIZED;
}

static bool nv0_register_fd(const int nv0_fd, int nvctl_fd) {
    const int ret = ioctl(nv0_fd, _IOC(_IOC_READ|_IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_REGISTER_FD, sizeof(int)), &nvctl_fd);

    if (ret != 0) {
        LOG("nv0_register_fd failed: %d %d", ret, errno)
        return false;
    }

    return true;
}

static bool get_device_info(const int fd, NVDriverContext *context) {
    //NVIDIA driver v545.29.02 changed the devInfo struct, and partly broke it in the process
    //...who adds a field to the middle of an existing struct....
    if (context->driverMajorVersion >= 545 && context->driverMinorVersion >= 29) {
        struct drm_nvidia_get_dev_info_params_545 devInfo545;
        const int ret = ioctl(fd, DRM_IOCTL_NVIDIA_GET_DEV_INFO_545, &devInfo545);

        if (ret != 0) {
            LOG("get_device_info failed: %d %d", ret, errno)
            return false;
        }

        context->gpu_id = devInfo545.gpu_id;
        context->sector_layout = devInfo545.sector_layout;
        context->page_kind_generation = devInfo545.page_kind_generation;
        context->generic_page_kind = devInfo545.generic_page_kind;
    } else {
        struct drm_nvidia_get_dev_info_params devInfo;
        const int ret = ioctl(fd, DRM_IOCTL_NVIDIA_GET_DEV_INFO, &devInfo);

        if (ret != 0) {
            LOG("get_device_info failed: %d %d", ret, errno)
            return false;
        }

        context->gpu_id = devInfo.gpu_id;
        context->sector_layout = devInfo.sector_layout;
        context->page_kind_generation = devInfo.page_kind_generation;
        context->generic_page_kind = devInfo.generic_page_kind;
    }

    return true;
}

bool get_device_uuid(const NVDriverContext *context, uint8_t uuid[16]) {
    NV0000_CTRL_GPU_GET_UUID_FROM_GPU_ID_PARAMS uuidParams = {
        .gpuId = context->gpu_id,
        .flags = NV0000_CTRL_CMD_GPU_GET_UUID_FROM_GPU_ID_FLAGS_FORMAT_BINARY |
                 NV0000_CTRL_CMD_GPU_GET_UUID_FROM_GPU_ID_FLAGS_TYPE_SHA1
    };
    const int ret = nv_rm_control(context->nvctlFd, context->clientObject, context->clientObject, NV0000_CTRL_CMD_GPU_GET_UUID_FROM_GPU_ID, 0, sizeof(uuidParams), &uuidParams);
    if (ret) {
        return false;
    }

    for (int i = 0; i < 16; i++) {
        uuid[i] = uuidParams.gpuUuid[i];
    }

    return true;
}

bool init_nvdriver(NVDriverContext *context, const int drmFd) {
    LOG("Initing nvdriver...")
    int nv0Fd = -1;

    int nvctlFd = open("/dev/nvidiactl", O_RDWR|O_CLOEXEC);
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
    context->driverMinorVersion = atoi(ver+4);
    LOG("NVIDIA kernel driver version: %s, major version: %d, minor version: %d", ver, context->driverMajorVersion, context->driverMinorVersion)
    free(ver);

    if (!get_device_info(drmFd, context)) {
        return false;
    }

    LOG("Got dev info: %x %x %x %x", context->gpu_id, context->sector_layout, context->page_kind_generation, context->generic_page_kind)

    //allocate the root object
    bool ret = nv_alloc_object(nvctlFd, context->driverMajorVersion, NULL_OBJECT, NULL_OBJECT, &context->clientObject, NV01_ROOT_CLIENT, 0, (void*)0);
    if (!ret) {
        LOG("nv_alloc_object NV01_ROOT_CLIENT failed")
        goto err;
    }

    //attach the drm fd to this handle
    ret = nv_attach_gpus(nvctlFd, context->gpu_id);
    if (!ret) {
        LOG("nv_attach_gpu failed")
        goto err;
    }

    //allocate the parent memory object
    NV0080_ALLOC_PARAMETERS deviceParams = {
       .hClientShare = context->clientObject
    };

    //allocate the device object
    ret = nv_alloc_object(nvctlFd, context->driverMajorVersion, context->clientObject, context->clientObject, &context->deviceObject, NV01_DEVICE_0, sizeof(deviceParams), &deviceParams);
    if (!ret) {
        LOG("nv_alloc_object NV01_DEVICE_0 failed")
        goto err;
    }

    //allocate the subdevice object
    NV2080_ALLOC_PARAMETERS subdevice = { 0 };
    ret = nv_alloc_object(nvctlFd, context->driverMajorVersion, context->clientObject, context->deviceObject, &context->subdeviceObject, NV20_SUBDEVICE_0, sizeof(subdevice), &subdevice);
    if (!ret) {
        LOG("nv_alloc_object NV20_SUBDEVICE_0 failed")
        goto err;
    }

    //TODO honestly not sure if this is needed
    ret = nv0_register_fd(nv0Fd, nvctlFd);
    if (!ret) {
        LOG("nv0_register_fd failed")
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

    LOG("Got error initing")
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

bool alloc_memory(const NVDriverContext *context, const uint32_t size, int *fd) {
    //allocate the buffer
    NvHandle bufferObject = {0};

    NV_MEMORY_ALLOCATION_PARAMS memParams = {
        .owner = context->clientObject,
        .type = NVOS32_TYPE_IMAGE,
        .flags = NVOS32_ALLOC_FLAGS_IGNORE_BANK_PLACEMENT |
                 NVOS32_ALLOC_FLAGS_MAP_NOT_REQUIRED |
                 NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM,

        .attr = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _BIG) |
                DRF_DEF(OS32, _ATTR, _DEPTH, _UNKNOWN) |
                DRF_DEF(OS32, _ATTR, _FORMAT, _BLOCK_LINEAR) |
                DRF_DEF(OS32, _ATTR, _PHYSICALITY, _CONTIGUOUS),
        .format = 0,
        .width = 0,
        .height = 0,
        .size = size,
        .alignment = 0, //see flags above
        .attr2 = DRF_DEF(OS32, _ATTR2, _ZBC, _PREFER_NO_ZBC) |
                 DRF_DEF(OS32, _ATTR2, _GPU_CACHEABLE, _YES)
    };
    bool ret = nv_alloc_object(context->nvctlFd, context->driverMajorVersion, context->clientObject, context->deviceObject, &bufferObject, NV01_MEMORY_LOCAL_USER, sizeof(memParams), &memParams);
    if (!ret) {
        LOG("nv_alloc_object NV01_MEMORY_LOCAL_USER failed")
        return false;
    }

    //open a new handle to return
    int nvctlFd2 = open("/dev/nvidiactl", O_RDWR|O_CLOEXEC);
    if (nvctlFd2 == -1) {
        LOG("open /dev/nvidiactl failed")
        goto err;
    }

    //attach the new fd to the correct gpus
    ret = nv_attach_gpus(nvctlFd2, context->gpu_id);
    if (!ret) {
        LOG("nv_attach_gpus failed")
        goto err;
    }

    //actually export the object
    ret = nv_export_object_to_fd(context->nvctlFd, nvctlFd2, context->clientObject, context->deviceObject, context->deviceObject, bufferObject);
    if (!ret) {
        LOG("nv_export_object_to_fd failed")
        goto err;
    }

    ret = nv_free_object(context->nvctlFd, context->clientObject, bufferObject);
    if (!ret) {
        LOG("nv_free_object failed")
        goto err;
    }

    *fd = nvctlFd2;
    return true;

 err:
    LOG("error")
    if (nvctlFd2 > 0) {
        close(nvctlFd2);
    }

    ret = nv_free_object(context->nvctlFd, context->clientObject, bufferObject);
    if (!ret) {
        LOG("nv_free_object failed")
    }

    return false;
}

uint32_t calculate_image_size(const NVDriverContext *context, NVDriverImage images[], const uint32_t width, const uint32_t height,
                              const uint32_t bppc, const uint32_t numPlanes, const NVFormatPlane planes[]) {
    //first figure out the gob layout
    const uint32_t log2GobsPerBlockX = 0;
    const uint32_t log2GobsPerBlockZ = 0;

    uint32_t offset = 0;
    for (uint32_t i = 0; i < numPlanes; i++) {
        //calculate each planes dimensions and bpp
        const uint32_t planeWidth = width >> planes[i].ss.x;
        const uint32_t planeHeight = height >> planes[i].ss.y;
        const uint32_t bytesPerPixel = planes[i].channelCount * bppc;

        //Depending on the height of the allocated image, the modifiers
        //needed for the exported image to work correctly change. However, this can cause problems if the Y surface
        //needs one modifier, and UV need another when attempting to use a single surface export (as only one modifier
        //is possible). So for now we're just going to limit the minimum height to 88 pixels so we can use a single
        //modifier.
        //Update: with the single buffer export this no longer works, as we're only allowed one mod per fd when exporting
        //so different memory layouts for different planes can't work. Luckily this only seems to effect videos <= 128 pixels high.
        uint32_t log2GobsPerBlockY = 4;
        //uint32_t log2GobsPerBlockY = (planeHeight < 88) ? 3 : 4;
        LOG("Calculated log2GobsPerBlockY: %dx%d == %d", planeWidth, planeHeight, log2GobsPerBlockY);

        //These two seem to be correct, but it was discovered by trial and error so I'm not 100% sure
        const uint32_t widthInBytes = ROUND_UP(planeWidth * bytesPerPixel, GOB_WIDTH_IN_BYTES << log2GobsPerBlockX);
        const uint32_t alignedHeight = ROUND_UP(planeHeight, GOB_HEIGHT_IN_BYTES << log2GobsPerBlockY);

        images[i].width = planeWidth;
        images[i].height = alignedHeight;
        images[i].offset = offset;
        images[i].memorySize = widthInBytes * alignedHeight;
        images[i].pitch = widthInBytes;
        images[i].mods = DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, context->sector_layout, context->page_kind_generation, context->generic_page_kind, log2GobsPerBlockY);
        images[i].fourcc = planes[i].fourcc;
        images[i].log2GobsPerBlockX = log2GobsPerBlockX;
        images[i].log2GobsPerBlockY = log2GobsPerBlockY;
        images[i].log2GobsPerBlockZ = log2GobsPerBlockZ;

        offset += images[i].memorySize;
        offset = ROUND_UP(offset, 64);
    }

    return offset;
}

bool alloc_buffer(NVDriverContext *context, const uint32_t size, const NVDriverImage images[], int *fd1, int *fd2, int *drmFd) {
    int memFd = -1;
    const bool ret = alloc_memory(context, size, &memFd);
    if (!ret) {
        LOG("alloc_memory failed")
        return false;
    }

    //now export the dma-buf
    //duplicate the fd so we don't invalidate it by importing it
    const int memFd2 = dup(memFd);
    if (memFd2 == -1) {
        LOG("dup failed")
        goto err;
    }

    struct NvKmsKapiPrivImportMemoryParams nvkmsParams = {
        .memFd = memFd2,
        .surfaceParams = {
            .layout = NvKmsSurfaceMemoryLayoutBlockLinear,
            .blockLinear = {
                .genericMemory = 0,
                .pitchInBlocks = images[0].pitch / GOB_WIDTH_IN_BYTES,
                .log2GobsPerBlock.x = images[0].log2GobsPerBlockX,
                .log2GobsPerBlock.y = images[0].log2GobsPerBlockY,
                .log2GobsPerBlock.z = images[0].log2GobsPerBlockZ,
            }
        }
    };

    struct drm_nvidia_gem_import_nvkms_memory_params params = {
        .mem_size = size,
        .nvkms_params_ptr = (uint64_t) &nvkmsParams,
        .nvkms_params_size = context->driverMajorVersion == 470 ? 0x20 : sizeof(nvkmsParams) //needs to be 0x20 in the 470 series driver
    };
    int drmret = ioctl(context->drmFd, DRM_IOCTL_NVIDIA_GEM_IMPORT_NVKMS_MEMORY, &params);
    if (drmret != 0) {
        LOG("DRM_IOCTL_NVIDIA_GEM_IMPORT_NVKMS_MEMORY failed: %d %d", drmret, errno)
        goto err;
    }

    //export dma-buf
    struct drm_prime_handle prime_handle = {
        .handle = params.handle
    };
    drmret = ioctl(context->drmFd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_handle);
    if (drmret != 0) {
        LOG("DRM_IOCTL_PRIME_HANDLE_TO_FD failed: %d %d", drmret, errno)
        goto err;
    }

    struct drm_gem_close gem_close = {
        .handle = params.handle
    };
    drmret = ioctl(context->drmFd, DRM_IOCTL_GEM_CLOSE, &gem_close);
    if (drmret != 0) {
        LOG("DRM_IOCTL_GEM_CLOSE failed: %d %d", drmret, errno)
        goto prime_err;
    }

    *fd1 = memFd;
    *fd2 = memFd2;
    *drmFd = prime_handle.fd;
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
