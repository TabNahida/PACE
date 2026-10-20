# CI comparison: 20 October 2026

Source: [Actions run 39378454679](https://github.com/TabNahida/PACE/actions/runs/39378454679), PR head `6404b61`. Native reports record tested merge revision `7c260d6d1238c0a7c7f644487fa45209482aee66`. The FreeBSD report has null Git metadata because of VM checkout ownership; its source attribution comes from the linked workflow checkout log. Subsequent runs use an explicit read-only Git directory trust setting to capture this metadata in the VM.

All five jobs use release builds, one server thread, four persistent connections, a 64-byte payload, one outstanding request per connection, 10,000 measured and 1,000 warmup requests per connection. Backends run in rotating order for five trials each. All 75 trials validate complete payloads and request counts.

The JSON files below are unmodified combined reports downloaded from Actions artifacts. They retain every raw run, compiler/library/CPU metadata, command line, binary hash and Git revision. Repository copies preserve the evidence after CI artifacts expire.

These are shared-runner smoke comparisons, without controlled CPU placement or frequency. Compare backends only within a row/platform. Do not infer HTTP speed, multiworker scaling, peak capacity, or a general win from this workload.

## Linux / C++20

[Job log](https://github.com/TabNahida/PACE/actions/runs/39378454679/job/118481785085) · [Raw report](ubuntu-latest-cxx20.json)

OS: `Linux-6.17.0-1022-azure-x86_64-with-glibc2.39`. CPU: AMD EPYC 7763 64-Core Processor. Compiler: GCC 13.3.0. Asio: 1.34.2; libuv: 1.52.1.

| Backend | Median requests/s | Min–max requests/s | Median p50 µs | Median p99 µs |
|---|---:|---:|---:|---:|
| pace | 61,555 | 59,897–62,590 | 64.67 | 89.32 |
| asio | 64,126 | 62,801–64,418 | 60.57 | 87.28 |
| libuv | 57,505 | 56,183–57,785 | 60.96 | 113.63 |

Measured trial durations: 0.621–0.712 seconds. Latency columns are medians of per-run percentiles, not pooled percentiles.

## Linux / C++23

[Job log](https://github.com/TabNahida/PACE/actions/runs/39378454679/job/118481785000) · [Raw report](ubuntu-latest-cxx23.json)

OS: `Linux-6.17.0-1022-azure-x86_64-with-glibc2.39`. CPU: AMD EPYC 7763 64-Core Processor. Compiler: GCC 13.3.0. Asio: 1.34.2; libuv: 1.52.1.

| Backend | Median requests/s | Min–max requests/s | Median p50 µs | Median p99 µs |
|---|---:|---:|---:|---:|
| pace | 59,916 | 59,673–60,486 | 66.78 | 89.84 |
| asio | 64,411 | 61,273–64,800 | 59.82 | 85.24 |
| libuv | 57,497 | 55,972–62,030 | 52.99 | 116.71 |

Measured trial durations: 0.617–0.715 seconds. Latency columns are medians of per-run percentiles, not pooled percentiles.

## macOS arm64 / C++20

[Job log](https://github.com/TabNahida/PACE/actions/runs/39378454679/job/118481785274) · [Raw report](macos-latest-cxx20.json)

OS: `macOS-26.6.2-arm64-arm-64bit`. CPU: Apple M1 (Virtual). Compiler: Clang 21.0.0. Asio: 1.34.2; libuv: 1.52.1.

| Backend | Median requests/s | Min–max requests/s | Median p50 µs | Median p99 µs |
|---|---:|---:|---:|---:|
| pace | 49,940 | 40,608–53,282 | 78.62 | 183.04 |
| asio | 42,655 | 31,481–48,508 | 84.29 | 262.83 |
| libuv | 55,623 | 51,559–71,217 | 69.42 | 170.08 |

Measured trial durations: 0.562–1.271 seconds. Latency columns are medians of per-run percentiles, not pooled percentiles.

## Windows x64 / C++20

[Job log](https://github.com/TabNahida/PACE/actions/runs/39378454679/job/118481785051) · [Raw report](windows-latest-cxx20.json)

OS: `Windows-2025Server-10.0.26100-SP0`. CPU: AMD64 Family 25 Model 1 Stepping 1, AuthenticAMD. Compiler: MSVC 1951. Asio: 1.34.2; libuv: 1.52.1.

| Backend | Median requests/s | Min–max requests/s | Median p50 µs | Median p99 µs |
|---|---:|---:|---:|---:|
| pace | 37,531 | 37,226–37,734 | 102.70 | 197.60 |
| asio | 38,367 | 36,772–39,079 | 102.00 | 159.90 |
| libuv | 39,245 | 38,921–39,482 | 101.00 | 210.00 |

Measured trial durations: 1.013–1.088 seconds. Latency columns are medians of per-run percentiles, not pooled percentiles.

## FreeBSD 14.3 VM / C++20

[Job log](https://github.com/TabNahida/PACE/actions/runs/39378454679/job/118481784899) · [Raw report](freebsd.json)

OS: `FreeBSD-14.3-RELEASE-amd64-64bit-ELF`. CPU model: not reported (architecture amd64). Compiler: Clang 19.1.7. Asio: 1.34.2; libuv: 1.52.1.

| Backend | Median requests/s | Min–max requests/s | Median p50 µs | Median p99 µs |
|---|---:|---:|---:|---:|
| pace | 216,926 | 208,875–219,225 | 16.59 | 43.63 |
| asio | 221,328 | 218,358–221,629 | 16.39 | 42.68 |
| libuv | 218,280 | 216,339–220,499 | 16.82 | 40.47 |

Measured trial durations: 0.180–0.192 seconds. Latency columns are medians of per-run percentiles, not pooled percentiles.
