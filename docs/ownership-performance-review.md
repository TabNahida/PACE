# Ownership and hot-path review (16–17 November 2026)

This review covers runtime scheduling and worker ownership, HTTP dispatch and
routing, TLS configuration, DNS lookups, and QUIC pumps. Changes preserve the
existing context-affinity and shutdown contracts. Benchmark timings describe
local isolated components, not end-to-end network throughput.

## Changes

- `http::make_handler` stores function pointers and trivial empty callables by
  value. Their invocation coroutine owns its copy until the nested handler
  finishes. This removes the shared callable allocation and reference counting
  from this path, while keeping lazy invocation and exception propagation.
  Stateful and nontrivial callables still use shared ownership: copied handlers
  must observe the same mutable state, and pending calls must survive adapter
  destruction. The optimization invokes the original call operator, not an
  arbitrary user-defined conversion to a function pointer.
- `http_dispatch::completion_awaiter` borrows the dispatch state owned by the
  awaiting protocol coroutine. `close_http_dispatch` borrows the same owner from
  a nested scope. Neither needs its own `shared_ptr`. Detached request handlers
  retain ownership because they can outlive a closed connection.
- Router entries are grouped by path in a sorted vector. Lookup is
  `O(log N + M)` for `N` routes and `M` methods on the matching path, instead of
  scanning all routes. Within each path, registration order is preserved for
  `Allow`; HEAD fallback, explicit HEAD priority, query exclusion, exact path
  matching, copy-on-write tables and moved-from router reuse are unchanged.
  Registration may shift vector elements; it is a pre-serving operation.
- The reactor reuses two inbox drain deques across iterations. The mutex and
  pending-flag ordering are unchanged, including the empty-to-nonempty wakeup.
  The producer/consumer path still transfers tasks and callbacks by move.
  This reduces allocator work; local enqueue timing does not demonstrate a
  consistent throughput gain.

## Ownership retained deliberately

| Area | Ownership/lifetime finding |
|---|---|
| Workers | `runtime` already owns contexts through `unique_ptr`; threads borrow contexts and join before returning. No shared reference counter is needed here. |
| Cross-thread submission | Incoming task and callback queues already transfer ownership by move. Replacing them with shared task nodes would add overhead; no such change was made. |
| Router tables | Connection handler copies share immutable tables; copies detach on registration. Deep-copying per connection would allocate and copy all routes. Registration on the same router object still requires external synchronization. |
| Stateful handlers | Instances can be shared across workers. Reference counting protects lifetime, **not** mutable captured state; the application must synchronize that state. |
| Callback timers | Handles may outlive `io_context`. The shared registry/weak handle pair allows safe cancellation after context destruction and recursive capture cleanup. Raw registry pointers would violate this contract. |
| TLS | Streams and SNI mappings retain configuration after the public context is destroyed. Configuration can be shared across workers; freezing/mutation uses the existing mutex. |
| DNS | Resolver destruction and socket removal may precede cancellation continuations. Lookup/socket owners must survive those continuations, including descriptor reuse. |
| QUIC | Connections/streams retain parents; background pumps use weak references to avoid keeping abandoned engines alive. A raw pointer across suspension would be unsafe. |
| HTTP/2 and HTTP/3 | Detached application handlers may remain suspended after protocol/session teardown. They retain only dispatch state; the close guard clears the borrowed transport wake callback. |

## Remaining candidates for profiling

These were inspected but are not established as dominant bottlenecks in this
change. Further changes need workload-specific measurements:

- Root coroutine tracking allocates unordered-set nodes on spawn and erases them
  on completion. An intrusive owner list could remove allocations, but would
  enlarge promise state and needs exception/shutdown analysis.
- DNS initialization reads resolver/hosts configuration synchronously. Sharing
  initialized resolver channels would change cancellation and concurrency
  semantics; large lookup workloads should measure this cost separately.
- QUIC on non-Linux POSIX uses a periodic timer fallback (1–10 ms), while Linux
  uses a rearmable timerfd. A cancellable portable timer could reduce wakeups
  and latency, but cannot be validated by Linux-only timing.
- HTTP parsing/serialization and buffered message bodies still copy/allocate.
  Measure response size, streaming usage, TLS encryption and socket backpressure
  before replacing buffers or changing ownership APIs.

## Validation

- GCC 13, C++20, all protocols: 12 correctness suites, including HTTP/2 and
  HTTP/3 disconnect/cancellation and delayed handler paths.
- GCC 13 debug, ASan + UBSan + leak detection: the same 12 suites.
- Router regressions cover unsorted registration, exact path matching, HEAD and
  Allow ordering, copy-on-write, stateful adapter sharing, lazy exceptions,
  stateless coroutine lifetime and user-defined callable conversion semantics.
- Runtime regressions cover concurrent mixed post/spawn producers, repeated
  sleeping/wakeup, cancellation and destruction.
- Named-module checks: Clang 18/libstdc++ 13, C++20 default/core and C++23 all
  protocols, including header/import cross-translation-unit calls. GCC 13's
  export-using import limitation is recorded in the build guide.

See [raw measurements and reproduction](../benchmarks/results/module-hotpaths-2026-11-16/README.md).
