//
// Created by stephen on 12/11/23.
//

#ifndef COMMON_H
#define COMMON_H

typedef struct
{
    uint32_t x;
    uint32_t y;
} NVSubSampling;

typedef struct
{
    uint32_t channelCount;
    uint32_t fourcc;
    NVSubSampling ss; // subsampling
} NVFormatPlane;

#endif //COMMON_H
