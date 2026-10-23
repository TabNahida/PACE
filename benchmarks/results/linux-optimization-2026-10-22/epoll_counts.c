// Linux diagnostic only: count libc epoll_ctl calls with LD_PRELOAD.
// Run separately from timing comparisons; interposition adds overhead.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>

static int (*next_epoll_ctl)(int, int, int, struct epoll_event*);
static _Atomic uint64_t counts[4];
static _Atomic uint64_t failures;

__attribute__((constructor)) static void initialize(void) {
    next_epoll_ctl = (int (*)(int, int, int, struct epoll_event*))dlsym(RTLD_NEXT, "epoll_ctl");
    if (!next_epoll_ctl) abort();
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event) {
    if (op >= 1 && op <= 3) atomic_fetch_add_explicit(&counts[op], 1, memory_order_relaxed);
    int result = next_epoll_ctl(epfd, op, fd, event);
    if (result < 0) atomic_fetch_add_explicit(&failures, 1, memory_order_relaxed);
    return result;
}

__attribute__((destructor)) static void report(void) {
    fprintf(stderr, "{\"epoll_ctl_add\":%llu,\"epoll_ctl_mod\":%llu,\"epoll_ctl_del\":%llu,\"failed_calls\":%llu}\n",
            (unsigned long long)atomic_load(&counts[EPOLL_CTL_ADD]),
            (unsigned long long)atomic_load(&counts[EPOLL_CTL_MOD]),
            (unsigned long long)atomic_load(&counts[EPOLL_CTL_DEL]),
            (unsigned long long)atomic_load(&failures));
}
