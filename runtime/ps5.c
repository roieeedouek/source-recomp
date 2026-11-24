// PS5 (Prospero) host layer: guest address window and main thread.
//
// The console hands a title only ~440 MiB of "flexible" memory for anonymous
// mmap, far less than the guest needs. The guest window (4 GiB of address
// space, rt.c) is therefore only *reserved* here, and each 2 MiB chunk is
// backed with direct memory the first time rt_map/rt_mmap commits it. This
// follows the memory model proven by prospero-win (wine/ps5/pw_wine_dmem_ps5.c,
// LGPL-2.1+): reserve with sceKernelReserveVirtualRange, back with
// sceKernelMapDirectMemory, change protection with sceKernelMprotect.
// Not yet run on hardware (see docs/ps5.md).
#ifdef __PROSPERO__
#include "rt.h"
#include <pthread.h>
#include <stdarg.h>
#include <dirent.h>
#include <time.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

extern int sceKernelReserveVirtualRange(void **address, size_t bytes, int flags, size_t alignment);
extern int sceKernelAllocateMainDirectMemory(size_t bytes, size_t alignment, int type, int64_t *offset);
extern size_t sceKernelGetDirectMemorySize(void);
extern int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t alignment, int64_t *address, size_t *bytes);
extern size_t sceKernelAvailableFlexibleMemorySize(size_t *bytes);
extern int sceKernelMapNamedFlexibleMemory(void **address, size_t bytes, int protection, int flags, const char *name);
extern int sceKernelMapDirectMemory(void **address, size_t bytes, int protection, int flags, int64_t offset, size_t alignment);

enum {
    PS5_PAGE = 0x4000,
    KERNEL_FIXED = 0x10,
    MEMORY_TYPE = 0x0c,          // as prospero-win's heap
    CPU_READ_WRITE = 0x03,
    CPU_GPU_READ_WRITE = 0xf2,
    CHUNK = 2 << 20,
    MAX_CHUNKS = (int)((UINT64_C(1) << 32) / CHUNK) + 1,
};

typedef struct { char useless1[45]; char message[3075]; } ps5_notify_request;
extern int sceKernelSendNotificationRequest(int, ps5_notify_request *, size_t, int);
// Console toast: the only output channel without a log server.
void rt_ps5_notify(const char *text) {
    ps5_notify_request r;
    memset(&r, 0, sizeof r);
    snprintf(r.message, sizeof r.message, "%s", text);
    sceKernelSendNotificationRequest(0, &r, sizeof r, 0);
}

#ifdef RT_PS5_TITLE
void rt_ps5_trace(const char *text);
// Standard error, asynchronously: text for stderr goes into this ring and a thread of its own writes it to the log file, so a
// writer never waits on the console's storage. (A pipe was tried first: pipe() is refused in a title.) When the ring is full,
// the writer waits for room rather than losing lines.
static volatile int log_ready;
static int log_file = -1;
static char log_ring[1 << 18];
static size_t log_head, log_tail;   // under log_lock; head - tail bytes pending
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t log_wake = PTHREAD_COND_INITIALIZER, log_room = PTHREAD_COND_INITIALIZER;
static void *log_drain(void *unused) {
    (void)unused;
    static char chunk[1 << 16];
    pthread_mutex_lock(&log_lock);
    for (;;) {
        while (log_head == log_tail) pthread_cond_wait(&log_wake, &log_lock);
        size_t length = log_head - log_tail;
        if (length > sizeof chunk) length = sizeof chunk;
        for (size_t i = 0; i < length; ++i) chunk[i] = log_ring[(log_tail + i) % sizeof log_ring];
        log_tail += length;
        pthread_cond_broadcast(&log_room);
        pthread_mutex_unlock(&log_lock);
        for (size_t done = 0; done < length;) {
            ssize_t put = write(log_file, chunk + done, length - done);
            if (put < 0 && errno == EINTR) continue;
            if (put <= 0) break;
            done += (size_t)put;
        }
        pthread_mutex_lock(&log_lock);
    }
    return NULL;
}
// tools/ps5/build_title.sh wraps the stdio calls: text for stderr is formatted here and queued for the log thread. (RADV prints its
// 10 s statistics under the GPU submit lock: written directly, that line was a ~90 ms hitch every 10 s.)
int __real_vfprintf(FILE *stream, const char *format, va_list arguments);
int __real_fputs(const char *text, FILE *stream);
int __real_fputc(int character, FILE *stream);
size_t __real_fwrite(const void *data, size_t size, size_t count, FILE *stream);
static int to_pipe(FILE *stream) { return stream == stderr && log_ready; }
static void pipe_write(const char *data, size_t length) {
    pthread_mutex_lock(&log_lock);
    while (length) {
        while (log_head - log_tail == sizeof log_ring) pthread_cond_wait(&log_room, &log_lock);
        size_t room = sizeof log_ring - (log_head - log_tail), part = length < room ? length : room;
        for (size_t i = 0; i < part; ++i) log_ring[(log_head + i) % sizeof log_ring] = data[i];
        log_head += part; data += part; length -= part;
        pthread_cond_signal(&log_wake);
    }
    pthread_mutex_unlock(&log_lock);
}
int __wrap_vfprintf(FILE *stream, const char *format, va_list arguments) {
    if (!to_pipe(stream)) return __real_vfprintf(stream, format, arguments);
    char small[2048];
    va_list again;
    va_copy(again, arguments);
    int length = vsnprintf(small, sizeof small, format, arguments);
    if (length >= (int)sizeof small) {
        char *large = malloc((size_t)length + 1);
        if (large) { vsnprintf(large, (size_t)length + 1, format, again); pipe_write(large, (size_t)length); free(large); }
    } else if (length > 0) pipe_write(small, (size_t)length);
    va_end(again);
    return length;
}
int __wrap_fprintf(FILE *stream, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int length = __wrap_vfprintf(stream, format, arguments);
    va_end(arguments);
    return length;
}
int __wrap_fputs(const char *text, FILE *stream) {
    if (!to_pipe(stream)) return __real_fputs(text, stream);
    pipe_write(text, strlen(text));
    return 1;
}
int __wrap_fputc(int character, FILE *stream) {
    if (!to_pipe(stream)) return __real_fputc(character, stream);
    char c = (char)character;
    pipe_write(&c, 1);
    return (unsigned char)character;
}
size_t __wrap_fwrite(const void *data, size_t size, size_t count, FILE *stream) {
    if (!to_pipe(stream)) return __real_fwrite(data, size, count, stream);
    pipe_write(data, size * count);
    return count;
}
// Lets a dying process's last lines reach the file (fatal-signal handler, ps5_title.c).
void rt_ps5_log_settle(void) {
    for (int i = 0; i < 50; ++i) {
        pthread_mutex_lock(&log_lock);
        int pending = log_head != log_tail;
        pthread_mutex_unlock(&log_lock);
        if (!pending) break;
        usleep(10000);
    }
    usleep(20000);
}
#endif

#ifdef RT_PS5_TITLE
extern void platform_init(const char *title_name);
#endif
static pthread_t main_thread;
// Runs before main (priority 101): notification at startup, stderr to a file so failures can be read back.
__attribute__((constructor(101))) static void remember_main_thread(void) {
    main_thread = pthread_self();
#ifdef RT_PS5_TITLE
    platform_init("Portal 2");   // the console glue (deps/ps5-doom3/ps5/src/platform.c): klog, the shell's splash, the pad
#else
    rt_ps5_notify("portal2: payload started");
    setenv("SDL_VULKAN_LIBRARY", "/data/portal2/lib/libvulkan.so.1", 0);
#endif
    mkdir("/data/portal2/tmp", 0777);
    mkdir("/data/portal2/user", 0777);
    mkdir("/data/portal2/user/dxvk-cache", 0777);
    setenv("DXVK_STATE_CACHE_PATH", "/data/portal2/user/dxvk-cache", 1);   // an empty path makes DXVK throw in create_directories
    // RADV's compiled-shader cache: a title has no HOME, so without a directory every pipeline is recompiled on each run (the
    // stutter when a new material or effect first appears). Warm runs then only pay DXVK's own work.
    mkdir("/data/portal2/user/mesa-cache", 0777);
    setenv("MESA_SHADER_CACHE_DIR", "/data/portal2/user/mesa-cache", 0);
    setenv("MESA_SHADER_CACHE_MAX_SIZE", "2G", 0);
    setenv("TMPDIR", "/data/portal2/tmp", 1);   // /tmp is not writable on the console; the emulated /proc files are built here
    if (!freopen("/data/portal2/log.txt", "a", stderr)) rt_ps5_notify("portal2: cannot open /data/portal2/log.txt");
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(fileno(stderr), 1);   // stdout into the same file, unbuffered: it is lost when the process dies otherwise
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef RT_PS5_TITLE
    // Save folders made 0744 by earlier builds (runtime/posix.c now makes them 0777): only this process's user may open them up.
    {
        static const char save_root[] = "/data/portal2/user/portal2/SAVE";
        chmod(save_root, 0777);
        DIR *saves = opendir(save_root);
        for (struct dirent *entry; saves && (entry = readdir(saves));) {
            if (entry->d_name[0] == '.') continue;
            char path[512];
            snprintf(path, sizeof path, "%s/%s", save_root, entry->d_name);
            chmod(path, 0777);
        }
        if (saves) closedir(saves);
    }
    // A write to the log file can take tens of milliseconds on the console, and some writers hold locks while they print: RADV's
    // 10 s submission statistics are printed under the GPU submit lock (a ~90 ms hitch every 10 s). Standard output and error
    // become a pipe drained into the file by a thread of their own; a writer only waits when 64 KiB are pending.
    log_file = open("/data/portal2/log.txt", O_WRONLY | O_APPEND);   // dup() is refused in a title, like pipe()
    pthread_t drain;
    if (log_file >= 0 && !pthread_create(&drain, NULL, log_drain, NULL)) {
        pthread_detach(drain);
        log_ready = 1;
        rt_ps5_trace("log: standard error is written by a thread of its own");
    } else {
        char note[120];
        snprintf(note, sizeof note, "log: no log thread (file %d, errno %d), standard error is written directly", log_file, errno);
        rt_ps5_trace(note);
    }
#endif
}
// Progress trace independent of stdio: klog and /data/portal2/trace.txt (one write() per line, appended).
extern int sceKernelDebugOutText(int channel, const char *text);
void rt_ps5_trace(const char *text) {
    char line_[1400];
    int length = snprintf(line_, sizeof line_, "[p2] %s\n", text);
    if (length > (int)sizeof line_ - 1) { length = (int)sizeof line_ - 1; line_[length - 1] = '\n'; }
    sceKernelDebugOutText(0, line_);
    int fd = open("/data/portal2/trace.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) { if (length > 0) (void)!write(fd, line_, (size_t)length); close(fd); }
}
pthread_t rt_ps5_main_thread(void) { return main_thread; }

static uint8_t *window;                    // base of the reserved guest window
static uint8_t backed[MAX_CHUNKS];
static int map_protection = CPU_READ_WRITE;
static int memory_type = MEMORY_TYPE;   // switched if the console refuses it
static unsigned backed_chunks, flexible_chunks;
#ifdef RT_PS5_TITLE
// A title gets ~12 GiB of direct memory but only ~420 MiB of flexible memory (measured, tools/ps5/title_mem_probe.c): the guest
// window is backed by direct memory from the start, leaving flexible memory to the platform's own use.
static int flexible_refused = 1;
#else
static int flexible_refused;
#endif

// Reserves `bytes` of address space; returns the base or NULL.
void *rt_ps5_reserve(size_t bytes) {
    // High hint: the GPU driver (RADV) needs a 16 MiB ring below 4 GiB and fails ("outside the 32-bit window") when the kernel
    // has handed that low space to this window. The hint is not binding; the result is logged.
    void *at = (void *)(uintptr_t)0x2000000000ull;
    int rc = sceKernelReserveVirtualRange(&at, bytes, 0, PS5_PAGE);
    fprintf(stderr, "ps5: guest window reserved at %p (%zu bytes)\n", at, bytes);
    if (rc || !at) { fprintf(stderr, "ps5: sceKernelReserveVirtualRange(%zu) failed: %#x\n", bytes, (unsigned)rc); return NULL; }
    window = at;
    return at;
}

// Makes [address, address + size) of the guest window backed memory.
int rt_ps5_commit(uint32_t address, uint64_t size) {
    if (!window) return 0;
    uint64_t first = address / CHUNK, last = ((uint64_t)address + size + CHUNK - 1) / CHUNK;
    for (uint64_t chunk = first; chunk < last && chunk < MAX_CHUNKS; ++chunk) {
        if (backed[chunk]) continue;
        // Flexible memory first: this console reports ~8 GiB of it free to a payload but only ~54 MiB of main direct memory,
        // which the GPU driver needs more than the guest does.
        if (!flexible_refused) {
            void *flex = window + chunk * CHUNK;
            int frc = sceKernelMapNamedFlexibleMemory(&flex, CHUNK, CPU_READ_WRITE, KERNEL_FIXED, "p2guest");
            if (!frc && flex == window + chunk * CHUNK) { backed[chunk] = 1; ++backed_chunks; ++flexible_chunks; continue; }
            fprintf(stderr, "ps5: flexible map refused at guest %#llx (%#x) after %u flexible chunks; using direct memory\n",
                    (unsigned long long)(chunk * CHUNK), (unsigned)frc, flexible_chunks);
            flexible_refused = 1;
        }
        int64_t offset = 0;
        int rc = sceKernelAllocateMainDirectMemory(CHUNK, PS5_PAGE, memory_type, &offset);
        if (rc) {   // other memory types before giving up (0x0c is prospero-win's; the pool rules are not documented)
            static const int types[] = { 0x0c, 0, 3, 1, 2, 10, 0x0b, 0x0d };
            for (unsigned t = 0; rc && t < sizeof types / sizeof *types; ++t)
                if (types[t] != memory_type && !(rc = sceKernelAllocateMainDirectMemory(CHUNK, PS5_PAGE, types[t], &offset))) {
                    fprintf(stderr, "ps5: memory type %#x refused after %u chunks (%u MiB), type %#x works\n", memory_type, backed_chunks, backed_chunks * 2, types[t]);
                    memory_type = types[t];
                }
        }
        if (rc) {
            int64_t where = 0; size_t largest = 0, flexible = 0;
            sceKernelAvailableDirectMemorySize(0, (int64_t)sceKernelGetDirectMemorySize(), PS5_PAGE, &where, &largest);
            sceKernelAvailableFlexibleMemorySize(&flexible);
            fprintf(stderr, "ps5: direct memory: total %zu MiB, largest free block %zu MiB at %#llx, flexible free %zu MiB, backed so far %u chunks (%u MiB)\n",
                    (size_t)(sceKernelGetDirectMemorySize() >> 20), largest >> 20, (unsigned long long)where, flexible >> 20, backed_chunks, backed_chunks * 2); fprintf(stderr, "ps5: out of direct memory at guest %#llx: %#x\n", (unsigned long long)(chunk * CHUNK), (unsigned)rc); return -1; }
        void *at = window + chunk * CHUNK;
        rc = sceKernelMapDirectMemory(&at, CHUNK, map_protection, KERNEL_FIXED, offset, PS5_PAGE);
        if (rc && map_protection == CPU_READ_WRITE) {   // refused CPU-only: the GPU bits, kept if they work
            at = window + chunk * CHUNK;
            rc = sceKernelMapDirectMemory(&at, CHUNK, CPU_GPU_READ_WRITE, KERNEL_FIXED, offset, PS5_PAGE);
            if (!rc) map_protection = CPU_GPU_READ_WRITE;
        }
        if (rc || at != window + chunk * CHUNK) { fprintf(stderr, "ps5: sceKernelMapDirectMemory failed at guest %#llx: %#x\n", (unsigned long long)(chunk * CHUNK), (unsigned)rc); return -1; }
        backed[chunk] = 1; ++backed_chunks;
    }
    return 0;
}
// libc gaps of the console SDK. wctype_l: the locale argument is ignored.
#include <wctype.h>
#include <xlocale.h>
wctype_t wctype_l(const char *name, locale_t locale) { (void)locale; return wctype(name); }
// towupper_l and towlower_l are inline in the SDK's headers and absent from the console's libc: when the compiler keeps an
// out-of-line call (a profile-guided build does), tools/ps5/build_title.sh binds the names to these. The game uses the C locale.
// The inline versions reach FreeBSD's rune tables (___toupper, __getCurrentRuneLocale), absent too: the common alphabets are
// mapped here (Latin-1, Latin Extended-A, Greek, Cyrillic), enough for the game's localized text; anything else is unchanged.
static int case_map(int c, int upper) {
    if (c < 0x80) return upper ? (c >= 'a' && c <= 'z' ? c - 32 : c) : (c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (c >= 0xc0 && c <= 0xfe && c != 0xd7 && c != 0xf7 && c != 0xdf) {
        if (upper) return c >= 0xe0 ? c - 32 : c;
        return c < 0xe0 ? c + 32 : c;
    }
    if (c == 0xff) return upper ? 0x178 : c;
    if (c == 0x178) return upper ? c : 0xff;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177))   // pairs: even upper, odd lower
        return upper ? (c & ~1) : (c | 1);
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e))   // pairs: odd upper, even lower
        return upper ? ((c & 1) ? c : c - 1) : ((c & 1) ? c + 1 : c);
    if (c >= 0x391 && c <= 0x3a9 && c != 0x3a2) return upper ? c : c + 32;
    if (c >= 0x3b1 && c <= 0x3c9) return upper ? (c == 0x3c2 ? 0x3a3 : c - 32) : c;
    if (c >= 0x410 && c <= 0x42f) return upper ? c : c + 32;
    if (c >= 0x430 && c <= 0x44f) return upper ? c - 32 : c;
    if (c >= 0x400 && c <= 0x40f) return upper ? c : c + 80;
    if (c >= 0x450 && c <= 0x45f) return upper ? c - 80 : c;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48a && c <= 0x4bf) || (c >= 0x4d0 && c <= 0x52f))
        return upper ? (c & ~1) : (c | 1);
    return c;
}
int rt_towupper_l(int c, locale_t locale) { (void)locale; return case_map(c, 1); }
int rt_towlower_l(int c, locale_t locale) { (void)locale; return case_map(c, 0); }
// Apple-only GL extensions used by the OpenGL path of gl.c: no-ops here (Direct3D 9 via DXVK is the renderer).
__attribute__((weak)) void glBufferParameteriAPPLE(unsigned target, unsigned pname, int param) { (void)target; (void)pname; (void)param; }
__attribute__((weak)) void glFlushMappedBufferRangeAPPLE(unsigned target, long offset, long size) { (void)target; (void)offset; (void)size; }
#ifdef RT_PS5_TITLE
// A title has no Vulkan loader: RADV is linked into the executable and its entry point is vk_icdGetInstanceProcAddr.
extern void *vk_icdGetInstanceProcAddr(void *instance, const char *name);
// Frame-pacing diagnostics: vkQueueSubmit and vkQueuePresentKHR are interposed (DXVK gets every function through here) to time
// each call and remember the thread that submits. A call over 20 ms is noted (written to the trace once a second by the
// watchdog thread, rt_ps5_vk_flush) and the submitting thread can be sampled during a hitch (ps5_title.c).
typedef int32_t (*VkSubmitFn)(void *queue, uint32_t count, const void *submits, uint64_t fence);
typedef int32_t (*VkPresentFn)(void *queue, const void *info);
typedef void *(*VkDeviceProcFn)(void *device, const char *name);
static VkSubmitFn real_submit;
static VkPresentFn real_present;
static VkDeviceProcFn real_device_proc;
static pthread_t submit_thread;
static _Atomic int have_submit_thread;
static char slow_notes[8][96];
static _Atomic unsigned slow_written, slow_read;
static uint64_t vk_now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}
static void note_slow(const char *call, uint64_t ns) {
    if (ns < 20000000u) return;
    unsigned slot = slow_written;
    if (slot - slow_read >= 8) return;   // the watchdog has not caught up: dropped
    snprintf(slow_notes[slot % 8], sizeof slow_notes[0], "vulkan: %s took %.1f ms", call, (double)ns / 1e6);
    slow_written = slot + 1;
}
void rt_ps5_vk_flush(void) {
    while (slow_read != slow_written) {
        rt_ps5_trace(slow_notes[slow_read % 8]);
        slow_read = slow_read + 1;
    }
}
int rt_ps5_vk_submit_thread(pthread_t *thread) {
    if (!have_submit_thread) return 0;
    *thread = submit_thread;
    return 1;
}
static int32_t timed_submit(void *queue, uint32_t count, const void *submits, uint64_t fence) {
    if (!have_submit_thread) { submit_thread = pthread_self(); have_submit_thread = 1; }
    uint64_t start = vk_now_ns();
    int32_t result = real_submit(queue, count, submits, fence);
    note_slow("vkQueueSubmit", vk_now_ns() - start);
    return result;
}
static int32_t timed_present(void *queue, const void *info) {
    uint64_t start = vk_now_ns();
    int32_t result = real_present(queue, info);
    note_slow("vkQueuePresentKHR", vk_now_ns() - start);
    return result;
}
// The console's own submission calls, under RADV's (tools/ps5/build_title.sh: --wrap): which part of a slow vkQueueSubmit stalls.
int32_t __real_sceAgcDriverSubmitDcb(void *description);
int32_t __wrap_sceAgcDriverSubmitDcb(void *description) {
    uint64_t start = vk_now_ns();
    int32_t result = __real_sceAgcDriverSubmitDcb(description);
    note_slow("sceAgcDriverSubmitDcb", vk_now_ns() - start);
    return result;
}
int32_t __real_sceAgcSuspendPoint(void);
int32_t __wrap_sceAgcSuspendPoint(void) {
    uint64_t start = vk_now_ns();
    int32_t result = __real_sceAgcSuspendPoint();
    note_slow("sceAgcSuspendPoint", vk_now_ns() - start);
    return result;
}
// The thread recording command buffers (DXVK's command-stream worker): known from its first vkCmdDrawIndexed.
typedef void (*VkDrawIndexedFn)(void *buffer, uint32_t indices, uint32_t instances, uint32_t first, int32_t offset, uint32_t first_instance);
static VkDrawIndexedFn real_draw_indexed;
static pthread_t record_thread;
static _Atomic int have_record_thread;
static void noted_draw_indexed(void *buffer, uint32_t indices, uint32_t instances, uint32_t first, int32_t offset, uint32_t first_instance) {
    if (!have_record_thread) { record_thread = pthread_self(); have_record_thread = 1; }
    real_draw_indexed(buffer, indices, instances, first, offset, first_instance);
}
int rt_ps5_vk_record_thread(pthread_t *thread) {
    if (!have_record_thread) return 0;
    *thread = record_thread;
    return 1;
}
static void *interpose(const char *name, void *real) {
    if (!real) return NULL;
    if (!strcmp(name, "vkCmdDrawIndexed")) { real_draw_indexed = (VkDrawIndexedFn)real; return (void *)noted_draw_indexed; }
    if (!strcmp(name, "vkQueueSubmit")) { real_submit = (VkSubmitFn)real; return (void *)timed_submit; }
    if (!strcmp(name, "vkQueuePresentKHR")) { real_present = (VkPresentFn)real; return (void *)timed_present; }
    return real;
}
static void *device_proc(void *device, const char *name) { return interpose(name, real_device_proc(device, name)); }
void *vkGetInstanceProcAddr(void *instance, const char *name) {
    void *real = vk_icdGetInstanceProcAddr(instance, name);
    if (real && !strcmp(name, "vkGetDeviceProcAddr")) { real_device_proc = (VkDeviceProcFn)real; return (void *)device_proc; }
    return interpose(name, real);
}
#else
// Vulkan is not a load-time dependency: the console's loader cannot resolve libvulkan.so.1 by name, but dlopen by path works.
// DXVK's loader calls vkGetInstanceProcAddr directly; this resolves it from the library on first use.
#include <dlfcn.h>
#include <stdlib.h>
#ifndef RT_PS5_VULKAN
#define RT_PS5_VULKAN "/data/portal2/lib/libvulkan.so.1"
#endif
void *vkGetInstanceProcAddr(void *instance, const char *name) {
    static void *(*real)(void *, const char *);
    if (!real) {
        void *library = dlopen(RT_PS5_VULKAN, RTLD_NOW | RTLD_GLOBAL);
        if (!library) { fprintf(stderr, "ps5: dlopen(%s): %s\n", RT_PS5_VULKAN, dlerror()); rt_ps5_notify("portal2: cannot load libvulkan"); return NULL; }
        real = (void *(*)(void *, const char *))dlsym(library, "vkGetInstanceProcAddr");
        if (!real) { fprintf(stderr, "ps5: vkGetInstanceProcAddr missing\n"); return NULL; }
    }
    return real(instance, name);
}
#endif
#ifdef RT_PS5_TITLE
// A title cannot dlopen anything (the console's loader answers NULL for every module): the three libraries the game asks for
// are all inside the executable. dlopen(NULL) / "@self" is the executable (the offline Steam stub is looked up there, from a table
// tools/ps5/gen_dl_table.py writes at link time); a path naming Vulkan is RADV. Everything else fails with a message.
struct rt_dl_entry { const char *name; void *address; };
extern const struct rt_dl_entry rt_dl_table[];
extern const unsigned rt_dl_count;
extern void *vk_icdGetInstanceProcAddr(void *instance, const char *name);
static char self_handle, vulkan_handle;
static const char *dl_error;
void *__wrap_dlopen(const char *path, int mode) {
    (void)mode;
    void *handle = NULL;
    if (!path || !*path || !strcmp(path, "@self")) handle = &self_handle;
    else if (strstr(path, "vulkan")) handle = &vulkan_handle;
    else dl_error = "not available in a title (no dynamic loading)";
    fprintf(stderr, "ps5: dlopen(%s, %#x) -> %p%s%s\n", path ? path : "(null)", mode, handle, handle ? "" : " ", handle ? "" : dl_error);
    return handle;
}
void *__wrap_dlsym(void *handle, const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkGetInstanceProcAddr") && (handle == &vulkan_handle || handle == &self_handle)) return (void *)vkGetInstanceProcAddr;
    if (handle == &self_handle)
        for (unsigned i = 0; i < rt_dl_count; i++)
            if (!strcmp(rt_dl_table[i].name, name)) return rt_dl_table[i].address;
    dl_error = "symbol not found";
    return NULL;
}
char *__wrap_dlerror(void) { const char *error = dl_error; dl_error = NULL; return (char *)error; }
int __wrap_dlclose(void *handle) { (void)handle; return 0; }
#else
// dl* inside the game library. The SDK's libc.a defines dlopen/dlsym (dlfcn.o), so each ELF that links it carries its own copy
// with its own state: the copy inside this library does not know the libvulkan the launcher already loaded, and its dlopen
// returns NULL without an error text. The library is linked with --wrap=dlopen/dlsym/dlerror/dlclose (tools/build_game.py), and the
// launcher registers its own functions through rt_ps5_set_dl() before main runs. Without a launcher the real ones are used.
typedef void *(*dlopen_fn)(const char *, int);
typedef void *(*dlsym_fn)(void *, const char *);
typedef char *(*dlerror_fn)(void);
typedef int (*dlclose_fn)(void *);
extern void *__real_dlopen(const char *, int);
extern void *__real_dlsym(void *, const char *);
extern char *__real_dlerror(void);
extern int __real_dlclose(void *);
static dlopen_fn launcher_dlopen;
static dlsym_fn launcher_dlsym;
static dlerror_fn launcher_dlerror;
static dlclose_fn launcher_dlclose;
void rt_ps5_set_dl(dlopen_fn open, dlsym_fn symbol, dlerror_fn error, dlclose_fn close) {
    launcher_dlopen = open; launcher_dlsym = symbol; launcher_dlerror = error; launcher_dlclose = close;
}
void *__wrap_dlopen(const char *path, int mode) {
    void *handle = (launcher_dlopen ? launcher_dlopen : __real_dlopen)(path, mode);
    char *error = handle ? NULL : (launcher_dlerror ? launcher_dlerror : __real_dlerror)();
    fprintf(stderr, "ps5: dlopen(%s, %#x) -> %p%s%s\n", path ? path : "(null)", mode, handle, error ? " " : "", error ? error : "");
    return handle;
}
void *__wrap_dlsym(void *handle, const char *name) { return (launcher_dlsym ? launcher_dlsym : __real_dlsym)(handle, name); }
char *__wrap_dlerror(void) { return (launcher_dlerror ? launcher_dlerror : __real_dlerror)(); }
int __wrap_dlclose(void *handle) { return (launcher_dlclose ? launcher_dlclose : __real_dlclose)(handle); }
#endif
#endif
