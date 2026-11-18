# TCP long connections with separate client and server processes

With eight persistent connections, eight PACE workers completed 218,270 requests/s versus 233,487 for four workers (−6.5%). With 32 connections, eight workers completed 1,167,192 requests/s versus 1,129,958 for four (+3.3%). Each connection stayed on the same worker. Each client waited for its previous response before sending again.

| Connections | Workers | Requests/s before → after | RTT p50 after (µs) | RTT p99 after (µs) | Events / epoll wait | Server voluntary switches / exchange |
|---:|---:|---:|---:|---:|---:|---:|
| 8 | 4 | 235,633 → 233,487 | 32.89 | 53.42 | 1.78 | 0.50 |
| 8 | 8 | 217,769 → 218,270 | 34.90 | 53.71 | 1.00 | 1.00 |
| 32 | 4 | 1,130,107 → 1,129,958 | 24.64 | 80.63 | 3.29 | 0.02 |
| 32 | 8 | 1,166,723 → 1,167,192 | 20.97 | 76.96 | 1.74 | 0.13 |

At eight workers and eight connections, every successful epoll wait in a separate diagnostic run returned one event. At four workers, 705,000 waits returned more than one. The four-worker server used 5.96 µs of kernel CPU per exchange versus 8.50 µs for eight workers. Together these measurements point to the cost of waking a worker for each tiny round trip. More connections allow workers to handle several ready sockets in a turn. The crossover depends on placement, load and hardware.

## CPU profiles and batching

At eight connections, the share of server user-space samples at the libc epoll_wait boundary rose from 12.0% to 16.9% when moving from four to eight workers. At eight workers, the reactor run loop accounted for 16.4%, the TCP read coroutine for 10.7%, and libc free for 8.2%. The client received 28.9% of its samples in libc recv. These are on-CPU samples. Linux WSL2 exposed software counters and user-space sampling. Hardware cycles, instructions and kernel stacks were unavailable. Kernel CPU and voluntary switches come from per-process resource usage. [Both peers' perf samples](perf-samples.csv) and [perf stat and epoll counts](diagnostics.csv).

The release caches the poller's fixed interest mode and skips a clock read on turns without timers. Throughput changes across the four depth-one workloads ranged from −0.9% to +0.2%. This run gives no evidence of a general throughput gain from those two changes.

For applications that can group independent messages, the same eight clients also sent eight tagged 64-byte requests in a single 512-byte write and read the whole echo before continuing. Throughput rose to 1,847,859 messages/s on four workers and 1,735,962 on eight (7.9× and 8.0× the corresponding depth-one medians). Batch-completion p99 was 53.95 and 53.69 µs respectively. This changes application write size and the number of requests in flight. The echo server and TCP defaults are identical for both depths.

## Method and reproduction

GCC 13.3, C++20, -O3 -g -fno-omit-frame-pointer -DNDEBUG, Linux 6.18.40.1 WSL2, epoll, Intel Core Ultra 7 270K Plus, 24 logical CPUs. Each fresh server process owned one listener per worker on IPv4 loopback. Worker i used CPU i. A separate client process pinned client i to CPU 8 + (i mod 8). The 32-connection run therefore multiplexed four client threads per client CPU. The experiment isolates the two processes' CPU sets on one host. A physical network or a remote client can introduce different bottlenecks.

Both binaries used the same scaling benchmark. The baseline used the runtime from commit 31401fa and the candidate used bd9a861. Each trial warmed up 1,000 requests per connection and timed 480,000 validated requests in total. Each cell reports the median of nine fresh runs with shuffled workload order and alternating revisions. Timings exclude connection setup, warm-up and percentile sorting. CPU and switch counts cover process lifetime and are normalized by warm-up plus measured exchanges. The diagnostic runs processed 1,600,000 measured requests, and ran separately from the timing trials. [Per-trial counts, checksums, percentiles and CPU usage](trials.csv).

Build two copies of pace_scaling using the same benchmark source. Run the commands below from this directory, substituting the two binary paths. The server prints its ephemeral listener ports on stdout and stops when stdin receives a line or EOF. The client accepts the port list via --ports.

```sh
python3 compare.py /path/to/baseline/pace_scaling /path/to/release/pace_scaling measurements
c++ -O2 -fPIC -shared epoll_counts.cpp -ldl -o epoll_counts.so
python3 compare.py /path/to/baseline/pace_scaling /path/to/release/pace_scaling measurements --profile --counts ./epoll_counts.so
```
