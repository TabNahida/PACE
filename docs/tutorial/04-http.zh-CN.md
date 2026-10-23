# 4. 简明 HTTP 与 HTTPS 教程

[English](04-http.md) · [完整 API 参考](../reference/http.md)

先运行不需要外部服务的客户端示例：

```sh
xmake f -m release --examples=y --tests=y -y
xmake -y
xmake run tutorial_http_client
```

它会在本机启动 HTTP 服务，用客户端读取 chunked 响应及 trailer，检查结果后结束。
完整源码在 [tutorial_http_client.cpp](../../examples/tutorial_http_client.cpp)。

## 一次 GET 和 POST

在属于 `io` 的协程中，GET 只需一次调用：

```cpp
auto response = co_await pace::http::get(io, {"127.0.0.1", 8080}, "/health");
std::cout << response.status << " " << response.body << '\n';
```

库负责连接、Host、响应长度/分块解析和关闭。404、500 等是正常 HTTP 响应，
需要检查 `status`；传输失败或非法协议消息通过异常报告。endpoint 支持数字 IPv4 和 IPv6，
不自动解析域名，也不自动重定向、重试或解压。

POST 使用拥有数据的 request：

```cpp
pace::http::request request;
request.method = "POST";
request.target = "/echo";
request.headers.push_back({"Content-Type", "text/plain"});
request.body = "hello";
auto response = co_await pace::http::fetch(io, endpoint, std::move(request));
```

不要手动填写 Content-Length、Transfer-Encoding 或 Connection。
需要 chunked 时设置 `request.chunked = true`；可在 `request.trailers` 中放入
`X-Checksum` 等安全元数据。body 仍然完整保存在内存中，这不是流式上传接口。

## 启动服务

handler 是一个按值接收 request 的具名协程：

```cpp
pace::task<pace::http::response> route(pace::http::request request) {
    if (request.target == "/health")
        co_return pace::http::response{.body = "ok\n"};
    co_return pace::http::response{.status = 404, .body = "Not Found\n"};
}
```

在 `main` 中先声明 `io_context io`，再创建 listener，调用
`io.spawn(pace::http::serve(listener, route))`，最后 `io.run()`。
listener 必须活到 run 返回，并且先于 context 销毁。已有完整服务示例：

```sh
xmake run tutorial_http serve
curl --http1.1 http://127.0.0.1:8080/health
curl --http1.1 -H "Expect:" --data-binary "hello" http://127.0.0.1:8080/echo
```

用 Ctrl-C 请求停止。`Expect:` 覆盖避免客户端发送当前 HTTP/1.1 接口尚未实现的
`100-continue` 请求握手。响应设置 `chunked=true` 可使用分块编码；HEAD、204、205、304
等无 body 情况由库正确处理。跨工作线程共享的 handler 状态仍须由应用同步。

## 启用 HTTPS

配置并构建 `--tls=y`，然后给客户端增加 TLS 配置和要验证的名称：

```cpp
pace::tls::context trust;
trust.load_verify_file("my-ca.pem"); // 按需添加 CA；默认使用 OpenSSL 信任路径。
auto response = co_await pace::http::get(io, trust, {"127.0.0.1", 8443},
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

给 `serve`、`get`、`fetch` 或 client 构造传入 `http::limits` 可调整请求头、body、
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
