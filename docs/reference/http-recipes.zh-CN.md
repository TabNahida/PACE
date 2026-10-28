# HTTP 配置与进阶用法

[HTTP 入门教程](../tutorial/04-http.zh-CN.md) · [English](http-recipes.md)

## 完整响应与连接关闭

默认的 `async_get`、`async_fetch` 和 `client.send` 在完整解析 Content-Length、chunked 或无 body
响应后返回，不因 `Connection: close` 继续等待对端 FIN。库会尝试发送本端 TCP FIN / TLS
`close_notify` 并释放连接，没有后台清理任务。完整响应后的清理错误不会覆盖结果；上下文
取消仍会传播。没有长度或分块 framing 的响应仍须等到 EOF，HTTPS 还须验证 TLS 关闭通知。

需要保留有界等待时，在原有 `limits` 参数之后传入 `client_options`：

```cpp
pace::http::client_options closing{
    .close_policy = pace::http::client_close_policy::graceful,
    .close_timeout = std::chrono::milliseconds(25),
};
auto response = co_await pace::http::async_get(io, endpoint, "/health", {}, closing);
// 对持久客户端：http::client client(std::move(socket), {}, closing);
```

`graceful` 等待对端关闭或预算耗尽，并传播清理错误和取消。预算为 0 时只尝试本端通知，
负数会被拒绝；默认 `no_wait` 不使用这个预算。它不是请求或读取超时。截断 body、未完成
chunk、非法 trailer 仍会失败。服务端发送最终响应后的有界 drain 保持不变，包括提前拒绝
尚未收完的请求 body。[完整语义](../reference/http.md#final-response-close-policy)。

## 需要独立控制监听时，显式持有 listener

希望立即得到绑定错误、查询临时端口，或者先停止接收再排空已有连接时，
使用原来的 listener 重载。listener 放在任务外面，保持到 `io.run()` 返回：

```cpp
pace::io_context io;
pace::tcp::listener listener(io, {"127.0.0.1", 0});
pace::http::router app;
app.get("/health", [](pace::http::request) { return pace::http::response{.body = "ok\n"}; });
io.spawn(pace::http::serve(listener, app));
// 再提交一个最终调用 io.stop() 的客户端或控制任务。
io.run();
```

router 也能作为 HTTP/2、HTTP/3 的共享 handler 使用。
只有一个普通回调、不需要路由时，用 `http::make_handler(callback)` 即可。
不同 worker 共享的可变状态仍需同步；[接口契约](../reference/http-router.md)
说明了闭包所有权、线性查找和适用边界。

## 启用 HTTPS

配置并构建 `--tls=y`，然后给客户端增加 TLS 配置和要验证的名称：

```cpp
pace::tls::context trust;
trust.load_verify_file("my-ca.pem"); // 按需添加 CA；默认使用 OpenSSL 信任路径。
auto response = co_await pace::http::async_get(io, trust, {"127.0.0.1", 8443},
                                       "localhost", "/health");
```

TCP 连接地址和证书身份是分开的：上例连接本机地址，但必须验证证书中的 `localhost`。
TLS 配置必须保持有效，直到这个惰性任务使用结束。库没有跳过证书验证的开关。
如果 OpenSSL 安装没有可用的默认根证书，需要显式提供 CA 文件，详见
[TLS 配置说明](../reference/tls.zh-CN.md#配置与连接)。

HTTPS 服务端先配置证书链及对应私钥：

```cpp
pace::tls::context security(pace::tls::role::server);
security.use_certificate_chain("server-chain.pem", "server-key.pem");
security.set_alpn_protocols({"http/1.1"});
pace::tcp::listener listener(io, {"127.0.0.1", 8443});
io.spawn(pace::http::serve(listener, security, route));
io.run();
```

这些对象须活到 accept 任务结束。每个连接独立完成 TLS 握手。HTTP/1.1 接口只接受
`http/1.1` 或未协商 ALPN；HTTP/2 的 `h2` 使用单独 API。仓库 `tests/certs` 中的密钥
是公开测试材料，不可作为部署凭据。

## 复用与限额

将已连接的 `tcp::stream` 移入 `http::client`，便可依次 `co_await client.send(request)`；
对已握手的 `tls::stream` 使用 `http::secure_client`。持续客户端的 HTTP/1.1 request
须自行提供 Host。一次只能有一个 send 未完成，不能把 HTTP/1.1 当作并发多路复用。
收到关闭响应、非法响应或遇到取消后，客户端不能再复用。需要新请求时重新连接。

给 `serve`、`async_get`、`async_fetch` 或 client 构造传入 `http::limits` 可调整请求头、body、
trailer、字段数量、chunk 行长、chunk 数量等上限。默认 body 上限为 1 MiB；
歧义 Content-Length/Transfer-Encoding、非法 chunk 或危险 trailer 都会被拒绝并关闭连接。
这些限额不是全局连接数或全局内存策略。下方可选服务端 API 另提供超时和流式响应。
Expect、CONNECT、流式请求 body、HTTP/3 不在这个接口范围内；HTTP/2、[HTTP/3](../reference/http3.md) 和 WebSocket
使用各自独立接口。主机名可先用可选的 [DNS 扩展](../reference/dns.md)显式解析。


## 先检查请求头，再决定是否收取 body

把新 handler 的返回类型设为 `task<http::server_response>`，即可通过
`server_options` 启用准入，仍可直接返回原有 `response`：

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

准入回调拿到已经检查请求行、头字段和 framing 的只读视图；视图只在同步回调内有效。
拒绝时立即发送所选响应，再关闭连接，不等待客户端上传其声明的 body。接受时选择的限额
只作用于本次请求，同时约束 Content-Length 和 chunked；下一个 keep-alive 请求重新选择。
小量 socket 预读允许带入少量 body，但不会先等待或收集完整 body。最终 handler 仍收到
限额内完整缓冲的请求。

响应也可返回 `streaming_response`，提供已知的 `content_length` 和独占的 `body_source`。
PACE 给 `read(span<char>)` 的缓冲区默认最多 64 KiB；上一块写完才继续读取。断连、取消或
写超时会停止后续读取并释放 source。可把已经打开的文件句柄放进 source，使发送期间一直
持有同一个快照，避免把整个归档载入内存。详见[已打开文件的完整示例](../reference/http.md#bounded-response-producers)。

请求超时覆盖每次请求头和 body 的接收（含 keep-alive 等待），写超时覆盖响应头和 body 的
发送；两者均为绝对期限，收到少量数据不会重新计时。提前拒绝后撤销请求定时器，响应使用
写期限。过期直接关闭连接，不保证另发 408。最终业务 handler 和 TLS 握手不在这两段期限内。
准入和 producer 是同步调用，必须及时返回；期限无法抢占一个阻塞的应用回调。
