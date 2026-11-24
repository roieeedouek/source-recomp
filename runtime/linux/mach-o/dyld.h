#pragma once
#include <stdint.h>
#include <unistd.h>
static inline int _NSGetExecutablePath(char *buf, uint32_t *size) {
    ssize_t n = readlink("/proc/self/exe", buf, *size ? *size - 1 : 0);
    if (n < 0 || (uint32_t)n >= *size - 1) return -1;
    buf[n] = 0; *size = (uint32_t)n + 1; return 0;
}
