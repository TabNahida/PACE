#pragma once
namespace pace {
enum class io_backend { automatic, epoll, kqueue, wsapoll, io_uring, iocp };
inline const char* backend_name(io_backend value) noexcept {
    switch (value) {
    case io_backend::epoll: return "epoll";
    case io_backend::kqueue: return "kqueue";
    case io_backend::wsapoll: return "wsapoll";
    case io_backend::io_uring: return "io_uring";
    case io_backend::iocp: return "iocp";
    default: return "automatic";
    }
}
}
