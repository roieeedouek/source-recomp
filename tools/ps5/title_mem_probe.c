// Throwaway title: how much memory does a native title get? Results to klog and /app0/title-mem.txt (read over FTP).
// Probes (and releases) direct memory, flexible memory and malloc, in that order, each as far as it goes.
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int sceKernelDebugOutText(int channel, const char *text);
int sceKernelUsleep(unsigned int microseconds);
int sceSystemServiceLoadExec(const char *path, const char *const *argv);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t alignment, int64_t *physical, size_t *size);
int sceKernelAvailableFlexibleMemorySize(size_t *size);
int sceKernelAllocateMainDirectMemory(size_t length, size_t alignment, int type, int64_t *physical);
int sceKernelReleaseDirectMemory(int64_t start, size_t length);
int sceKernelMapNamedFlexibleMemory(void **address, size_t length, int protection, int flags, const char *name);
int sceKernelMunmap(void *address, size_t length);

static FILE *results;
static void say(const char *format, ...)
{
    char line[400];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    char klog[420];
    snprintf(klog, sizeof klog, "[mem-probe] %s\n", line);
    sceKernelDebugOutText(0, klog);
    if (results) { fprintf(results, "%s\n", line); fflush(results); }
}

#define MIB (1024ull * 1024)
int main(void)
{
    results = fopen("/app0/title-mem.txt", "w");
    size_t total = sceKernelGetDirectMemorySize(), largest = 0, flexible = 0;
    int64_t where = 0;
    int rc = sceKernelAvailableDirectMemorySize(0, (int64_t)total, 0x10000, &where, &largest);
    say("direct memory: total %zu MiB, largest free block %zu MiB at %#llx (rc %#x)", total >> 20, largest >> 20, (unsigned long long)where, rc);
    rc = sceKernelAvailableFlexibleMemorySize(&flexible);
    say("flexible memory available: %zu MiB (rc %#x)", flexible >> 20, rc);

    // Direct memory: blocks of 256 MiB then 64, 16, 2 as far as they go; released afterwards
    static const size_t sizes[] = {256 * MIB, 64 * MIB, 16 * MIB, 2 * MIB};
    int64_t held[1200][2];
    unsigned count = 0;
    size_t got = 0;
    for (unsigned s = 0; s < 4; s++)
        while (count < 1200) {
            int64_t physical = 0;
            if (sceKernelAllocateMainDirectMemory(sizes[s], 0x4000, 3, &physical)) break;
            held[count][0] = physical; held[count][1] = (int64_t)sizes[s]; count++; got += sizes[s];
        }
    say("direct memory allocatable: %zu MiB in %u blocks", got >> 20, count);
    for (unsigned i = 0; i < count; i++) sceKernelReleaseDirectMemory(held[i][0], (size_t)held[i][1]);

    // Flexible memory: 2 MiB maps at a time
    void *maps[2048];
    unsigned mapped = 0;
    for (; mapped < 2048; mapped++) {
        void *address = NULL;
        if (sceKernelMapNamedFlexibleMemory(&address, 2 * MIB, 3, 0, "probe")) break;
        maps[mapped] = address;
    }
    say("flexible memory mappable: %u MiB", mapped * 2);
    for (unsigned i = 0; i < mapped; i++) sceKernelMunmap(maps[i], 2 * MIB);

    // malloc: 16 MiB blocks until it fails or 3 GiB
    static void *blocks[256];
    unsigned allocated = 0;
    for (; allocated < 256; allocated++) {
        blocks[allocated] = malloc(16 * MIB);
        if (!blocks[allocated]) break;
        memset(blocks[allocated], 1, 16 * MIB);
    }
    say("malloc: %u MiB in 16 MiB blocks (touched)", allocated * 16);
    for (unsigned i = 0; i < allocated; i++) free(blocks[i]);
    say("mem probe ends");
    return 0;
}

void catchReturnFromMain(int status)
{
    say("exit status %d: asking the shell to close the title", status);
    if (results) { fclose(results); results = NULL; }
    (void)sceSystemServiceLoadExec("exit", NULL);
    for (;;) sceKernelUsleep(100000);
}
