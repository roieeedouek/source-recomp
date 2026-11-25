// PS5 (Prospero) host layer for the native title (tools/ps5/build_title.sh, docs/ps5.md): guest address window, standard
// error, Vulkan entry point and dl* for a title that cannot load libraries.
//
// The guest window (4 GiB of address space, rt.c) is reserved here and each 2 MiB chunk is backed with direct memory the first
// time rt_map/rt_mmap commits it (a title gets ~12 GiB of direct memory but only ~420 MiB of flexible memory). This follows the
// memory model proven by prospero-win (wine/ps5/pw_wine_dmem_ps5.c, LGPL-2.1+): reserve with sceKernelReserveVirtualRange, back
// with sceKernelMapDirectMemory, change protection with sceKernelMprotect.
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

// Where the game's files live: the title's own folder (/app0, that is /data/homebrew/<title>) when it holds the game
// (game/portal2), else /data/portal2. Saves, caches, the log and the option files go there too.
static const char *data_root = "/data/portal2";
const char *rt_ps5_root(void) { return data_root; }
// rt_ps5_data("user/x") is "<root>/user/x": one string per name, kept for the process (a few dozen names).
const char *rt_ps5_data(const char *relative) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static struct { const char *relative; char *path; } kept[64];
    static unsigned count;
    pthread_mutex_lock(&lock);
    const char *found = NULL;
    for (unsigned i = 0; i < count && !found; ++i) if (!strcmp(kept[i].relative, relative)) found = kept[i].path;
    if (!found && count < 64) {
        size_t length = strlen(data_root) + 1 + strlen(relative) + 1;
        char *path = malloc(length);
        if (path) { snprintf(path, length, "%s/%s", data_root, relative); kept[count].relative = strdup(relative); kept[count++].path = path; found = path; }
    }
    pthread_mutex_unlock(&lock);
    return found ? found : "/data/portal2/missing";
}
extern void platform_init(const char *title_name);
static pthread_t main_thread;
// Runs before main (priority 101): notification at startup, stderr to a file so failures can be read back.
__attribute__((constructor(101))) static void remember_main_thread(void) {
    main_thread = pthread_self();
    if (!access("/app0/game/portal2", F_OK)) data_root = "/app0";
    platform_init("Portal 2");   // the console glue (deps/ps5-doom3/ps5/src/platform.c): klog, the shell's splash, the pad
    mkdir(rt_ps5_data("tmp"), 0777);
    mkdir(rt_ps5_data("user"), 0777);
    mkdir(rt_ps5_data("user/dxvk-cache"), 0777);
    setenv("DXVK_STATE_CACHE_PATH", rt_ps5_data("user/dxvk-cache"), 1);   // an empty path makes DXVK throw in create_directories
    // RADV's compiled-shader cache: a title has no HOME, so without a directory every pipeline is recompiled on each run (the
    // stutter when a new material or effect first appears). Warm runs then only pay DXVK's own work.
    mkdir(rt_ps5_data("user/mesa-cache"), 0777);
    setenv("MESA_SHADER_CACHE_DIR", rt_ps5_data("user/mesa-cache"), 0);
    setenv("MESA_SHADER_CACHE_MAX_SIZE", "2G", 0);
    setenv("TMPDIR", rt_ps5_data("tmp"), 1);   // /tmp is not writable on the console; the emulated /proc files are built here
    if (!freopen(rt_ps5_data("log.txt"), "a", stderr)) rt_ps5_notify("portal2: cannot open the log file");
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(fileno(stderr), 1);   // stdout into the same file, unbuffered: it is lost when the process dies otherwise
    setvbuf(stdout, NULL, _IONBF, 0);
    // Save folders made 0744 by earlier builds (runtime/posix.c now makes them 0777): only this process's user may open them up.
    {
        const char *save_root = rt_ps5_data("user/portal2/SAVE");
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
    log_file = open(rt_ps5_data("log.txt"), O_WRONLY | O_APPEND);   // dup() is refused in a title, like pipe()
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
}
// Progress trace independent of stdio: klog and trace.txt in the data folder (one write() per line, appended).
extern int sceKernelDebugOutText(int channel, const char *text);
void rt_ps5_trace(const char *text) {
    char line_[1400];
    int length = snprintf(line_, sizeof line_, "[p2] %s\n", text);
    if (length > (int)sizeof line_ - 1) { length = (int)sizeof line_ - 1; line_[length - 1] = '\n'; }
    sceKernelDebugOutText(0, line_);
    int fd = open(rt_ps5_data("trace.txt"), O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) { if (length > 0) (void)!write(fd, line_, (size_t)length); close(fd); }
}
pthread_t rt_ps5_main_thread(void) { return main_thread; }

static uint8_t *window;                    // base of the reserved guest window
static uint8_t backed[MAX_CHUNKS];
static int map_protection = CPU_READ_WRITE;
static int memory_type = MEMORY_TYPE;   // switched if the console refuses it
static unsigned backed_chunks;

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
// A title has no Vulkan loader: RADV is linked into the executable and its entry point is vk_icdGetInstanceProcAddr.
extern void *vk_icdGetInstanceProcAddr(void *instance, const char *name);
void *vkGetInstanceProcAddr(void *instance, const char *name) { return vk_icdGetInstanceProcAddr(instance, name); }
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
#endif
