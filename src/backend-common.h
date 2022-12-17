#ifndef BACKENDCOMMON_H
#define BACKENDCOMMON_H

#include <stdbool.h>

bool checkModesetParameterFromFd(int fd);
bool isNvidiaDrmFd(int fd, bool log);

#endif // BACKENDCOMMON_H
