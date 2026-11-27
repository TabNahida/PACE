<!-- source-hash: 4ebde132413a -->
# 3. 构建 HTTP 服务器

[English](03-http-server.md) · [学习路线](README.zh.md) · [上一章：HTTP 请求](02-http-client.zh.md)

本章扩展[第 1 章](01-install-and-first-program.zh.md)的服务器：路由如何应答请求，如何构造响应，以及处理函数如何在不阻塞服务器的前提下等待。继续修改 `hello` 项目的 `main.cpp`。每次修改后，先停止服务器，再用 `xmake -y hello` 和 `xmake run hello` 重新构建并运行。

## 由内而外读懂服务器

```cpp
pace::http::app app;
app.get("/health", [] { return pace::http::text("ok\n"); });
app.post("/echo", [](pace::http::request request) { return pace::http::text(std::move(request.body)); });
app.run(8080);
```

`http::app` 拥有路由。`app.run(8080)` 在当前线程上创建事件循环，绑定 `127.0.0.1:8080`，并持续处理连接直到进程停止。要监听所有网络接口，传入一个端点：`app.run({"0.0.0.0", 8080})`。

处理函数可以接受 `http::request`，也可以不带参数，返回一个 `http::response`。`http::text`、`http::html` 和 `http::json` 用你已有的字符串生成带相应内容类型的响应；`json` 不会替你做任何序列化。

对运行中的服务器试试这些请求：

- `curl -i http://127.0.0.1:8080/missing`：没有匹配的路由，路由器返回 404。
- `curl -I http://127.0.0.1:8080/health`：HEAD 使用 GET 的处理函数，响应发送时不带 body。
- `curl -i -X POST http://127.0.0.1:8080/health`：路径存在但不支持 POST，返回 405 并带上 `Allow: GET, HEAD`。
- `curl -i 'http://127.0.0.1:8080/health?probe=1'`：查询串不参与匹配；处理函数仍能在 `request.target` 中看到它。

路径按精确、区分大小写的方式匹配，不做百分号解码，也没有通配符。

## 自己构造响应

`http::text` 只是一个快捷方式。为了让后面的路由写得简短，在包含头文件之后加上 `namespace http = pace::http;`。然后把 echo 路由换成直接填写 `http::response` 字段的版本：

```cpp
app.post(
    "/echo",
    [](http::request request)
    { return http::response{.body = std::move(request.body), .content_type = "application/octet-stream"}; }
);
```

运行 `curl -i --data-binary hello http://127.0.0.1:8080/echo`，预期得到 `hello` 和新的内容类型。移动 `request.body` 会把已缓冲的字符串交给响应，不做复制。`response` 还有 `status`、`headers` 和 `close` 字段。不要自己设置 `Content-Length` 或 `Connection`：这些分帧头部由服务器写出。其他方法使用 `app.route("PUT", "/echo", handler)`。

## 加入真正的挂起点

包含 `<chrono>`，再添加一个需要等待的路由：

```cpp
app.get(
    "/slow",
    []() -> pace::task<http::response>
    {
        co_await pace::sleep_for(std::chrono::milliseconds(5));
        co_return http::text("done\n");
    }
);
```

现在处理函数返回 `task<response>`，以 `co_return` 结束；app 两种形式都接受。等待期间，事件循环继续服务其他连接。`pace::sleep_for` 使用处理函数所在的事件循环。仅有协程语法并不会把工作移出事件循环：阻塞调用（例如同步的数据库查询）仍会阻塞这个循环上的所有连接。[第 8 章](08-workers-and-threads.zh.md)说明这类工作应放在哪里。

## 在处理函数中等待另一个请求

调用其他服务的处理函数使用异步客户端，这样等待期间事件循环仍在提供服务：

```cpp
app.get(
    "/proxy",
    []() -> pace::task<http::response>
    {
        auto reply = co_await http::async_get("http://127.0.0.1:8080/health");
        co_return http::text("upstream said: " + reply.body, reply.status);
    }
);
```

`curl http://127.0.0.1:8080/proxy` 输出 `upstream said: ok`：处理函数向同一个服务器请求 `/health`，再用得到的回复构造自己的响应。请构造新的响应，不要把客户端的 `reply` 原样返回：`reply.headers` 保存着收到的全部头部，其中包括 `Content-Length` 和 `Content-Type`，这些头部由服务器自己写出，出现在处理函数的响应中会被拒绝。[第 2 章](02-http-client.zh.md)的阻塞式 `http::get` 在这里会抛出异常，因为它拒绝在正在运行的 PACE 上下文中执行。异步调用没有默认截止时间；[第 7 章](07-structured-concurrency.zh.md)会加上它。

## 用配套程序检查

在 PACE 源码目录中，`xmake run tutorial_http` 在一个空闲端口上以显式的请求限制提供 `/health`、`/echo` 和 `/slow`，用 `async_get` 请求 `/health?probe=1` 和 `/slow`，输出 `HTTP /health and /slow verified` 后退出。`xmake run tutorial_http serve` 在 8080 端口上保持同样的服务器直到按下 Ctrl-C，并通过[第 9 章](09-graceful-shutdown.zh.md)讲解的信号处理干净地停止。

## 添加 HTTPS

提供 HTTPS 需要证书和 `--tls=y` 构建。[第 10 章](10-tls-and-dns.zh.md)用本地测试证书配置 TLS；之后 [HTTP 实用配方](../guides/http-recipes.zh.md)给出 HTTPS 服务器，并涵盖连接复用、截止时间、body 限制、流式响应和客户端关闭策略，供应用需要时查阅。完整约定见 [HTTP/1](../reference/http1/README.zh.md) 和[路由](../reference/http/router.zh.md)。

下一章：[4. 任务与定时器](04-tasks-and-timers.zh.md)
