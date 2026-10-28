# Structured concurrency, cancellation and deadlines

[中文](structured-concurrency.zh-CN.md) · [Guided tutorial](../tutorial/12-structured-concurrency.md)

Include `<pace/task_group.hpp>` for groups, `<pace/operation.hpp>` for operation
boundaries, or `<pace/cancellation.hpp>` for sources/tokens/deadlines. All link to
`pace_runtime` and need no third-party dependency. These APIs are unreleased.

## Scope and ownership

`with_task_group(factory, operation_options={})` returns a lazy `task<T>`; the owned
factory takes `task_group&` and returns `task<T>`. The group runs on the current
context. Its constructor is private; it cannot accidentally be stack-created and
destroyed with active children. `task_group::run(factory)` is the same boundary
without extra options. Groups cannot be copied or moved.

| API | Contract |
|---|---|
| `group.spawn(factory)` | Owns a nullary or `io_context&` factory, starts a child on this loop, returns `join_handle<T>` |
| `group.spawn(task<T>)` | Owns the task, but references already captured by that task remain borrowed |
| `co_await handle` | Consumes its result once, rethrows its failure; a handle is move-only and context-affine |
| `group.own(value)` | Moves a value into group storage and returns `T&`; storage is destroyed after joining children, before the boundary returns/throws |
| `co_await group.join()` | Seals the group and waits for children; observes local cancellation, rejects overlapping joins; no further spawn/own; the boundary always drains before reporting failure |
| `group.cancel()` | Requests cooperative cancellation of the body and descendants; still requires join |
| `group.token()` | Copyable token observing the group's local cancellation |

Normal body completion waits for children. The first observed escaping body/child
exception cancels siblings, and is rethrown only after all children finish. Later
failures do not replace it. Cancellation observed first can therefore win over a
later application error; errors are not aggregated. A dropped handle does not
detach a child or suppress its error. A failure escaping a nested group fails its
parent child; catch a recoverable operation error inside the child to contain it.
A group is cooperative concurrency on one reactor, not a CPU thread pool.

Borrowing data from the caller *outside* the entire boundary is safe while that
caller awaits the boundary. Body-local variables are destroyed when the body
returns/throws, potentially before children: use value captures or `group.own`.
Factories remain owned until child completion and captures are destroyed before
join finishes. Return owned values; a view/reference into scope storage cannot
outlive it. Never immediately invoke a temporary capturing coroutine lambda.

Existing `io.spawn` remains an explicit unstructured root operation: it does not
inherit a caller's cancellation token or join ownership. Use `group.spawn` for
structured application work. `pace::run` remains the outer execution entry.

## Local cancellation

`cancellation_source` is copyable; `token()` produces a cheap copyable observer.
`request_cancellation()` is thread-safe and returns whether it won the first
request; requests are idempotent. `request_timeout()` records `errc::timed_out`.
`token.error()`/`stop_requested()` also observe an expired token deadline;
`throw_if_cancelled()` throws `system_error`. `source.link(parent)` propagates
cancellation one way; configure links before concurrent use. Deadline composition
belongs to operation boundaries. Use `io.on_cancel` to integrate custom asynchronous waits.

`with_cancellation(token, work)` and `with_options(work, options)` own a task or
factory until completion. Ordinary `co_await task` inherits the current task's
token. New boundaries link parent and explicit tokens; cancellation never flows
back up merely because an inner deadline expires. `co_await this_task::cancellation()`
returns the current token without throwing, useful for inspection during cleanup.

Cancellation wakes only the registered wait. A read cancellation does not cancel
an independent write on the same descriptor. TCP/UDP sockets remain open at the
primitive layer, but partial writes/reads may have occurred. Protocol operations
may close an owned connection when its state can no longer be reused: HTTP/1
`client.send` closes on failure, and HTTP/2/3 batches own/close their connection.
QUIC cancellation unlinks only that event waiter; other streams continue. DNS
cancels its lookup and can be reused afterward. Cancellation is not rollback.

Callbacks from another thread only mark pending cancellation and wake the poller.
The reactor unregisters and resumes operations on their owning thread. Timer
cancellation removes its pending wait rather than leaving a dangling heap pointer.
Ready operations check cancellation at resumption; cancellation observed at that
point wins over an otherwise completed result. Never destroy an active coroutine
to cancel it; request cancellation and join it.

## One deadline across layers

`deadline{steady_clock::time_point}` is absolute; default construction means no
deadline. `deadline::after(duration)` calculates one now, saturating on overflow;
zero/negative durations are already expired. `earlier(a,b)` selects the tighter
budget. `operation_options` contains `cancellation_token cancellation` and
`pace::deadline deadline`.

`with_deadline(deadline, work)` is an operation boundary. Its budget covers nested
resolution, connection, handshake, transfer and cleanup. Parent, explicit token
and new deadlines compose by minimum, so a nested timeout cannot reset the total.
The boundary arms a scoped callback timer; local expiry reports `timed_out`.
No timeout is added by an unwrapped async API. Protocol-specific phase timers and
limits remain available and can fail sooner; they do not extend a total deadline.

Direct clients accept `blocking_options` with the same `cancellation` and `deadline`
fields. Its existing positive `timeout` (default 30 seconds) is shorthand for a
relative total bound; the earlier absolute bound wins. All HTTP versions, TCP and
UDP use the same boundary implementation. Blocking calls still reject a current
PACE context.

Cancellation cannot preempt synchronous computation, OS scheduling or blocking
foreign code. A child that ignores all cancellation points can prevent join from
finishing. Custom awaiters must participate through `io.on_cancel(token, callback)`:
the callback runs on the owner loop, and the returned move-only handle unregisters
it. Handles are context-affine (or destroyed after the loop joins), may outlive
the context, and callbacks must promptly finish/unlink the wait exactly once.
Register only for a token from an active operation boundary to get its deadline
timer. Context stop also dispatches registered cancellation; registration with a live
token after stop is rejected. An empty callback with a live token is invalid. Callback exceptions
follow the reactor's error/stop/drain policy.

## Cleanup shielding

`shield(work)` masks inherited local cancellation for that task subtree. Use
`shield(with_deadline(deadline::after(100ms), cleanup_factory))` for bounded async
cleanup. The outer boundary still reports its original cancellation afterward.
Shield does not override `io.stop()`, and does not permit detached cleanup. The boundary
shields its final drain so cancellation of an explicit join cannot skip children.
Do not join a group from its own child; that would wait for itself. If the body
exits with another join pending, the boundary rejects that misuse and cancels/joins
the children before throwing.
Prefer synchronous RAII cleanup when sufficient.

## Protocol concurrency and server lifetime

All sockets/protocol objects are context-affine, must stay unmoved during pending
operations, and must outlive those operations. Source cancellation is thread-safe;
this does not make socket methods thread-safe.

| Component | Permitted overlap / ownership |
|---|---|
| TCP / UDP socket | One read/receive and one write/send; no competing same-direction operations; caller owns borrowed buffers |
| TLS stream | One async public stream operation total; HTTP/2's internal duplex driver owns its exclusive TLS access |
| QUIC connection / stream | One protocol driver per connection; independent stream waits can overlap; same stream/direction operations must not compete |
| HTTP/1 persistent client | One `send` at a time; independent clients may overlap |
| HTTP/1/2/3 URL client | Each invocation owns a fresh connection; independent calls may overlap |
| HTTP/2/3 batch | One call owns its transport/session; multiple streams are multiplexed within that batch, results preserve input order |
| HTTP handler | HTTP/1 serializes requests per connection; HTTP/2/3 bound concurrent handlers by their configured cap |

HTTP `serve` invocations own accepted tasks through a group. Closing the listener,
parent cancellation or context shutdown cancels and joins accepted work before
`serve` returns. HTTP/2/3 connection drivers also cancel and join active handlers
before returning or propagating a transport failure. This intentionally replaces
the old detached-late-handler behavior. A TCP half-close still allows complete
requests to finish while the protocol driver remains active. A reset HTTP stream
may discard its result; the handler is guaranteed joined by connection exit, not
necessarily canceled individually at reset. User handlers must cooperate with
cancellation; no library can interrupt a blocking callback.
