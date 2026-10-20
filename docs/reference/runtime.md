# `pace::runtime`

Include `<pace/runtime.hpp>`. A runtime owns a fixed collection of independent
`io_context` objects. Tasks remain on the context selected by the application;
the runtime has no work stealing, automatic round-robin submission or CPU pinning.

```cpp
explicit runtime(std::size_t workers =
    std::max(1u, std::thread::hardware_concurrency()));
~runtime();
runtime(const runtime&) = delete;
runtime& operator=(const runtime&) = delete;

std::size_t size() const noexcept;
io_context& context(std::size_t index);
void run();
void run_on_current_thread();
void stop() noexcept;
```

Construction creates the requested number of contexts, not their run threads.
The default is reported hardware concurrency, with one as the fallback when the
report is zero. Pass an explicit positive count for controlled deployments. Zero
throws `std::invalid_argument`; poller construction and allocation errors can
also propagate. The runtime is not copyable or movable.

`size()` reports the fixed context count. `context(index)` returns a borrowed
reference and uses bounds checking: `index >= size()` throws `std::out_of_range`.
Contexts keep stable addresses during the runtime lifetime. Submit with
`runtime.context(index).spawn(std::move(work))` or `.post(callback)`. The application chooses
the index and is responsible for any shared state across contexts.

## Execution modes

| Call with `N = size()` | Calling thread | Threads created by the runtime |
|---|---|---|
| `run()` | Waits for workers and joins them | N, one per context |
| `run_on_current_thread()` | Executes context 0, then joins peers | N−1, for contexts 1 through N−1 |

Both methods are blocking. They do not start a detached service and return
immediately. Both retain idle contexts until explicit stop. With
`runtime(1).run_on_current_thread()`, no runtime worker threads are created.
The total number of reactor contexts is unchanged by caller mode.

A runtime is single-use across both methods. Concurrent or repeated entry throws
`std::logic_error` without stopping a currently active run. Caller mode additionally
rejects entry from a thread already running a PACE context; that rejected nested
attempt does not consume the runtime, so it can be called later from outside.
Do not independently call `run()` on a context owned by a runtime and later ask
the runtime to run it: individual contexts are also single-use.

An unhandled task/callback error in a context requests stop on all peers. Both
methods let cooperative cancellation drain, join all created worker threads, then
rethrow the first observed context error. If worker thread creation fails, already
started contexts are stopped and joined before the launch error propagates.
Queued work on contexts that never started is released during destruction; its
body is not executed just to clean it up.

`stop()` is thread-safe and requests stop on every context. It is idempotent and
does not wait for threads. It may be called by a coroutine, a posted callback or
an external control thread. A stopped runtime cannot be reset/restarted.

## Lifetimes and scheduling

The caller must wait for `run` or `run_on_current_thread` to return before
destroying the runtime. Its destructor does not coordinate with a concurrently
executing caller. Destroy sockets/listeners associated with its contexts before
destroying the runtime. A successful run return means worker threads were joined;
it does not destroy application-owned sockets for you.

Threads are not pinned and OS scheduling remains in control. A running coroutine
or callback blocks its own context until it suspends or returns. A worker count
equal to the logical CPU count is a default, not a tuning recommendation for all
workloads; reserve CPU for clients, blocking workers and other services as needed.

For complete control, use standalone contexts and application-owned threads
instead. The runtime does not accept externally supplied executors or manage a
separate blocking-work pool. See [execution choices](../tutorial/05-execution.md).
