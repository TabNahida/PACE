# `pace::io_context`

Include `<pace/io_context.hpp>`. A context owns one native readiness reactor,
submission queues, ready coroutine queues and steady-clock timers. Exactly one
thread may execute it. It is neither copyable nor movable.

```cpp
io_context();
~io_context();
io_context(const io_context&) = delete;
io_context& operator=(const io_context&) = delete;
```

Construction creates native poller resources and may throw `std::system_error`.
Destruction requests stop and releases remaining owned tasks and resources; it
does **not** join an application-owned run thread. Join that thread first, then
destroy associated sockets/listeners before destroying the context. Destruction
of unstarted tasks does not run their bodies.

## Submission and execution

```cpp
void spawn(task<void> work);
void post(std::function<void()> work);
void run();
void stop() noexcept;
bool stopped() const noexcept;
static io_context* current() noexcept;
```

| Member | Behavior |
|---|---|
| `spawn` | Takes ownership of one root coroutine. Schedules it without running its body inline. An empty task produces no work. Thread-safe before or during `run`; rejects submissions after stop with `operation_canceled`. |
| `post` | Owns a copyable callable through `std::function<void()>`; invokes it on this context. Never promises inline invocation. An empty function throws `invalid_argument`; submission after stop throws `operation_canceled`. Thread-safe. |
| `run` | Executes on the calling thread until stop and draining finish. An idle context keeps waiting. Throws `logic_error` for concurrent, repeated or nested runs on the same thread, including nesting a different PACE context. |
| `stop` | Idempotent, thread-safe, permanent request. Wakes a blocked poller. It does not wait or close application-owned sockets. |
| `stopped` | Thread-safe observation of the stop-request flag. True does not mean the run thread has finished. |
| `current` | Returns the context running on this calling thread, or `nullptr` outside `run`. It is thread-local information, not a lookup of another worker. |

Queued work accepted before stop is drained. Such work must observe cancellation
and finish; it cannot submit new work after stop. Unhandled root/callback errors
record the first exception, request stop, drain and then rethrow from `run()`.
An `operation_canceled` escaping a root after shutdown began is suppressed.
There is no restart, polling-one-handler or work-guard API.

## Readiness and explicit cancellation

```cpp
io_awaiter wait_readable(native_socket_type fd) noexcept;
io_awaiter wait_writable(native_socket_type fd) noexcept;
void cancel(native_socket_type fd) noexcept;
```

Readiness waits suspend on this context; they do not read or write bytes and do
not own the descriptor. Keep its native resource alive and preserve its identity
until cancellation or completion. Retry the nonblocking operation after wakeup:
readiness can also represent EOF or an error. PACE's TCP methods handle that loop.

Only one pending waiter per descriptor **per direction** is supported. A second
read or second write registration throws `std::logic_error`; one of each is
allowed. Await from the owning context. Native registration failures propagate as
`std::system_error`. A stopped context makes the wait fail with cancellation.

`cancel(fd)` unregisters that descriptor's current waits and schedules their
continuations with `operation_canceled`. Unknown descriptors do nothing. It does
not close the descriptor, stop the context or prevent future waits. Call it only
on the owning loop or while no run thread can access the context, such as before
`run()` or after joining. It is not a cross-thread entry point; use `post` to
request it from another thread. Do not bypass this operation by closing a native
handle that still has registered waits.

## Timers and cooperative yielding

```cpp
template<class Rep, class Period>
timer_awaiter sleep_for(std::chrono::duration<Rep, Period> duration) noexcept;
yield_awaiter yield() noexcept;
```

The timer deadline is computed with `steady_clock` **when `sleep_for` is called**,
not when the returned awaiter is eventually awaited. Prefer an immediate
`co_await io.sleep_for(duration)`. Zero and negative durations are immediately due
but still suspend and yield; they cannot form an inline loop that prevents stop
from being observed. Large positive durations saturate at the clock's maximum
time point. Use finite durations. Timers may fire late because of OS resolution,
other ready work or blocking application code; they provide no real-time guarantee.

`yield()` always schedules the current coroutine for a later ready-queue turn.
Both waits belong to the owning context, and both throw cancellation when the
context stops. These awaiters have no individual cancellation handle. They are
cooperative scheduling tools, not CPU preemption or a task join.

### Cancellable callback timers

```cpp
[[nodiscard]] timer_handle call_at(std::chrono::steady_clock::time_point deadline,
                                   std::function<void()> callback);
template<class Rep, class Period>
[[nodiscard]] timer_handle call_after(std::chrono::duration<Rep, Period> delay,
                                      std::function<void()> callback);
// timer_handle is default-constructible and move-only.
void timer_handle::cancel() noexcept;
```

Keep the returned handle alive to arm a one-shot callback. `cancel()`, handle
destruction and move assignment release the old callback and its captures
immediately. The handle may outlive its context. A due callback is removed before
invocation, runs on the owning reactor, and is never invoked inline by `call_at`.
Callbacks with equal deadlines run in registration order. Exceptions follow the
same stop/drain/rethrow policy as `post`. Stopping the context discards remaining
callbacks without invoking them. Dispatch is bounded so recursively scheduling
already-due callbacks cannot starve other reactor work.

Registration and cancellation are context-affine, or allowed while no thread is
running the context. Use `post` from another thread. Empty callbacks are rejected;
registration after stop throws `operation_canceled`. These timers are useful for
scoped I/O deadlines: the callback can close the operation's stream, and normal
completion destroys its timer handle without retaining a sleeping coroutine.
They cannot interrupt a callback or coroutine that blocks the reactor thread.

## Awaiter types

The returned aggregates expose the coroutine protocol below. Treat their fields
as implementation state: create them through the context functions, await each
once, and never move, mutate or destroy them while registered.

```cpp
struct io_awaiter {
    io_context& context;
    native_socket_type fd;
    bool writable;
    std::error_code error;
    std::coroutine_handle<> continuation;
    bool await_ready() noexcept;
    bool await_suspend(std::coroutine_handle<> handle);
    void await_resume();
};
struct timer_awaiter {
    io_context& context;
    std::chrono::steady_clock::time_point deadline;
    std::error_code error;
    std::coroutine_handle<> continuation;
    bool await_ready() noexcept;
    bool await_suspend(std::coroutine_handle<> handle);
    void await_resume();
};
struct yield_awaiter {
    io_context& context;
    bool await_ready() const noexcept;
    void await_suspend(std::coroutine_handle<> handle);
    void await_resume();
};
```

For readiness/timers, `await_ready` returns true only to report an already-stopped
context; otherwise `await_suspend` registers the continuation and may throw for
affinity, registration or allocation failures. It returns false if stop is
observed during registration. `await_resume` throws a stored error or cancellation.
Yield's `await_ready` is always false; its suspend operation enqueues the
continuation and its resume operation checks stop. These protocol functions are
not a supported way to resume an unrelated coroutine from another thread.

See [execution choices](../tutorial/05-execution.md) and
[shutdown](../tutorial/06-shutdown.md) for complete examples.
