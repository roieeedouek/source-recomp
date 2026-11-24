// Throwaway title: which clock pthread_cond_timedwait measures its absolute deadline against on the console.
// Waits 300 ms against a CLOCK_REALTIME deadline, then against a CLOCK_MONOTONIC one, logging how long each took.
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <x86intrin.h>

int sceKernelDebugOutText(int channel, const char *text);
int sceKernelUsleep(unsigned int microseconds);
int sceSystemServiceLoadExec(const char *path, const char *const *argv);
uint64_t sceKernelGetTscFrequency(void);

static void say(const char *format, ...) {
    char line[200], klog[220];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    snprintf(klog, sizeof klog, "[cond-probe] %s\n", line);
    sceKernelDebugOutText(0, klog);
    FILE *out = fopen("/app0/title-cond.txt", "a");
    if (out) { fprintf(out, "%s\n", line); fclose(out); }
}

static double seconds_since(uint64_t start) { return (double)(__rdtsc() - start) / (double)sceKernelGetTscFrequency(); }

static void timed(clockid_t clock, const char *label) {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    struct timespec deadline;
    clock_gettime(clock, &deadline);
    say("%s now: %lld.%09ld", label, (long long)deadline.tv_sec, deadline.tv_nsec);
    deadline.tv_nsec += 300000000;
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&mutex);
    uint64_t start = __rdtsc();
    // A deadline from the wrong clock domain could block for decades: a watchdog thread would be needed; instead the far
    // case is detected by the result arriving at once (deadline long past) or by the probe never printing "returned".
    int result = pthread_cond_timedwait(&cond, &mutex, &deadline);
    say("%s deadline: returned %d after %.3f s", label, result, seconds_since(start));
    pthread_mutex_unlock(&mutex);
}

int main(void) {
    say("start");
    timed(CLOCK_MONOTONIC, "monotonic");
    timed(CLOCK_REALTIME, "realtime");
    say("done");
    return 0;
}

void catchReturnFromMain(int status) {
    say("exit %d", status);
    (void)sceSystemServiceLoadExec("exit", NULL);
    for (;;) sceKernelUsleep(100000);
}
