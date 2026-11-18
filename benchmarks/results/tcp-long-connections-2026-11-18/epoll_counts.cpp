// Linux diagnostic interposer. Count returned events separately on each worker.
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <sys/epoll.h>

namespace {
struct counts {
    unsigned long long waits{}, events{}, single{}, multiple{};
    ~counts() {
        if (waits) std::fprintf(stderr, "epoll-counts %llu %llu %llu %llu\n", waits, events, single, multiple);
    }
};
thread_local counts local;
}
extern "C" int epoll_wait(int fd, epoll_event* events, int maximum, int timeout) {
    static const auto next = reinterpret_cast<decltype(&epoll_wait)>(dlsym(RTLD_NEXT, "epoll_wait"));
    const int result = next(fd, events, maximum, timeout);
    ++local.waits;
    if (result > 0) local.events += static_cast<unsigned>(result);
    if (result == 1) ++local.single;
    if (result > 1) ++local.multiple;
    return result;
}
