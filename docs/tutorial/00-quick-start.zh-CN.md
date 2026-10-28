# 0. 从一个小服务开始

[English](00-quick-start.md) · [学习路线](README.zh-CN.md) · [下一章：构建细节](01-build.zh-CN.md)

本章使用 **PACE 0.3.1**。先让一个服务跑起来，再逐步加入 POST、异步等待和客户端。
需要 xmake 2.9.8+ 和 C++20 编译器。以下命令都在你自己的空目录运行。

## 1. 创建工程

保存为 `xmake.lua`：

```lua
add_rules("mode.debug", "mode.release")
add_repositories("tab-repo https://github.com/TabNahida/repo-xmake.git")
add_requires("pace 0.3.1")

target("hello")
    set_kind("binary")
    set_languages("c++20")
    add_files("main.cpp")
    add_packages("pace")
```

这里通过包管理器安装 PACE，不需要把库源码复制到项目里。
默认包含 HTTP/1 和 WebSocket；只学这一章不用安装 TLS 或 DNS 的运行依赖。
源代码集成方式仍见第一章。

## 2. 写健康检查

保存为 `main.cpp`：

```cpp
#include <pace/http.hpp>

int main() {
    pace::http::app app;
    app.get("/health", [] { return pace::http::text("ok\n"); });
    app.run(8080);
}
```

执行 `xmake f -m release -y`、`xmake -y`、`xmake run hello`。
在另一个终端执行 `curl -i http://127.0.0.1:8080/health`，应得到 `200` 和正文 `ok`。
`/missing` 返回 404，POST 到 `/health` 返回 405，HEAD 不发送正文。

`app.run(8080)` 在当前线程运行，默认只绑定 `127.0.0.1`。
要监听其他地址，明确写 `app.run({"0.0.0.0", 8080})`。
服务器持续运行，Ctrl-C 使用系统默认终止行为；它没有隐含信号处理或后台线程。
绑定失败会抛异常，实际应用可在 `main` 中捕获 `std::exception` 并记录。

## 3. 增加响应和请求正文

在 `app.run` 前加入：

```cpp
app.get("/status", [] { return pace::http::json(R"({"ready":true})"); });
app.post("/echo", [](pace::http::request request) {
    return pace::http::text(std::move(request.body));
});
```

重启后执行 `curl -H 'Expect:' --data-binary hello http://127.0.0.1:8080/echo`，
应收到 `hello`。`text`、`html`、`json` 持有字符串并选择 Content-Type；第二个参数可设置状态码。
`json` 接收已经序列化的 JSON，不负责把 C++ 对象转换成 JSON，也不校验内容。
不需要读取请求的 handler 可以省略参数；需要读取时按值接收 request。
其他方法通过 `app.route("PUT", "/path", handler)` 注册。

## 4. 等待时再使用协程

加入 `<chrono>`，在 `app.run` 前注册：

```cpp
app.get("/slow", []() -> pace::task<pace::http::response> {
    co_await pace::sleep_for(std::chrono::milliseconds(10));
    co_return pace::http::text("done\n");
});
```

这里无需传递 `io_context`；`sleep_for` 使用正在执行此 handler 的事件循环。
暂停期间其他就绪工作仍可执行，但耗时计算或同步数据库调用仍会阻塞这个线程。
在事件循环外直接调用 `sleep_for` 会抛 `logic_error`。

## 5. 写自己的客户端

创建 `client.cpp`，在 xmake.lua 复制 hello target，改名 `client`、源文件改为 `client.cpp`：

```cpp
#include <pace/http.hpp>
#include <iostream>

int main() {
    try {
        const auto reply = pace::run(pace::http::get("http://127.0.0.1:8080/health"));
        std::cout << reply.status << " " << reply.body;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

保持服务运行，执行 `xmake -y client` 和 `xmake run client`，应输出 `200 ok`。
`get` 产生惰性任务，`run` 启动入口并返回响应；多个请求可放在一个无参数协程里：

```cpp
pace::run([]() -> pace::task<void> {
    auto reply = co_await pace::http::post("http://127.0.0.1:8080/echo", "hello", "text/plain");
    std::cout << reply.body << '\n';
});
```

访问域名或 HTTPS 时，把依赖改为
`add_requires("pace 0.3.1", {configs = {dns = true, tls = true}})`，重新配置构建。
HTTPS 默认使用系统信任根并验证 URL 主机名。URL 中的片段不发送给服务器；
Host 由 URL 生成，不能额外覆盖。不自动跟随重定向、重试请求或解压响应。
详细边界和自定义 CA 见[便利接口参考](../reference/convenience.md)。

## 6. 转入更低层的 TCP

在协程里，连接和读取固定长度消息可以写成：

```cpp
auto socket = co_await pace::tcp::connect({"127.0.0.1", 9090});
co_await socket.write_all("ping");
auto reply = co_await socket.read_exact(4);
```

这里需要一个按约定回显 4 字节的服务器，第三章带你从头写出它。
`read_exact` 会处理短读，提前 EOF 会抛异常；字符串重载默认最多分配 1 MiB。
TCP 仍然是字节流，不能用一次 `read_some` 代替消息边界。

需要端口检查、自己的线程、停止接收后排空连接或流式 HTTP 响应时，继续学习显式接口。
`app.serve(io, endpoint)` 可放入已有事件循环；它持有路由快照，注册之后的新路由不会影响已经启动的服务。
完整教程从[任务与定时器](02-tasks-timers.zh-CN.md)继续。
