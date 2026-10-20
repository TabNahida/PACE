# Worker placement and count — 20 October 2026

This study evaluates the new `runtime::run_on_current_thread()` alongside
existing `runtime::run()` and application-owned `io_context` threads. All modes
have N active reactors. Caller mode creates N−1 background reactor threads;
the other modes create N. Every mode has the same extra coordinator and blocking
client threads. See the [complete methodology](../../scaling.md).

## Local measurements

150 fresh-process trials, five per cell, with cell order rotated each trial:

- [8 clients: all modes, throughput, p99 and lifecycle tables](local-8-clients.md)
  · [raw JSON](local-8-clients.json) — workers 1, 2, 4, 8.
- [48 clients: all modes, throughput, p99 and lifecycle tables](local-48-clients.md)
  · [raw JSON](local-48-clients.json) — workers 1, 2, 4, 8, 24, 48.

Host: Linux/WSL2, Intel Core Ultra 7 270K Plus, 24 reported logical CPUs,
GCC 13.3, C++20, xmake release (`-O3 -DNDEBUG`, no profiling or sanitizers).
No CPU pinning or frequency control. No concurrent build/benchmark tasks were
run by the development agents during the measurements; the host is not dedicated
benchmark hardware. Both clients and servers run on this host.

Each client keeps one persistent connection and one outstanding request, with
64-byte messages, 10,000 measured + 1,000 warmup requests. Each context listens
on its own ephemeral loopback port; connections are evenly distributed.
Every response is validated, including its changing sequence/connection tag.
The count, per-worker count and checksum all passed for all 150 records.

### Observations and decisions

1. **Worker count depends on workload.** With eight clients, one `run()` worker
   measured 303,599 req/s, versus 210,373 with eight workers. With 48 clients,
   the corresponding medians were 314,390 and 1,086,909 req/s. The 24-worker
   `run()` median was 2,380,521 req/s, versus 1,931,579 for 48 workers. The
   24-worker range was wide (1,116,543–2,609,039); these data do not establish a
   stable universal optimum. We retain an explicit count and the existing
   default rather than choosing a new global default from a local sample.
2. **Caller participation reduces startup work.** With one worker/eight clients,
   median launch-to-all-reactors-ready was 50.1 µs for `run()` and 1.9 µs for
   caller mode; shutdown was 154.9 vs 76.9 µs. N=1 caller mode creates no runtime
   thread. That is useful for embedding and short-lived runtimes.
3. **No consistent steady-state throughput win from caller mode.** In that
   eight-client/one-worker sample, caller throughput was 297,995 vs 303,599 req/s,
   about 1.8% lower. Other cells also vary by placement/mode. Keep both APIs;
   choose based on ownership and measured workload, not an assumed network gain.
4. **Application-owned threads work across the same workload.** The external
   mode requires no runtime object. CPU/blocking application work may run outside
   reactors and send results with `post`; there is no new awaitable offload pool
   or unsafe socket/coroutine migration in this change.

The measurements isolate launch, warmup, steady-state and shutdown windows. They
do not isolate server CPU consumption, shared-port accept distribution or HTTP
handling. Most steady phases are below five seconds. Interpret the ranges as
exploratory tuning evidence and repeat on deployment hardware with longer trials.

### Provenance and reproduction

The working tree was based on merged main
`561ca353af696e1b95e378382b857ae033cb24c4`, with this change applied. Each raw
report preserves the tracked dirty state, all source hashes, binary hash, build
metadata, commands, timing boundaries and results. The manifest verifies that
the relevant source hashes match the files published with this study. The same
release target was rebuilt with verbose output after measurement; its binary
hash matches both measured reports. [Build log](local-build.txt) · [Manifest](manifest.json).

```sh
XMAKE_GLOBALDIR=/tmp/pace-xmake xmake f -m release -o build/worker-study \
  --benchmarks=n --components=y --scaling=y --profiling=n --sanitizers=n \
  --examples=y --tests=y -y
XMAKE_GLOBALDIR=/tmp/pace-xmake xmake -y -j4 pace_scaling
python3 scripts/run_scaling.py --build-dir build/worker-study \
  --workers 1,2,4,8,24,48 --clients 48 --requests 10000 --warmup 1000 \
  --repeats 5 --label local-wsl2-24cpu --output local-48-clients.json
python3 scripts/run_scaling.py --build-dir build/worker-study \
  --workers 1,2,4,8 --clients 8 --requests 10000 --warmup 1000 \
  --repeats 5 --label local-wsl2-24cpu --output local-8-clients.json
```

`XMAKE_GLOBALDIR` is only a local build-environment choice, not required by PACE.
The fair Asio/libuv benchmark remains unchanged and is reported separately in
the [complete performance document](../../../docs/performance.md).
