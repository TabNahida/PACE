# I/O backends

[简体中文](io-backends.zh.md) · [Runtime](README.md)

## Overview

`io_backend` names the readiness poller an `io_context` uses; the choice changes no socket or protocol code.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/runtime/io_backend.hpp>` (also `<pace/runtime.hpp>`) | `pace_runtime` | always built; io_uring needs `--uring=y` on Linux |

All backends are **readiness** backends: they report that a socket can be read or written, and PACE then performs
the nonblocking socket call itself. The io_uring backend submits `IORING_OP_POLL_ADD`/`POLL_REMOVE`; the Windows
IOCP backend uses `ProcessSocketNotifications` with a completion port. Neither does completion-based I/O (io_uring
`SEND`/`RECV`, registered buffers, zero copy, or overlapped `WSARecv`/`WSASend`/`AcceptEx`), and a newer backend is
not automatically faster: measure your own workload.

## Synopsis

```cpp
#include <pace/runtime.hpp>

#include <iostream>

int main()
{
    pace::io_context io;                         // automatic: PACE_IO_BACKEND, else the platform default
    std::cout << pace::backend_name(io.backend()) << '\n';
    pace::runtime workers(2, pace::io_backend::automatic);
    std::cout << pace::backend_name(workers.context(0).backend()) << '\n';
}
```

Choosing io_uring for a program that uses the automatic default, without changing its code:

```sh
xmake f -m release --uring=y -y
xmake -y
PACE_IO_BACKEND=io_uring ./your-server
```

## Types

### `io_backend`

```cpp
enum class io_backend
{
    automatic,
    epoll,
    kqueue,
    wsapoll,
    io_uring,
    iocp
};
```

| Value | Platform | Notes |
|:--|:--|:--|
| `automatic` | all | `PACE_IO_BACKEND` if set, otherwise the platform default below |
| `epoll` | Linux | default on Linux |
| `kqueue` | macOS, FreeBSD | default there |
| `wsapoll` | Windows | fallback when IOCP notifications are unavailable; always used by 32-bit builds |
| `io_uring` | Linux built with `--uring=y` | explicit choice only |
| `iocp` | 64-bit Windows, build 20348 or later | default on Windows when available |

The value is passed to `io_context`'s and `runtime`'s constructors. `io_context::backend()` reports the backend
actually in use, never `automatic`.

### Selection

1. A constructor argument other than `automatic` is used as given. If that backend is not available in this build
   or on this system, construction throws `std::system_error` with `std::errc::not_supported`; PACE never
   substitutes another backend for an explicit choice.
2. With `automatic`, the environment variable `PACE_IO_BACKEND` is read. Accepted values are the enumerator names
   (`automatic`, `epoll`, `kqueue`, `wsapoll`, `io_uring`, `iocp`); the named backend is then treated as in step 1.
   Any other value throws `std::invalid_argument` at construction.
3. Otherwise the platform default is used: epoll on Linux, kqueue on macOS and FreeBSD, and on Windows IOCP if the
   system supports socket notifications, else WSAPoll.

## Functions

### `backend_name`

```cpp
inline const char* backend_name(io_backend value) noexcept;
```

The lower-case name of `value`, the same string `PACE_IO_BACKEND` accepts: `"epoll"`, `"kqueue"`, `"wsapoll"`,
`"io_uring"`, `"iocp"` or `"automatic"`.

## Errors

| Error | When |
|:--|:--|
| `std::invalid_argument` | `PACE_IO_BACKEND` holds an unknown name |
| `std::system_error(not_supported)` | An explicitly chosen backend is unavailable: io_uring without `--uring=y` or off Linux, a kernel without `IORING_FEAT_NODROP`, IOCP off Windows, in a 32-bit build or before build 20348, or a native backend of another platform |
| `std::system_error` (native code) | The kernel refused the poller, for example io_uring setup blocked by a container's seccomp policy or a low locked-memory limit |

PACE does not change system limits for you.

## Threads and concurrency

Every context owns its own poller: an epoll or kqueue descriptor, an io_uring ring, or a completion port, plus a
wakeup channel (eventfd on Linux, a kqueue user event, `PostQueuedCompletionStatus` on Windows) that `spawn`, `post`
and `stop` use from other threads. No helper thread polls sockets. The one-thread-per-context rules of
[`io_context`](io-context.md#threads-and-concurrency) apply to every backend.

## Lifetime and ownership

A context registers a socket on its first wait and keeps that registration until the socket is cancelled or
released. When you own a raw socket:

- `io_context::cancel(fd)` interrupts its waits and leaves it open; it can be awaited again.
- `io_context::release_socket(fd)` must be called immediately before you close it. It cancels the waits and drops
  the backend's registration, so a later socket that reuses the descriptor number cannot be confused with the old
  one.

PACE's sockets, and the DNS module's resolver, do this themselves. Internally, registrations carry monotonically
increasing integer keys, never pointers to awaiters or buffers, so a notification that arrives late (after
cancellation or descriptor reuse) is discarded instead of resuming a destroyed coroutine. On IOCP a socket
re-registered after a cancellation waits for the removal to be acknowledged first.

## Limits and defaults

| Item | Value |
|:--|:--|
| Default | `io_backend::automatic` |
| Linux default | epoll, with persistent level-triggered interests |
| macOS / FreeBSD default | kqueue, one-shot filters |
| Windows default | IOCP (64-bit, build 20348+), else WSAPoll |
| io_uring submission ring | 256 entries per context; a batch size for submissions, not a connection limit |
| io_uring kernel requirement | `IORING_FEAT_NODROP` |
| io_uring polls | one-shot, so the same workload can need more submissions than epoll |
| IOCP sockets | Microsoft Winsock provider sockets; one-shot level-triggered registrations |
| Readiness events per poll | 256 |

io_uring is not the Linux default: it stays an explicit choice until measurements justify a change. The
`pace_iocp_diagnostic` test program exercises the Windows API directly, without PACE's poller; the 64-bit-only
limit for IOCP comes from its result that every 32-bit registration fails with `WSAEINVAL` under WoW64.

## See also

- [`io_context`](io-context.md) and [`runtime`](runtime.md): the constructors that take a backend
- [Build options](../build-options.md): `--uring`
- [Platform support](../platform-support.md)
- [HTTP gateway performance](../../performance/http-gateway.md)
- Microsoft: [Winsock socket state notifications](https://learn.microsoft.com/en-us/windows/win32/winsock/winsock-socket-state-notifications)
  and [`SOCK_NOTIFY_REGISTRATION`](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/ns-winsock2-sock_notify_registration);
  [liburing](https://github.com/axboe/liburing)
