#include <stdint.h>

extern "C" __global__ void convert_nv12_bt701_block_linear(uint8_t *out, uint8_t *luma, uint8_t *chroma, uint32_t width, uint32_t height, uint32_t log2GobsPerBlockX, uint32_t log2GobsPerBlockY) {

    //TODO these need to be passed in
    uint32_t gobWidth    = 16;//px TODO calculate these from hardware
    uint32_t gobHeight   = 8;//px
    uint32_t bytesPerPixel = 4;//bpc * channels / 8;

//    uint32_t blockWidth  = gobWidth * (1<<log2GobsPerBlockX);//px
//    uint32_t blockHeight = gobHeight * (1<<log2GobsPerBlockY);//px

    uint32_t gobSize     = gobWidth * gobHeight * bytesPerPixel;
    uint32_t gobsPerX    = gridDim.x;
//    uint32_t gobsPerY    = gridDim.y;

    uint32_t gobX        = blockIdx.x;
    uint32_t gobY        = blockIdx.y;

    uint32_t gobPixelX   = gobX * gobWidth;
    uint32_t gobPixelY   = gobY * gobHeight;

    uint32_t blockY      = gobY / (1<<log2GobsPerBlockY);

    gobY = gobY % (1<<log2GobsPerBlockY);

    uint32_t blockOffset = (blockY * (gobsPerX * (1<<log2GobsPerBlockY) * gobSize));
    uint32_t gobOffset = ((gobX * (1<<log2GobsPerBlockY)) + gobY) * gobSize;

    uint32_t subGobWidth  = 4;//px
    uint32_t subGobHeight = 4;//px

    for (uint32_t i = 0; i < gobSize; i+=4) {
        uint32_t t = i / bytesPerPixel;
        uint32_t idx = (i / 64);
        uint32_t half = idx / 4;

        uint32_t subGobX = idx&1;
        uint32_t subGobY = (idx&2)>>1;

        uint32_t subSubGobX = t&3;
        uint32_t subSubGobY = (t%16)/4;

        uint32_t x = gobPixelX + (half * subGobWidth * 2) + (subGobX * subGobWidth) + subSubGobX;
        uint32_t y = gobPixelY + (subGobY * subGobHeight) + subSubGobY;

        uint32_t pixelOffset = i;
        uint8_t *pixelOut =  out + blockOffset + gobOffset + pixelOffset;

//        pixelOut[2] = t;//R
//        pixelOut[1] = idx;//G
//        pixelOut[0] = 0;//B

        uint8_t *lumaOffset   = luma   + (y*width) + x;
        uint8_t *chromaOffset = chroma + (y>>1)*width + (x & ~1);

        uint8_t Y = lumaOffset[0];
        uint8_t U = chromaOffset[0];
        uint8_t V = chromaOffset[1];

        pixelOut[2] = (uint8_t) fmaxf(fminf(Y + 1.402 * (V - 128), 255.0), 0.0);
        pixelOut[1] = (uint8_t) fmaxf(fminf(Y - 0.34413 * (U - 128) - 0.71414*(V - 128), 255.0), 0.0);
        pixelOut[0] = (uint8_t) fmaxf(fminf(Y + 1.772*(U - 128), 255.0), 0.0);
    }
}