#ifndef NVD_ENCODE_IMAGE_H
#define NVD_ENCODE_IMAGE_H

#include "../vabackend.h"

VAStatus nvenc_put_image(NVDriver *drv,
                         NVSurface *surfaceObj,
                         NVImage *imageObj,
                         uint32_t copy_width,
                         uint32_t copy_height);

#endif
