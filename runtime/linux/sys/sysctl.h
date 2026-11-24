// Minimal sysctlbyname() for the few hw.* keys the runtime reads on Linux.
#pragma once
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysinfo.h>
static inline int sysctlbyname(const char *name, void *out, size_t *size, const void *in, size_t n) {
    (void)in; (void)n;
    uint64_t v;
    if (!strcmp(name, "hw.memsize")) { struct sysinfo si; if (sysinfo(&si)) return -1; v = (uint64_t)si.totalram * si.mem_unit; }
    else if (!strcmp(name, "hw.logicalcpu")) v = (uint64_t)sysconf(_SC_NPROCESSORS_ONLN);
    else if (!strcmp(name, "hw.perflevel0.l2cachesize") || !strcmp(name, "hw.l2cachesize")) v = 512u * 1024;
    else return -1;
    if (*size == 4) { uint32_t v32 = (uint32_t)v; memcpy(out, &v32, 4); } else { memcpy(out, &v, 8); *size = 8; }
    return 0;
}
