// Throwaway title: which time-stamp-counter calls a title may use. Each step is logged before the next (klog and
// /app0/title-tsc.txt), so a crash shows which one it was.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <x86intrin.h>

int sceKernelDebugOutText(int channel, const char *text);
int sceKernelUsleep(unsigned int microseconds);
int sceSystemServiceLoadExec(const char *path, const char *const *argv);
uint64_t sceKernelReadTsc(void);
uint64_t sceKernelGetTscFrequency(void);

static void say(const char *format, ...) {
    char line[200], klog[220];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    snprintf(klog, sizeof klog, "[tsc-probe] %s\n", line);
    sceKernelDebugOutText(0, klog);
    FILE *out = fopen("/app0/title-tsc.txt", "a");
    if (out) { fprintf(out, "%s\n", line); fclose(out); }
}

int main(void) {
    say("start");
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    say("clock_gettime ok: %lld.%09ld", (long long)a.tv_sec, a.tv_nsec);
    uint64_t f = sceKernelGetTscFrequency();
    say("sceKernelGetTscFrequency: %llu", (unsigned long long)f);
    uint64_t t1 = sceKernelReadTsc();
    say("sceKernelReadTsc: %llu", (unsigned long long)t1);
    uint64_t r1 = __rdtsc();
    say("rdtsc: %llu", (unsigned long long)r1);
    sceKernelUsleep(200000);
    uint64_t t2 = sceKernelReadTsc(), r2 = __rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &b);
    double dt = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
    say("over %.3f s: ReadTsc %.0f Hz, rdtsc %.0f Hz", dt, (double)(t2 - t1) / dt, (double)(r2 - r1) / dt);
    uint64_t start = __rdtsc();
    for (int i = 0; i < 100000; ++i) clock_gettime(CLOCK_MONOTONIC, &b);
    uint64_t mid = __rdtsc();
    for (int i = 0; i < 100000; ++i) (void)sceKernelReadTsc();
    uint64_t end = __rdtsc();
    say("cost: clock_gettime %.0f ticks, sceKernelReadTsc %.0f ticks", (double)(mid - start) / 1e5, (double)(end - mid) / 1e5);
    say("done");
    return 0;
}

void catchReturnFromMain(int status) {
    say("exit %d", status);
    (void)sceSystemServiceLoadExec("exit", NULL);
    for (;;) sceKernelUsleep(100000);
}
