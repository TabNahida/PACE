# Component study — 20 October 2026

This local before/after study measures the effect of reducing empty-inbox work,
coalescing external-submission wakeups, avoiding nonblocking polls in task-only
loops, and removing unnecessary native poll-buffer initialization. It compares
two PACE builds. It does not measure TCP/HTTP throughput or compare Asio/libuv.

Linux/WSL2 x86_64, Intel Core Ultra 7 270K Plus, GCC 13.3.0, C++20,
`-O3 -DNDEBUG -pthread`, profiling disabled. Both variants compile and link the
same five core source files in the same order. Each process performs 10,000 warmup
operations followed by 1,000,000 measured operations. Chain depth is 4 and batch
size is 256. Runs were sequential; CPU affinity and frequency were not fixed.

## Paired results

Eight pairs per component alternate before/after and after/before execution.
Times are medians of process-level **batch-average ns per operation**, not
per-operation latency percentiles. A paired ratio is after/before; lower is
better. The ratio column is the median of the eight paired ratios, which need
not equal the ratio of the two timing medians.

| Component / raw evidence | Before ns/op | After ns/op | Paired ratio | Exploratory 95% interval | After wins |
|---|---:|---:|---:|---:|---:|
| [`lazy_task`](matched-lazy_task.json) | 5.70 | 5.69 | 0.997 | 0.992–1.014 | 4/8 |
| [`await_chain`](matched-await_chain.json) | 35.84 | 35.86 | 0.998 | 0.967–1.010 | 4/8 |
| [`empty_poll`](matched-empty_poll.json) | 74.60 | 58.26 | 0.785 | 0.771–0.809 | 8/8 |
| [`yield`](matched-yield.json) | 125.72 | 18.88 | 0.150 | 0.149–0.150 | 8/8 |
| [`post`](matched-post.json) | 129.11 | 22.09 | 0.171 | 0.169–0.172 | 8/8 |
| [`cross_thread_post`](matched-cross_thread_post.json) | 325.27 | 22.89 | 0.071 | 0.070–0.073 | 8/8 |
| [`spawn_completion`](matched-spawn_completion.json) | 31.66 | 30.61 | 0.960 | 0.931–0.979 | 7/8 |
| [`zero_timer`](matched-zero_timer.json) | 138.52 | 33.13 | 0.239 | 0.238–0.243 | 8/8 |

The clearest changes are in task-only loop work and batched producer enqueue
cost. The task-only cases have no registered sockets, so skipping an empty
nonblocking poll is relevant there; network workloads still poll for readiness.
The cross-thread result is producer enqueue cost with a live consumer and
coalesced wakeups, excluding batch-drain waits. It is not callback completion
latency. Lazy task allocation and the whole four-frame await chain show no
meaningful change within the observed variation. Completion cost shows a small
reduction, which cannot be attributed to one optimization from this combined-build
experiment.

Intervals use 10,000 whole-pair bootstrap resamples with seed 0. Eight pairs on
one WSL2 machine provide exploratory evidence only: correlated scheduling or
frequency drift can invalidate the independence assumption. No performance
threshold determines test success. Keep these results separate from the
[platform TCP comparisons](../ci-2026-10-20/README.md).

## Exactly one operation

| Component | Measured unit and boundary |
|---|---|
| `lazy_task` | Create and destroy one unstarted task through a non-inlined indirect factory; includes allocation/deallocation, no coroutine body or reactor. |
| `await_chain` | Await and destroy one complete chain of four nested task frames by direct coroutine resume; includes transfer and value propagation, no reactor. |
| `empty_poll` | One native poller wait with timeout zero, 256 output slots and no user descriptors; internal wakeup registration remains present. |
| `yield` | One yield suspension/resumption, including queue and reactor bookkeeping; one task outstanding. |
| `post` | Enqueue and execute one same-context callback with a pointer capture; one callback outstanding. |
| `cross_thread_post` | One producer enqueue into a concurrent reactor consumer, timed in batches of 256; includes queue/mutex/wakeup work, excludes waiting for batch drain. |
| `spawn_completion` | Create, spawn, run, complete and destroy one child task in batches of 256; includes the amortized completion-marker cost after frame reaping. |
| `zero_timer` | One `sleep_for(0ns)` suspension/resumption including deadline clock read, heap enqueue/expiry and scheduling; one timer outstanding. |

[Full boundaries, validation and limitations](../../components.md). Construction,
initial scheduling, stop and final teardown are outside the single-thread
reactor timing. Counts and checksums are validated; loop and harness overhead
remain included. These units are not additive estimates of request latency.

## Evidence and reproduction

- [Before rotated suite](components-before-matched.json) and [after rotated suite](components-after.json): five trials for each of eight components, 40 raw records per build. These were separate sessions; the table above uses the later alternating paired runs.
- The eight linked paired reports contain 128 further raw records, execution order, exact commands, settings, machine metadata and binary hashes.
- [Build manifest](build-manifest.json): binary and per-file source SHA-256, compiler arguments and report hashes. Both builds came from modified working trees based on the same Git commit, so that commit alone does not identify either measured binary.
- [Core source patch](core-before-to-after.patch): the preserved baseline `include/` and `src/` snapshot to the measured after source. It can be reversed against sources matching the after manifest hashes to recover the baseline core.

One unrelated untracked local path was omitted from each archived report's Git
status metadata, with an explicit archive note. All timing records, commands,
settings and computed results are unchanged; the manifest retains hashes of the
original and archived reports.

Build the dependency-free component target with xmake:

```sh
xmake f -m release --components=y --profiling=n --sanitizers=n -y
xmake pace_components
python3 scripts/run_components.py --iterations 1000000 --warmup 10000 \
  --repeats 5 --output benchmark-results/components.json
```

Preserve each build's executable, then repeat each component separately:

```sh
python3 scripts/compare_builds.py --before /tmp/pace-components-baseline-full \
  --after /tmp/pace-components-after --component empty_poll --pairs 8 \
  --iterations 1000000 --warmup 10000 --depth 4 --batch 256 \
  --output benchmark-results/matched-empty_poll.json
```

The archived builds used direct GCC invocations recorded in the manifest;
xmake is the normal project workflow. Run no other benchmarks or builds during
measurement, and repeat on the deployment hardware before making performance
decisions.
