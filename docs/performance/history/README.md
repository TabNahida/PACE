# Performance history

[简体中文](README.zh.md) · [Performance](../README.md)

Earlier measurements, kept as they were published. Their numbers describe the code and machines of their time;
the current reports are one level up. Each study links its raw data under `benchmarks/results/`.

| Date | Study | What came out of it |
|:--|:--|:--|
| 16–18 November 2026 | [Ownership and hot-path review](ownership-review-2026-11.md) | Which owned copies and reference counts on the request path were removed, which were kept on purpose, and why |
| 3 November 2026 | [Gateway comparison with nginx](gateway-matrix-2026-11-03.md) | The complete static, cache and proxy matrix before the 0.5.0 optimization, measured twice under different host load |
| 3 November 2026 | [Gateway optimization](gateway-optimization-2026-11-03.md) | Reactor-local cache snapshots and hashing changes in the gateway, measured against nginx |
| 22 October 2026 | [Local Linux performance](2026-10-22.md) | A long-run TCP comparison with Asio and libuv, and what was slowing PACE down |
| 20–21 October 2026 | [Historical studies](2026-10-20-21.md) | The first local and CI comparisons, the reactor optimization study, and worker placement |

Figures that are no longer used by a current page, such as the 0.5.0 project card, are kept in
[`assets/`](assets/project-card.svg).
