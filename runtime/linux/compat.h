// Darwin -> Linux glue, force-included (-include) when building on Linux.
// The host libc here is glibc on x86-64; the guest is i386 Linux, so most
// Darwin translation layers become identity mappings.
#pragma once
#ifdef __linux__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#undef linux
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/mman.h>

// BSD stat timestamps.
#define st_atimespec st_atim
#define st_mtimespec st_mtim
#define st_ctimespec st_ctim
// pthread extensions.
#include <unistd.h>
#include <sys/syscall.h>
#define pthread_main_np() (getpid() == (pid_t)syscall(SYS_gettid))
#define pthread_setname_np_1(n) pthread_setname_np(pthread_self(), (n))
#define QOS_CLASS_USER_INTERACTIVE 0
#define pthread_set_qos_class_self_np(a, b) 0
#define pthread_attr_set_qos_class_np(a, b, c) 0
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
static inline uint64_t clock_gettime_nsec_np(clockid_t id) {
    struct timespec ts; clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
// Wait for a relative timeout on a condition variable.
static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel) {
    struct timespec abs; clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_sec += rel->tv_sec; abs.tv_nsec += rel->tv_nsec;
    if (abs.tv_nsec >= 1000000000L) { abs.tv_sec++; abs.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(c, m, &abs);
}
#endif
