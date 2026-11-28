# Runtime

[简体中文](README.zh.md) · [Reference](../README.md)

## Overview

The runtime module runs PACE's coroutines: lazy tasks, the single-threaded `io_context` reactor with its timers and
readiness waits, multi-threaded runtimes, the synchronous `pace::run` entry, cancellation, deadlines, structured
task groups, and channels, snapshots and `on()` between contexts. Every other module builds on it, and it has no third-party dependency.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/runtime.hpp>` | `pace_runtime` | always built |
| `<pace/pace.hpp>` | `pace` | always built; includes the modules enabled in the build |

With C++ named modules (`--modules=y`), `import pace.runtime;` exports the application-facing names of this module,
and `import pace;` exports every module.

### Pages

| Page | Headers | Contents |
|:--|:--|:--|
| [`task`](task.md) | `runtime/task.hpp` | `task<T>`, its awaiter and promise, `this_task::cancellation` |
| [`io_context`](io-context.md) | `runtime/io_context.hpp`, `runtime/platform.hpp` | the reactor: `spawn`, `post`, `run`, `stop`, timers, readiness waits, `on_cancel`, `native_socket_type` |
| [`runtime`](runtime.md) | `runtime/runtime.hpp` | one context per worker thread |
| [`run`](run.md) | `runtime/run.hpp`, `runtime/this_context.hpp`, `runtime/blocking.hpp` | `pace::run`, `this_context`, `sleep_for`, `yield`, `blocking_options`, `blocking_call` |
| [Structured concurrency](structured-concurrency.md) | `runtime/task_group.hpp`, `runtime/operation.hpp`, `runtime/cancellation.hpp` | `task_group`, `join_handle`, `with_task_group`, `with_options`, `with_deadline`, `with_cancellation`, `shield`, `deadline`, `cancellation_source`, `cancellation_token` |
| [Cross-context communication](cross-context.md) | `runtime/channel.hpp`, `runtime/snapshot.hpp`, `runtime/on.hpp` | `channel`, `overflow_policy`, `send_status`, `snapshot`, `on` |
| [I/O backends](io-backends.md) | `runtime/io_backend.hpp` | `io_backend`, `backend_name`, `PACE_IO_BACKEND` |
| [Handles and signals](handles-and-signals.md) | `runtime/io_context.hpp`, `runtime/file_watch.hpp`, `runtime/signal.hpp` | readiness on pipes, terminals and TUN devices; Windows `wait_handle`; `process_signal`, `signal_set`, `wait_for_signal`, `deliver_signal`, `signal_name` |
| this page | `runtime/ascii.hpp`, `pace.hpp` | `ascii::to_lower`, `ascii::iequals`; the all-modules header |

All headers are under `include/pace/`. `<pace/runtime.hpp>` includes every one of them except `pace.hpp`.

`<pace/pace.hpp>` includes `<pace/runtime.hpp>`, `<pace/net.hpp>` and `<pace/http.hpp>`, and the headers of the
optional modules enabled in this build (TLS, DNS, QUIC, HTTP/1 with the gateway, HTTP/2, HTTP/3, WebSocket). It
declares nothing itself. Prefer the header of the module you use.

## Synopsis

```cpp
#include <pace/runtime.hpp>

#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

pace::task<int> slow_answer()
{
    co_await pace::sleep_for(10ms);
    co_return 42;
}

pace::task<int> entry()
{
    // One budget for everything inside; the caller gets std::system_error(timed_out) if it runs out.
    co_return co_await pace::with_deadline(pace::deadline::after(1s), slow_answer);
}

int main()
{
    std::cout << pace::run(entry) << '\n'; // a fresh io_context on this thread
}
```

## Functions

### `ascii::to_lower`, `ascii::iequals`

```cpp
namespace pace::ascii
{
[[nodiscard]] constexpr char to_lower(char c) noexcept;
[[nodiscard]] constexpr bool iequals(std::string_view a, std::string_view b) noexcept;
[[nodiscard]] inline std::string to_lower(std::string text);
}
```

Locale-independent ASCII case folding for protocol tokens such as HTTP field names and DNS names. Only `A`–`Z` are
changed, so UTF-8 and other bytes pass through untouched. `to_lower(char)` folds one character,
`to_lower(std::string)` returns a folded copy, and `iequals` compares two strings ignoring ASCII case.

## Errors

These rules hold for every API built on the runtime.

**Awaited tasks rethrow.** An exception that escapes a task is rethrown where the task is awaited. A root task
(given to `io_context::spawn`) or a posted callback whose exception nobody catches stops its context; `run()`
rethrows the first such exception after the context has drained, and a [`runtime`](runtime.md) stops all its
contexts and rethrows it after joining them. Besides the errors each page lists, allocation failures can propagate
from almost any call.

**Cancellation is cooperative.** `io_context::stop()` is a permanent request, not a join or a forced interruption:
pending readiness waits and timers resume with `std::system_error` whose code equals
`std::errc::operation_canceled`, and tasks are expected to finish. Catch that code where cancellation is expected.
While a context is stopping, an `operation_canceled` that escapes a root is the expected result and is not reported
by `run()`; other exceptions still are. [Tokens and deadlines](structured-concurrency.md) cancel part of the work
the same way, with `operation_canceled` or `std::errc::timed_out`, without stopping the context.

## Threads and concurrency

**One thread per context.** Each `io_context` is run by one thread, and its sockets, timers and waits are used only
from that thread. `spawn`, `post` and `stop` are the cross-thread entry points; [channels, snapshots and
`on()`](cross-context.md) build on them. Creating objects before `run()` and
destroying them after it returns is fine; touching them from an unrelated thread while it runs is not, read-only
accessors included. Each protocol's page states how many operations may be outstanding on one object.

**Blocking work starves the reactor.** A coroutine runs until it suspends or returns. A blocking call or a long CPU
loop stops every other task, timer and I/O on that context, and keeps shutdown from finishing. An idle context keeps
running until it is stopped, and a context cannot be restarted.

## Lifetime and ownership

- **Tasks are lazy and consumed once.** Creating a task runs nothing; awaiting or spawning it consumes it.
- **Value parameters are owned by the frame; references are not.** Arguments passed by value become part of the
  coroutine frame. References, pointers, spans and string views do not keep their objects alive. Prefer named
  coroutine functions with value parameters, and pass capturing lambdas to the APIs that own them (`pace::run`,
  `with_options`, `task_group::spawn`) instead of calling them.
- **The context outlives what is bound to it.** Sockets, listeners and pending operations must not outlive their
  context, and a socket must stay at the same address until its operations finish.

## See also

- [Tutorial: tasks and timers](../../tutorial/04-tasks-and-timers.md),
  [structured concurrency](../../tutorial/07-structured-concurrency.md),
  [workers and threads](../../tutorial/08-workers-and-threads.md),
  [graceful shutdown](../../tutorial/09-graceful-shutdown.md)
- [Execution model](../../explanation/execution-model.md) · [Ownership](../../explanation/ownership.md)
- [Networking](../net/README.md): TCP and UDP on top of the runtime
- [Build options](../build-options.md)
