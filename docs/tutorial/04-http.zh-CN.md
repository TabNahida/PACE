# 4. 一步步写出 HTTP 服务

[English](04-http.md) · [上一章：TCP](03-tcp.zh-CN.md) · [下一章：执行方式](05-execution.zh-CN.md)

这一章先写健康检查，再增加 POST 回显和异步处理器。沿用第一章的应用，
把依赖改为 `pace_http1`，执行 `xmake f --http1=y -y`。
先把 `main.cpp` 替换成这个完整服务器：

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

构建并运行 `my_application`，在另一个终端执行
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

## 第四步：用 C++ 客户端访问它

保存以下程序为 `client.cpp`，新增名为 `client` 的 binary target，
使用 `add_files("client.cpp")` 和 `add_deps("pace_http1")`。
保持服务器运行，在第二个终端构建并运行 `client`：

```cpp
#include <pace/http.hpp>
#include <pace/run.hpp>
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

预期输出 `200 ok`。URL 接口负责连接、生成 Host、读取响应和关闭连接。
检查响应的 status；传输或分帧错误抛异常。POST 可用 `http::post(url, body, content_type)`。
数字回环地址不需要 DNS；域名需要 `--dns=y`，HTTPS 还需要 `--tls=y` 和有效信任根。

仓库里的 `xmake run tutorial_http` 检查健康路由和异步路由；
[配套程序](../../examples/tutorial_http.cpp)另有有界的信号关闭。

## 启用 HTTPS

先按[第八章](08-dns-tls.zh-CN.md)验证本地 TLS 对端，再参考
[HTTPS 配置](../reference/http-recipes.zh-CN.md#启用-https)加入证书。
连接复用、期限、请求准入、流式响应及关闭策略统一放在
[HTTP 进阶参考](../reference/http-recipes.zh-CN.md)，按应用需要逐项引入。

下一章：[执行方式](05-execution.zh-CN.md)。
