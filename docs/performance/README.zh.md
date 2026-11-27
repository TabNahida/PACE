<!-- source-hash: 14fd8c07fdb2 -->
# 性能

[English](README.md) · [文档](../README.zh.md)

PACE 公布的每一个数字，连同得出它的方法和背后的原始数据。

## 报告

| 报告 | 测量内容 | 最新一次 |
|:--|:--|:--|
| [运行时与网络热路径](runtime.zh.md) | 任务、yield、post、定时器、取消和就绪通知的开销；路由；工作线程扩展 | PACE 0.6.1，macOS arm64 |
| [TCP 回显](tcp.zh.md) | PACE、Asio 和 libuv 处理同一回显负载 | PACE 0.6.1，macOS arm64；PACE 0.5.0，Linux x86_64 |
| [HTTP 网关](http-gateway.zh.md) | 静态文件、缓存和反向代理，与 nginx 对比 | Linux x86_64 |
| [协议](protocols.zh.md) | HTTP/1 响应完成、WebSocket 分帧、HTTP/2 与 HTTP/3 处理函数的并发 | Linux x86_64 |
| [历史](history/README.zh.md) | 早期的研究以及由此带来的优化 | |

## 1.0 基线

2026 年 11 月 27 日测量了 PACE 0.6.1，作为 1.0 工作的基准：每个重写协议或运行时的阶段都在同一台机器上与它对比。
包含所有测试套件和每次试验的完整记录见
[`benchmarks/results/baseline-0.6.1-macos-arm64/`](../../benchmarks/results/baseline-0.6.1-macos-arm64/README.md)。

| 测试套件 | 主要结果 | 报告 |
|:--|:--|:--|
| 调度器 | `yield` 5.0 ns，`post` 7.4 ns，任务 spawn 与完成 18.3 ns | [运行时](runtime.zh.md#调度器组件) |
| 取消 | 取消一个等待的 p50 为 250 ns，另有 65,536 个注册时不变 | [运行时](runtime.zh.md#反应器与取消延迟) |
| TCP 回显 | 227,410 req/s，p50 31.7 µs（Asio 190,933；libuv 248,687） | [TCP](tcp.zh.md) |
| WebSocket | 64 字节消息每秒 98,532 次往返，p50 9.2 µs | [记录](../../benchmarks/results/baseline-0.6.1-macos-arm64/README.md) |
| HTTP/2 与 HTTP/3 | 16 个请求，每个处理函数等待 10 ms：8 个处理函数并发时 24.6 ms（h2）和 25.6 ms（h3），1 个时 188 ms | [记录](../../benchmarks/results/baseline-0.6.1-macos-arm64/README.md) |

需要 Linux 的测试套件（QUIC 传输对比和 nginx 网关对比）在这台主机上跳过了。Linux 基线尚待测量。

## 数字是怎样得出的

- **同一台机器、同一次运行、同一种方法。** 与其他库的对比，只有在双方都于同一台机器、同一次会话中、以相同的
  客户端和设置测得时才会公布。
- **全部保留。** 每份报告都链接到原始数据：每次试验、环境、构建和确切的命令。一次完整会话中的任何试验都不会被
  剔除。
- **中位数及其离散范围。** 表格给出各次试验的中位数，以及观察到的最小值和最大值。延迟百分位数先按每次试验计算，
  再取这些值的中位数。
- **不用 CI 的数字。** 共享的 CI 运行器用来检查正确性和可移植性；它们的计时噪声太大，不能用于任何排名。
- **落后也照实报告。** 其他库更快的地方，报告会如实写明。
- **README 中的图是生成的。** `scripts/render_readme_figures.py` 从一个结果目录绘制这些图，并在每张图上标出那次
  运行的版本、日期和主机；图与数据不一致时 CI 会失败。

## 运行测量

```sh
xmake f -c -m release -o build/baseline --tls=y --dns=y --quic=y --http2=y --http3=y \
    --benchmarks=y --components=y --scaling=y --protocol_benchmarks=y --tests=n --examples=n -y
xmake -y
python3 scripts/run_baseline.py --source . --build-dir build/baseline \
    --output benchmarks/results/<name> --label "<host description>" --repeats 5
```

[Baseline 工作流](../../.github/workflows/baseline.yml)可以按需在 Linux x86_64、Linux arm64 和 macOS 运行器上
运行同样的测试套件，目的是确认它们仍能运行，而不是公布其计时。
[基准测试程序与运行脚本](../../benchmarks/README.md)

## 参见

- [执行模型](../explanation/execution-model.zh.md)
- [架构](../explanation/architecture.zh.md)
