# `pace::io_context`

[简体中文](io-context.zh.md) · [Runtime](README.md)

## Overview

An `io_context` is one reactor run by one thread: it owns a native readiness poller, the queues of ready coroutines
and posted callbacks, and the timers of the tasks it runs.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/runtime/io_context.hpp>` (also `<pace/runtime.hpp>`) | `pace_runtime` | always built |
| `<pace/runtime/platform.hpp>` (also `<pace/runtime.hpp>`) | `pace_runtime` | always built |

Most programs never create one directly: [`pace::run`](run.md) makes a fresh context for one entry task, and
[`pace::runtime`](runtime.md) owns one per worker. Create an `io_context` yourself to control its thread and its
lifetime. Sockets, listeners, timers and waits all belong to exactly one context.

## Synopsis

```cpp
#include <pace/runtime.hpp>

#include <chrono>

using namespace std::chrono_literals;

pace::task<void> tick(pace::io_context& io)
{
    co_await io.sleep_for(10ms);
    io.stop(); // run() returns once the queues have drained
}

int main()
{
    pace::io_context io;
    auto guard = io.call_after(1s, [&io] { io.stop(); }); // keep the handle to keep the timer armed
    io.spawn(tick(io));
    io.run();
}
```

## Types

### `io_context`

```cpp
class io_context
{
public:
    explicit io_context(io_backend selected = io_backend::automatic);
    [[nodiscard]] io_backend backend() const noexcept;
    ~io_context();
    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;

    std::shared_ptr<void> local_state(
        std::shared_ptr<const void> key,
        const std::function<std::shared_ptr<void>()>& create
    );
    void* find_local_state(const void* key);
    void spawn(task<void> work);
    void post(std::function<void()> work);
    void run();
    void stop() noexcept;
    bool stopped() const noexcept;
    static io_context* current() noexcept;
    void release_socket(native_socket_type fd) noexcept;
    void cancel(native_socket_type fd) noexcept;

    class timer_handle;
    [[nodiscard]] timer_handle call_at(std::chrono::steady_clock::time_point deadline, std::function<void()> callback);
    template <class Rep, class Period>
    [[nodiscard]] timer_handle call_after(std::chrono::duration<Rep, Period> duration, std::function<void()> callback);

    class cancellation_handle;
    [[nodiscard]] cancellation_handle on_cancel(cancellation_token, std::function<void(std::error_code)>);

    struct io_awaiter;
    io_awaiter wait_readable(native_socket_type fd) noexcept;
    io_awaiter wait_writable(native_socket_type fd) noexcept;
    io_awaiter wait_ready(native_socket_type fd) noexcept;
#ifdef _WIN32
    struct handle_awaiter;
    handle_awaiter wait_handle(HANDLE object) noexcept;
#endif

    struct timer_awaiter;
    template <class Rep, class Period>
    timer_awaiter sleep_for(std::chrono::duration<Rep, Period> duration) noexcept;

    struct yield_awaiter;
    yield_awaiter yield() noexcept;
};
```

The context is neither copyable nor movable.

#### Construction and destruction

| Member | Behaviour |
|:--|:--|
| `io_context(selected)` | Creates the poller for `selected`; `automatic` consults `PACE_IO_BACKEND` first (see [I/O backends](io-backends.md)). Throws `std::system_error` when the native poller cannot be created or the backend is unavailable (`std::errc::not_supported`), `std::invalid_argument` for an unknown `PACE_IO_BACKEND`. |
| `backend()` | The backend actually in use: never `automatic`. |
| `~io_context()` | Requests stop, cancels registered waits, then destroys queued tasks and callbacks, the root frames it still owns and its `local_state` services, in that order. Unstarted bodies are not run. It does not join a thread: `run()` must have returned first. |

#### Submitting and running work

| Member | Behaviour |
|:--|:--|
| `spawn(work)` | Takes ownership of a root task and schedules it. The body never runs inline inside `spawn`. An empty task is ignored. Thread-safe, before or during `run()`. Throws `std::system_error(operation_canceled)` once the context is stopped. |
| `post(work)` | Queues a callback to run on this context, never inline. Thread-safe. Throws `std::invalid_argument` for an empty function and `std::system_error(operation_canceled)` once stopped. |
| `run()` | Runs the context on the calling thread until `stop()` is requested and the queued work has drained, then rethrows the first recorded error. An idle context keeps waiting: there is no work guard and no "run until out of work". Throws `std::logic_error` when this thread is already running a context (nested run), when another thread is running this one, or when this context has run before. |
| `stop()` | Requests a permanent stop. Thread-safe, idempotent, does not wait. It wakes the poller; it does not close sockets. |
| `stopped()` | Whether stop was requested. Thread-safe. `true` does not mean `run()` has returned. |
| `current()` | The context `run()` is executing on the calling thread, or `nullptr`. |

When stop is requested the context cancels every registered readiness wait and timer (they resume with
`operation_canceled`), dispatches its `on_cancel` registrations, discards pending callback timers, and keeps
running ready coroutines and posted callbacks until all queues are empty. Coroutines resumed during this drain
observe the stop and should finish; `spawn` and `post` already refuse new work. A context cannot be restarted.

#### Context-local services

| Member | Behaviour |
|:--|:--|
| `local_state(key, create)` | Returns the service stored under the address `key.get()`, calling `create` to make it the first time. The context keeps both `key` and the value until it is destroyed; values may own sockets. Throws `std::invalid_argument` for a null key, an empty `create` or a null result, and `std::system_error(operation_canceled)` once stopped. |
| `find_local_state(key)` | Returns the stored value for `key`, or `nullptr`, without touching reference counts. The pointer stays valid until the context is destroyed. Throws `std::system_error(operation_canceled)` once stopped. |

Both must be called from the thread running this context (otherwise `std::logic_error`). They exist for bounded
per-context services such as connection pools, not per-request data.

#### Raw descriptors

| Member | Behaviour |
|:--|:--|
| `cancel(fd)` | Unregisters the descriptor's pending waits and resumes them with `operation_canceled`. Does nothing for an unknown descriptor. It does not close the descriptor or stop the context, and the descriptor can be awaited again. |
| `release_socket(fd)` | `cancel(fd)` and then drops the backend's registration for it. Call it immediately before closing a raw socket yourself, so a recycled descriptor number never matches an old registration. |

Both are `noexcept` and must run on the context's thread, or while no thread is running it (before `run()` or after
it returns). From another thread, `post` a callback that calls them. PACE's own socket types do this for you.

### `io_context::timer_handle`

```cpp
class timer_handle
{
public:
    timer_handle() noexcept = default;
    ~timer_handle();
    timer_handle(timer_handle&&) noexcept;
    timer_handle& operator=(timer_handle&&) noexcept;
    timer_handle(const timer_handle&) = delete;
    timer_handle& operator=(const timer_handle&) = delete;
    void cancel() noexcept;
};
```

The owner of one callback timer made by `call_at` or `call_after`. The timer stays armed while the handle exists.
`cancel()`, the destructor and move assignment onto an armed handle disarm it and destroy the callback (with its
captures) at once. A handle may outlive its context; cancelling it then does nothing.

### `io_context::cancellation_handle`

```cpp
class cancellation_handle
{
public:
    cancellation_handle() noexcept = default;
    ~cancellation_handle();
    cancellation_handle(cancellation_handle&&) noexcept;
    cancellation_handle& operator=(cancellation_handle&&) noexcept;
    cancellation_handle(const cancellation_handle&) = delete;
    cancellation_handle& operator=(const cancellation_handle&) = delete;
    void cancel() noexcept;
};
```

The owner of one `on_cancel` registration. `cancel()`, the destructor and move assignment onto a live handle
unregister the callback; a callback that has not run by then never runs. A handle may outlive its context.

### `io_context::io_awaiter`, `timer_awaiter`, `yield_awaiter`

```cpp
struct io_awaiter
{
    io_context& context;
    native_socket_type fd;
    bool writable;
    std::error_code error;
    std::coroutine_handle<> continuation;
    bool either = false;
    cancellation_token token{};
    cancellation_handle registration{};
    bool await_ready() noexcept;
    template <class P>
    bool await_suspend(std::coroutine_handle<P> handle);
    void await_resume();
};

struct timer_awaiter
{
    io_context& context;
    std::chrono::steady_clock::time_point deadline;
    std::error_code error;
    std::coroutine_handle<> continuation;
    cancellation_token token{};
    cancellation_handle registration{};
    std::size_t heap_index = static_cast<std::size_t>(-1);
    bool await_ready() noexcept;
    template <class P>
    bool await_suspend(std::coroutine_handle<P> handle);
    void await_resume();
};

struct yield_awaiter
{
    io_context& context;
    cancellation_token token{};
    bool await_ready() const noexcept;
    template <class P>
    void await_suspend(std::coroutine_handle<P> handle);
    void await_resume();
};
```

The results of `wait_*`, `sleep_for` and `yield`. Their fields are the state of one pending wait: create them
through those functions, `co_await` each one once, immediately, on the owning context, and never move or modify
one while it is suspended.

- `io_awaiter` and `timer_awaiter`: `await_ready` is true only when the context is already stopped (the await then
  fails with `operation_canceled`). `await_suspend` copies the awaiting task's token, fails at once if it is
  already cancelled, registers an `on_cancel` callback for it, and registers the wait with the reactor.
  `await_resume` throws the token's error, a stored error, or `operation_canceled` if the context stopped.
- `yield_awaiter`: never ready; `await_suspend` puts the coroutine at the back of the ready queue; `await_resume`
  throws if the task's token is cancelled or the context is stopped.

`heap_index` is the timer's position in the reactor's deadline heap while it is suspended.

### `scheduler_access`

```cpp
struct scheduler_access
{
    static void schedule(io_context& io, std::coroutine_handle<> handle);
};
```

Implementation support for task groups: puts a suspended coroutine on the ready queue of `io`. It must run on the
thread running `io`. It is not a way to resume a coroutine from another thread; use `post` for that.

### Platform types

```cpp
// <pace/runtime/platform.hpp>
using native_socket_type = int;            // std::uintptr_t on Windows
inline constexpr native_socket_type invalid_socket = static_cast<native_socket_type>(-1);
```

`native_socket_type` is the descriptor type every readiness wait and PACE socket uses: a POSIX file descriptor, or
a `SOCKET` value on Windows. `invalid_socket` marks "no socket"; waiting on it fails with
`std::errc::bad_file_descriptor`.

## Functions

### Readiness waits

```cpp
io_awaiter wait_readable(native_socket_type fd) noexcept;
io_awaiter wait_writable(native_socket_type fd) noexcept;
io_awaiter wait_ready(native_socket_type fd) noexcept;
```

`co_await io.wait_readable(fd)` suspends until the descriptor is readable (or reports an error or end of stream);
`wait_writable` likewise for writing; `wait_ready` until either direction can make progress, resuming once even if
both become ready together. They transfer no bytes and report no readiness mask: retry the nonblocking operation
after the wait. They do not own the descriptor, which must stay open, with the same identity, until the wait ends.
PACE's TCP and UDP types run this loop for you.

On POSIX the descriptor need not be a socket: pipes, terminals, TUN devices and Linux's `eventfd` family work on
every backend. On Windows readiness waits take Winsock sockets only, and `wait_handle` waits for kernel objects. See
[Handles and signals](handles-and-signals.md) for both, and for awaitable process signals.

### Timers and yielding

```cpp
template <class Rep, class Period>
timer_awaiter sleep_for(std::chrono::duration<Rep, Period> duration) noexcept;
yield_awaiter yield() noexcept;
```

`sleep_for` computes its `steady_clock` deadline when it is **called**, not when it is awaited, so await it
immediately. Zero and negative durations are already due but still suspend for one turn, so a loop of zero sleeps
cannot keep the context from seeing `stop()`. Durations too large for the clock saturate at its maximum. Timers
fire no earlier than their deadline and can fire late because of poll resolution, other ready work or blocking code.

`yield()` moves the coroutine to the back of the ready queue: other ready coroutines, posted callbacks and I/O get a
turn first.

### Callback timers

```cpp
[[nodiscard]] timer_handle call_at(std::chrono::steady_clock::time_point deadline, std::function<void()> callback);
template <class Rep, class Period>
[[nodiscard]] timer_handle call_after(std::chrono::duration<Rep, Period> duration, std::function<void()> callback);
```

Arms a one-shot callback that runs on this context at `deadline` (or after `duration`, computed like `sleep_for`).
It is never invoked inside `call_at`. Callbacks with equal deadlines run in registration order; a due callback is
removed before it runs. Exceptions it throws are handled like a posted callback's. Stopping the context discards the
remaining callbacks without calling them.

Registration is allowed from the context's thread, or before `run()`. It throws `std::invalid_argument` for an empty
callback and `std::system_error(operation_canceled)` once the context is stopped. Callback timers are how scoped
deadlines are built: the callback can cancel or close the operation, and normal completion simply drops the
handle.

### Cancellation callbacks

```cpp
[[nodiscard]] cancellation_handle on_cancel(cancellation_token, std::function<void(std::error_code)>);
```

The hook for custom awaiters. When `token` is cancelled, `callback` runs **on this context** with the token's error
code (`operation_canceled` or `timed_out`), and should finish or unlink the wait it guards exactly once. Requests
may come from any thread; only the callback's execution is moved to the context. When the context stops, every
remaining registration runs with the token's code, or `operation_canceled` if the token has none.

- An empty token (`stop_possible()` is false) returns an empty handle and registers nothing.
- Otherwise it must run on the context's thread (`std::logic_error` if not) and throws
  `std::system_error(operation_canceled)` once the context is stopped, `std::invalid_argument` for an empty callback,
  and the token's error if it is already cancelled.
- The callback fires on a cancellation request. A token whose deadline merely passes does not fire it: that needs
  the timer an [operation boundary](structured-concurrency.md) arms, which requests a timeout when the deadline
  arrives.
- Exceptions from the callback follow the reactor's error policy below.

### Platform helpers

```cpp
// <pace/runtime/platform.hpp>
inline void initialize_sockets();
inline void initialize_winsock();
#ifdef _WIN32
inline SOCKET create_windows_socket(int family, int type, int protocol) noexcept;
#endif
```

`initialize_sockets()` starts Winsock 2.2 once per process on Windows (thread-safe; `WSACleanup` runs at exit) and
throws `std::system_error` if `WSAStartup` fails. Elsewhere it does nothing. PACE calls it before creating any
socket; call it yourself only before using raw Winsock functions. `initialize_winsock()` is another name for it.
`create_windows_socket` creates an overlapped, non-inheritable socket with `WSASocketW`, as every PACE socket on
Windows is; it returns `INVALID_SOCKET` on failure.

## Errors

| Error | When |
|:--|:--|
| `std::logic_error` | Nested, concurrent or repeated `run()`; `local_state`, `find_local_state`, `on_cancel`, readiness waits, timers and `yield` used from a thread that is not running this context; a second pending wait in the same direction on one descriptor (`wait_ready` occupies both) |
| `std::system_error(operation_canceled)` | `spawn`, `post`, `call_at`, `on_cancel`, `local_state`, `find_local_state` after stop; any pending wait when the context stops or `cancel(fd)` is called |
| `std::system_error` with the token's code | A wait whose task token is cancelled (`operation_canceled`, or `timed_out` for a deadline) |
| `std::system_error(bad_file_descriptor)` | Waiting on `invalid_socket` |
| `std::system_error` (native code) | Poller creation or registration failure |
| `std::invalid_argument` | Empty `post` function, timer or cancellation callback; bad `local_state` arguments; unknown `PACE_IO_BACKEND` |

**Reactor error policy.** An exception that escapes a root task, a posted callback, a callback timer, an
`on_cancel` callback or the poller itself is recorded (the first one wins), stops the context, and is rethrown by
`run()` after the drain. A `std::system_error` equal to `operation_canceled` that escapes while the context is
already stopping is the expected outcome of shutdown and is not recorded.

## Threads and concurrency

Exactly one thread runs a context, and everything that touches its sockets, timers and waits runs there. The
thread-safe entry points are `spawn`, `post`, `stop` and `stopped`; `current` is per-thread. Construction before
`run()` and cleanup after it returns may happen on any one thread: `call_at` and `call_after` work before `run()`,
`cancel(fd)`, `release_socket(fd)` and the handles' `cancel()` before or after it.

Each descriptor supports one pending read-direction wait and one pending write-direction wait at a time.

A coroutine or callback runs until it suspends or returns. CPU-bound loops and blocking calls hold the whole
context: no other task, timer or I/O on it makes progress, and shutdown cannot finish until they return. Each turn
of the loop runs a bounded batch of each kind of work (see the limits below) so one busy source cannot starve the
others.

## Lifetime and ownership

- The context must outlive every socket, listener, timer handle use and pending operation bound to it, and must
  not be destroyed while `run()` is executing. Join the thread that runs it, destroy sockets and listeners, then
  destroy the context.
- `spawn` owns its root task until it finishes; roots still suspended when the context is destroyed are destroyed
  with it.
- Readiness waits borrow the descriptor; `local_state` owns its keys and values until destruction.
- `timer_handle` and `cancellation_handle` own their callbacks and may outlive the context.

## Limits and defaults

| Item | Value |
|:--|:--|
| Default backend | `io_backend::automatic` (see [I/O backends](io-backends.md)) |
| Threads per context | 1 |
| Runs per context | 1 |
| Pending waits per descriptor | 1 read-direction + 1 write-direction |
| Work per loop turn | up to 256 each of posted callbacks, ready coroutines, due timers, due callback timers, cancellation callbacks and newly submitted tasks/callbacks |
| Readiness events per poll | 256 |
| Poll timeout resolution | milliseconds, rounded up |

## See also

- [`pace::run`](run.md): a fresh context for one entry task; `this_context`, `sleep_for`, `yield`
- [`pace::runtime`](runtime.md): one context per worker thread
- [I/O backends](io-backends.md): choosing the poller
- [Structured concurrency](structured-concurrency.md): tokens, deadlines and `on_cancel` in context
- [Tutorial: workers and threads](../../tutorial/08-workers-and-threads.md) ·
  [graceful shutdown](../../tutorial/09-graceful-shutdown.md)
- [Execution model](../../explanation/execution-model.md)
