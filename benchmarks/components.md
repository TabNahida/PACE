# Component microbenchmarks

These measurements identify costs in PACE before changing the runtime. They do
not split a TCP request into synthetic percentages, compare against Asio/libuv,
or establish application throughput. Each row measures its own explicitly
defined operation. Compare a component with the same component, configuration,
compiler and hardware before and after a change.

```sh
xmake f -m release --components=y --profiling=n --sanitizers=n -y
xmake pace_components
python3 scripts/run_components.py --iterations 100000 --warmup 5000 --repeats 5 \
  --output benchmark-results/components.json
```

The `components` build option adds no third-party dependencies. It is separate
from `benchmarks`, which enables the TCP comparison and its Asio/libuv packages.
Use `python` and a single command line on Windows. The runner finds the newest
`build/**/pace_components(.exe)` or accepts `--executable`. Save the baseline
executable before rebuilding an optimization; each report records its SHA-256.
The runner rotates component order across trials, warms each fresh process,
enforces a process timeout, and saves all raw measurements, commands, compiler
metadata, OS, CPU, Git state, operation definitions and spread. No external
Python packages are required. Run benchmark jobs sequentially on an otherwise
idle machine; concurrent builds or benchmarks invalidate close comparisons.

| Component | Exactly one operation | Included costs and boundaries |
|---|---|---|
| `lazy_task` | Create and destroy one unstarted `task<uint64_t>` | Frame allocation/deallocation and an indirect call through a volatile, non-inlined factory; the coroutine body must never execute. No reactor. |
| `await_chain` | Await one synchronous nested task chain | `--depth` task frames (default 4), symmetric transfer, returned-value propagation and child-frame destruction. One outer driver is created/destroyed outside timing. It runs by one direct coroutine resume, with no reactor or scheduler. |
| `empty_poll` | Call the native poller once with timeout zero | No user descriptors, a 256-event output buffer initialized once outside timing, the poller's internal wakeup registration and native syscall. Tests empty-poll overhead, not readiness delivery. |
| `yield` | Suspend and resume once with `io_context::yield()` | Ready-queue insertion/removal, actual reactor-loop/poller bookkeeping and coroutine resume. One task is outstanding. |
| `post` | Enqueue and invoke one callback on the same context | A callback with one pointer capture, `std::function` handling, queue and loop dispatch. Each callback posts the next; only one is outstanding. No cross-thread wakeup. |
| `cross_thread_post` | Submit one callback from a producer thread to a concurrent reactor consumer | Producer-side queue/mutex/wakeup work, measured in bounded `--batch` groups. One clock pair per batch; thread startup, waiting for each batch to drain and final shutdown are excluded. This is enqueue cost under a live consumer, not end-to-end callback latency. |
| `spawn_completion` | Create, spawn, run, complete and destroy one child task | Bounded batches (`--batch`, default 256), root tracking and completion cleanup. One posted marker per batch runs after completed frames are reaped; that amortized marker cost is included. |
| `zero_timer` | Suspend and resume once with `sleep_for(0ns)` | Deadline clock read, timer heap insertion/expiry, ready scheduling and reactor passes. One timer is outstanding; this is not a timer-heap scaling test. |

For single-thread reactor cases, construction, initial bootstrap scheduling, stop and final
destruction are outside the timed interval. `io_context::run()` executes on the
calling thread. Warmup uses a separate context because contexts are single-use.
The zero timer deliberately suspends: it is not a ready-awaiter shortcut.

The cross-thread case uses two threads. Its consumer updates a small completion
counter per callback and signals a condition variable once a batch drains. This
control work can influence contention, although the producer's drain wait is
outside timing; keep the batch size fixed in before/after comparisons.

The executable checks operation counts and checksums before reporting success;
the Python runner checks those again together with configuration, finite timing
and `ns/op = elapsed_ns / iterations`. `lazy_task` also verifies the body was
never entered. The loop and small validation counters remain included; there is
no empty-loop subtraction or claim that a row isolates one CPU instruction.
`await_chain` reports **ns per whole chain**, not ns per nested await.

Reports contain repeated **batch-average ns/op**, with median and min–max across
trials. They are not per-operation latency samples, p50 or p99. There are only two
clock reads around each measured batch (each enqueue batch for cross-thread
posts); timer deadline reads are intrinsic to
`zero_timer`. Increase `--iterations` until runs are long enough to reduce clock
and startup effects, then repeat the experiment in another session. Do not add
these costs together to estimate TCP/HTTP latency: the cases have different
queue occupancy, scheduling and operation boundaries. Socket readiness, system
calls under network load, wakeup-to-dispatch latency, HTTP parsing and timer
scaling need separate probes. `--list` records whether the binary was compiled
with `PACE_ENABLE_PROFILING`; use uninstrumented builds for final timings because
instrumentation adds its own measurement overhead.

## Paired before/after experiments

After preserving two uninstrumented binaries, alternate their execution order
to reduce consistent first/second-run bias:

```sh
python3 scripts/compare_builds.py --before /tmp/pace-components-before \
  --after /tmp/pace-components-after --component empty_poll --pairs 10 \
  --iterations 1000000 --output benchmark-results/empty-poll-pairs.json

python3 scripts/compare_builds.py --before /tmp/pace-echo-before \
  --after /tmp/pace-echo-after --tcp --pairs 10 --clients 4 --requests 100000 \
  --output benchmark-results/tcp-pairs.json
```

Use equivalent executable paths on Windows. The runner reuses the ordinary
component/TCP validators, rejects profiling binaries, checks compiler/standard
metadata, and saves both binary hashes, configuration, exact commands and every
raw result. Run each paired experiment alone, not concurrently with another
benchmark or build. It does not infer full compiler flags from metadata; build
both variants with matching flags yourself.

Ratios are **after / before**: below 1 favors the new build for ns/op, above 1
favors it for TCP requests/s. Reports preserve each pair, ratio spread, wins,
median ratio and a reproducible percentile bootstrap interval for that median
(10,000 whole-pair resamples, seed 0 by default). This exploratory interval
assumes representative independent pairs; shared-runner noise and correlated
clock/frequency drift can violate that assumption. It is not a guarantee of
general improvement. The process fails on invalid/incomplete measurements;
performance gains or regressions do not determine its exit code.
