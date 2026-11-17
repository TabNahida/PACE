# Local ownership/hot-path measurements, 16–17 November 2026

Linux x86-64, Intel Core Ultra 7 270K Plus (virtualized environment), GCC 13.3.0,
`-std=c++20 -O3 -DNDEBUG -pthread`. Profiling and sanitizers disabled.
Baseline source: `d3842473b2bea63e291eb25fe44f610cbd49b02e` (the initial dirty
xmake version/source-list settings do not affect these direct compiler runs).
The same benchmark sources and flags were used against old and new sources.
Seven paired trials alternate before/after order; processes were allowed CPUs
0 and 1, with no fixed producer/consumer placement. Values below are medians
of ns/operation, **not network throughput**. Raw results retain every trial.

Router cases run 200,000 operations after 5,000 warmups, matching the last
registered path, with a trivial empty synchronous handler. A thread is created
and joined first so standard-library shared ownership uses its multithreaded
path. Small-route results change little; the 1,024-route case demonstrates the
benefit of replacing the linear scan. Other route distributions may differ.

Cross-thread cases run 200,000 submissions with 5,000 warmups and measure only
producer enqueue time, excluding final drain/synchronization. Other components
run 500,000 operations with 5,000 warmups. Small percentage changes on this
shared machine are not evidence of a repeatable speedup or regression. In
particular, inbox reuse reduces allocations but shows no consistent enqueue
latency improvement. Changes in yield/post timing cannot be attributed to
inbox reuse because those cases do not repeatedly drain external work.

| Case | Before (ns/op) | After (ns/op) | Change |
|---|---:|---:|---:|
| `cross_thread_post_batch_1` | 1951.50 | 1937.43 | -0.7% |
| `cross_thread_post_batch_64` | 34.05 | 34.68 | +1.9% |
| `cross_thread_post_batch_256` | 26.40 | 27.28 | +3.4% |
| `yield` | 20.65 | 18.82 | -8.9% |
| `post` | 23.42 | 22.21 | -5.2% |
| `spawn_completion` | 33.04 | 33.18 | +0.4% |
| `router_1` | 80.54 | 77.13 | -4.2% |
| `router_16` | 84.41 | 82.61 | -2.1% |
| `router_1024` | 435.29 | 94.70 | -78.2% |

## Allocation diagnostic

`pace_inbox_allocations` instruments ordinary `operator new` on the reactor
thread only. The producer waits for each post to finish before submitting the
next one, for 10,000 rounds. It is separate from timing instrumentation. All
three paired runs counted **40,629 → 625 allocations** (98.46% fewer).
This measures allocations in this diagnostic, not total process allocations
or retained memory. It confirms removal of two deque maps/initial blocks per
drain; callback transfer, synchronization and shutdown semantics are unchanged.

Raw files: [timings](measurements.json), [allocation counts](allocations.json).

## Reproduce

From the repository root, extract the baseline without changing your checkout:

```sh
mkdir -p build/hotpath-before-src build/hotpath-before build/hotpath-after
git archive d3842473b2bea63e291eb25fe44f610cbd49b02e include src | tar -x -C build/hotpath-before-src
```

Compile each benchmark with `g++ -std=c++20 -O3 -DNDEBUG -pthread`. For the old
binary, use `-Ibuild/hotpath-before-src/include` and the runtime sources under
`build/hotpath-before-src/src/runtime/`; for the new binary, use `-Iinclude` and
`src/runtime/`. In both cases use these **current** benchmark sources:

- `benchmarks/router_bench.cpp` → `pace_router_bench`
- `benchmarks/components_bench.cpp` → `pace_components`
- `benchmarks/inbox_allocations.cpp` → `pace_inbox_allocations`

For example, build the new router binary:

```sh
g++ -std=c++20 -O3 -DNDEBUG -pthread -Iinclude benchmarks/router_bench.cpp src/runtime/io_context.cpp src/runtime/poller.cpp src/runtime/runtime.cpp -o build/hotpath-after/pace_router_bench
```

Run old/new executables alternately seven times; keep CPU placement the same:

```sh
taskset -c 0,1 build/hotpath-after/pace_router_bench 1024 200000
taskset -c 0,1 build/hotpath-after/pace_components --component cross_thread_post --iterations 200000 --warmup 5000 --batch 64
build/hotpath-after/pace_inbox_allocations
```

Repeat the router case with 1 and 16 routes, cross-thread post with batches 1
and 256, and other component cases with 500,000 iterations. Benchmark targets
are also available through xmake with `--components=y`.

[Ownership decisions and validation](../../../docs/ownership-performance-review.md)
