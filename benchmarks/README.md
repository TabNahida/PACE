# Reproducible TCP echo comparison

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
All latencies are stored and nearest-rank p50 and p99 are computed after measurement.
There is no sampling, histogram rounding or coordinated-omission correction.

```sh
# Build a release-mode pace_bench target with all optional dependencies enabled.
python3 scripts/run_benchmarks.py \
  --executable build/linux/x86_64/release/pace_bench \
  --require-all --repeats 5 --clients 8 --requests 5000 --warmup 500 --bytes 64 \
  --output benchmarks/results/local
```

Use the executable path for your platform; on Windows it ends in `.exe`. Omitting
`--executable` selects the newest benchmark executable under `build/`.
`--message-size` is an alias for `--bytes`. If `--output` ends in `.json`, the runner
writes a combined JSON report there and puts individual evidence files in a
sibling `<name>-evidence/` directory. It also appends its Markdown table to
`GITHUB_STEP_SUMMARY` when run in GitHub Actions. The
runner rotates backend order, enforces a hard per-run process timeout, fails on
missing required backends or invalid output, and writes all raw runs plus binary
hash, dependency versions, OS, CPU, Git revision, invocation and aggregation rules.
Individual sockets also have send/receive timeouts. A failed comparison never
produces a successful aggregate. The runner reports medians and min–max spread;
it does not cherry-pick the fastest trial. The optional `--list` executable flag
reports exactly which backends were compiled in.

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
