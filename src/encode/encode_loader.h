#ifndef NVD_ENCODE_LOADER_H
#define NVD_ENCODE_LOADER_H

#include "../vabackend.h"

void nvenc_loader_init(NvencFunctions **nvenc_out);
void nvenc_loader_cleanup(NvencFunctions **nvenc_io);

#endif
