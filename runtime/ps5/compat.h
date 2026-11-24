// Darwin -> PS5 (Prospero: FreeBSD-derived, x86-64) glue, force-included
// (-include) when building for the console. The BSD socket, stat and errno
// layouts match Darwin's; what is missing is Apple's own API (mach, os/lock,
// QoS), replaced below. See docs/ps5.md.
#pragma once
#ifdef __PROSPERO__
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <pthread_np.h>
#define RT_HOST_PS5 1
#ifndef O_DSYNC
#define O_DSYNC O_SYNC
#endif
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#ifndef IUTF8
#define IUTF8 0x0
#endif
#ifndef ENOATTR
#define ENOATTR 206
#endif
#ifndef EPROCLIM
#define EPROCLIM 207
#endif
#define EBADEXEC 208
#define EBADARCH 209
#define EBADMACHO 210
#define ESHLIBVERS 211
#ifndef EFTYPE
#define EFTYPE 212
#endif
#define EAUTH 213
#define ENEEDAUTH 214
#define EPWROFF 215
#define EDEVERR 216
#define EBADRPC 217
#define ERPCMISMATCH 218
#define EPROGUNAVAIL 219
#define EPROGMISMATCH 220
#define EPROCUNAVAIL 221
#define ENOPOLICY 222
#define EQFULL 223
// Errno names FreeBSD lacks: unique unused values so the Darwin tables compile.
#ifndef ENODATA
#define ENODATA 200
#define ENOSR 201
#define ENOSTR 202
#define ETIME 203
#define ENOTRECOVERABLE 204
#define EOWNERDEAD 205
#endif
#ifndef MNT_NODEV
#define MNT_NODEV 0
#endif
#define pthread_main_np() (getpid() == getpid() && pthread_self() == rt_ps5_main_thread())
pthread_t rt_ps5_main_thread(void);
#define QOS_CLASS_USER_INTERACTIVE 0
#define pthread_set_qos_class_self_np(a, b) 0
#define pthread_attr_set_qos_class_np(a, b, c) 0
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
static inline uint64_t clock_gettime_nsec_np(clockid_t id) {
    struct timespec ts; clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel) {
    struct timespec abs; clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_sec += rel->tv_sec; abs.tv_nsec += rel->tv_nsec;
    if (abs.tv_nsec >= 1000000000L) { abs.tv_sec++; abs.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(c, m, &abs);
}
extern int sceKernelMprotect(const void *address, size_t bytes, int protection);
void *rt_ps5_reserve(size_t bytes);
int rt_ps5_commit(uint32_t address, uint64_t size);
void rt_ps5_notify(const char *text);
#define mprotect(a, b, c) sceKernelMprotect((a), (b), (c))
#define pthread_setname_np_1(n) pthread_set_name_np(pthread_self(), (n))
#endif
