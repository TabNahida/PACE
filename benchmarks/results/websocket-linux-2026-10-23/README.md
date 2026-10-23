# WebSocket framing on local Linux · 23 October 2026

[Protocol investigation](../../../docs/protocol-performance.md) · [Probe source](../../websocket_bench.cpp) · [Runner](../../../scripts/run_websocket_bench.py)

Ten alternating before/after trials per workload. Both peers run on one local
Linux reactor/thread, using TCP_NODELAY and one persistent connection. The
baseline is v0.1.1 (`61fe0e21d62205fca77d1fb1e07e91aa200b6050`); the candidate
adds read-ahead, direct fragment assembly, reusable send storage, word-at-a-time
masking and the runtime readiness helper. Compiled source/binary hashes, compiler
flags and commands are in [final/build.json](final/build.json); the checkout was
dirty during measurement, so its HEAD alone does not identify the candidate.

The 64 B, 16 KiB and 64 KiB/1 KiB-fragment workloads were fixed before measuring.
Each has 1,000 excluded warmup exchanges, followed by 300,000, 30,000 or 3,000
measured exchanges respectively. Every echoed byte is validated, including an
incrementing sequence. Handshake/teardown are excluded. Throughput includes
client payload preparation and validation; per-operation latency ends before
validation. No CPU affinity/frequency pinning was used; other local builds and
benchmarks were paused for measurement. This is neither a multi-worker nor a
TLS benchmark, and does not compare other libraries.

## Final measurements

Values are medians of ten trial summaries, not pooled latency percentiles.
Ranges for every metric are in [summary.json](final/summary.json), with every
individual trial in [trials.json](final/trials.json).

| Payload / fragmentation | Revision | Roundtrips/s | p50 µs | p95 µs | p99 µs |
|---|---|---:|---:|---:|---:|
| 64 B / single frame | before | 256,431 | 3.729 | 3.983 | 6.644 |
| 64 B / single frame | after | 330,083 | 2.886 | 3.059 | 5.255 |
| 16 KiB / single frame | before | 44,342 | 21.553 | 24.360 | 32.921 |
| 16 KiB / single frame | after | 158,337 | 5.807 | 6.264 | 11.727 |
| 64 KiB / 1 KiB fragments | before | 3,339 | 290.698 | 332.524 | 414.616 |
| 64 KiB / 1 KiB fragments | after | 6,252 | 153.255 | 174.774 | 251.652 |

Median throughput improves by 28.7%, 257.1% (3.57×) and 87.2% respectively.
The fixed eight-byte masking operation removes the byte-wise XOR loop that was
particularly costly in the large-frame case. The final candidate improves all
three workloads in this session; it is not evidence of universal superiority.
Each connection additionally owns 8 KiB of read-ahead storage and retains send
buffer capacity, so no connection-memory reduction is claimed.

## Separate syscall diagnostics

Link-time wrappers count calls during 1,000 validated steady-state roundtrips,
covering **both peers together**. Instrumented timings are intentionally omitted
from [diagnostics.json](final/diagnostics.json).

| Workload | `recv` calls/roundtrip, before → after | `send` calls/roundtrip | `getrandom` calls/roundtrip |
|---|---:|---:|---:|
| 64 B | 7 → 2 | 2 → 2 | 1 → 1 |
| 16 KiB | 9 → 6 | 2 → 2 | 1 → 1 |
| 64 KiB / 1 KiB fragments | 450 → 18 | 128 → 128 | 64 → 64 |

Read-ahead amortizes frame-field receives. Masking keys still come from the OS
cryptographic random generator for every client frame; this optimization does
not substitute a cheaper random generator.

## Intermediate result retained

The first candidate only changed read-ahead, assembly and allocation reuse.
Its ten-pair [raw trials](read-ahead-only/trials.json) showed throughput gains
of 20.8% for 64 B and 27.8% for fragmented messages, but a **3.0% regression for
16 KiB frames**. That result motivated the masking change and is retained,
along with its [exact patch](read-ahead-only/implementation.patch) against
v0.1.1 and [build hashes](read-ahead-only/build.json). These are development
iterations on the same host, not independent hardware replications.

## Reproduce

Run from a checkout containing this report. Use a fresh output directory; the
runner refuses to overwrite existing measurements.

```sh
mkdir -p /tmp/pace-ws-baseline
git archive v0.1.1 | tar -x -C /tmp/pace-ws-baseline
python3 scripts/run_websocket_bench.py \
  --baseline-root /tmp/pace-ws-baseline --build-dir /tmp/pace-ws-comparison \
  --output /tmp/pace-ws-results
# Pause other builds/workloads, then measure the frozen binaries.
python3 scripts/run_websocket_bench.py \
  --baseline-root /tmp/pace-ws-baseline --build-dir /tmp/pace-ws-comparison \
  --output /tmp/pace-ws-results --measure-only
```

The runner records the host, binary hashes, invocation order and all trial
summaries. [Environment](final/environment.json) identifies the measured host.
The baseline and candidate use the same probe source and `-O3 -DNDEBUG -std=c++20`.
No instrumented binary contributes to the throughput or latency tables.
