# 中文学习指南

这条学习路线从一个线程开始，再扩展到 TCP、HTTP 和多线程。
英文正文提供完整 API 细节；这里给出每一步的运行方法和应理解的关键行为。

## 1. 构建与运行

需要支持 C++20 协程的编译器和 xmake 2.9.8 以上版本。
在仓库根目录运行：

```sh
xmake f -m release --cxxstandard=cxx20 --examples=y --tests=y -y
xmake -y
xmake test -v
```

常规库不依赖 Asio 或 libuv；只有开启对比基准时才会引入它们。
用 `--cxxstandard=cxx23` 可以选择 C++23，`--profiling=y` 是会引入额外开销的诊断模式。
参见[构建教程](01-build.md)。

## 2. 任务与计时器

```sh
xmake run tutorial_tasks
```

查看[完整源码](../../examples/tutorial_tasks.cpp)和[任务教程](02-tasks-timers.md)。
`io_context io; io.spawn(lesson(io)); io.run();` 就能在当前线程执行任务，
无需 runtime。任务拥有按值传入的字符串，但只是借用 context。
`sleep_for` 使用单调时钟，零时长也会挂起；`yield` 给其他就绪工作一个机会。
任务结束不会自动让 context 退出，示例显式调用 `stop()`。

## 3. TCP 客户端和服务端

```sh
xmake run tutorial_tcp
```

默认模式自动创建回环 listener 和一个客户端，逐字节验证回显后结束。
在两个终端分别运行：

```sh
xmake run tutorial_tcp serve 9090
xmake run tutorial_tcp client 9090
```

查看[TCP 教程](03-tcp.md)和[源码](../../examples/tutorial_tcp.cpp)。
服务端将接受的 stream 按值移入具名连接协程。读缓冲区位于协程帧中，
等写操作完成后才会被下次读取覆盖。TCP 没有消息边界，客户端必须累计读取，
不能假设一次 `read_some` 对应一次 `write_all`。

## 4. HTTP 服务

```sh
xmake run tutorial_http
xmake run tutorial_http serve
```

第二条命令启动 `127.0.0.1:8080` 服务。在另一终端使用：

```sh
curl --http1.1 http://127.0.0.1:8080/health
curl --http1.1 -H "Expect:" --data-binary "hello" http://127.0.0.1:8080/echo
```

查看[HTTP 教程](04-http.md)和[源码](../../examples/tutorial_http.cpp)。
handler 签名是 `task<response>(request)`，request/response 均拥有字符串数据。
自行设置状态、body、content_type，库负责响应长度和连接 framing。
限额控制请求字节数及请求次数，并不控制慢连接时间或总连接数量。

## 5. 选择线程模型

```sh
xmake run tutorial_workers
xmake run tutorial_workers background
```

默认模式使用 `runtime(2).run_on_current_thread()`：调用线程运行 context 0，
另有一个后台线程运行 context 1。`background` 模式使用 `run()`，
两个 context 都使用后台线程，调用线程等待。

用 `runtime(1).run_on_current_thread()` 可以避免创建 runtime 工作线程；
也可以像第一个教程那样直接使用 `io_context::run()`，或者在应用自己管理的
线程中运行多个独立 context。这里没有强制线程池、自动任务窃取或 CPU 绑核。
详见[执行模型教程](05-execution.md)和[源码](../../examples/tutorial_workers.cpp)。

## 6. 停止、取消和优雅关闭

```sh
xmake run tutorial_shutdown
```

示例会验证一个长时间 timer 被取消，且协程局部资源在 run 返回前已释放。
取消按 `error.code() == std::errc::operation_canceled` 判断。
`stop()` 不等于 join，也不允许之后重启 context。

生产服务需要分阶段关闭：先关闭 listener 停止接受，等待已有请求完成或达到
宽限期，再停止 context，最后 join 线程并按生命周期顺序销毁资源。
TCP/HTTP 示例通过只设标志的信号处理函数接收 Ctrl-C，再由普通协程调用 stop。
参见[关闭教程](06-shutdown.md)和[源码](../../examples/tutorial_shutdown.cpp)。

下一步可以查询 [API 中文索引](../reference/README.zh-CN.md)，
或阅读[完整英文参考](../reference/README.md)。修改运行参数后要使用
[基准测试方法](../../benchmarks/README.md)重新测量；示例运行速度不是性能结论。
