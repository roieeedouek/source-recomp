// libc names a native PS5 title cannot import: no module a title loads exports them, so the first call would jump to address 0.
// The libraries linked in (fontconfig, freetype, SDL) reference them; these are small stand-ins for the C locale and the
// console's file system (a title's temporary files live in /data/portal2/tmp). Only for title builds (RT_PS5_TITLE).
#if defined(__PROSPERO__) && defined(RT_PS5_TITLE)
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sched.h>
#include <stddef.h>
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// FreeBSD's MB_CUR_MAX: the C locale, one byte per character.
int ___mb_cur_max(void) { return 1; }

// Days between 1970-01-01 and the civil date (Howard Hinnant's algorithm), so timegm needs no time-zone database.
time_t timegm(struct tm *tm) {
    long year = tm->tm_year + 1900L, month = tm->tm_mon;
    year += month / 12; month %= 12;
    if (month < 0) { month += 12; year--; }
    year -= month < 2;
    long era = (year >= 0 ? year : year - 399) / 400;
    long yoe = year - era * 400;
    long doy = (153 * (month + (month > 1 ? -2 : 10)) + 2) / 5 + tm->tm_mday - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + doe - 719468;
    return ((days * 24 + tm->tm_hour) * 60 + tm->tm_min) * 60 + tm->tm_sec;
}

int mkostemp(char *pattern, int flags) { (void)flags; return mkstemp(pattern); }

char *mkdtemp(char *pattern) {
    size_t length = strlen(pattern);
    if (length < 6 || strcmp(pattern + length - 6, "XXXXXX")) { errno = EINVAL; return NULL; }
    for (int attempt = 0; attempt < 100; attempt++) {
        static const char digits[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        unsigned seed = (unsigned)rand() ^ (unsigned)getpid() * 2654435761u;
        for (int i = 0; i < 6; i++, seed = seed * 1103515245u + 12345u) pattern[length - 6 + i] = digits[(seed >> 16) % 36];
        if (mkdir(pattern, 0700) == 0) return pattern;
        if (errno != EEXIST) return NULL;
    }
    errno = EEXIST;
    return NULL;
}

FILE *tmpfile(void) {
    char name[] = "/data/portal2/tmp/tmpfileXXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) return NULL;
    unlink(name);
    FILE *file = fdopen(fd, "w+");
    if (!file) close(fd);
    return file;
}

int mkfifo(const char *path, mode_t mode) { (void)path; (void)mode; errno = ENOSYS; return -1; }
int getloadavg(double loadavg[], int count) { (void)loadavg; (void)count; return -1; }

int faccessat(int directory, const char *path, int mode, int flags) {
    (void)flags;
    if (directory == AT_FDCWD || (path && path[0] == '/')) return access(path, mode);
    errno = ENOSYS;
    return -1;
}

// A title has no /proc: DXVK builds std::string(path, path + readlink("/proc/self/exe")), and the -1 of a failed call is a
// length_error (abort at startup, from a static initializer). The executable is /app0/eboot.bin. tools/ps5/build_title.sh binds the
// name readlink to this instead of the platform layer's (--defsym=readlink=rt_title_readlink); everything else goes there.
ssize_t ps5_readlink(const char *path, char *buffer, size_t size);
ssize_t rt_title_readlink(const char *path, char *buffer, size_t size) {
    if (path && !strcmp(path, "/proc/self/exe")) {
        static const char executable[] = "/app0/eboot.bin";
        size_t length = sizeof executable - 1;
        if (length > size) length = size;
        memcpy(buffer, executable, length);
        return (ssize_t)length;
    }
    return ps5_readlink(path, buffer, size);
}

// A title is refused chdir (EPERM), and the runtime resolves the guest's relative paths against getcwd: the working directory is
// kept here. chdir checks the target is an absolute directory and records it; getcwd gives it back (the platform's answer until
// one is set). tools/ps5/build_title.sh binds the names chdir and getcwd to these (--defsym).
char *ps5dl_getcwd(char *buffer, size_t size);
static char virtual_cwd[1024];
int rt_title_chdir(const char *path) {
    struct stat st;
    if (!path || path[0] != '/') { errno = ENOENT; return -1; }
    if (stat(path, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/') length--;
    if (length >= sizeof virtual_cwd) { errno = ENAMETOOLONG; return -1; }
    memcpy(virtual_cwd, path, length);
    virtual_cwd[length] = 0;
    return 0;
}
char *rt_title_getcwd(char *buffer, size_t size) {
    if (!virtual_cwd[0]) return ps5dl_getcwd(buffer, size);
    size_t need = strlen(virtual_cwd) + 1;
    if (buffer && size < need) { errno = ERANGE; return NULL; }
    if (!buffer && !(buffer = malloc(size > need ? size : need))) { errno = ENOMEM; return NULL; }
    memcpy(buffer, virtual_cwd, need);
    return buffer;
}

#ifdef RT_PGO_GENERATE
// LLVM's profiling runtime is only installed built for Linux (libclang_rt.profile-x86_64.a): the glibc names it calls.
#include <stdarg.h>
#include <sys/utsname.h>
int *__errno_location(void) { return &errno; }
int __fprintf_chk(FILE *stream, int flag, const char *format, ...) {
    (void)flag;
    va_list arguments;
    va_start(arguments, format);
    int result = vfprintf(stream, format, arguments);
    va_end(arguments);
    return result;
}
int __snprintf_chk(char *buffer, size_t size, int flag, size_t room, const char *format, ...) {
    (void)flag; (void)room;
    va_list arguments;
    va_start(arguments, format);
    int result = vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return result;
}
void *__memcpy_chk(void *to, const void *from, size_t size, size_t room) { (void)room; return memcpy(to, from, size); }
int prctl(int option, ...) { (void)option; return 0; }   // PR_SET_PDEATHSIG and the like: nothing to do in a title
int rt_title_uname(struct utsname *name) {
    memset(name, 0, sizeof *name);
    strcpy(name->sysname, "PS5"); strcpy(name->nodename, "ps5"); strcpy(name->machine, "x86_64");
    return 0;
}
#endif

// Ownership: a title runs as one user and changes nothing about files that are not its own.
int chown(const char *path, uid_t owner, gid_t group) { (void)path; (void)owner; (void)group; return 0; }
int lchown(const char *path, uid_t owner, gid_t group) { (void)path; (void)owner; (void)group; return 0; }

// Terminal speeds: nothing on the console has a terminal.
int cfsetospeed(struct termios *t, speed_t speed) { (void)t; (void)speed; return 0; }
int cfsetispeed(struct termios *t, speed_t speed) { (void)t; (void)speed; return 0; }
speed_t cfgetospeed(const struct termios *t) { (void)t; return 0; }
speed_t cfgetispeed(const struct termios *t) { (void)t; return 0; }

// Crash diagnostics: the console reports only "abort was called". A fatal signal writes its backtrace (addresses, plus the
// address of main to get the load slide: llvm-symbolizer on build/ps5-title-game/link/eboot.elf) to the log, then dies as before.
int backtrace(void **frames, int count);
int main(int, char **);
void rt_ps5_log_settle(void);
static pthread_t title_main_thread;
static void fatal_signal(int signal_number, siginfo_t *info, void *context) {
    void *frames[48];
    int count = backtrace(frames, 48);
    // The backtrace stops at the signal frame: the faulting instruction and stack come from the context (the console's
    // ucontext_t has uc_mcontext at 64, see profile_sample).
    const char *registers = (const char *)context + 64;
    uintptr_t rip = *(const uintptr_t *)(registers + offsetof(mcontext_t, mc_rip));
    uintptr_t rsp = *(const uintptr_t *)(registers + offsetof(mcontext_t, mc_rsp));
    fprintf(stderr, "\n=== fatal signal %d at rip %#lx (return %#lx), fault address %p, %s thread", signal_number,
            (unsigned long)rip, rsp ? (unsigned long)*(const uintptr_t *)rsp : 0ul, info ? info->si_addr : NULL,
            pthread_equal(pthread_self(), title_main_thread) ? "main" : "another");
    fprintf(stderr, "\n=== fatal signal %d, main at %p, %d frames:", signal_number, (void *)main, count);
    for (int i = 0; i < count; i++) fprintf(stderr, " %p", frames[i]);
    fprintf(stderr, "\n");
    fflush(NULL);
    rt_ps5_log_settle();   // standard error is a pipe drained by a thread (runtime/ps5.c): let it reach the file
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}
// Watchdog: every 15 s the main thread is interrupted and writes its host backtrace (generated functions are fn_<guest address>
// in build/ps5-title-game/link/llvm-pie.elf) and the present count to /data/portal2/trace.txt: shows where a hang sits.
void rt_ps5_trace(const char *text);
uint64_t rt_d3d9_presents(void);
void rt_ps5_frame_stats_flush(void);
extern _Atomic uint64_t rt_ps5_last_present_ns;
int rt_ps5_vk_submit_thread(pthread_t *thread);
int rt_ps5_vk_record_thread(pthread_t *thread);
int rt_d3d9_present_thread(pthread_t *thread);
void rt_threads_sample(int signal);
int rt_threads_native(unsigned tid, pthread_t *native);
void rt_ps5_vk_flush(void);
static volatile int sample_is_hitch;
// Profiling on request: SIGPROF records only the interrupted instruction address, from the signal context. No stack walk: a
// backtrace taken from a signal in the middle of recompiled code crashed the game once (2026-11-24).
#define PROFILE_SAMPLES 2000
static uintptr_t profile_pcs[PROFILE_SAMPLES], profile_tops[PROFILE_SAMPLES];
static _Atomic unsigned profile_next;
static void profile_sample(int signal, siginfo_t *info, void *context) {
    (void)signal; (void)info;
    unsigned slot = profile_next++;
    // The console's ucontext_t has 48 bytes more before uc_mcontext than FreeBSD's (the upstream SDK headers this is built
    // with): uc_mcontext is at 64 (deps/ps5-vulkan-template's ps5platform/context.h). The word on top of the stack is kept
    // too: inside a system call stub (no frame of its own) it is the caller's return address.
    if (slot < PROFILE_SAMPLES) {
        const char *registers = (const char *)context + 64;
        profile_pcs[slot] = *(const uintptr_t *)(registers + offsetof(mcontext_t, mc_rip));
        uintptr_t stack = *(const uintptr_t *)(registers + offsetof(mcontext_t, mc_rsp));
        profile_tops[slot] = stack ? *(const uintptr_t *)stack : 0;
    }
}
static void watchdog_sample(int signal) {
    (void)signal;
    void *frames[40];
    int count = backtrace(frames, 40);
    char line[1200];
    int length = snprintf(line, sizeof line, "%s: main at %p, %s %p, presents %llu, %d frames:", sample_is_hitch ? "hitch" : "watchdog",
                          (void *)main, pthread_equal(pthread_self(), title_main_thread) ? "main thread" : "thread",
                          (void *)pthread_self(), (unsigned long long)rt_d3d9_presents(), count);
    for (int i = 0; i < count && length < (int)sizeof line - 20; i++) length += snprintf(line + length, sizeof line - length, " %lx", (unsigned long)(uintptr_t)frames[i]);
    rt_ps5_trace(line);
}
// 60 frames a second, while /data/portal2/fps60 exists (checked every second, so it can be switched while the game runs): each
// frame stays on screen for two refreshes at 119.88 Hz (one at 59.94 Hz), paced by the display itself rather than by a timer
// in the game. RADV's video output (patched deps/ps5-vulkan RADV) does the flipping.
// Weak: wsi_videoout_refresh_millihertz is a local addition to that RADV; without it the switch does nothing.
__attribute__((weak)) int wsi_videoout_set_flip_rate(int rate);
__attribute__((weak)) int wsi_videoout_refresh_millihertz(void);
static void frame_cap_poll(void) {
    static int applied = -1;   // flip rate in force, -1 before the output is open
    if (!wsi_videoout_set_flip_rate || !wsi_videoout_refresh_millihertz) return;
    int refresh = wsi_videoout_refresh_millihertz();
    if (!refresh) return;
    int wanted = !access("/data/portal2/fps60", F_OK) && refresh > 100000 ? 1 : 0;
    if (wanted == applied) return;
    int result = wsi_videoout_set_flip_rate(wanted);
    char note[120];
    snprintf(note, sizeof note, "frame cap: %s (refresh %.2f Hz, flip rate %d, answer %d)", wanted ? "60 fps" : "off",
             refresh / 1000.0, wanted, result);
    rt_ps5_trace(note);
    applied = wanted;
}

static void *watchdog(void *unused) {
    (void)unused;
    // Once a second: the frame statistics and slow Vulkan calls to the trace; every 15 s, a stall (no frame at all) has the main
    // thread sampled. (A 2 ms hitch poller found RADV's statistics print, 2026-11-24; it cost a core's wakeups, so it is gone.)
    uint64_t last = 0;
    for (unsigned tick = 1;; ++tick) {
        sleep(1);
        rt_ps5_frame_stats_flush();
        rt_ps5_vk_flush();
        frame_cap_poll();
        // /data/portal2/profile.request (tools/ps5/run_title.sh --profile): the main thread's instruction address every 5 ms for
        // 10 s, or the GPU-submission thread's when the file says "submit"; one address a line in /data/portal2/profile.txt.
        if (!access("/data/portal2/profile.request", F_OK)) {
            char what[16] = {0};
            FILE *request = fopen("/data/portal2/profile.request", "r");
            if (request) { if (!fgets(what, sizeof what, request)) what[0] = 0; fclose(request); }
            unlink("/data/portal2/profile.request");
            pthread_t target = title_main_thread;
            int submit = !strncmp(what, "submit", 6) && rt_ps5_vk_submit_thread(&target);
            int record = !submit && !strncmp(what, "record", 6) && rt_ps5_vk_record_thread(&target);
            int present = !submit && !record && !strncmp(what, "present", 7) && rt_d3d9_present_thread(&target);
            unsigned guest = 0;   // "guest N": guest thread N (2 is the first one the game starts)
            if (!submit && !record && !present && sscanf(what, "guest %u", &guest) == 1 && !rt_threads_native(guest, &target)) guest = 0;
            profile_next = 0;
            for (int i = 0; i < PROFILE_SAMPLES; ++i) { pthread_kill(target, SIGPROF); usleep(5000); }
            usleep(20000);
            unsigned taken = profile_next < PROFILE_SAMPLES ? profile_next : PROFILE_SAMPLES;
            FILE *out = fopen("/data/portal2/profile.tmp", "w");
            if (out) {
                char label[24];
                if (guest) snprintf(label, sizeof label, "guest %u", guest);
                else snprintf(label, sizeof label, "%s", submit ? "submit" : record ? "record" : present ? "present" : "main");
                fprintf(out, "profile: %s thread, main at %p, %u samples\n", label, (void *)main, taken);
                for (unsigned i = 0; i < taken; ++i) fprintf(out, "%lx %lx\n", (unsigned long)profile_pcs[i], (unsigned long)profile_tops[i]);
                fclose(out);
                rename("/data/portal2/profile.tmp", "/data/portal2/profile.txt");
            }
            continue;
        }
        if (tick % 15) continue;
        uint64_t now = rt_d3d9_presents();
        if (now == last) {
            sample_is_hitch = 0;
            pthread_kill(title_main_thread, SIGINFO);
            static unsigned all_threads;   // every guest thread too, for the first two stalls
            if (all_threads++ < 2) { usleep(200000); rt_threads_sample(SIGINFO); }
        }
        last = now;
    }
    return NULL;
}

__attribute__((constructor(102))) static void install_crash_log(void) {
    fprintf(stderr, "=== portal2 title starts, main at %p\n", (void *)main);
#ifdef RT_PGO_GENERATE
    // Profile-guided optimization run on the console (tools/build_game.py --pgo-generate, PS5_PGO_GENERATE=1 for the link):
    // runtime/profile.c writes the counts here every 20 s; a title never exits through exit().
    setenv("LLVM_PROFILE_FILE", "/data/portal2/tmp/portal2-ps5.profraw", 0);
#endif
    // DXVK's Logger is a static object: these are read before main runs.
    setenv("DXVK_LOG_LEVEL", "warn", 0);   // "debug" writes a line per compiled shader: a hitch of its own
    setenv("DXVK_LOG_PATH", "/data/portal2/tmp", 0);
    // No system fonts: tools/ps5/fonts.conf (the game's own fonts and the family aliases VGUI uses).
    if (!access("/data/portal2/fonts.conf", R_OK)) setenv("FONTCONFIG_FILE", "/data/portal2/fonts.conf", 0);
    // Environment for the runtime from /data/portal2/env.txt, NAME=value per line (no shell on the console to set it), e.g.
    // P2_GUEST_CPUS=4. Lines starting with # are ignored; an existing variable is not overridden.
    {
        FILE *env = fopen("/data/portal2/env.txt", "r");
        char line[256];
        while (env && fgets(line, sizeof line, env)) {
            line[strcspn(line, "\r\n")] = 0;
            char *equals = strchr(line, '=');
            if (line[0] == '#' || !equals || equals == line) continue;
            *equals = 0;
            setenv(line, equals + 1, 0);
            char note[300];
            snprintf(note, sizeof note, "env: %s=%s", line, equals + 1);
            rt_ps5_trace(note);
        }
        if (env) fclose(env);
    }
    // Per-import guest call counts, to the log every 10 s (runtime/rt.c), while this file exists.
    if (!access("/data/portal2/import_counts", F_OK)) setenv("P2_IMPORT_COUNTS", "1", 0);
    // The guest lock's wait time on the main thread, to the log every 5 s (runtime/scheduler.c), while this file exists.
    if (!access("/data/portal2/gate_timing", F_OK)) setenv("P2_GATE_TIMING", "1", 0);
    // Frame counter (runtime/fps_overlay.c, top left) while /data/portal2/show_fps exists.
    if (!access("/data/portal2/show_fps", F_OK)) setenv("P2_FPS", "1", 0);
    static const int fatal[] = {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE};
    struct sigaction fatal_action = {0};
    fatal_action.sa_sigaction = fatal_signal;
    fatal_action.sa_flags = SA_SIGINFO;
    sigemptyset(&fatal_action.sa_mask);
    for (unsigned i = 0; i < sizeof fatal / sizeof *fatal; i++) sigaction(fatal[i], &fatal_action, NULL);
    title_main_thread = pthread_self();
    {
        // Where the console's library functions are (taking an imported function's address reads its resolved entry): the
        // system's libraries have no symbols here, and a profile's system addresses are named against these.
        static const struct { const char *name; void *address; } known[] = {
            {"clock_gettime", (void *)clock_gettime}, {"gettimeofday", (void *)gettimeofday}, {"nanosleep", (void *)nanosleep},
            {"usleep", (void *)usleep}, {"sched_yield", (void *)sched_yield}, {"pthread_mutex_lock", (void *)pthread_mutex_lock},
            {"pthread_mutex_unlock", (void *)pthread_mutex_unlock}, {"pthread_mutex_trylock", (void *)pthread_mutex_trylock},
            {"pthread_cond_wait", (void *)pthread_cond_wait}, {"pthread_cond_timedwait", (void *)pthread_cond_timedwait},
            {"pthread_cond_signal", (void *)pthread_cond_signal}, {"pthread_cond_broadcast", (void *)pthread_cond_broadcast},
            {"pthread_self", (void *)pthread_self}, {"pthread_getspecific", (void *)pthread_getspecific},
            {"pthread_setspecific", (void *)pthread_setspecific}, {"read", (void *)read}, {"write", (void *)write},
            {"open", (void *)open}, {"close", (void *)close}, {"lseek", (void *)lseek}, {"fstat", (void *)fstat},
            {"stat", (void *)stat}, {"access", (void *)access}, {"mmap", (void *)mmap}, {"munmap", (void *)munmap},
            {"mprotect", (void *)mprotect}, {"getpid", (void *)getpid}, {"sigprocmask", (void *)sigprocmask},
            {"pthread_sigmask", (void *)pthread_sigmask}, {"malloc", (void *)malloc}, {"free", (void *)free},
        };
        for (unsigned i = 0; i < sizeof known / sizeof *known; ++i) {
            char note[96];
            snprintf(note, sizeof note, "symbol: %p %s", known[i].address, known[i].name);
            rt_ps5_trace(note);
        }
    }
    struct sigaction action = {0};
    action.sa_handler = watchdog_sample;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINFO, &action, NULL);
    struct sigaction profiling = {0};
    profiling.sa_sigaction = profile_sample;
    profiling.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&profiling.sa_mask);
    sigaction(SIGPROF, &profiling, NULL);
    pthread_t thread;
    if (!pthread_create(&thread, NULL, watchdog, NULL)) pthread_detach(thread);
}
#endif
