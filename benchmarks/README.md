# Reproducible TCP echo comparison

[Complete performance results](../docs/performance.md) · [Worker/execution scaling](scaling.md)

[HTTP/WebSocket protocol investigation](../docs/protocol-performance.md) uses
separate probes for response completion, frame processing and multiplexed handlers:

```sh
xmake f -m release --protocol_benchmarks=y --tls=y --http2=y --http3=y -y
xmake -y
xmake run pace_http_close_probe --transport tcp --trials 10
xmake run pace_websocket_bench --bytes 16384 --iterations 30000
xmake run pace_protocol_multiplex --protocol h2 --handlers 8 --streams 8 --delay-ms 20 --batches 10
```

Only probes for enabled protocol modules are built. HTTP/3 requires POSIX;
omit `--http3=y` on Windows. Published measurements use local Linux and preserve
build manifests, individual trials and workload validation in the linked report.

For isolated PACE operation timings before optimization, see the
[component microbenchmarks](components.md). Their ns/op units are explicitly
defined and separate from this end-to-end TCP workload.
For instrumented reactor-stage diagnosis, see [stage profiling](profiling.md).
Profiling builds are explicitly excluded from backend comparisons.

`echo_bench.cpp` hosts PACE, standalone Asio, and libuv behind the same TCP echo
protocol. Every backend uses one server thread and the same independent blocking
client implementation. Each client has one persistent connection and one request
in flight. Both client and server enable `TCP_NODELAY`; every response is checked
in full, including a changing sequence number. PACE and Asio use C++20 coroutines;
libuv uses callbacks, its native public programming model.

The benchmark excludes server startup, connection establishment and per-connection
warmup. A start gate releases all clients after warmup. Throughput uses total
completed round trips divided by wall time from gate release until clients have
joined. Each round-trip latency includes sending, receiving and payload validation.
All latencies are stored and nearest-rank p50, p95 and p99 plus the maximum are
computed after measurement. The returned sample count must equal completed requests.
There is no sampling, histogram rounding or coordinated-omission correction.

Each fresh benchmark process records a peak resident-memory metric for its
**current executable image**, where a verified OS provider exists:

| Platform | Source recorded in JSON | Meaning |
|---|---|---|
| Linux | `/proc/self/status.VmHWM_KiB` | Peak RSS of the current memory image, converted from KiB to bytes; kernel RSS accounting is approximate. |
| macOS | `task_info(MACH_TASK_BASIC_INFO).resident_size_max_bytes` | Current Mach task's lifetime resident peak in bytes; exec creates a new task and accounting ledger. |
| Windows | `GetProcessMemoryInfo.PeakWorkingSetSize` | Current process's peak working set in bytes. |
| FreeBSD | `unavailable` | No verified current-image peak provider; `available: false`, `bytes: null`, and an explicit reason. |

The reading is taken after clients and server have joined, before allocating the
merged latency vector for reporting. Its window includes image startup, warmup,
stacks, server, identical blocking clients and stored latency samples. It is
**whole-process peak RSS/working set, not isolated library footprint**, steady-state
memory, allocation count, PSS or per-connection cost. Sources have different OS
accounting semantics; compare only within the same OS, executable and workload.
Small differences do not establish a library-memory advantage.

The earlier version 1 used POSIX `getrusage().ru_maxrss`. That counter can preserve
a launcher's pre-exec RSS high-water mark, so a Python runner's memory could
dominate small workloads. Such raw results are retained as diagnostic evidence,
but their POSIX memory figures must not support benchmark-image or library memory
claims. Version 2 never falls back to that counter, never substitutes zero for
missing data, and adds no sampling thread or child-process wrapper to timed runs.
Throughput and latency from completed old trials retain their original scope;
only their memory interpretation is invalidated by this issue.

Linux describes [VmHWM and its accounting limitations](https://man7.org/linux/man-pages/man5/proc_pid_status.5.html).
The macOS [task implementation](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/task.c)
and [exec implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_exec.c)
show the task ledger used for `resident_size_max` and task replacement on exec.
These source semantics are complemented by a native-runner diagnostic:

```sh
python3 scripts/run_benchmarks.py --build-dir build/fair --check-memory
```

This runs **no network benchmark**. `pace_bench --memory-probe` touches and frees
32 MiB, then verifies that the peak remains recorded. The Python diagnostic runs
it once normally and again while its launcher holds an additional 128 MiB;
the child's initial peak must remain within a 16 MiB tolerance, and its retained
peak must increase by at least 24 MiB after the allocation. The generous tolerances
allow OS accounting granularity while catching current-RSS substitution and
launcher contamination. FreeBSD prints an explicit unavailable result. Use a
normal uninstrumented binary for this check, and preserve its JSON output with
the measurement evidence. Run it separately before comparative trials.

```sh
# Build a release-mode pace_bench target with all optional dependencies enabled.
python3 scripts/run_benchmarks.py \
  --executable build/linux/x86_64/release/pace_bench \
  --require-all --repeats 5 --clients 8 --requests 5000 --warmup 500 --bytes 64 \
  --output benchmarks/results/local
```

Use the executable path for your platform; on Windows it ends in `.exe`. Omitting
`--executable` selects the newest benchmark executable under `build/`.
Pass `--build-dir build/fair` to restrict discovery to one build directory.
`--message-size` is an alias for `--bytes`. If `--output` ends in `.json`, the runner
writes a combined JSON report there and puts individual evidence files in a
sibling `<name>-evidence/` directory. It also appends its Markdown table to
`GITHUB_STEP_SUMMARY` when run in GitHub Actions. The
runner rotates backend order, enforces a hard per-run process timeout, fails on
missing required backends or invalid output, and writes all raw runs plus binary
hash, dependency versions, OS, CPU, Git revision, invocation and aggregation rules.
Individual sockets also have send/receive timeouts. A failed comparison never
produces a successful aggregate. The runner reports medians and min–max spread;
it does not cherry-pick the fastest trial. Every measured latency and peak-memory
metric retains median/min/max across trials in JSON. The Markdown maximum column
is the median of each trial's maximum, while `max_max_us` retains the worst
observed request across all trials. Neither is a pooled latency percentile.
Historical reports without extended metrics remain valid; absent p95/memory is
reported as unavailable, never filled with a guessed value. The optional `--list` executable flag
reports exactly which backends were compiled in.

## Acceptance criteria

A successful CI comparison requires every requested backend to complete every
trial within its timeout, with all response bytes verified. Each record must
match the requested trial and workload exactly, contain finite positive timing
and throughput values, satisfy `p50 <= p95 <= p99 <= max` for new records, and agree with the completed
request count and payload size. The runner rejects a mismatch before aggregation
and preserves the failure evidence. Use `--require-all --repeats 3` or more for
CI comparisons. This acceptance criterion establishes a working, measurable
benchmark; it does not certify that PACE is faster.

New executables advertise `extended_metrics_version=2`, requiring p95,
sample-count and a peak-memory object together. Available memory must be a positive
integer byte count with a current-image source, scope, window and `available: true`;
KiB values must convert to whole multiples of 1024. Unavailable memory must use
`available: false`, `bytes: null`, `source: "unavailable"` and its documented reason.
It appears as `n/a` in Markdown; summaries omit numeric memory aggregates entirely.
Version 1 records remain readable with their original source/window contract,
and pre-extension records retain p50/p99/max validation. No schema versions or
different memory sources can be silently mixed in a backend-comparison aggregate.

For a performance claim, evaluate each baseline separately on the same dedicated
hardware and compiler configuration. A proposed release target is a throughput
improvement of at least 5%, with the lower 95% confidence bound of paired trial
ratios above 1.05 and median p99 latency no more than 5% worse than that baseline.
Use at least 10 rotated trials per backend, choose request counts that keep each
measured trial running for at least five seconds, and repeat the experiment in
another session. Predeclare client counts and payload sizes, and report every
scenario, including regressions. These are proposed performance criteria, not
currently enforced CI thresholds; the present shared-runner smoke comparison is
too short and noisy to establish them. HTTP speed and multiworker scaling need
separate workloads before making those claims.

## What the numbers establish

These are closed-loop, localhost TCP echo measurements. They include client,
kernel, scheduling and server costs. They are useful for regressions and comparing
this specific workload, but do not isolate reactor overhead, establish maximum
server capacity, measure multiworker scaling, or compare HTTP implementations.
Shared GitHub Actions runners are noisy and may use different CPUs on each run;
only compare backends within the same job and inspect the run spread.

For deeper investigation, vary client count and payload size, use a dedicated
machine, record compiler/build flags, control CPU placement and frequency, then
run an independent open-loop load generator from a different machine. Interpret
p99 alongside throughput: closed-loop clients reduce offered load when responses
slow, so their latency does not model overload or correct coordinated omission.

## Optional compile definitions

- `PACE_HAS_ASIO`: include standalone `asio.hpp` and enable its C++20 coroutine backend.
- `PACE_HAS_LIBUV`: include/link libuv and enable its callback backend.
- Neither dependency is bundled into PACE's library or required by normal consumers.

All servers use a 64 KiB per-connection application buffer. libuv pauses reads while
each asynchronous write is outstanding, preserving that buffer until completion;
PACE and Asio likewise await each write before reading again. Library-internal
allocation and scheduling strategies remain part of the comparison.
