<!-- source-hash: fabb23a39b47 -->
# HTTP 配置与进阶用法

[English](http-recipes.md) · [指南](README.zh.md) · [HTTP 服务端教程](../tutorial/03-http-server.zh.md)

本页的用法都基于 HTTP/1 模块：包含 `<pace/http1.hpp>`，链接 `pace_http1`；该模块默认构建（`--http1=y`）。
HTTPS 部分还需要 `--tls=y`。代码片段运行在协程内或 `io.run()` 之前，`io` 是持有这些 socket 的
`pace::io_context`。

## 完整响应与连接关闭

默认情况下，响应按 Content-Length、chunked 完整收齐或本身没有 body 时，调用直接返回，
不再等待对端 FIN。PACE 会尝试发送本端 TCP FIN / TLS `close_notify`，然后释放连接，
不留后台清理任务。清理阶段的传输错误不会覆盖已经完成的响应；上下文取消仍会传播。
以 EOF 界定的响应仍要等到 EOF，HTTPS 还要验证 TLS 关闭通知。

需要有界的优雅关闭时，在 limits 参数之后传入 `client_options`：

```cpp
pace::http::client_options closing{
    .close_policy = pace::http::client_close_policy::graceful,
    .close_timeout = std::chrono::milliseconds(25),
};
auto response = co_await pace::http::async_get(io, endpoint, "/health", {}, closing);
// 持久客户端：http::client client(std::move(socket), {}, closing);
```

`graceful` 等待对端关闭或预算耗尽，并传播清理错误。预算为 0 时只尝试本端通知；负数会以
`std::invalid_argument` 拒绝。它不是请求超时，也不是读取超时。默认的 `no_wait` 不使用这个预算
（默认 100 ms）。两种策略下，截断的 body、未完成的 chunk 和非法 trailer 都仍会失败。
服务端发送最终响应后的 drain 与此无关，保持不变，包括提前拒绝请求的情形。
关闭策略的完整语义见 [HTTP/1 参考](../reference/http1/README.zh.md)。

## 需要控制监听时，显式持有 listener

`http::serve(io, endpoint, handler)` 自己持有 listener，在任务启动时才绑定。
需要立即拿到绑定结果、查询端口 0 分配到的端口，或者单独停止接收新连接时，改用 listener 重载。
listener 声明在任务之外，并保持到 `io.run()` 返回：

```cpp
pace::io_context io;
pace::tcp::listener listener(io, {"127.0.0.1", 0});
pace::http::router app;
app.get("/health", [](pace::http::request) { return pace::http::response{.body = "ok\n"}; });
io.spawn(pace::http::serve(listener, app));
// 再提交一个最终调用 io.stop() 的客户端或控制任务。
io.run();
```

router（`<pace/http/router.hpp>`，包含在 `<pace/http.hpp>` 中）同时也是 HTTP/2 和 HTTP/3 接受的 handler。
只有一个回调、不需要路由时，用 `http::make_handler(callback)`。handler 可能同时在多个 worker 上下文中运行，
共享的可变状态需要自行同步。添加共享捕获之前，先读 [router 契约](../reference/http/router.zh.md)。

## 启用 HTTPS

用 `--tls=y` 构建。HTTP 调用的形式不变，只需加上 TLS 配置和要验证的对端名称：

```cpp
pace::tls::context trust; // OpenSSL 默认信任路径，验证对端。
trust.load_verify_file("my-ca.pem"); // 可选：私有 CA 或显式 CA 文件。
auto response = co_await pace::http::async_get(io, trust, {"127.0.0.1", 8443},
                                       "localhost", "/health");
```

证书按 `localhost` 验证，与连接所用的数字 TCP 地址无关。这个惰性任务使用 TLS 配置期间，
配置必须保持有效；endpoint、对端名称和 target 则由任务自己持有。
如果 OpenSSL 安装需要显式提供 CA 文件，见 [TLS 参考](../reference/tls/README.zh.md)。

HTTPS 服务端在接收连接之前配置证书链和私钥：

```cpp
pace::tls::context security(pace::tls::role::server);
security.use_certificate_chain("server-chain.pem", "server-key.pem");
security.set_alpn_protocols({"http/1.1"});
pace::tcp::listener listener(io, {"127.0.0.1", 8443});
io.spawn(pace::http::serve(listener, security, route));
io.run();
```

证书与私钥必须匹配，并覆盖客户端要验证的名称。这些对象必须活到 accept 循环结束；`io` 要声明在它们之前。
每个连接独立完成握手。基于 TLS 的 HTTP/1.1 只接受 ALPN `http/1.1` 或未协商 ALPN；`h2` 请使用单独的
HTTP/2 接口。仓库中 `tests/certs` 的密钥是公开的测试材料，不能用作部署凭据。

## 复用连接

需要多次交换时，用已连接的传输构造一个持久客户端：

```cpp
pace::tcp::stream socket(io);
co_await socket.connect(endpoint);
pace::http::client client(std::move(socket));
pace::http::request request;
request.method = "GET";
request.target = "/health";
request.headers.push_back({"Host", "localhost"});
auto first = co_await client.send(request);
auto second = co_await client.send(std::move(request));
client.close();
```

客户端持有 socket 和解析缓冲区。交换进行期间，客户端对象的地址必须保持不变。持久客户端上的 HTTP/1.1
请求必须自带 `Host`。同一时间只能有一个 `send` 未完成；HTTP/1.1 不做多路复用。
收到要求关闭或以 EOF 界定的响应、收到非法响应或被取消之后，客户端不能再发下一个请求
（`is_open()` 返回 false），需要重新连接。对已完成握手的 `tls::stream` 使用 `secure_client`，
即可在 HTTPS 上沿用同样的模式。不要在 HTTP 操作之间穿插对底层传输的直接读取。

## 设置合理的上限

```cpp
pace::http::limits bounds;
bounds.max_header_bytes = 8192;
bounds.max_body_bytes = 65536;
bounds.max_requests_per_connection = 100;
bounds.max_trailer_bytes = 2048;
io.spawn(pace::http::serve(listener, route, bounds));
```

同一个 `limits` 也可以传给 `async_get`、`async_fetch` 和客户端构造函数。默认值还限制了字段数量、
chunk 行长度、chunk 数量和客户端可接收的 1xx 响应数；body 默认上限为 1 MiB。格式错误或有歧义的
Content-Length/chunked 分帧会被拒绝并关闭连接。trailer 不能重新定义分帧、Host、连接控制或认证字段。

这些限额约束的是单条消息和解析元数据。下文的可选服务端选项另外提供接收和写入期限；全局内存和连接数
策略仍由应用负责。endpoint 重载不解析主机名；启用可选的 [DNS 模块](../reference/dns/README.zh.md)后，
URL 客户端会解析主机名。不支持重定向、重试和解压。HTTP/2、[HTTP/3](../reference/http3/README.zh.md)
和 [WebSocket](../reference/websocket/README.zh.md) 各有独立接口。

## 先检查请求头，再决定是否接收 body

返回 `task<http::server_response>` 的 handler（即 `server_handler` 类型）可以启用准入、逐请求 body 上限、
期限和流式响应，同时仍能直接返回普通的缓冲 `response`：

```cpp
pace::task<pace::http::server_response> admitted_route(pace::http::request request) {
    co_return pace::http::response{.body = request.body};
}

pace::http::server_options options;
options.admit = [](const pace::http::request_head& head) {
    if (head.header("Authorization") != "Bearer example")
        return pace::http::admission::reject(pace::http::response{.status = 401});
    return pace::http::admission::accept(head.target == "/import" ? 1024 * 1024 : 4096);
};
options.request_timeout = std::chrono::seconds(10);
options.write_timeout = std::chrono::seconds(30);
io.spawn(pace::http::serve(listener, admitted_route, options));
```

准入回调在请求行、头字段和分帧验证之后、收集 body 之前运行。拒绝时立即发送所给响应，即使对端迟迟不发
body 也不等待，随后关闭连接。接受时选定的上限同时约束 Content-Length 和 chunked 输入，每个 keep-alive
请求重新选择一次。借给回调的请求头视图只在这次同步调用内有效；一次有界的 socket 读取可能已经带入少量
body 字节，但不会等待 body。被接受的请求仍以完整缓冲的 body 交给最终 handler。准入回调抛出异常时返回 500。

同一个 handler 也可以返回 `streaming_response`，提供独占的 `body_source` 和已知的 `content_length`。
PACE 用一个有界缓冲区（`server_options::response_buffer_bytes`，默认 64 KiB）调用 `source->read(span<char>)`，
等这一块写完再读下一块。断连、取消或写入超时会停止后续读取并释放 source。把已经打开的文件放进 source，
就能在发送期间始终使用同一份快照，而不必把整个文件读进内存；完整示例见
[HTTP/1 参考](../reference/http1/README.zh.md)。

请求超时和写入超时分别是接收阶段和发送阶段的绝对期限：收到少量数据不会重新计时，设为 0 表示不启用。
两者都不包括最终 handler 和 TLS 握手。超时直接关闭传输，不保证另发 408。提前拒绝时请求期限随之结束，
拒绝响应改由写入期限约束。准入回调和 producer 都是同步调用，必须及时返回：期限无法抢占一个阻塞的应用回调。

参考：[HTTP/1 模块](../reference/http1/README.zh.md) · [共享 HTTP 客户端](../reference/http/clients.zh.md)。
下一步：[worker 与线程](../tutorial/08-workers-and-threads.zh.md)。
