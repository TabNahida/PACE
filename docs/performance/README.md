# Performance

[简体中文](README.zh.md) · [Documentation](../README.md)

Every number PACE publishes, with the method that produced it and the raw data behind it.

## Reports

| Report | What it measures | Latest |
|:--|:--|:--|
| [Runtime and networking hot paths](runtime.md) | Task, yield, post, timer, cancellation and readiness costs; routing; worker scaling | PACE 0.6.1, macOS arm64 |
| [TCP echo](tcp.md) | PACE, Asio and libuv serving the same echo workload | PACE 0.6.1, macOS arm64; PACE 0.5.0, Linux x86_64 |
| [HTTP gateway](http-gateway.md) | Static files, caching and reverse proxying against nginx | Linux x86_64 |
| [Protocols](protocols.md) | HTTP/1 response completion, WebSocket framing, HTTP/2 and HTTP/3 handler concurrency | Linux x86_64 |
| [History](history/README.md) | Earlier studies and the optimizations they led to | |

## The 1.0 baseline

PACE 0.6.1 was measured on 27 November 2026 as the anchor for the 1.0 work: each phase that rewrites a protocol or
the runtime is compared with it on the same machine. The complete record, with every suite and trial, is
[`benchmarks/results/baseline-0.6.1-macos-arm64/`](../../benchmarks/results/baseline-0.6.1-macos-arm64/README.md).

| Suite | Headline | Report |
|:--|:--|:--|
| Scheduler | `yield` 5.0 ns, `post` 7.4 ns, task spawn and completion 18.3 ns | [runtime](runtime.md#scheduler-components) |
| Cancellation | 250 ns p50 to cancel one wait, unchanged with 65,536 other registrations | [runtime](runtime.md#reactor-and-cancellation-latency) |
| TCP echo | 227,410 req/s, p50 31.7 µs (Asio 190,933; libuv 248,687) | [TCP](tcp.md) |
| WebSocket | 98,532 round trips/s with 64-byte messages, p50 9.2 µs | [record](../../benchmarks/results/baseline-0.6.1-macos-arm64/README.md) |
| HTTP/2 and HTTP/3 | 16 requests whose handlers each wait 10 ms: 24.6 ms (h2) and 25.6 ms (h3) with 8 handlers running concurrently, 188 ms with 1 | [record](../../benchmarks/results/baseline-0.6.1-macos-arm64/README.md) |

The suites that need Linux (the QUIC transport comparison and the nginx gateway comparison) were skipped on this
host. A Linux baseline is still to be recorded.

## How numbers are made

- **Same machine, same run, same method.** A comparison with another library is only published when both were
  measured on one machine, in one session, with the same clients and settings.
- **Everything is kept.** Each report links to its raw data: every trial, the environment, the build and the exact
  commands. No trial is dropped from a completed session.
- **Medians with their spread.** Tables show the median across trials with the observed minimum and maximum.
  Latency percentiles are per trial, then the median of those.
- **No numbers from CI.** Shared CI runners check correctness and portability; their timings are too noisy to rank
  anything.
- **Losses are reported.** Where another library is faster, the report says so.
- **The README figures are generated.** `scripts/render_readme_figures.py` draws them from one result directory and
  prints that run's version, date and host on every figure; CI fails if the figures and their data disagree.

## Run the measurements

```sh
xmake f -c -m release -o build/baseline --tls=y --dns=y --quic=y --http2=y --http3=y \
    --benchmarks=y --components=y --scaling=y --protocol_benchmarks=y --tests=n --examples=n -y
xmake -y
python3 scripts/run_baseline.py --source . --build-dir build/baseline \
    --output benchmarks/results/<name> --label "<host description>" --repeats 5
```

The [Baseline workflow](../../.github/workflows/baseline.yml) runs the same suites on Linux x86_64, Linux arm64 and
macOS runners on demand, to check that they still run, not to publish their timings.
[Benchmark programs and runners](../../benchmarks/README.md)

## See also

- [Execution model](../explanation/execution-model.md)
- [Architecture](../explanation/architecture.md)
