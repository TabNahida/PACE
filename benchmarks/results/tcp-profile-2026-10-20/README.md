# Local TCP and stage study — 20 October 2026

The scheduler improvements are large in task-only microbenchmarks. TCP echo has
a different cost distribution, so this study measures it separately. Both
variants use the committed echo harness from `62c8724b177a9ca03ed0b607bc5a74a852c26753`.
The before core has the same optional probes but predates the inbox/poll changes;
[the source patch](before-to-after.patch) and [manifest](manifest.json) identify
both builds. Historical benchmark snapshots remain unchanged.

Machine: Linux/WSL2 x86_64, Intel Core Ultra 7 270K Plus, GCC 13.3.0,
C++20, `-O3 -DNDEBUG -pthread`. CPU affinity and frequency were not controlled.

## Uninstrumented TCP comparison

Eight alternating before/after pairs, one server thread, four client threads,
four persistent connections, one outstanding request per connection, 64-byte
payloads. Each client performs 1,000 warmup and 100,000 measured requests. All
payloads and reported arithmetic were validated.

| Metric | Before | After |
|---|---:|---:|
| Median requests/s | 153,840 | 159,048 |
| Min–max requests/s | 146,142–163,018 | 151,352–190,660 |

The median paired after/before ratio was **1.0492**, with 6/8 after wins. The
exploratory bootstrap 95% interval was **0.9786–1.1440**, which includes no change.
This local sample does **not establish a reliable TCP speedup**. The ratio is the
median of paired ratios, not the ratio of the two throughput medians. All raw
records, execution order and settings are in [tcp-pairs.json](tcp-pairs.json).

See the separately controlled [CI before/after comparison](../ci-runtime-2026-10-20/README.md)
for results on a shared Linux runner. Neither experiment establishes superiority
over Asio/libuv across platforms or predicts HTTP/multicore throughput.

## Instrumented stage diagnostics

Each variant ran three diagnostics with the same workload except for 20,000
measured requests per client. Stage counters cover the **whole server event-loop
lifetime**, including setup, warmup and shutdown. They measure wall-clock time,
including descheduling and kernel waits. The timing probes add overhead; these
runs are not evidence of fair throughput performance.

| Stage | Before average ns/call¹ | After average ns/call¹ | After exclusive lifetime share¹ |
|---|---:|---:|---:|
| Inbox handling | 83.9 | 16.7 | 0.17% |
| Native send | 2,723.3 | 2,779.3 | 45.68% |
| Native recv | 278.0 | 283.1 | 9.31% |
| Native poller update | 176.5 | 176.2 | 2.90% |
| Blocking poll | 4,359.2 | 3,500.5 | 34.70% |

¹ Median of each trial's average/share. These are not latency percentiles.
Inclusive stages overlap and cannot be added. Blocking poll includes idle
waiting and is not pure CPU overhead.

Inbox processing became cheaper, but its original share was only 0.74% of the
instrumented lifetime. Most observed time was in send/recv and waiting. This
explains why faster task-only scheduling need not produce a similar percentage
increase in network throughput. The measurements do not justify changing socket
one-shot semantics or claiming that kernel waiting itself should be removed.

- Before: [raw report](profile-before.json), [stage table](profile-before-stages.md).
- After: [raw report](profile-after.json), [stage table](profile-after-stages.md).
- [Profiling boundaries and commands](../../profiling.md).
- [Component operations and matched-build evidence](../component-2026-10-20/README.md).

Numeric/raw records are preserved. One unrelated untracked local path was
omitted from Git-status metadata; the manifest retains original and archived
report hashes. Reproduce using the commands in each report and the source/flag
manifest. Run the compared processes sequentially without other build or test
loads, and repeat on deployment hardware before making capacity decisions.
