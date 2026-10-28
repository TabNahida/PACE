# 4. 先获取 HTTP 响应，再编写服务

[English](04-http.md) · [学习路线](README.zh-CN.md)

使用第一章的当前源码构建，链接 `pace_http1` 并配置 `--http1=y`。
本章的直接调用和 `async_` 命名需要 PACE 0.4.0 或更新版本。

## 先从直接 GET 开始

保存为 `client.cpp`，添加同名 binary target、`add_files("client.cpp")` 和
`add_deps("pace_http1")`：

```cpp
#include <pace/http.hpp>
#include <iostream>

int main() {
    try {
        const auto reply = pace::http::get("http://127.0.0.1:8080/health");
        std::cout << reply.status << " " << reply.body;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

下方的服务端运行后，构建并执行 `client` 应输出 `200 ok`。`get` 阻塞当前线程并直接返回
响应，默认总超时 30 秒；没有 `run` 或 `co_await`。状态码保留在结果中，网络或协议错误抛异常。
向后面添加的 `/echo` 路由发送 body，只需把请求行替换成：

```cpp
auto reply = pace::http::post("http://127.0.0.1:8080/echo", "hello", "text/plain");
```

其他方法用 `fetch`，例如 HEAD：

```cpp
pace::http::request request;
request.method = "HEAD";
auto reply = pace::http::fetch("http://127.0.0.1:8080/health", std::move(request));
```

数字 HTTP 地址只需默认 HTTP/1 模块；域名需要 `--dns=y`，HTTPS 需要 `--tls=y`。
总超时、自定义 CA 和旧代码迁移见[直接客户端参考](../reference/direct-clients.zh-CN.md)。

## 准备本地服务

将下面的完整程序保存为 `server.cpp`，同样创建链接 `pace_http1` 的 `server` target。

```cpp
#include <pace/http.hpp>
#include <pace/run.hpp>
#include <iostream>

namespace http = pace::http;

int main() {
    try {
        http::app app;
        app.get("/health", [] { return http::text("ok\n"); });
        app.run({"127.0.0.1", 8080});
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

执行 `xmake -y server client` 后运行 `xmake run server`，在另一个终端执行
`curl -i http://127.0.0.1:8080/health`。预期状态 200、正文 `ok`。
服务器会持续监听；这里 Ctrl-C 是系统默认终止行为。
配套 `tutorial_http serve` 则加入第六章解释的协作式信号关闭。

## 第一步：从内向外理解四个职责

`http::app` 持有路由和监听器，`app.run` 创建并运行事件循环。
不需要请求内容时，处理器可以省略参数，直接返回 `http::text`、`http::html`
或 `http::json`；需要正文或请求头时再接收 request。
没有异步等待就不需要写协程。

现在试三种请求：`/missing` 得到 404；`curl -I .../health` 使用 HEAD，
复用 GET 路由但不发送正文；POST 到 `/health` 得到带 `Allow: GET, HEAD` 的 405。
`/health?probe=1` 仍匹配 `/health`，处理器拿到的 target 则保留 query。
匹配区分大小写，不进行百分号解码，也没有隐含的通配符规则。

## 第二步：增加 POST 回显

在 `app.run(...)` 前插入：

```cpp
app.post("/echo", [](http::request request) {
    return http::response{.body = std::move(request.body),
                          .content_type = "application/octet-stream"};
});
```

重新构建和启动，再执行
`curl -H 'Expect:' --data-binary 'hello' http://127.0.0.1:8080/echo`，应收到 `hello`。
`std::move` 把缓冲好的正文交给响应。Content-Length、Connection 等分帧字段由库生成，
不需要手写。其他方法可以用 `app.route("PUT", "/echo", handler)` 注册。

## 第三步：把普通处理器换成异步处理器

加入 `<chrono>`，再注册一个路由：

```cpp
app.get("/slow", []() -> pace::task<http::response> {
    co_await pace::sleep_for(std::chrono::milliseconds(5));
    co_return http::text("done\n");
});
```

出现 `co_await` 后，返回类型换成 `task<response>`，最后使用 `co_return`。
同一个 router 同时接受普通函数和协程。等待期间，reactor 可以处理其他就绪工作；
但同步数据库调用或耗时计算仍会阻塞当前线程，协程语法不会自动把它们送到线程池。
`pace::sleep_for` 使用当前任务所在的 context。

## 第四步：需要等待其他网络请求时

处理函数里不能调用阻塞客户端；用异步名字让等待期间的线程继续处理其他工作：

```cpp
app.get("/proxy", []() -> pace::task<http::response> {
    co_return co_await http::async_get("http://127.0.0.1:8080/health");
});
```

`async_get`、`async_post`、`async_fetch` 返回任务，在已有 context 上执行。
它们没有直接调用的默认 30 秒总超时。复用连接和主动管理 deadline 见 reference。
仓库的 `xmake run tutorial_http` 验证路由与异步客户端；配套程序还提供有界信号关闭。

## 启用 HTTPS

先按[第八章](08-dns-tls.zh-CN.md)验证本地 TLS 对端，再参考
[HTTPS 配置](../reference/http-recipes.zh-CN.md#启用-https)加入证书。
连接复用、期限、请求准入、流式响应及关闭策略统一放在
[HTTP 进阶参考](../reference/http-recipes.zh-CN.md)，按应用需要逐项引入。

下一章：[执行方式](05-execution.zh-CN.md)。
