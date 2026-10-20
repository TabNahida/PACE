# CI runtime study — 20 October 2026

[Successful Actions run](https://github.com/TabNahida/PACE/actions/runs/39397883915) · [PR #3](https://github.com/TabNahida/PACE/pull/3).

All seven jobs passed: Linux C++20/23, macOS, Windows, FreeBSD, sanitizers and a separate Linux before/after comparison. Native/FreeBSD jobs ran both ordinary and instrumented tests.

Source head: `62c8724b177a9ca03ed0b607bc5a74a852c26753`. GitHub checkout merge: `0bc9df91ba4582e629ada6c5a77e7d62103d0804`. Fixed before reference: `0c42dcc9c1e481909e3283d2f80e2891ec4c2b89`. [Manifest](manifest.json) preserves artifact paths and file hashes. Reports are copied without modification.

## Fair TCP comparison

Five rotating trials per backend: one server thread, four persistent connections, one outstanding request per connection, 64-byte messages, 10,000 measured plus 1,000 warmup requests per client. Asio 1.34.2 and libuv 1.52.1. Each report contains its environment and all 15 raw runs. Compare within rows only: runner hardware differs, including between Linux jobs.

| Platform | PACE req/s | Asio req/s | libuv req/s |
|---|---:|---:|---:|
| [Linux / C++20](ubuntu-latest-cxx20.json) | 101,093 | 103,577 | 99,196 |
| [Linux / C++23](ubuntu-latest-cxx23.json) | 61,553 | 64,620 | 56,866 |
| [macOS arm64 / C++20](macos-latest-cxx20.json) | 78,807 | 57,981 | 89,020 |
| [Windows x64 / C++20](windows-latest-cxx20.json) | 52,733 | 55,843 | 57,554 |
| [FreeBSD 14.3 VM / C++20](freebsd.json) | 355,542 | 344,665 | 408,762 |

PACE does not lead both libraries on every platform. These closed-loop echo numbers do not measure HTTP throughput, peak capacity or multicore scaling.

## Linux before/after pairs

The separate paired job builds both revisions with GCC C++20, `-O3 -DNDEBUG -pthread` and no profiling. TCP uses 100,000 measured requests plus 1,000 warmup requests per client; component cases use 1,000,000 iterations plus 10,000 warmup, depth 4 and batch 256. Eight pairs alternate execution order. Source revision text files and executable hashes are retained.

| Operation | Before median | After median | Paired after/before | Exploratory 95% interval | After wins |
|---|---:|---:|---:|---:|---:|
| [tcp](paired-tcp.json), req/s | 60,655.29 | 62,689.67 | 1.0347 | 1.0288–1.0396 | 8/8 |
| [lazy_task](paired-lazy_task.json), ns/op | 16.92 | 16.20 | 0.9523 | 0.9473–0.9642 | 7/8 |
| [await_chain](paired-await_chain.json), ns/op | 95.71 | 100.02 | 1.0454 | 1.0392–1.0533 | 0/8 |
| [empty_poll](paired-empty_poll.json), ns/op | 307.78 | 258.57 | 0.8405 | 0.8396–0.8524 | 8/8 |
| [yield](paired-yield.json), ns/op | 426.16 | 35.78 | 0.0838 | 0.0832–0.0849 | 8/8 |
| [post](paired-post.json), ns/op | 435.75 | 43.10 | 0.0991 | 0.0985–0.0996 | 8/8 |
| [cross_thread_post](paired-cross_thread_post.json), ns/op | 651.38 | 33.81 | 0.0520 | 0.0475–0.0642 | 8/8 |
| [spawn_completion](paired-spawn_completion.json), ns/op | 76.30 | 72.93 | 0.9562 | 0.9439–0.9672 | 8/8 |
| [zero_timer](paired-zero_timer.json), ns/op | 455.53 | 68.08 | 0.1495 | 0.1492–0.1502 | 8/8 |

Higher favors after for TCP requests/s; lower favors after for ns/op. Each component has its own [operation boundary](../../components.md). In particular, cross-thread post measures producer enqueue cost with a live consumer, excluding batch-drain waits; it is not callback completion latency.

**Negative result:** the four-frame synchronous await chain was about 4.5% slower in this CI sample, despite unchanged task headers. The [matched local study](../component-2026-10-20/README.md) found no meaningful change for that operation. This experiment does not isolate a cause; do not hide the regression or extrapolate it to every machine.

The TCP paired ratio was 1.0347 (8/8 wins). The exploratory bootstrap interval resamples whole pairs and assumes representative independent samples; scheduling/frequency drift can violate that assumption. [Local TCP pairs](../tcp-profile-2026-10-20/README.md) had a wider interval containing 1.0. No hard performance threshold decides CI success; malformed/incomplete measurements and correctness failures do.

## Stage diagnostics

Three instrumented PACE trials per platform with 10,000 measured and 1,000 warmup requests per client. Probes cover server lifetime including startup/warmup/shutdown. Values are wall time, not CPU samples or per-request latency, and the instrumentation adds overhead. **Do not compare diagnostic throughput with uninstrumented Asio/libuv.** Blocking poll includes idle waits.

| Platform | Raw profile | Stage table | Uninstrumented components |
|---|---|---|---|
| Linux / C++20 | [JSON](profile-ubuntu-latest-cxx20.json) | [Stages](profile-ubuntu-latest-cxx20-stages.md) | [JSON](components-ubuntu-latest-cxx20.json) |
| Linux / C++23 | [JSON](profile-ubuntu-latest-cxx23.json) | [Stages](profile-ubuntu-latest-cxx23-stages.md) | [JSON](components-ubuntu-latest-cxx23.json) |
| macOS arm64 / C++20 | [JSON](profile-macos-latest-cxx20.json) | [Stages](profile-macos-latest-cxx20-stages.md) | [JSON](components-macos-latest-cxx20.json) |
| Windows x64 / C++20 | [JSON](profile-windows-latest-cxx20.json) | [Stages](profile-windows-latest-cxx20-stages.md) | [JSON](components-windows-latest-cxx20.json) |
| FreeBSD 14.3 VM / C++20 | [JSON](profile-freebsd.json) | [Stages](profile-freebsd-stages.md) | [JSON](components-freebsd.json) |

Linux and Windows diagnostics spend most observed time in send; macOS has substantial blocking wait and recv time; FreeBSD distributes more time across send/recv, coroutine execution and poller updates. These differences motivate workload/platform-specific follow-up. They do not justify treating wait time as CPU overhead or attributing all observed time to PACE itself.

Every combined report retains raw trials, exact commands and machine metadata. Component summaries are process batch averages, not operation-latency percentiles. The older [CI snapshot](../ci-2026-10-20/README.md) is preserved as historical evidence and should not be compared across different shared machines as a controlled before/after test.
