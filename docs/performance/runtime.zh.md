<!-- source-hash: b96f4edeb9c0 -->
# 运行时与网络热路径

[English](runtime.md) · [性能](README.zh.md)

调度器、反应器和套接字层每个操作的开销，在 PACE 0.6.1 上测得。这些是 1.0 的回归基准：后续每个阶段都在同一台
机器上与它们对比。

| | |
|:--|:--|
| 版本 | PACE 0.6.1（`v0.6.1`） |
| 主机 | Apple M6，12 个逻辑 CPU，16 GiB，macOS 27.2，接通电源，无其他负载 |
| 编译器 | Apple clang 21.0.0，release 构建 |
| 测量 | 2026 年 11 月 27 日，每项负载 5 次试验 |
| 原始数据 | [`benchmarks/results/baseline-0.6.1-macos-arm64/`](../../benchmarks/results/baseline-0.6.1-macos-arm64/README.md) |

所有数值都是各次试验的中位数，并附观察到的最小值和最大值。这里不与任何其他库比较：这些数字只有与同一台机器上的
同一负载相比才有意义。

## 调度器组件

`pace_components` 在单个线程上按批重复执行某一种操作并计时，报告每批平均到每个操作的开销。循环和测试框架的开销
包含在内；这些开销不能简单相加来估算一次 TCP 请求的成本。

| 操作 | 一次操作是什么 | ns/op | 最小–最大 |
|:--|:--|--:|--:|
| `lazy_task` | 通过一个非内联的工厂函数创建并销毁一个未启动的任务 | 13.96 | 13.81–14.92 |
| `await_chain` | 等待并销毁一条由 4 层嵌套任务帧组成的同步链，不经过反应器 | 64.05 | 62.39–64.54 |
| `yield` | 经过就绪队列和一轮反应器循环的一次挂起与恢复 | 5.03 | 4.85–5.58 |
| `post` | 在同一上下文上入队并运行一个小回调 | 7.38 | 6.77–7.67 |
| `cross_thread_post` | 从另一个线程向正在运行的反应器 `post` 一次，包括唤醒 | 7.27 | 6.54–8.20 |
| `spawn_completion` | 创建、spawn、运行并销毁一个子任务 | 18.32 | 17.54–19.15 |
| `zero_timer` | 一次 `sleep_for(0ns)`：读时钟、插入定时器堆并到期、恢复 | 28.25 | 27.77–29.52 |
| `empty_poll` | 一次超时为零、没有任何注册的 kqueue 调用 | 12,094.58 | 12,034.22–12,121.30 |

`empty_poll` 是系统调用本身的开销，运行循环只在需要阻塞或检查 I/O 时才付出这笔开销；只要有就绪的工作，`yield`
和 `post` 都留在用户态。

## 反应器与取消延迟

`pace_runtime_hotpaths` 端到端地测量单个事件，报告多次迭代的延迟百分位数。

| 负载 | 计时的区间 | p50 ns | p99 ns |
|:--|:--|--:|--:|
| cancel sparse，0 个后台注册 | 从取消一个已注册的等待到它的回调运行 | 250 | 292 |
| cancel sparse，65,536 个后台注册 | 同上，另有 65,536 个其他注册存活 | 250 | 292 |
| readiness，1 个套接字 | 从一个 UDP 套接字变为可读到轮询器报告它 | 1,750 | 5,125 |
| readiness，256 个套接字 | 从 256 个套接字变为可读到轮询器把它们全部报告完 | 25,000 | 46,875 |
| inbox，65,536 个根任务 | 从在 `run()` 之前 spawn 65,536 个根任务到它们全部运行完 | 3,875 | 6,958 |

取消的开销与其他注册的数量无关。另一个探针（`pace_inbox_allocations`）在另一个线程向反应器 post 10,000 个回调
期间，统计反应器线程上的堆分配次数：总共 2 次，因此跨线程提交路径在稳态下不分配内存。

## 套接字层

`pace_net_hotpaths` 直接驱动轮询器和 UDP 套接字。

| 负载 | 一次操作是什么 | ns/op | 最小–最大 |
|:--|:--|--:|--:|
| poll-dense | 一个就绪事件，每次轮询时 192 个已注册套接字中有 128 个就绪 | 117.2 | 116.2–120.1 |
| poll-sparse | 一个就绪事件，每次轮询时 192 个已注册套接字中有 1 个就绪 | 2,292 | 1,791–2,417 |
| udp | 回环上一次 `udp::async_exchange` 往返，包括过滤一个来自其他发送方的数据报 | 53,504 | 49,765–58,708 |

## HTTP 路由

`pace_router_bench` 在一张精确匹配的路由表中解析请求路径。

| 路由数 | 每次查找 ns | 最小–最大 |
|--:|--:|--:|
| 16 | 67.9 | 66.7–68.6 |
| 1,024 | 82.8 | 80.2–84.1 |

## 工作线程扩展

`pace_scaling` 运行一个只有 PACE 的 TCP 回显，每个工作线程一个监听端口，固定 8 个阻塞式客户端（64 字节消息，每个
客户端计入测量的请求为 2,000 个）。三种执行模式就是应用运行反应器的三种方式：`runtime` 线程池、调用线程作为其中
一个工作线程加入（`caller`），以及由应用自己拥有的线程（`external`）。

| 工作线程数 | `runtime` req/s | `caller` req/s | `external` req/s | p50 µs（`runtime`） | p99 µs（`runtime`） |
|--:|--:|--:|--:|--:|--:|
| 1 | 227,144 | 226,760 | 226,054 | 31.62 | 87.79 |
| 2 | 259,619 | 263,376 | 262,697 | 28.96 | 58.96 |
| 4 | 237,828 | 237,731 | 237,711 | 31.88 | 60.33 |
| 8 | 212,234 | 211,696 | 215,138 | 36.42 | 64.17 |

三种模式的开销相同。吞吐量在两个工作线程之后不再增长，是因为 8 个客户端、协调程序和服务器共用同一台机器的核心：
这里测量的是执行模式，而不是服务器容量。对于 1 到 8 个工作线程，启动反应器需要 0.02–0.10 ms，关闭需要
0.13–0.18 ms，各模式相同（[生命周期表](../../benchmarks/results/baseline-0.6.1-macos-arm64/scaling.md)）。

## 复现

```sh
xmake f -c -m release -o build/baseline --tls=y --dns=y --quic=y --http2=y --http3=y \
    --benchmarks=y --components=y --scaling=y --protocol_benchmarks=y --tests=n --examples=n -y
xmake -y
python3 scripts/run_baseline.py --source . --build-dir build/baseline \
    --output benchmarks/results/<name> --label "<host description>" --repeats 5
```

请在没有其他负载的机器上运行；没有任何设置固定 CPU 或频率。在 Apple Silicon 上，单个进程可能被调度到能效核心：
比较两个构建时，应交替运行多次（`scripts/compare_runtime_perf.py`），而不是各跑一次。

## 参见

- [TCP 回显](tcp.zh.md)：同一版本与 Asio 和 libuv 的对比
- [执行模型](../explanation/execution-model.zh.md)：运行循环在这些操作之间做了什么
- [`io_context` 参考](../reference/runtime/io-context.zh.md)
