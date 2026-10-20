# Public API reference

[中文索引与关键约定](README.zh-CN.md)

This reference describes the public headers in `include/pace/`. Signatures use
C++20. A type in `pace::detail` is an implementation detail even if visible through
a public header; it has no compatibility guarantee.

| Header | Public symbols |
|---|---|
| [`task.hpp`](task.md) | `task<T = void>`, `task<void>`, their `promise_type`, `handle_type`, constructors, move assignment, destructor, `release`, `operator co_await`, nested `awaiter` and its protocol members |
| [`io_context.hpp`](io-context.md) | `io_context`, construction/destruction, `spawn`, `post`, `run`, `stop`, `stopped`, `current`, `cancel`, `wait_readable`, `wait_writable`, `sleep_for`, `yield`, `io_awaiter`, `timer_awaiter`, `yield_awaiter` |
| [`runtime.hpp`](runtime.md) | `runtime`, construction/destruction, `size`, `context`, `run`, `run_on_current_thread`, `stop` |
| [`tcp.hpp`](tcp.md) | `tcp::endpoint`, `tcp::listen_options`, `tcp::stream`, `tcp::listener` and all their members |
| [`http.hpp`](http.md) | `http::header_field`, `http::request`, `http::response`, `http::limits`, `http::handler`, `http::serve_connection`, `http::serve` |
| [`platform.hpp`](platform.md) | `native_socket_type`, `invalid_socket` |
| [`net.hpp`](platform.md) | Compatibility aliases `tcp_stream`, `tcp_listener`, `ipv4_endpoint`, `listen_options` |
| [`pace.hpp`](platform.md) | Umbrella header; no additional types or functions |

## Rules shared by the APIs

**Ownership.** A task is lazy and consumed once. Parameters passed by value become
part of its coroutine frame, but references, pointers, spans and string views do
not acquire ownership. The context must outlive its sockets, listeners and pending
operations. A socket object must remain at a stable address until its member
operations finish. Prefer named coroutine functions with owned value parameters.

**Execution.** One thread runs each context. `spawn`, `post` and `stop` are the
cross-thread entry points. Socket I/O and timer/yield waits execute on their owning
context. Construction before `run()` and cleanup after it returns are supported;
concurrent object access from an unrelated thread is not. Read-only accessors do
not make a concurrently modified socket safe to inspect.

**Errors.** Awaited tasks rethrow their exceptions. A root task or posted callback
with an unhandled exception stops its context; `run()` rethrows the first recorded
exception after draining. Runtime worker errors stop peer contexts and propagate
after joining. Allocation failures can propagate in addition to documented
argument, state and native I/O errors.

**Cancellation.** `stop()` is a permanent cooperative shutdown request, not a task
join or a forced interruption. Registered readiness waits and timers resume with
`std::system_error` whose code compares equal to
`std::errc::operation_canceled`. Catch that code where cancellation is expected.
An already-stopping context suppresses uncaught root cancellation while draining;
other errors remain failures. No per-task cancellation token exists.

**Progress.** A coroutine runs until it suspends or finishes. Blocking work and
unbounded CPU loops prevent that reactor from serving other work or finishing
shutdown. `run()` stays alive while idle until explicitly stopped. Contexts are
single-use and cannot be restarted.

The [tutorials](../README.md) show these rules together. The signatures below
describe the current implementation; they do not promise APIs from Asio or libuv.
