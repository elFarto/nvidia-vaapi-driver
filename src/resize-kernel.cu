// Source for resize-ptx.inc. Regenerate with tools/generate-resize-ptx.py.
// Each thread handles one pixel, including all its interleaved components.
template<typename T>
__device__ void resize_plane(const T *src, T *dst, unsigned int src_width,
                             unsigned int src_height, unsigned int dst_width,
                             unsigned int dst_height, unsigned int channels,
                             float scale_x, float scale_y) {
    unsigned int x = blockIdx.x * blockDim.x + threadIdx.x;
    unsigned int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dst_width || y >= dst_height) return;

    float sx = ((float)x + 0.5f) * scale_x - 0.5f;
    float sy = ((float)y + 0.5f) * scale_y - 0.5f;
    sx = fminf(fmaxf(sx, 0.0f), (float)(src_width - 1));
    sy = fminf(fmaxf(sy, 0.0f), (float)(src_height - 1));
    unsigned int x0 = (unsigned int)sx, y0 = (unsigned int)sy;
    unsigned int x1 = min(x0 + 1, src_width - 1);
    unsigned int y1 = min(y0 + 1, src_height - 1);
    float wx = sx - (float)x0, wy = sy - (float)y0;
    unsigned long long a = ((unsigned long long)y0 * src_width + x0) * channels;
    unsigned long long b = ((unsigned long long)y0 * src_width + x1) * channels;
    unsigned long long c = ((unsigned long long)y1 * src_width + x0) * channels;
    unsigned long long d = ((unsigned long long)y1 * src_width + x1) * channels;
    unsigned long long out = ((unsigned long long)y * dst_width + x) * channels;
    for (unsigned int ch = 0; ch < channels; ch++) {
        float top = (float)src[a + ch] + ((float)src[b + ch] - (float)src[a + ch]) * wx;
        float bottom = (float)src[c + ch] + ((float)src[d + ch] - (float)src[c + ch]) * wx;
        dst[out + ch] = (T)__float2uint_rn(top + (bottom - top) * wy);
    }
}

extern "C" __global__ void resize_plane_u8(const unsigned char *src, unsigned char *dst,
                                               unsigned int sw, unsigned int sh,
                                               unsigned int dw, unsigned int dh,
                                               unsigned int channels, float scale_x,
                                               float scale_y) {
    resize_plane(src, dst, sw, sh, dw, dh, channels, scale_x, scale_y);
}

extern "C" __global__ void resize_plane_u16(const unsigned short *src, unsigned short *dst,
                                                unsigned int sw, unsigned int sh,
                                                unsigned int dw, unsigned int dh,
                                                unsigned int channels, float scale_x,
                                                float scale_y) {
    resize_plane(src, dst, sw, sh, dw, dh, channels, scale_x, scale_y);
}
