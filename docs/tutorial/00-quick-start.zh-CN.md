# 0. 一次调用，拿到响应

[English](00-quick-start.md) · [学习路线](README.zh-CN.md) · [构建细节](01-build.zh-CN.md)

先从普通 C++ 开始。单次客户端请求不需要协程、事件循环配置，也不需要 `run`。
本教程使用**当前源码 checkout**：直接客户端和 HTTP 的 `async_` 命名尚未发布，
0.3.1 安装包不包含这些接口。

## 1. 先写客户端

将完整程序保存为 `client.cpp`：

```cpp
#include <pace/http.hpp>
#include <iostream>

int main() {
    try {
        auto reply = pace::http::get("http://127.0.0.1:8080/health");
        std::cout << reply.status << " " << reply.body;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`get` 完成连接、GET 请求、响应读取和连接关闭，然后直接返回。调用期间当前线程会阻塞；
`reply` 自己持有响应数据。404 等 HTTP 错误状态也会正常返回，因此要检查 `reply.status`；
连接、协议或超时错误会抛异常。整个调用默认最多等待 30 秒。

这个 URL 指向本地服务。接下来创建服务端，再一起构建两个文件。

## 2. 准备一个小服务端

保存为 `server.cpp`：

```cpp
#include <pace/http.hpp>
#include <iostream>

int main() {
    try {
        pace::http::app app;
        app.get("/health", [] { return pace::http::text("ok\n"); });
        app.post("/echo", [](pace::http::request request) {
            return pace::http::text(std::move(request.body));
        });
        app.run(8080);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`http::app` 持有路由，`text` 用字符串构造响应，POST 处理函数返回收到的 body。
`app.run(8080)` 在当前线程监听回环地址并持续服务。本入门程序用 Ctrl-C 结束。

## 3. 构建并运行

需要 xmake 2.9.8+ 和 C++20 编译器。将当前 PACE 源码放在应用目录的
`third_party/PACE`，与 `client.cpp`、`server.cpp` 和下面的 `xmake.lua` 配套：

```lua
set_project("hello")
set_languages("cxx20")
add_rules("mode.debug", "mode.release")
includes("third_party/PACE")

for _, name in ipairs({"client", "server"}) do
    target(name)
        set_kind("binary")
        add_files(name .. ".cpp")
        add_deps("pace_http1")
end
```

在应用目录执行：

```sh
xmake f -m release --tests=n --examples=n --http1=y --websocket=n -y
xmake -y client server
xmake run server
```

另开终端执行 `xmake run client`，应输出 `200 ok` 后退出。服务端继续等待下次请求。
[第一章](01-build.zh-CN.md)解释模块选择和仓库内配套 example 的构建。

## 4. 发送 body 或设置超时

把客户端的 `get` 那行替换为：

```cpp
auto reply = pace::http::post("http://127.0.0.1:8080/echo", "hello", "text/plain");
```

重新构建并运行客户端，应输出 `200 hello`。如果希望 GET 最多等待两秒，包含 `<chrono>` 后写：

```cpp
auto reply = pace::http::get("http://127.0.0.1:8080/health",
                             {.timeout = std::chrono::seconds(2)});
```

域名 URL 需要配置 `--dns=y`，HTTPS 需要 `--tls=y`。HTTPS 默认通过系统信任根验证 URL
中的主机名。其他方法和自定义信任见 [HTTP 章节](04-http.zh-CN.md)。请求不会自动重定向或重试。

## 5. TCP、UDP 也从一行开始

连接[第三章](03-tcp.zh-CN.md)的四字节 TCP 回显服务端：

```cpp
auto reply = pace::tcp::exchange({"127.0.0.1", 9090}, "ping", 4);
```

连接[第七章](07-udp.zh-CN.md)的 UDP 回显服务端：

```cpp
auto packet = pace::udp::exchange({"127.0.0.1", 9091}, "hello");
if (packet.truncated) throw std::runtime_error("UDP reply truncated");
```

分别包含 `<pace/tcp.hpp>`、`<pace/udp.hpp>` 并链接 `pace_net`，两者使用同样的超时选项。
TCP 需要约定回复长度；UDP 返回一个包含来源地址的数据报。对应章节会带你写出完整客户端和服务端。

## 6. 需要组合异步操作时再用协程

在 PACE 协程里使用 `async_get`、`async_post` 或 `async_fetch`：

```cpp
pace::run([]() -> pace::task<void> {
    auto reply = co_await pace::http::async_get("http://127.0.0.1:8080/health");
    std::cout << reply.body;
});
```

异步版本等待时会让出事件循环线程，适合已经在运行任务、处理连接或管理并发的应用。
直接客户端会拒绝在运行中的 PACE context 内调用；处理函数里使用异步版本。
异步调用不会自动获得直接客户端的 30 秒总超时。

接着按需要阅读 [HTTP](04-http.zh-CN.md)、[TCP](03-tcp.zh-CN.md) 或 [UDP](07-udp.zh-CN.md)，
需要异步组合时再读[任务与定时器](02-tasks-timers.zh-CN.md)。精确约定、调优和 0.3.1
迁移表见[直接客户端参考](../reference/direct-clients.zh-CN.md)。

[下一步：并发多个请求，共用一个 deadline](12-structured-concurrency.zh-CN.md)。
