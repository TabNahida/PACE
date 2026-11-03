# Select an I/O backend

The development branch after 0.4.0 exposes selection without changing TCP/UDP or
protocol code:

```cpp
#include <pace/runtime.hpp>
pace::io_context io(pace::io_backend::io_uring);
pace::runtime workers(4, pace::io_backend::io_uring);
// io.backend() reports the selected enum; pace::backend_name(...) returns its name.
```

For existing helpers such as `app.run()` / `run_workers()`:

```sh
xmake f -m release --uring=y -y
xmake -y
PACE_IO_BACKEND=io_uring ./your-server
```

`--uring=y` adds liburing on Linux. It does not change the Linux default.
The core without that option still has no third-party runtime dependencies.
`PACE_IO_BACKEND` is consulted only for `automatic` constructors; an explicit
constructor takes precedence. Accepted values are `automatic`, `epoll`, `kqueue`,
`wsapoll`, `io_uring`, `iocp`. Invalid names fail immediately; an explicitly selected
unavailable backend throws instead of silently substituting another.

| Platform | Automatic selection | Other choice |
|---|---|---|
| Linux | epoll | io_uring, built with `--uring=y` |
| macOS / FreeBSD | kqueue | — |
| Windows | 64-bit IOCP when supported by the OS API | WSAPoll compatibility path |

These are **socket readiness backends**. The current io_uring implementation uses
`IORING_OP_POLL_ADD`/`POLL_REMOVE`; Windows IOCP uses the documented
`ProcessSocketNotifications` API and a completion port. Actual socket read/write
operations still use nonblocking socket calls. This is not an implementation of
io_uring direct SEND/RECV, registered buffers, zero-copy or overlapped
WSARecv/WSASend/AcceptEx. Merely selecting a newer backend does not guarantee a
speedup; compare your own workload and the [local Linux results](../gateway-performance.md).

## Linux io_uring

A 256-entry submission ring batches subscriptions/cancellations, submitting when
full. This is a batch size, **not** a maximum number of connections. Small rings
bound per-worker locked memory. The kernel must support `IORING_FEAT_NODROP`;
restricted containers/seccomp and low per-user locked-memory limits can still
prevent construction. PACE does not alter these system limits automatically.
Each context owns its ring and an eventfd wakeup. Monotonic registration identities
reject late poll/cancel completions after cancellation or descriptor reuse.

Linux epoll retains persistent interests to avoid unnecessary re-registration.
io_uring currently uses one-shot polls, so it can issue more submission/completion
work for the same workload. It remains an explicit choice until measurements
justify changing the default.

## Windows IOCP

Socket state notifications require Windows build 20348+ and Microsoft Winsock
provider sockets. PACE x86 builds use WSAPoll: the independent SDK probe returns
WSAEINVAL for registrations under WoW64 on Server 2025. Explicit IOCP selection
on 32-bit builds reports `not_supported`; there is no silent fallback. The function is loaded dynamically so older Windows can use
WSAPoll. Each context owns a completion port and one-shot level-triggered socket
registrations. Cross-thread wakeups use `PostQueuedCompletionStatus`; no helper
thread scans sockets with WSAPoll when IOCP is selected.

Registrations carry opaque monotonically increasing integer keys, not awaiter or
buffer pointers, so late packets cannot dereference a destroyed coroutine.
Re-registration after cancellation waits for the REMOVE acknowledgement before
changing the completion key.
The backend is still subject to PACE's one-reactor-per-worker ownership rules.
Multi-worker listener sharing on Windows is a separate concern: `run_workers`
currently uses `SO_REUSEPORT` and supports only one Windows worker.

Microsoft describes this API as an alternative to WSAPoll and also explains when
[overlapped I/O can use fewer kernel transitions](https://learn.microsoft.com/en-us/windows/win32/winsock/winsock-socket-state-notifications).
See the [registration contract](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/ns-winsock2-sock_notify_registration)
and [liburing API source](https://github.com/axboe/liburing).

Backend tests exercise queue pressure beyond the ring size, cancellation, late
notifications and descriptor reuse. Remote OS checks are functional evidence;
performance results in this repository are measured on local Linux only.

### Raw-handle ownership

Use `io_context::cancel(fd)` to interrupt waits while retaining an open socket.
When the owner closes a raw socket, call `release_socket(fd)` immediately before
that native close. Socket wrappers and the c-ares adapter do this automatically.
This distinguishes cancellation (which waits for REMOVE before re-registering)
from closure (which auto-deregisters and may suppress REMOVE). A context must not
retain an old key for a recycled native handle.

The independent `pace_iocp_diagnostic` executable exercises the SDK API directly,
without PACE's poller, and logs structure layout, flags and native return codes.
On the tested Server 2025 WoW64 runner, all 32-bit variants returned registration
error WSAEINVAL; x64/ARM64 one-shot level registrations succeeded. This is the
basis for the explicit 64-bit support boundary, not a claim that all future
Windows releases necessarily behave identically.
