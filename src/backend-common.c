#include "vabackend.h"
#include "backend-common.h"
#include <sys/ioctl.h>
#include <string.h>

bool checkModesetParameterFromFd(int fd) {
    if (fd > 0) {
        //this ioctl should fail if modeset=0
        struct drm_get_cap caps = { .capability = DRM_CAP_DUMB_BUFFER };
        int ret = ioctl(fd, DRM_IOCTL_GET_CAP, &caps);
        if (ret != 0) {
            //the modeset parameter is set to 0
            LOG("ERROR: This driver requires the nvidia_drm.modeset kernel module parameter set to 1");
            return false;
        }
        return true;
    }
    return true;
}

bool isNvidiaDrmFd(int fd, bool log) {
    if (fd > 0) {
        char name[16] = {0};
        struct drm_version ver = {
            .name = name,
            .name_len = 15
        };
        int ret = ioctl(fd, DRM_IOCTL_VERSION, &ver);
        if (ret || strncmp(name, "nvidia-drm", 10)) {
            if (log) {
                LOG("Invalid driver for DRM device: %s", ver.name);
            }
            return false;
        }
        return true;
    }
    return false;
}
