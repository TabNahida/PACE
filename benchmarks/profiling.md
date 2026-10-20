# PACE stage diagnostics

Use stage instrumentation to locate expensive reactor work before choosing an
optimization. Keep it separate from the [TCP backend comparison](README.md) and
the [component microbenchmarks](components.md): reading a clock around each scope
adds overhead, so diagnostic throughput is not evidence against Asio or libuv.

## Build and collect

The `profiling` xmake option is off by default. Build diagnostic executables into
a separate directory so their results and binaries remain identifiable:

```sh
xmake f -m release -o build/profile --benchmarks=y --profiling=y --components=n --examples=n --tests=y -y
xmake -y
xmake test -v
python3 scripts/run_benchmarks.py --build-dir build/profile --backends pace \
  --repeats 3 --clients 4 --requests 10000 --warmup 1000 --message-size 64 \
  --output benchmark-results/profile-local.json
python3 scripts/run_profile.py --input benchmark-results/profile-local.json
```

Use `python` on systems where Python 3 has that name. The runner refuses an
instrumented binary unless `--backends pace` is selected explicitly; it rejects
`--require-all` with instrumentation. Every raw record carries an `instrumented`
flag, and the combined report identifies itself as `instrumented_diagnostic`.

The final command validates all requested trials and writes
`profile-local-stages.md` and `profile-local-stages.json` beside the raw report.
It also appends the stage table to `GITHUB_STEP_SUMMARY` in CI. Each stage reports
call count, inclusive and exclusive time, exclusive min/max spread, the share of
reactor lifetime, and average inclusive ns/call. Values are medians of per-trial
values; average ns/call omits trials with zero calls. They are not per-call latency
percentiles. Raw integer nanosecond counters remain in the input evidence.

Return to a separate uninstrumented build for performance comparisons:

```sh
xmake f -m release -o build/fair --benchmarks=y --components=y --profiling=n -y
xmake -y
python3 scripts/run_components.py --repeats 5 --output benchmark-results/components-local.json
python3 scripts/run_benchmarks.py --build-dir build/fair --require-all --repeats 5 \
  --output benchmark-results/fair-local.json
```

`--build-dir` restricts benchmark executable discovery. `--executable` can select
an exact file instead. Component timing also rejects profiling builds. CI runs
components and the three-backend comparison before configuring the diagnostic
build, then runs both test suites again with profiling enabled on Linux, macOS,
Windows and FreeBSD.

## What each stage measures

| Stage | Scope |
|---|---|
| `reactor_run` | One worker's event-loop lifetime; its exclusive time is work outside named child scopes |
| `inbox` | Inspect and transfer cross-thread submitted tasks and callbacks |
| `post_dispatch` | Execute the current bounded batch of callbacks |
| `coroutine_execution` | Resume the current bounded batch of ready coroutine handles |
| `root_reaping` | Reclaim completed root tasks and report their errors |
| `timers` | Read the clock, expire timers and determine the poll timeout |
| `poll_nonblocking` | Native poll with a zero timeout |
| `poll_blocking` | Native poll with a positive or indefinite timeout, including idle waiting |
| `event_dispatch` | Convert native readiness events into scheduled work |
| `io_arm` | Register a coroutine's pending socket operation |
| `poller_update` | Update native descriptor interests |
| `recv`, `send`, `accept` | Socket syscall attempts, including failures and would-block results |

These are **wall-clock scopes**, not CPU samples. Scheduling delays and blocked
poll waits are included. A large `poll_blocking` share can mean the server is
waiting for clients; it does not establish a CPU bottleneck. Use a sampling
profiler separately when CPU instruction cost is the question.

Inclusive time contains nested stages and must not be summed. Exclusive time
subtracts child scopes. The summarizer checks that exclusive totals account for
the root lifetime in each trial, then calculates each share using that trial's
root time. Medians from different trials need not sum to a median total or 100%.
Timing scopes close before coroutine suspension, so suspended time is not charged
to the coroutine that yielded.

## Measurement window

Counters reset on the server worker immediately before `io_context::run()` and
are collected after it returns, with the worker joined before the benchmark reads
them. The profile therefore includes event-loop startup, client connection setup,
warmup, measured requests and shutdown. Listener construction and the initial
spawn happen on the main thread before the reset and are excluded. The ordinary
throughput interval still starts after warmup and ends when measured clients have
joined. Dividing these stage counters by the measured request count would mix two
different windows, so the report does not present stage ns/request.

Instrumentation is thread-local and compile-time optional. It records no client
thread timings. Disabled builds compile out the timing scopes. Enabled builds
preserve native socket error state around clock reads, and must pass the same
runtime and network tests before the diagnostics are accepted.
