# Runtime and network measurements, 18 November 2026

The baseline is commit `039206c`. Both revisions used GCC 13.3, C++20, `-O3 -DNDEBUG -pthread`, and epoll on Linux 6.18.40.1 (WSL2), Intel Core Ultra 7 270K Plus. Each trial ran in a fresh process. The order alternated between baseline-first and candidate-first.

## Runtime and socket costs

Medians of nine trials, in nanoseconds per operation. The cancellation cost includes the requesting thread, reactor dispatch, exception handling and draining all 4,096 suspended roots. Socket timing covers construction and destruction of one IPv4 TCP stream and one UDP socket per iteration.

| Operation | Baseline | Updated | Change |
|---|---:|---:|---:|
| Local root spawn through completion | 43.91 | 16.28 | −62.9% |
| Cross-thread post, producer enqueue | 20.97 | 20.84 | −0.6% |
| Cross-thread batch cancellation, per sleeper | 8,322.16 | 961.11 | −88.5% |
| TCP + UDP socket pair lifecycle | 2,443.55 | 2,086.93 | −14.6% |
| Zero-duration coroutine sleep | 46.97 | 43.46 | −7.5% |

Root tasks use a reactor-owned vector. Each spawned void promise records its slot so reaping can move the last entry into the vacated slot. Sleeping coroutines record their deadline-heap slots, reducing individual cancellation from a scan and heap rebuild to logarithmic removal. Concurrent cancellation notifications share a pending wake. Ordinary socket and timer waits skip constructing a cancellation callback when the task has no cancellation source.

The task group and HTTP dispatch keep ownership in the coroutine that drains them. Their nested waiters borrow that state. A result handle can outlive its child task and a cross-thread cancellation notification can overlap unregistration, so those paths retain shared lifetime management. Linux TCP and UDP creation and TCP accept request nonblocking and close-on-exec flags in the system call.

## TCP echo

One reactor served eight independent blocking clients over IPv4 loopback. Each client warmed up with 1,500 exchanges, then verified 15,000 round trips of 64 bytes, with one request outstanding. Results are medians across twelve trials per revision and CPU affinity.

| CPUs available to the process | Requests/s, before → after | RTT p50, µs | RTT p99, µs |
|---|---:|---:|---:|
| 2, 4 | 275,155 → 277,671 | 26.79 → 26.58 | 68.11 → 67.64 |
| 2 through 10 | 276,594 → 286,518 | 25.53 → 25.14 | 66.24 → 62.55 |

The larger CPU set saw a 3.6% increase in median throughput, with individual paired changes from −13.1% to +10.0%. A shorter exploratory run on two CPUs gave a 7.4% lower median throughput. Loopback throughput varied between trials, and these results do not establish a general TCP throughput gain. The socket lifecycle measurement covers connection setup costs that the persistent echo connections mostly amortize.

## Reproduction

- Build [components_bench.cpp](../../components_bench.cpp) against each revision's runtime. Run `spawn_completion` with 1,200,000 iterations and 50,000 warm-ups, and `cross_thread_post` and `zero_timer` with 400,000 iterations and 30,000 warm-ups. Use batch size 512 for spawn and post. Pin single-thread cases to CPU 2 and cross-thread cases to CPUs 2 and 4.
- Build the [cancellation and socket probe](probe.cpp) against each revision's `pace_net` and `pace_runtime`. Run `cancel` on CPUs 2 and 4 and divide elapsed nanoseconds by 4,096. Run `socket` on CPU 2 and divide by 60,000.
- Run [echo_bench.cpp](../../echo_bench.cpp) with `--backend pace --clients 8 --bytes 64 --requests 15000 --warmup 1500` and the CPU sets in the table.

[Component and initial echo trials](components.csv) include the shorter 4,000-request/client echo workload. Its nanoseconds per operation are the inverse of aggregate throughput. [Longer echo trials](echo.csv) include throughput and both RTT percentiles.
