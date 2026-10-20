# Inbox queue reuse: no demonstrated TCP speedup

Reusing the reactor's incoming queues did not improve this local TCP echo
measurement. Across eight alternating before/after pairs, median throughput fell
from 154,696 to 149,090 requests/s (−3.62%). The median within-pair throughput ratio
was 0.9737 (−2.63%); the changed binary was faster in two of eight pairs.
These results do not support a speedup claim. The queue-reuse candidate was
reverted; it is not part of the library implementation.

| Metric | Before | Queue reuse |
|---|---:|---:|
| Median requests/s | 154,696 | 149,090 |
| Minimum–maximum requests/s | 145,684–159,555 | 146,302–158,799 |
| Median of per-run p50, µs | 24.633 | 25.669 |
| Median of per-run p99, µs | 45.1325 | 45.8495 |

The eight after/before throughput ratios were 0.9657, 0.9584, 0.9953, 1.0437,
0.9392, 1.0150, 0.9817 and 0.9618. Their range spans a 6.08% decrease to a 4.37%
increase. The ratio of aggregate medians and the median paired ratio differ
because they are different summaries; neither is selected to favor the change.

Each run used one PACE server thread, four client threads/connections, a 64-byte
payload, one outstanding request per connection, 1,000 warmup requests and
100,000 measured requests per connection. All 16 runs completed 400,000 measured
requests and verified every response. Measured runs lasted 2.51–2.75 seconds.
The before/after execution order alternated between pairs.

[Raw evidence and binary hashes](results/inbox-reuse-paired.json) preserve all
observations. The evidence file does not capture full compiler and host metadata,
so this is an exploratory local experiment, not a portable performance claim.
The sample count and run duration also fall below the proposed release-level
performance criteria in [the benchmark methodology](README.md#acceptance-criteria).
Fewer allocations alone do not establish higher application throughput, and this
experiment does not compare PACE against Asio or libuv.
