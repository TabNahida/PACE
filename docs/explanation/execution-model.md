# Execution model

[简体中文](execution-model.zh.md) · [Explanation](README.md)

Where PACE code runs, who owns the threads, and how work moves between them. The mechanics of the run loop are in
[Architecture](architecture.md#one-reactor-per-worker); this page is about the choices an application makes.

## One context, one thread at a time

The unit of execution is an `io_context`: one reactor with its own poller, ready queue, timers and socket
registrations. Exactly one thread runs a context, and every coroutine assigned to it resumes on that thread.
Nothing migrates: a task spawned on context 2 runs on context 2 until it finishes, and a socket opened on a context
is used on that context.

This is a deliberate trade. Work stealing and migration balance uneven load automatically, but they make every
socket, timer and coroutine frame potentially shared between threads, which costs synchronization on every
operation and makes ownership hard to see. PACE keeps each reactor's state single-threaded, so the hot paths take
no locks ([yield costs about 5 ns](../performance/runtime.md#scheduler-components)), and leaves placement to the
application, which knows its load.

## Four ways to run

| | Who owns the threads | Returns when | Use it for |
|:--|:--|:--|:--|
| `pace::run(entry)` | PACE, on the calling thread | The entry task finishes; its value is returned | Programs and tests with one main coroutine |
| `io_context::run()` | You, on the calling thread | `stop()` is called and the context has drained | One reactor under explicit control |
| `runtime::run()` | PACE: one worker thread per context | `stop()` is called and every worker has joined | Servers that want N reactors |
| `runtime::run_on_current_thread()` | PACE for N−1 workers; the caller runs context 0 | The same | The same, without an idle main thread |

Applications with their own threading model can also create several `io_context` objects and call `run()` on
threads they already own; a `runtime` is only a convenience for that pattern.

`pace::run` is the shortest form: it creates a context, runs the entry, stops the context when the entry finishes,
drains the cancellation of any background tasks, and returns the entry's result. `io_context::run()` and the
runtime keep running when idle: a server is idle between requests, and that must not end it. They stop only when
asked.

The direct clients (`pace::http::get`, `tcp::exchange`, `udp::exchange`) are a fifth, hidden form: each call runs a
private context on the calling thread for the length of the call, with a deadline. They are for code that is not
asynchronous at all.

## Placing work

`spawn` (a task), `post` (a function) and `stop` are the only operations that may be called from any thread. Everything
else on a context — socket I/O, timers, `yield`, closing a socket, cancelling a registration — belongs to the thread
running it. Setting objects up before the context runs and destroying them after it has stopped are allowed from
the owning thread.

To place work, choose the context: `runtime.context(i).spawn(task)`. A typical server gives every worker its own
listener so that connections are spread across reactors. To act on another context's socket, post a function to that context instead of touching the socket.

Cross-thread submissions go through one mutex-protected inbox per context; only the first submission after the
inbox drains wakes the reactor, so a burst of posts costs one wake-up.

`spawn` and `post` are fire-and-forget. When work on one context needs something from another, the
[cross-context tools](../reference/runtime/cross-context.md) build the usual patterns on that inbox, and each
resumes a waiting coroutine on the context it was running on:

- **State owned by one context.** Give a shared table, a quota counter or a device to one context, chosen with
  `runtime.shard_for(key)`, and let the others send it requests through a `channel<T>`. The owner applies them in
  order without locks; senders on other contexts wait when the channel is full instead of growing a queue without
  bound, and plain threads can `try_send`.
- **Read-mostly data on every context.** A `snapshot<T>` publishes an immutable value, such as routing rules, to
  every context. Each reads its own cached copy without locks; a publish replaces the copies between turns.
- **A result from another context.** `co_await pace::on(context, work)` runs the work there and resumes here with
  its value or exception. The caller's cancellation and deadline reach the work, and the caller waits for it like a
  child task, which replaces posting a function and then posting the result back.

## Cooperative scheduling

A coroutine runs until it suspends (on I/O, a timer, `yield`, or awaiting another task) or finishes. Nothing
preempts it. The run loop takes ready work in batches of at most 256 so that a stream of ready coroutines still
lets timers, I/O and shutdown through, but a single coroutine that never suspends holds its whole reactor.

So blocking calls do not belong on a reactor: a synchronous file read, a lock held across I/O, a long computation,
or a direct client call (`pace::http::get`) inside a coroutine all stall every other task on that context. Use the
`async_*` forms inside coroutines. PACE does not yet provide a pool for blocking work; run it on a thread you own
and hand the result back to the context that needs it with `post`, or with `channel::try_send` or `blocking_send`.

## Stopping

`stop()` is a permanent, cooperative request, safe from any thread. The reactor cancels every registered wait and
timer, which resume with `std::system_error` equal to `std::errc::operation_canceled`, and then keeps running until
the resumed coroutines have unwound. New submissions after `stop()` are rejected. A context runs once: it cannot be
restarted after `run()` returns.

Cooperative means that code which never reaches a suspension point, or is blocked in a system call PACE does not
own, delays shutdown. Cancellation that should not stop the whole context, such as a deadline on one request,
uses [cancellation tokens and structured scopes](../reference/runtime/structured-concurrency.md) instead.

## Errors across threads

An exception that escapes a root task or a posted function stops its context, and `run()` rethrows the first such
exception after draining. In a runtime, the first failing worker stops all the others; `run()` joins every worker
and then rethrows. Exceptions in awaited tasks propagate to the awaiting coroutine like ordinary exceptions.

## Choosing

- A command-line tool or a test: `pace::run`, or the direct clients if nothing is concurrent.
- A server: a `runtime` with one context per core you want to give it, a listener per context, and
  `run_on_current_thread()` so that `main` is one of the workers.
- A library inside an application with its own threads: one `io_context` per thread you dedicate to I/O, driven by
  `run()` on those threads, and `post` to hand results back.

## See also

- [Ownership and lifetimes](ownership.md)
- [`io_context`](../reference/runtime/io-context.md), [`runtime`](../reference/runtime/runtime.md),
  [`pace::run`](../reference/runtime/run.md)
- Tutorial: [Workers and threads](../tutorial/08-workers-and-threads.md),
  [Graceful shutdown](../tutorial/09-graceful-shutdown.md)
