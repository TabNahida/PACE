<!-- source-hash: 44a4eba17aed -->
# 参考

[English](README.md) · [文档](../README.zh.md)

这里收录 `include/pace/` 下的每一个公开头文件，每个模块一节。各模块页面的顺序相同：概述、概要、类型、函数、
错误、线程与并发、生命周期与所有权、限制与默认值。`src/` 下的头文件属于内部实现，不在此记录。

| 模块 | 伞头 | Target | 页面 |
|:--|:--|:--|:--|
| 运行时 | `<pace/runtime.hpp>` | `pace_runtime` | [概览](runtime/README.zh.md) · [task](runtime/task.zh.md) · [io_context](runtime/io-context.zh.md) · [runtime](runtime/runtime.zh.md) · [run](runtime/run.zh.md) · [结构化并发](runtime/structured-concurrency.zh.md) · [I/O 后端](runtime/io-backends.zh.md) |
| 网络 | `<pace/net.hpp>` | `pace_net` | [概览](net/README.zh.md) · [TCP](net/tcp.zh.md) · [UDP](net/udp.zh.md) |
| TLS | `<pace/tls.hpp>` | `pace_tls` | [TLS](tls/README.zh.md) |
| DNS | `<pace/dns.hpp>` | `pace_dns` | [DNS](dns/README.zh.md) |
| QUIC | `<pace/quic.hpp>` | `pace_quic` | [QUIC](quic/README.zh.md) |
| HTTP | `<pace/http.hpp>` | `pace_http` | [概览](http/README.zh.md) · [路由](http/router.zh.md) · [URL 客户端](http/clients.zh.md) |
| HTTP/1.1 | `<pace/http1.hpp>` | `pace_http1` | [HTTP/1.1](http1/README.zh.md) |
| HTTP/2 | `<pace/http2.hpp>` | `pace_http2` | [HTTP/2](http2/README.zh.md) |
| HTTP/3 | `<pace/http3.hpp>` | `pace_http3` | [HTTP/3](http3/README.zh.md) |
| WebSocket | `<pace/websocket.hpp>` | `pace_websocket` | [WebSocket](websocket/README.zh.md) |
| 网关 | `<pace/gateway.hpp>` | 在 `pace_http1` 中 | [网关](gateway/README.zh.md) |

`<pace/pace.hpp>` 包含所有已启用模块的伞头。

| 跨模块 | |
|:--|:--|
| [错误](errors.zh.md) | 各处通用的异常类型和错误码 |
| [构建选项](build-options.zh.md) | xmake 选项、target、具名模块、诊断 |
| [平台支持](platform-support.zh.md) | 操作系统、编译器、I/O 后端、原生句柄 |

## 所有模块共同遵守的规则

除非页面另有说明，以下规则适用于所有 API；[运行时概览](runtime/README.zh.md)给出精确表述，
[设计说明](../explanation/README.zh.md)解释其原因。

- **所有权。** 任务是惰性的，只能被消费一次。按值传入的参数归任务所有；引用、span 和 string view 只是借用，
  被引用的对象必须比任务活得久。套接字在其操作完成之前必须保持地址不变，上下文必须比它的套接字活得久。
  [详见](../explanation/ownership.zh.md)
- **线程。** 每个上下文由一个线程运行。只有 `spawn`、`post` 和 `stop` 可以从其他线程调用。
  [详见](../explanation/execution-model.zh.md)
- **单个对象上的并发。** TCP 同一时间允许一个读和一个写；TLS、WebSocket 和 HTTP 客户端同一时间只允许一个操作。
  每个页面都写明了各自的限制。
- **错误。** 使用异常，经 `co_await` 重新抛出。[详见](errors.zh.md)
- **取消。** `stop()` 和取消令牌以 `std::errc::operation_canceled` 恢复挂起的操作；截止时间则以
  `std::errc::timed_out` 恢复。
