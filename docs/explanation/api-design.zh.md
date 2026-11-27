<!-- source-hash: c47029673e2a -->
# API 设计：简短的调用，明确的所有权

[English](api-design.md) · [原理阐释](README.zh.md)

PACE 在同一套机制之上提供两层 API。简短调用用一行代码覆盖常见场景：获取一个 URL、为几条路由提供服务、
把一个任务运行到结束。其下的显式 API（`io_context`、`task`、socket、listener、各协议的客户端与服务端）
保持公开、完整可用；简短调用是在它之上实现的，而不是与它并列的另一套。两层都不引入框架依赖、隐式的
worker 池、隐藏的全局上下文或宏 DSL。本页说明简短调用背后的取舍，以及每一项的代价。

## 直接调用及其协程形式

每个客户端操作都有一个阻塞形式和一个同名、带 `async_` 前缀的协程形式：

| 阻塞形式，在普通线程上调用 | 协程形式，在上下文内调用 | 模块 |
|---|---|---|
| `http::get`、`http::post`、`http::fetch` | `http::async_get`、`http::async_post`、`http::async_fetch` | HTTP/1 |
| `http2::get`、`post`、`fetch`；`http3` 中相同 | `http2::async_get`、`async_post`、`async_fetch`；`http3` 中相同 | HTTP/2、HTTP/3 |
| `tcp::exchange`、`udp::exchange` | `tcp::async_exchange`、`udp::async_exchange` | net |

阻塞形式在调用线程上创建一个上下文，把协程形式运行到结束，然后返回持有数据的结果；只需要一个响应的程序
完全不必接触反应器。它接受 `blocking_options`（`<pace/runtime/blocking.hpp>`），其中有取消令牌和默认 30 秒的
总截止时间，因为没有截止时间的阻塞调用可能让一个别无出路的线程永远挂起。在已经运行 PACE 上下文的线程上
调用阻塞形式会抛出 `std::logic_error`：在那里阻塞会卡住该反应器上的所有协程，所以协程形式是唯一正确的选择。
协程形式没有隐含的截止时间，它们从所在的操作边界继承取消和截止时间。

URL 调用持有自己的参数，惰性解析 URL，并按构建配置选择 DNS 和 TLS：主机名需要 `--dns=y`，`https` 需要
`--tls=y`，数字地址的 `http` URL 在默认构建下即可使用。它们沿用显式 API 的协议限额，不增加重试、重定向、
cookie 或解压，因此行为与一次交换完全相同。参见[教程](../tutorial/02-http-client.zh.md)和
[客户端参考](../reference/http/clients.zh.md)。

## 一次性入口

`pace::run(factory)`（`<pace/runtime/run.hpp>`）在调用线程上创建一个上下文，运行 factory 返回的任务，
任务结束时停止上下文，并返回任务的值：

```cpp
const auto response = pace::run([](pace::io_context& io) {
    return pace::http::async_get(io, pace::tcp::endpoint{"127.0.0.1", 8080}, "/health");
});
```

没有它，每个程序都要重复同样的脚手架：构造上下文，spawn 一个把结果存到某处并停止上下文的任务，运行上下文，
最后重新抛出保存下来的异常。`run` 正好做这些，不多也不少。上下文仍然显式传入，所以 factory 里的代码与在
runtime worker 上运行的代码完全相同。factory 在关闭完成之前一直被持有，因此在这里使用带捕获的协程 lambda
是安全的；如果直接调用它，捕获就会悬空。代价是入口多一层包装协程，而不是多一个线程。入口任务完成时会取消
后台根任务；这不是对所有已 spawn 任务的结构化等待。需要等待并发工作时使用
[任务组](../reference/runtime/structured-concurrency.zh.md)；应用的生命周期有要求时，使用显式的上下文和 listener。

## 普通 handler 与异步 handler

不需要等待的 handler 应当是普通函数。`http::make_handler` 和 `http::router`（`<pace/http/router.hpp>`）
都能把 `response(request)` 和 `task<response>(request)` 形式的可调用对象（带或不带 `request` 参数）适配成
HTTP/1、HTTP/2 和 HTTP/3 共用的同一个 handler 类型。函数指针和可平凡复制的空可调用对象按值保存，因此最常见的
无状态 lambda 除了 `std::function` 本身外没有额外开销。有状态的可调用对象通过共享存储持有：挂起中的调用在
router 销毁后依然存活（包括只能移动的捕获），handler 的各个副本看到的是同一份状态。这份状态会在多个 worker
上下文之间共享，需要由应用同步。router 的副本共享只读的路由表，注册时才分离出自己的一份，因此服务端可以为
每个接受的连接复制 handler，而不必复制路由。

router 有意只做精确、区分大小写的方法与路径匹配，自己回答 404 和 405（带 `Allow`），并在 HEAD 没有注册时
回退到 GET；`mount()` 增加按路径段的前缀匹配，选择最长前缀。路由按路径排序保存，一次查找就是一次二分查找，
再扫描该路径上的几个方法。它不支持路径参数、中间件或 URL 规范化：需要这些功能的应用保留自己的 handler 即可；
由于三种 HTTP 服务端共用消息和 handler 类型，这样的 handler 同样适用于它们。

## 不需要控制时，少声明一个 listener

`http::serve(io, endpoint, handler)` 自己持有 HTTP/1 listener。绑定发生在任务启动时，因此绑定错误会像其他
失败一样经由协程传播。`http::app`（`<pace/http1.hpp>`）更进一步：它收集路由、挂载目录和代理，
`run(port)` 在调用线程上提供服务，只给端口时绑定回环地址。它在开始服务时快照路由，不安装信号处理器，也不创建
worker 线程；`run_workers` 是显式启用“每个 worker 一个 listener”的入口。

需要立即得到绑定结果、查询端口 0 分配到的端口、使用 TLS、准入和流式响应选项，或者单独停止接收新连接时，
保留 `serve(listener, …)` 重载。更短的入口不提供信号处理，也不提供连接的优雅排空；
[优雅关闭](../tutorial/09-graceful-shutdown.zh.md)演示了如何用显式 API 实现这两者。教程循序渐进地讲解两种
形式及其生命周期限制：[学习路线](../tutorial/README.zh.md)。
