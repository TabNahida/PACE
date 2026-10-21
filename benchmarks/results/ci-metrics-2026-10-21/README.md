# Four-platform TCP, runtime and memory evidence — 21 October 2026

[Actions run 39608371946](https://github.com/TabNahida/PACE/actions/runs/39608371946) passed all seven jobs: Linux C++20/C++23, macOS, Windows, FreeBSD, ASan/UBSan, and runtime before/after. Source head `3eb014c` was tested as checkout merge `f84f15ba5eb43835a1da298f18bc4d50bc28cb6f`.

The native and FreeBSD jobs built and tested both dependency-free and optional TLS/HTTP2 configurations, including 17 protocol test targets and independent Python WebSocket peers. [Workflow results](validation/workflow.json), [GitHub artifact IDs and digests](validation/artifacts.json), and [SHA256SUMS](SHA256SUMS) preserve provenance. This archive retains all six downloaded artifacts unchanged; the five platform artifact roots are flattened because their names are unique. The runtime artifact retains its directory layout.

## Full TCP comparison

All backends use one server thread, four persistent clients, 64-byte messages, pipeline depth one, 10,000 measured and 1,000 warmup requests per client. Five rotated trials/backend produce 75 raw trials and 3,000,000 verified measured round trips. Asio is 1.34.2 and libuv is 1.52.1. These are uninstrumented release builds; separate profile results are not used for the backend comparison.

Every cell is **median [min–max] across five trials**. Percentiles are medians of trial percentiles, not pooled values. Ranges are observed spread, not confidence intervals. Memory is whole-process peak MiB including the server, identical clients, stacks and latency samples; it is not isolated library footprint. Compare within one platform/job only. Linux language-standard jobs run on different machines, so their rows are not a C++20/C++23 performance experiment.

| Platform | Backend | Requests/s | p50 µs | p95 µs | p99 µs | Peak MiB |
|---|---|---:|---:|---:|---:|---:|
| [Linux C++20](ubuntu-latest-cxx20.json) | PACE | 68,827 [67,952–69,851] | 58.837 [58.267–59.017] | 68.061 [65.678–69.153] | 75.011 [74.671–75.872] | 4.906 [4.848–4.969] |
| [Linux C++20](ubuntu-latest-cxx20.json) | Asio | 70,212 [69,719–70,934] | 56.895 [56.694–57.214] | 66.869 [65.728–67.250] | 72.828 [72.036–72.938] | 4.562 [4.516–4.602] |
| [Linux C++20](ubuntu-latest-cxx20.json) | libuv | 67,935 [64,497–69,158] | 42.103 [41.892–42.864] | 85.917 [84.615–88.927] | 99.643 [99.257–100.299] | 4.625 [4.602–4.656] |
| [Linux C++23](ubuntu-latest-cxx23.json) | PACE | 112,249 [101,295–114,728] | 35.214 [34.300–38.573] | 40.624 [39.374–47.688] | 46.396 [45.808–63.207] | 5.039 [4.961–5.043] |
| [Linux C++23](ubuntu-latest-cxx23.json) | Asio | 116,049 [104,279–120,067] | 33.913 [33.391–37.686] | 39.115 [38.114–46.305] | 44.666 [43.947–56.875] | 4.711 [4.699–4.730] |
| [Linux C++23](ubuntu-latest-cxx23.json) | libuv | 109,598 [109,300–115,003] | 29.921 [28.684–36.035] | 49.424 [49.011–51.099] | 55.067 [54.340–56.782] | 4.777 [4.746–4.785] |
| [macOS arm64](macos-latest-cxx20.json) | PACE | 47,384 [41,070–57,630] | 80.375 [51.000–81.000] | 138.958 [119.375–188.750] | 253.959 [192.167–451.458] | 6.312 [6.312–6.328] |
| [macOS arm64](macos-latest-cxx20.json) | Asio | 40,271 [37,350–48,839] | 81.083 [73.334–84.792] | 168.125 [150.250–206.417] | 424.125 [200.875–612.833] | 6.297 [6.281–6.297] |
| [macOS arm64](macos-latest-cxx20.json) | libuv | 57,233 [47,353–82,944] | 63.792 [39.833–77.625] | 113.125 [93.791–133.875] | 175.500 [133.500–236.750] | 6.281 [6.266–6.281] |
| [Windows x64](windows-latest-cxx20.json) | PACE | 36,169 [35,414–37,360] | 104.200 [102.300–110.500] | 174.100 [170.000–178.900] | 244.300 [194.600–266.900] | 6.750 [6.742–6.758] |
| [Windows x64](windows-latest-cxx20.json) | Asio | 37,280 [36,513–37,754] | 104.900 [103.500–107.100] | 133.600 [124.900–138.800] | 171.200 [151.000–176.000] | 6.738 [6.734–6.738] |
| [Windows x64](windows-latest-cxx20.json) | libuv | 37,960 [37,650–39,163] | 103.200 [102.200–103.500] | 161.600 [135.500–175.900] | 242.300 [212.300–249.000] | 7.785 [7.777–7.805] |
| [FreeBSD 14.3 VM](freebsd.json) | PACE | 359,152 [353,157–371,582] | 9.959 [9.880–10.050] | 12.094 [11.493–14.138] | 33.938 [31.382–38.396] | unavailable |
| [FreeBSD 14.3 VM](freebsd.json) | Asio | 347,325 [337,522–350,514] | 10.521 [10.391–10.671] | 13.206 [12.666–16.182] | 35.129 [32.905–36.382] | unavailable |
| [FreeBSD 14.3 VM](freebsd.json) | libuv | 410,070 [408,244–420,480] | 8.938 [8.868–9.008] | 10.631 [10.371–10.822] | 31.392 [29.669–33.105] | unavailable |

## What the selected README panels omit

- **macOS throughput vs Asio:** PACE is 17.7% higher in the median, but ranges overlap and libuv is faster than both. This does not establish a general or statistically reliable throughput lead.
- **Windows process memory vs libuv:** PACE is 13.3% lower in the median, but Asio uses slightly less than PACE. PACE has the lowest throughput of the three and a slightly higher p99 than libuv in the same workload.
- PACE has lower p95/p99 than libuv in both Linux jobs, but Asio has better median throughput and tail latency than PACE. In FreeBSD, PACE beats Asio in these medians while libuv leads both.
- Peak process memory is highest for PACE in both Linux jobs and on macOS. The Windows comparison cannot be generalized to other systems.
- The README's local Linux p95 panel comes from a [separate longer 32-client experiment](../local-metrics-confirmation-2026-10-21/README.md), not this four-client CI workload.

Every job uses a shared runner or VM. CPU frequency, affinity, scheduler contention and background work are uncontrolled. These short closed-loop localhost tests do not establish overload behavior, production latency, multicore scaling, HTTP/TLS speed or server capacity; no coordinated-omission correction is applied.

## Memory provider validation

Linux reads `/proc/self/status.VmHWM`, macOS reads `task_info(MACH_TASK_BASIC_INFO).resident_size_max`, and Windows reads `GetProcessMemoryInfo().PeakWorkingSetSize`. All passed independent probes: retain a touched-and-freed 32 MiB peak, and reject pollution from a Python launcher holding an extra 128 MiB. Probe runs are separate from timing. Linux RSS accounting is approximate; OS residency semantics differ.

FreeBSD records `available: false`, `bytes: null`, and a reason. Its inherited process-history peak is not a verified current-image measurement, so no numeric memory aggregate is computed. This is unavailable data, not zero memory.

| Platform | Preserved probe | Result |
|---|---|---|
| Linux | [Raw probe and job provenance](validation/linux-memory-probe.json) | passed |
| macOS | [Raw probe and job provenance](validation/macos-memory-probe.json) | passed |
| Windows | [Raw probe and job provenance](validation/windows-memory-probe.json) | passed |
| FreeBSD | [Raw probe and job provenance](validation/freebsd-memory-probe.json) | explicitly unavailable |

## Components, workers and stage timings

These are separate PACE-only experiments. Component costs are batch-average ns/op, not per-operation latency percentiles. Worker scaling uses its own workload and thread placement; it cannot be divided by the single-reactor backend table to claim a speedup over other libraries. Stage profiling includes startup/warmup/shutdown and adds overhead; blocking poll includes idle time, not just CPU cost.

| Platform | Component costs | Worker modes/counts | Instrumented stages |
|---|---|---|---|
| Linux C++20 | [Table](components-ubuntu-latest-cxx20.md) | [Table](scaling-ubuntu-latest-cxx20.md) | [Table](profile-ubuntu-latest-cxx20-stages.md) |
| Linux C++23 | [Table](components-ubuntu-latest-cxx23.md) | [Table](scaling-ubuntu-latest-cxx23.md) | [Table](profile-ubuntu-latest-cxx23-stages.md) |
| macOS arm64 | [Table](components-macos-latest-cxx20.md) | [Table](scaling-macos-latest-cxx20.md) | [Table](profile-macos-latest-cxx20-stages.md) |
| Windows x64 | [Table](components-windows-latest-cxx20.md) | [Table](scaling-windows-latest-cxx20.md) | [Table](profile-windows-latest-cxx20-stages.md) |
| FreeBSD 14.3 VM | [Table](components-freebsd.md) | [Table](scaling-freebsd.md) | [Table](profile-freebsd-stages.md) |

## Current paired runtime check

The fixed pre-optimization baseline is `0c42dcc9c1e481909e3283d2f80e2891ec4c2b89`. Eight alternating before/after pairs use matching compiler flags. Ratios below are medians of paired after/before ratios; the interval is the archived exploratory bootstrap interval, not a multi-workload release guarantee. [Exact source revisions](runtime-before-after-linux/build/paired/candidate-revision.txt) and all commands/binary hashes are preserved.

| Operation | After/before ratio | Exploratory 95% interval | After wins / pairs | Direction |
|---|---:|---|---:|---|
| [await_chain](runtime-before-after-linux/benchmark-results/paired-await_chain.json) | 0.9983 | 0.9863–1.0031 | 5 / 8 | lower better |
| [cross_thread_post](runtime-before-after-linux/benchmark-results/paired-cross_thread_post.json) | 0.0559 | 0.0455–0.0673 | 8 / 8 | lower better |
| [empty_poll](runtime-before-after-linux/benchmark-results/paired-empty_poll.json) | 0.8409 | 0.8302–0.8471 | 8 / 8 | lower better |
| [lazy_task](runtime-before-after-linux/benchmark-results/paired-lazy_task.json) | 0.9968 | 0.9875–1.0037 | 6 / 8 | lower better |
| [post](runtime-before-after-linux/benchmark-results/paired-post.json) | 0.0963 | 0.0960–0.0972 | 8 / 8 | lower better |
| [spawn_completion](runtime-before-after-linux/benchmark-results/paired-spawn_completion.json) | 0.9738 | 0.9631–0.9787 | 8 / 8 | lower better |
| [tcp](runtime-before-after-linux/benchmark-results/paired-tcp.json) | 1.0103 | 0.9961–1.0177 | 6 / 8 | higher better |
| [yield](runtime-before-after-linux/benchmark-results/paired-yield.json) | 0.0814 | 0.0798–0.0821 | 8 / 8 | lower better |
| [zero_timer](runtime-before-after-linux/benchmark-results/paired-zero_timer.json) | 0.1441 | 0.1427–0.1455 | 8 / 8 | lower better |

The TCP ratio is 1.0103, with interval 0.9961–1.0177 and six wins in eight pairs: the interval includes no change. Component improvements should therefore not be advertised as a proven end-to-end TCP speedup in this run. The await-chain ratio is also consistent with no change. Earlier controlled studies remain historical evidence rather than being silently replaced.

## Reproduce and inspect

Use [the workflow](../../../.github/workflows/ci.yml) for exact commands, [benchmark methodology](../../README.md) for timing boundaries, and [performance documentation](../../../docs/performance.md) for chart reproduction. Combined JSON files contain all raw records, summaries, executable hashes, source/host/compiler details and commands. Each `*-evidence/` directory also retains the original JSONL, environment, summary JSON and Markdown. No trial was removed. New runs must use new destinations.

中文：七个 CI 作业全部通过。这里保留五种平台配置的三个后端、每项指标及范围，并归档组件、线程扩展性与分阶段诊断。README 选摘的是不同平台下的局部优势；不能拼成综合排名。FreeBSD 峰值内存明确不可用，吞吐与延迟照常报告。Windows 的内存优势也伴随吞吐劣势，完整表格一并展示。
