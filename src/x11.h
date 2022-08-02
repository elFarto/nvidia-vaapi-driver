#ifndef PUTSURFACE_H
#define PUTSURFACE_H

#include "vabackend.h"

VAStatus nvPutSurface(
        VADriverContextP ctx,
        VASurfaceID surface,
        void* draw, /* Drawable of window system */
        short srcx,
        short srcy,
        unsigned short srcw,
        unsigned short srch,
        short destx,
        short desty,
        unsigned short destw,
        unsigned short desth,
        VARectangle *cliprects, /* client supplied clip list */
        unsigned int number_cliprects, /* number of clip rects in the clip list */
        unsigned int flags /* de-interlacing flags */
    );

#endif // PUTSURFACE_H
