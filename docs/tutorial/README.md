# Tutorials

Follow these chapters in order. Every source link points to a complete C++20
program; default runs finish by themselves and check their expected result.
[中文学习指南](README.zh-CN.md).

| Chapter | Runnable source | Default command |
|---|---|---|
| [1. Build and run](01-build.md) | [Tasks example](../../examples/tutorial_tasks.cpp) | `xmake run tutorial_tasks` |
| [2. Tasks and timers](02-tasks-timers.md) | [Tasks example](../../examples/tutorial_tasks.cpp) | `xmake run tutorial_tasks` |
| [3. TCP echo](03-tcp.md) | [TCP example](../../examples/tutorial_tcp.cpp) | `xmake run tutorial_tcp` |
| [4. HTTP services](04-http.md) | [HTTP example](../../examples/tutorial_http.cpp) | `xmake run tutorial_http` |
| [5. Execution and workers](05-execution.md) | [Workers example](../../examples/tutorial_workers.cpp) | `xmake run tutorial_workers` |
| [6. Orderly shutdown](06-shutdown.md) | [Shutdown example](../../examples/tutorial_shutdown.cpp) | `xmake run tutorial_shutdown` |

TCP and HTTP examples accept `serve` for persistent interactive servers. Stop them
with Ctrl-C. The worker example accepts `background` to compare execution layout,
not performance. No example makes a speed claim; use the dedicated benchmark
tools and methodology for measurements.

Code fragments in chapters explain parts of these programs or explicitly label
application integration patterns. They are not alternate APIs. Use the complete
source for include lists and error handling, and the [reference](../reference/README.md)
for every signature and precondition.
