# Handles and signals

[简体中文](handles-and-signals.zh.md) · [Runtime](README.md)

## Overview

Readiness waits on descriptors that are not sockets (pipes, terminals, TUN devices), waits on Windows kernel object
handles, and process signals (`SIGINT`, `SIGTERM`, `SIGHUP`, `SIGUSR1`, `SIGUSR2` and their Windows equivalents) as
values a coroutine can `co_await`.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/runtime/io_context.hpp>` (also `<pace/runtime.hpp>`) | `pace_runtime` | always built |
| `<pace/runtime/signal.hpp>` (also `<pace/runtime.hpp>`) | `pace_runtime` | always built |

`io_context::wait_readable`, `wait_writable` and `wait_ready` (see [`io_context`](io-context.md#readiness-waits))
accept any descriptor the backend can poll, not only sockets. Windows has no such descriptors; there,
`io_context::wait_handle` waits for an event, process or other kernel object instead. `signal_set` and
`wait_for_signal` turn signals into awaitable values, so a service shuts down gracefully with two lines.

## Synopsis

Graceful shutdown on Ctrl-C or `SIGTERM`: the serving tasks run in a task group, and the signal cancels it.

```cpp
#include <pace/runtime.hpp>

#include <chrono>
#include <iostream>

pace::task<void> serve_forever()
{
    for (;;)
    {
        co_await pace::sleep_for(std::chrono::seconds(1)); // stands in for an accept loop
    }
}

int main()
{
    pace::run(
        []() -> pace::task<void>
        {
            co_await pace::with_task_group(
                [](pace::task_group& group) -> pace::task<void>
                {
                    (void)group.spawn(serve_forever());
                    co_await pace::wait_for_signal({pace::process_signal::interrupt, pace::process_signal::terminate});
                    group.cancel(); // the serving tasks see operation_canceled, unwind, and the group joins them
                }
            );
            std::cout << "stopped\n";
        }
    );
}
```

A long-lived subscription that reloads on `SIGHUP` and keeps running:

```cpp
pace::task<void> reload_on_hangup(pace::io_context& io)
{
    pace::signal_set signals(io, {pace::process_signal::hangup});
    for (;;)
    {
        co_await signals.wait(); // a SIGHUP raised while reloading is reported by the next wait
        reload_configuration();
    }
}
```

## Types

### `process_signal`

```cpp
enum class process_signal
{
    hangup,
    interrupt,
    terminate,
    user1,
    user2
};
```

| Value | POSIX | Windows |
|:--|:--|:--|
| `hangup` | `SIGHUP` | only `deliver_signal` |
| `interrupt` | `SIGINT` | `CTRL_C_EVENT`, `CTRL_BREAK_EVENT` |
| `terminate` | `SIGTERM` | `CTRL_CLOSE_EVENT`, `CTRL_LOGOFF_EVENT`, `CTRL_SHUTDOWN_EVENT`; a service's stop request through `deliver_signal` |
| `user1` | `SIGUSR1` | only `deliver_signal` |
| `user2` | `SIGUSR2` | only `deliver_signal` |

### `signal_set`

```cpp
class signal_set
{
public:
    struct awaiter;

    signal_set(io_context& io, std::initializer_list<process_signal> signals);
    ~signal_set();
    signal_set(const signal_set&) = delete;
    signal_set& operator=(const signal_set&) = delete;

    [[nodiscard]] awaiter wait() noexcept;
    [[nodiscard]] bool contains(process_signal value) const noexcept;
    [[nodiscard]] io_context& context() const noexcept;
};
```

A subscription of one context to some signals. It is neither copyable nor movable.

| Member | Behaviour |
|:--|:--|
| `signal_set(io, signals)` | Subscribes to `signals` (duplicates are ignored). From now on every delivery of these signals is counted for this set, whether or not a wait is pending. Throws `std::invalid_argument` for an empty list or an unknown value, `std::system_error` if the disposition cannot be installed or every notifier slot is in use. |
| `~signal_set()` | Unsubscribes. When it was the last subscription to a signal in the process, the previous disposition is restored. |
| `wait()` | `co_await` suspends until one of the signals is delivered and returns it. A delivery that is already counted completes it at once. Several pending deliveries are reported one per wait, in enumeration order (`hangup` first). |
| `contains(value)` | Whether `value` is subscribed. |
| `context()` | The context given to the constructor. |

**Ownership model.** Dispositions are process-wide and reference-counted per signal:

- The first `signal_set` in the process that subscribes to a signal installs PACE's handler (POSIX `sigaction` with
  `SA_RESTART`; on Windows a console control handler for `interrupt` and `terminate`) and saves the previous
  disposition. The last one to be destroyed restores it. While PACE owns a signal, the replaced handler is not
  called; the OS default action (termination for `SIGINT` and `SIGTERM`) does not run either.
- Deliveries are broadcast. Every `signal_set` subscribed to a signal, in any context on any thread, sees each
  delivery that happens after its construction. Several sets in one context each see it too.
- A delivery is never lost between waits: each set remembers how many deliveries it has reported. A delivery
  before the set exists is not reported to it.
- Changing a disposition yourself (`sigaction`, `signal`, `SetConsoleCtrlHandler`) while a `signal_set` subscribes
  to that signal is not supported; restoring then overwrites your change.

**Windows.** The console handler for `terminate` events (closing the console window, logoff, shutdown) blocks its
thread after the delivery, because Windows ends the process as soon as the handler returns; the program has the
system's grace period to shut down and return from `main`. A service does not receive console events: call
`deliver_signal(process_signal::terminate)` from its control handler for `SERVICE_CONTROL_STOP` and
`SERVICE_CONTROL_SHUTDOWN`.

### `signal_set::awaiter`

```cpp
struct awaiter
{
    signal_set& set;
    std::error_code error;
    process_signal value{};
    std::coroutine_handle<> continuation;
    cancellation_token token{};
    io_context::cancellation_handle registration{};
    awaiter* previous = nullptr;
    awaiter* next = nullptr;
    bool linked = false;

    explicit awaiter(signal_set& owner) noexcept;
    awaiter(const awaiter&) = delete;
    awaiter& operator=(const awaiter&) = delete;
    ~awaiter();
    bool await_ready();
    template <class P>
    bool await_suspend(std::coroutine_handle<P> handle);
    process_signal await_resume();
};
```

The result of `wait()`: `co_await` it once, immediately, on the context's thread, and never move or modify it while
it is suspended. `await_ready` consumes a delivery that is already counted. `await_suspend` copies the awaiting
task's cancellation token, registers an `on_cancel` callback for it, and registers the wait with the context.
`await_resume` returns the signal or throws. The destructor unlinks a wait whose coroutine is destroyed while
suspended.

### `io_context::handle_awaiter` (Windows)

```cpp
#ifdef _WIN32
struct handle_awaiter
{
    io_context& context;
    HANDLE object;
    std::error_code error;
    std::coroutine_handle<> continuation;
    cancellation_token token{};
    cancellation_handle registration{};
    void* wait = nullptr;
    std::uint64_t id = 0;
    bool await_ready() noexcept;
    template <class P>
    bool await_suspend(std::coroutine_handle<P> waiting);
    void await_resume();
};
#endif
```

The result of `wait_handle`, with the same rules as `io_awaiter`. `wait` is the thread-pool wait object while the
coroutine is suspended, and `id` its key in the context.

## Functions

### Readiness on non-socket descriptors

```cpp
io_awaiter wait_readable(native_socket_type fd) noexcept;
io_awaiter wait_writable(native_socket_type fd) noexcept;
io_awaiter wait_ready(native_socket_type fd) noexcept;
```

On POSIX `native_socket_type` is `int`, and these waits work on any descriptor whose driver supports polling, on
every backend. The reactor makes no socket call on the descriptor (no `SO_ERROR`, no `recv`): it only reports
readiness, and you perform the nonblocking `read` or `write` afterwards.

| Descriptor | epoll | io_uring | kqueue |
|:--|:--|:--|:--|
| Pipe, FIFO | yes | yes | yes |
| Terminal, pseudo-terminal | yes | yes | yes |
| `eventfd`, `timerfd`, `signalfd`, `pidfd`, `inotify` (Linux) | yes | yes | — |
| Linux TUN `/dev/net/tun`, each `IFF_MULTI_QUEUE` queue, Android `VpnService` descriptor | yes | yes | — |
| macOS and iOS `utun` (a `PF_SYSTEM` socket) | — | — | yes |
| FreeBSD `/dev/tun*` | — | — | yes |
| Regular file | rejected (`EPERM`) | always ready | ready until end of file |
| Device without poll support (`/dev/null`) | rejected (`EPERM`) | always ready | rejected (`EINVAL`) |

A rejected descriptor makes the `co_await` throw `std::system_error` with the kernel's code. Put the descriptor in
nonblocking mode (`O_NONBLOCK`) yourself: a readiness wait does not change it, and a blocking `read` after a wake-up
can still block. A TUN descriptor returns one packet per `read`; read until `EAGAIN` before waiting again. Before
closing a descriptor you awaited, call `io.release_socket(fd)` (the name is historical: it accepts any descriptor).
The [TUN guide](../../guides/tun-devices.md) shows a complete read/write loop.

On Windows, readiness waits accept only Winsock sockets; use `wait_handle` for everything else.

### `io_context::wait_handle` (Windows)

```cpp
#ifdef _WIN32
handle_awaiter wait_handle(HANDLE object) noexcept;
#endif
```

`co_await io.wait_handle(object)` suspends until a waitable kernel object is signaled: an event (for example
Wintun's `WintunGetReadWaitEvent`), a semaphore, a process or thread, a waitable timer, a change notification. Like
`WaitForSingleObject`, the wait consumes the signal of an auto-reset event or a semaphore. Mutexes are not supported:
ownership would belong to a thread-pool thread. Any number of waits may target one handle, and each one is
independent. The handle must stay open until the wait ends.

Each wait registers a thread-pool wait (`CreateThreadpoolWait`); its callback posts the wait's key to the context,
which wakes the poller: a completion packet on the IOCP backend, the loopback wake-up datagram on the WSAPoll backend
(the pool's wait thread is that backend's helper thread). Cancellation and `stop()` unregister the pool wait and
wait for a running callback before the coroutine resumes, so no callback outlives the operation.

POSIX has no `wait_handle`. Its equivalent is a readiness wait on a descriptor: an `eventfd` for an event, a
`pidfd` (Linux) for a process, a pipe for anything else.

### `wait_for_signal`

```cpp
task<process_signal> wait_for_signal(io_context& io, std::initializer_list<process_signal> signals);
task<process_signal> wait_for_signal(std::initializer_list<process_signal> signals);
```

Subscribes to `signals` when it is **called**, and returns a task that waits for the first delivery and returns it.
The task owns the subscription, so a signal raised after the call and before the task is awaited is not lost, and
destroying the task unsubscribes. The second form uses `this_context()` and throws `std::logic_error` when no context
runs on this thread. For repeated signals keep a `signal_set` instead.

### `deliver_signal`

```cpp
void deliver_signal(process_signal value) noexcept;
```

Delivers `value` to every subscribed `signal_set` as if the operating system had raised it, without running any
default action or replaced handler. It is thread-safe, and async-signal-safe on POSIX. Use it from a Windows service
control handler, or to trigger a reload or a shutdown from inside the program. With no subscriber it does nothing.

```cpp
DWORD WINAPI service_control(DWORD control, DWORD, LPVOID, LPVOID)
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN)
    {
        report_stop_pending();
        pace::deliver_signal(pace::process_signal::terminate);
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}
```

### `signal_name`

```cpp
const char* signal_name(process_signal value) noexcept;
```

`"SIGHUP"`, `"SIGINT"`, `"SIGTERM"`, `"SIGUSR1"` or `"SIGUSR2"`, on every platform; `"unknown"` otherwise.

## Errors

| Error | When |
|:--|:--|
| `std::system_error(operation_canceled)` | A pending readiness, handle or signal wait when its context stops; a readiness wait after `cancel(fd)` |
| `std::system_error` with the token's code | A wait whose task token is cancelled (`operation_canceled`, or `timed_out` for a deadline) |
| `std::system_error` (kernel code) | Waiting on a descriptor the backend rejects (`EPERM`, `EINVAL`); `CreateThreadpoolWait` failing; `sigaction`, `SetConsoleCtrlHandler`, `pipe` or `CreateEventW` failing when a set is created or first waits |
| `std::system_error(bad_file_descriptor)` | `wait_handle` on a null handle or `INVALID_HANDLE_VALUE` |
| `std::system_error(resource_unavailable_try_again)` | More than 256 contexts wait for signals at the same time |
| `std::invalid_argument` | `signal_set` with an empty list or a value outside `process_signal` |
| `std::logic_error` | A second pending wait on one `signal_set`; a wait awaited on a thread that is not running its context; `wait_for_signal` without `io` and without a running context |

## Threads and concurrency

- Readiness and handle waits follow the [`io_context`](io-context.md#threads-and-concurrency) rules: they run on
  the context's thread, and a descriptor has at most one pending wait per direction. `wait_handle` has no such limit.
- A `signal_set` can be created and destroyed on any thread, also before `run()`. `wait()` runs on the thread running
  its context, one pending wait per set; use several sets for several concurrent waiters.
- PACE's POSIX handler only increments lock-free counters and writes one byte to a pipe per context that waits for
  signals; it calls nothing else and preserves `errno`. Each such context has one internal pump task that waits on
  its pipe (an ordinary readiness wait, so every backend including io_uring works unchanged) and resumes the
  waits whose signals arrived. On Windows the notifier is an auto-reset event awaited with `wait_handle`.
- `signalfd` is deliberately not used: it needs the signal blocked in every thread of the process, and one read
  consumes a signal for all contexts. `EVFILT_SIGNAL` would work on kqueue, but a handler is needed anyway to keep the
  default action from running, so one path serves every POSIX system.
- `deliver_signal` may be called from any thread, and from a POSIX signal handler.

## Lifetime and ownership

- Readiness waits borrow the descriptor and `wait_handle` borrows the handle; both must stay open until the wait
  ends. Call `release_socket(fd)` before closing an awaited descriptor.
- A `signal_set` owns its subscription. Destroy it only while no wait is pending on it, unless the waiting
  coroutine itself is being destroyed. It may outlive its context, but must not be waited on afterwards.
- The context keeps its signal notifier (a pipe or an event) from the first signal wait until it is destroyed.
- `wait_for_signal`'s task owns its `signal_set`.

## Limits and defaults

| Item | Value |
|:--|:--|
| Signals | `SIGHUP`, `SIGINT`, `SIGTERM`, `SIGUSR1`, `SIGUSR2` |
| POSIX handler flags | `SA_RESTART`, empty mask |
| Pending waits per `signal_set` | 1 |
| Contexts waiting for signals at once | 256 |
| Order of several pending deliveries | enumeration order of `process_signal` |
| Deliveries counted per signal | 2³² before the counter wraps; the difference stays exact |
| Notifier per waiting context | one nonblocking pipe (POSIX) or auto-reset event (Windows) |
| `wait_handle` waits per handle | unlimited; one thread-pool wait each |

The operating system may merge deliveries of the same signal that arrive while it is pending; PACE reports every
delivery its handler sees.

## See also

- [`io_context`](io-context.md): readiness waits, `cancel`, `release_socket`
- [Structured concurrency](structured-concurrency.md): task groups and cancellation tokens
- [Guide: TUN devices](../../guides/tun-devices.md)
- [Tutorial: orderly shutdown](../../tutorial/09-graceful-shutdown.md)
- [I/O backends](io-backends.md) · [Platform support](../platform-support.md)
- Microsoft: [Thread pool waits](https://learn.microsoft.com/en-us/windows/win32/api/threadpoolapiset/nf-threadpoolapiset-createthreadpoolwait),
  [console control handlers](https://learn.microsoft.com/en-us/windows/console/handlerroutine);
  [Wintun](https://www.wintun.net/)
