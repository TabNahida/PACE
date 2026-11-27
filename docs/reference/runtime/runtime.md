# `pace::runtime`

[简体中文](runtime.zh.md) · [Runtime](README.md)

## Overview

A `runtime` owns a fixed set of independent `io_context` objects and runs each on its own thread.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/runtime/runtime.hpp>` (also `<pace/runtime.hpp>`) | `pace_runtime` | always built |

The contexts are shards, not a pool: a task stays on the context the application gave it to. There is no work
stealing, no automatic distribution of tasks, no CPU pinning and no separate pool for blocking work.

## Synopsis

```cpp
#include <pace/runtime.hpp>

#include <atomic>
#include <chrono>

using namespace std::chrono_literals;

pace::task<void> work(pace::runtime& workers, std::atomic<int>& done)
{
    co_await pace::sleep_for(5ms); // runs on the context it was spawned on
    if (++done == static_cast<int>(workers.size()))
    {
        workers.stop();
    }
}

int main()
{
    pace::runtime workers(4);
    std::atomic<int> done{0};
    for (std::size_t i = 0; i < workers.size(); ++i)
    {
        workers.context(i).spawn(work(workers, done));
    }
    workers.run_on_current_thread(); // context 0 here, contexts 1-3 on new threads
}
```

## Types

### `runtime`

```cpp
class runtime
{
public:
    explicit runtime(
        std::size_t workers = std::max(1u, std::thread::hardware_concurrency()),
        io_backend backend = io_backend::automatic
    );
    ~runtime();
    runtime(const runtime&) = delete;
    runtime& operator=(const runtime&) = delete;

    std::size_t size() const noexcept;
    io_context& context(std::size_t index);
    template <class Key>
    std::size_t shard_index(const Key& key) const;
    template <class Key>
    io_context& shard_for(const Key& key);
    void run();
    void run_on_current_thread();
    void stop() noexcept;
};
```

| Member | Behaviour |
|:--|:--|
| `runtime(workers, backend)` | Creates `workers` contexts, each with `backend`, but no threads. The default is the reported hardware concurrency, or 1 when that is unknown. Throws `std::invalid_argument` for 0, and whatever `io_context`'s constructor throws. |
| `~runtime()` | Destroys the contexts. `run()` or `run_on_current_thread()` must have returned. Work still queued on contexts that never ran is destroyed without running. |
| `size()` | The number of contexts, fixed at construction. |
| `context(index)` | The context at `index`; throws `std::out_of_range` for `index >= size()`. Addresses are stable for the runtime's lifetime. Submit work with `context(i).spawn(…)` or `context(i).post(…)`. |
| `shard_index(key)` | The index of the context that owns `key`: `std::hash<Key>` of the key, mixed and reduced modulo `size()`. The same key always gives the same index for the runtime's lifetime; across processes or builds it is only as stable as `std::hash`. Thread-safe. |
| `shard_for(key)` | `context(shard_index(key))`: the fixed owner of the state behind `key`, for example the context that owns a [`channel`](cross-context.md#channel) for that key. Thread-safe. |
| `run()` | Starts one thread per context and blocks until all of them have stopped and been joined. |
| `run_on_current_thread()` | Runs context 0 on the calling thread and the others on `size() - 1` new threads; blocks the same way. With one context no thread is created. |
| `stop()` | Requests stop on every context. Thread-safe, idempotent, does not wait. |

The two run functions differ only in where context 0 runs:

| Call, with `N = size()` | Calling thread | Threads the runtime creates |
|:--|:--|:--|
| `run()` | waits, then joins the workers | N |
| `run_on_current_thread()` | runs context 0, then joins the others | N − 1 |

## Errors

| Error | When |
|:--|:--|
| `std::invalid_argument` | Constructed with 0 workers |
| `std::out_of_range` | `context(index)` with `index >= size()` |
| `std::logic_error` | A second call to `run()` or `run_on_current_thread()` (a runtime runs once, in either mode); `run_on_current_thread()` from a thread that is already running a context. That nested call is rejected before anything starts and does not use up the runtime's single run. |
| First context error | An exception escaping any context's `run()` (see [`io_context`](io-context.md#errors)) stops every context; after all threads are joined the first one is rethrown. |
| `std::system_error` | Thread creation failed: contexts already started are stopped and joined, then the error propagates. |

Each context is also single-use: running a context yourself and then running the runtime makes that context's
`run()` fail, which stops the others.

## Threads and concurrency

`stop()`, `shard_index`, `shard_for`, and `spawn`/`post`/`stop` on any `context(i)`, may be called from any thread, including from a task or
callback on one of the runtime's own contexts. Everything else about a context, its sockets and its tasks follows
the [`io_context` rules](io-context.md#threads-and-concurrency): one thread per context, and objects shared between
contexts need the application's own synchronization.

The threads are ordinary `std::thread`s, not pinned. A task that blocks holds only its own context. One context
per logical CPU is a starting point, not a rule: leave CPU for clients, blocking work and other services, and
measure with your connection counts. With few connections, spreading them over more contexts can leave each poll
with a single ready descriptor; compare throughput and tail latency at several worker counts.

## Lifetime and ownership

- The runtime owns its contexts; `context(i)` returns a reference that is valid until the runtime is destroyed.
- `run()` and `run_on_current_thread()` return only after every thread has been joined. Destroy sockets and
  listeners bound to the contexts after that and before the runtime.
- The destructor does not coordinate with a thread that is still inside `run()`: do not destroy a running runtime.
- A stopped runtime cannot be restarted.

## Limits and defaults

| Item | Value |
|:--|:--|
| Default `workers` | `std::thread::hardware_concurrency()`, at least 1 |
| Default `backend` | `io_backend::automatic` |
| Minimum `workers` | 1 |
| Runs per runtime | 1, in either mode |

## See also

- [`io_context`](io-context.md): what each context does
- [Cross-context communication](cross-context.md): channels, snapshots and `on()` between these contexts
- [I/O backends](io-backends.md): the `backend` argument
- [Tutorial: workers and threads](../../tutorial/08-workers-and-threads.md)
- [Runtime performance](../../performance/runtime.md)
- [Execution model](../../explanation/execution-model.md)
