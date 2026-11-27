# Ownership and hot-path review (16–18 November 2026)

[简体中文](ownership-review-2026-11.zh.md) · [Performance history](README.md)

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

- QUIC now uses PACE callback timers on all supported POSIX platforms. Intrusive
  waiters avoid list-node allocations and queue continuations on the reactor.
  The ngtcp2 backend uses per-connection progress signals, uniquely owned stream
  buffers, bounded packet batches and bounded send/receive windows. Its connection
  routing table borrows reactor-affine states (removed on destruction), avoiding
  a weak-pointer lock on each received packet. Background pumps use weak references. This is a scheduling and
  ownership change. The [transport comparison](../../../benchmarks/results/quic-backends-2026-11-18/README.md) measures both backends.

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

- The root-task registry and cancellation of sleeping tasks were measured in the
  [runtime and socket follow-up](../../../benchmarks/results/runtime-network-2026-11-18/README.md).
- DNS initialization reads resolver/hosts configuration synchronously. Sharing
  initialized resolver channels would change cancellation and concurrency
  semantics; large lookup workloads should measure this cost separately.
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

See [raw measurements and reproduction](../../../benchmarks/results/module-hotpaths-2026-11-16/README.md).

### QUIC backend and modern compiler follow-up

- OpenSSL and ngtcp2 backends use the same QUIC and HTTP/3 tests. Additional cases
  drop the first Initial and subsequent packets in both directions, recycle 160
  streams (exceeding the stream count and connection window), and exchange
  192 KiB binary payloads between different backends with each acting as server.
- Both backends pass local ASan + UBSan + leak detection for runtime, QUIC and
  HTTP/3, including loss recovery and stream recycling. The ngtcp2 all-enabled
  local configuration also passes the Clang 18 C++20 named-module ABI test.
- [Remote validation](https://github.com/TabNahida/PACE/actions/runs/43988664266)
  tests code commit `cc258c0c1a73e9580f155d2e13438f559305928f` on Ubuntu 24.04.
  GCC 15.2, GCC 16.0.1 (20260315 experimental snapshot), and Clang 21.1.8 each
  pass 25 protocol/example tests with both backends. GCC 16 and Clang 21 also
  pass C++20 `import pace` with all protocols and header/import ABI mixing.
  The separate OpenSSL ↔ ngtcp2 interoperability job passes in both directions;
  all seven jobs are green.
- GCC 15 module imports reproduce an `expand_CO_FRAME` compiler ICE; GCC 13
  cannot expose this facade's exported using declarations. Module builds now
  reject GCC versions below 16 with a diagnostic. Header builds retain their
  existing compiler requirements. The upstream fix is described in the build guide.
- The [0.6.0 module checks](https://github.com/TabNahida/PACE/actions/runs/44238306831)
  cover both QUIC backends using the GCC 16.2 release and Clang 21.
- The [QUIC transport comparison](../../../benchmarks/results/quic-backends-2026-11-18/README.md)
  measures bulk goodput, short-message RTT and peak RSS for both backends and the 0.5.0 scheduler.
