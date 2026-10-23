#pragma once

#include <pace/platform.hpp>

#if defined(__FreeBSD__)
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

// Read-only, opt-in snapshots for the FreeBSD HTTP/2 regression. None of these
// calls consumes socket data, changes readiness interests, or retries I/O.
inline void pace_test_http2_snapshot(const char* label, pace::native_socket_type fd) noexcept {
    const int saved_errno = errno;
    pollfd readiness{fd, POLLIN | POLLOUT, 0};
    const int poll_result = ::poll(&readiness, 1, 0);
    const int poll_error = poll_result < 0 ? errno : 0;
    const int flags = ::fcntl(fd, F_GETFL);
    const int flags_error = flags < 0 ? errno : 0;
    std::fprintf(stderr, "HTTP2 socket: %s fd=%d poll=%d revents=0x%x poll_errno=%d flags=%d/%d nonblock=%d",
                 label, fd, poll_result, static_cast<unsigned>(readiness.revents), poll_error,
                 flags, flags_error, flags >= 0 && (flags & O_NONBLOCK) != 0);

    const auto option = [fd](const char* name, int level, int key) {
        int value = -1;
        socklen_t length = sizeof(value);
        const int result = ::getsockopt(fd, level, key, &value, &length);
        const int error = result < 0 ? errno : 0;
        std::fprintf(stderr, " %s=%d/%d", name, value, error);
    };
    option("sndbuf", SOL_SOCKET, SO_SNDBUF);
    option("sndlowat", SOL_SOCKET, SO_SNDLOWAT);
    option("rcvbuf", SOL_SOCKET, SO_RCVBUF);
    option("rcvlowat", SOL_SOCKET, SO_RCVLOWAT);
    option("mss", IPPROTO_TCP, TCP_MAXSEG);

    const auto queued = [fd](const char* name, unsigned long request) {
        int value = -1;
        const int result = ::ioctl(fd, request, &value);
        const int error = result < 0 ? errno : 0;
        std::fprintf(stderr, " %s=%d/%d", name, value, error);
    };
    queued("read_bytes", FIONREAD);
    // FIONWRITE includes TCP bytes not yet acknowledged by the peer.
    // FIONSPACE alone does not guarantee writability (e.g. mbuf limits).
    queued("unacked_bytes", FIONWRITE);
    queued("send_space", FIONSPACE);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    errno = saved_errno;
}
#endif
