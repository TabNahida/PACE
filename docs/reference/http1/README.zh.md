<!-- source-hash: aab668f720ac -->
# HTTP/1

[English](README.md) · [参考](../README.zh.md)

## 概述

基于 TCP 和 TLS 的 HTTP/1.0 与 HTTP/1.1：服务器、持久连接客户端、一次性调用与 URL 客户端，以及小巧的 `http::app`。内置 Content-Length 与 chunked 分帧、trailer、keep-alive 和严格的分帧校验。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/http1.hpp>`（内容在 `<pace/http1/protocol.hpp>`） | `pace_http1` | `--http1=y`（默认） |

具名模块为 `pace.http1`。名字位于命名空间 `pace::http`（也可写作 `pace::http1`），与本模块使用的、与协议无关的[消息和路由器](../http/README.zh.md)放在一起。构建同时启用 `--tls=y`（`PACE_HAS_TLS`）时才有 HTTPS 重载；URL 中使用主机名需要 `--dns=y`。`pace_http1` 还包含[网关服务](../gateway/README.zh.md)，`<pace/http1.hpp>` 也声明了它们。

| 页面 | 内容 |
|:--|:--|
| 本页 | HTTP/1 如何写出和读取消息、`limits`，以及服务器和客户端共用的分帧规则与错误 |
| [服务器](server.zh.md) | `serve`、`serve_connection`、`server_options`、准入、流式响应、截止时间 |
| [客户端](client.zh.md) | `client`、`secure_client`、`client_options`、一次性调用、URL 调用、HTTPS 重载 |
| [应用](app.zh.md) | `http::app`：用几行代码完成路由、挂载、代理并运行服务器 |

这里的 HTTP/1 指完整消息的交换。它不实现 `Expect: 100-continue`、`Upgrade`、`CONNECT` 隧道、服务器发送的信息性（1xx）响应、流式请求正文和内容编码（压缩）。[WebSocket](../websocket/README.zh.md)、[HTTP/2](../http2/README.zh.md) 和 [HTTP/3](../http3/README.zh.md) 各有自己的模块；本模块既不协商也不升级到这些协议。

## 概要

```cpp
#include <pace/http1.hpp>

#include <stdexcept>

pace::task<pace::http::response> hello(pace::http::request request)
{
    co_return pace::http::text("Hello from " + request.target + "\n");
}

pace::task<void> check(pace::io_context& io, pace::tcp::endpoint endpoint)
{
    auto reply = co_await pace::http::async_get(io, endpoint, "/PACE");
    io.stop();
    if (reply.body != "Hello from /PACE\n")
    {
        throw std::runtime_error("unexpected reply");
    }
}

int main()
{
    pace::io_context io;
    pace::tcp::listener listener(io, {"127.0.0.1", 0});
    io.spawn(pace::http::serve(listener, hello));
    io.spawn(check(io, listener.local_endpoint()));
    io.run();
}
```

## 类型

### `limits`

```cpp
struct limits
{
    std::size_t max_header_bytes = std::size_t{16} * 1024;
    std::size_t max_body_bytes = std::size_t{1024} * 1024;
    std::size_t max_requests_per_connection = 1000;
    std::size_t max_header_fields = 100;
    std::size_t max_trailer_bytes = std::size_t{8} * 1024;
    std::size_t max_trailer_fields = 32;
    std::size_t max_chunk_line_bytes = 1024;
    std::size_t max_chunks = 65536;
    std::size_t max_informational_responses = 8;
};
```

服务器或客户端所接收内容的上限。它们不限制应用自己构造的消息，也不限制总内存、连接数或连接存活时间（后者见服务器的[截止时间](server.zh.md)）。

| 成员 | 限制对象 | 适用方 |
|:--|:--|:--|
| `max_header_bytes` | 起始行和头部字段，包括最后的空行 | 双方 |
| `max_body_bytes` | 去除分帧后的正文，无论由 Content-Length、分块还是连接结束来界定 | 双方 |
| `max_requests_per_connection` | 一条连接上服务的请求数；最后一个允许的响应会关闭连接 | 服务器 |
| `max_header_fields` | 头部字段个数 | 双方 |
| `max_trailer_bytes` | trailer 字段行和最后的空行 | 双方 |
| `max_trailer_fields` | trailer 字段个数 | 双方 |
| `max_chunk_line_bytes` | 一个分块大小行，包括扩展和 CRLF | 双方 |
| `max_chunks` | 分块个数，包括结尾的零长度分块 | 双方 |
| `max_informational_responses` | 最终响应之前跳过的 1xx 响应个数 | 客户端 |

超过 `max_body_bytes` 的 Content-Length 在分配内存或读取任何内容之前就会被拒绝。零不表示“不限”：限制为零就是不允许该资源，因此 `max_body_bytes = 0` 只允许空正文，`max_requests_per_connection = 0` 会不经服务就关闭每条连接。

### HTTP/1 如何使用 `request` 与 `response`

消息类型见 [HTTP 页面](../http/README.zh.md#类型)。HTTP/1 对它们应用以下规则。

**收到的消息。** `method` 和 `target` 保持线路上的拼写。`version` 为 `HTTP/1.0` 或 `HTTP/1.1`。`body` 保存去除分帧后的载荷，不保留分块边界和分块扩展。trailer 存放在 `trailers` 中，从不并入 `headers`。重复字段保持独立。收到的响应中，`content_type` 是第一个 `Content-Type` 的值（或为空），`chunked` 表示它是否以分块方式到达，`version` 是服务器的版本，`close` 表示连接是否在它之后结束。收到的响应可能包含 `Content-Length` 等受管字段；若要把这个值作为服务器响应转发出去，需先删除这些字段。

**写出的消息。** 字段名必须是 HTTP token，字段值不得包含 CR、LF、NUL 或除制表符以外的其他控制字符。`Content-Length`、`Transfer-Encoding`、`Connection` 和 `Trailer` 由 PACE 生成，不得出现在 `headers` 中；服务器响应还不得在 `headers` 中携带 `Content-Type`（请使用 `content_type`）。请求不得携带 `Expect` 或 `Upgrade`，也不会发送 `CONNECT`：这套 API 无法完成这些握手。HTTP/1.1 请求需要恰好一个合法的 `Host` 字段；一次性调用和 URL 客户端会自动添加。

- 请求的 `keep_alive` 生成 `Connection: keep-alive` 或 `Connection: close`。响应的 `close` 在该响应之后结束连接。
- `chunked = false` 时以 Content-Length 发送正文。`chunked = true` 时正文作为一个数据块发送（空正文则没有数据块），随后是结尾分块和 `trailers`。分块只改变分帧方式，正文仍是一整块缓冲区。
- trailer 需要 `chunked = true`。PACE 会校验它们，并写出列出其名字的 `Trailer` 字段。影响分帧、路由、连接控制、认证、Cookie 或内容处理的字段不能作为 trailer；`X-Checksum` 之类的普通字段可以。接收方同样拒绝被禁止的 trailer 以及 `Connection` 中点名的字段，即使它们没有被声明。
- 对 HTTP/1.0 请求，即使处理器要求 `chunked`，响应也使用 Content-Length；对 HTTP/1.0 请求返回带 trailer 的响应是无效的。
- 服务器总是写出 `HTTP/1.1` 状态行，常见状态码使用标准原因短语，其余使用 `Response`。

**HEAD 与无正文的状态码。** 对 `HEAD` 的应答携带正文本应具有的分帧字段（正文的 Content-Length，或设置了的 `representation_length`），但不带正文、分块和 trailer。204 和 304 没有正文，也没有 Content-Length 或 Transfer-Encoding；205 带 `Content-Length: 0`，没有正文。客户端对 `HEAD`、204 和 304 不读取正文，它们声明的较大长度也不计入 `max_body_bytes`。客户端拒绝带有 Content-Length 或 Transfer-Encoding 的 204，以及正文非空的 205。

**信息性响应。** 客户端最多跳过 `max_informational_responses` 个 1xx 响应，再多则失败；遇到 `101 Switching Protocols` 也会失败。收到 `100 Continue` 不会启动 Expect 握手：客户端总是先发完整个请求再读取。服务器从不发送 1xx 响应，对所有携带 `Expect` 的请求应答 417。

## 函数

本模块的函数在各分页中说明：[`serve`、`serve_connection`](server.zh.md)；[`async_get`、`async_fetch`、`async_post`、`get`、`post`、`fetch`](client.zh.md)；[`app`](app.zh.md)。

## 错误

双方都严格校验分帧。服务器以下表中的状态码应答错误输入并关闭连接；客户端抛出 `http::protocol_error` 并关闭连接。

| 输入 | 服务器应答 |
|:--|:--|
| 请求行、头部或分块语法错误；分块大小或 trailer 无效 | 400 |
| 重复的 Content-Length（即使值相同）、无效的 Content-Length | 400 |
| 同时有 Transfer-Encoding 和 Content-Length；重复的 Transfer-Encoding 字段 | 400 |
| `Connection` 点名 `Host`、分帧字段或 `Trailer` | 400 |
| HTTP/1.1 请求没有 `Host`，或 `Host` 重复、为空或无效 | 400 |
| 带 Transfer-Encoding 的 HTTP/1.0 请求 | 400 |
| 输入结束时消息不完整 | 仍能应答时为 400 |
| 单个 `chunked` 以外的传输编码 | 501 |
| `CONNECT` 请求或 `Upgrade` 字段 | 501 |
| `Expect` 字段，包括 `100-continue` | 417 |
| 正文或分块个数超限 | 413 |
| 头部、trailer 或分块行的字节数或字段数超限 | 431 |
| `HTTP/1.0`、`HTTP/1.1` 以外的版本 | 505 |

分块扩展会被校验（token 或带引号字符串形式的值），然后忽略。`Host` 只做语法检查；虚拟主机、认证和来源策略由应用负责。其他错误（应用输入无效、传输错误、取消）见[服务器](server.zh.md#错误)和[客户端](client.zh.md#错误)页面。

## 线程与并发

本模块的每个对象都绑定到运行它的 context，所有操作都在该 context 的线程上执行。服务器依次处理同一连接上的请求；不同连接并发运行，服务器跑在多个 worker 上时会分布在不同线程上，并共享处理器的状态。客户端同一时刻只允许一次交换。细节见[服务器](server.zh.md#线程与并发)和[客户端](client.zh.md#线程与并发)页面。

## 生命周期与所有权

消息是拥有数据的值。服务器拥有每条已接受的连接及其处理器副本；客户端拥有自己的传输。监听器、context 和 TLS 配置是借用的，必须比使用它们的 task 活得更久。具体规则见各页面。

## 限制与默认值

| `limits` 成员 | 默认值 |
|:--|:--|
| `max_header_bytes` | 16 KiB |
| `max_body_bytes` | 1 MiB |
| `max_requests_per_connection` | 1000 |
| `max_header_fields` | 100 |
| `max_trailer_bytes` | 8 KiB |
| `max_trailer_fields` | 32 |
| `max_chunk_line_bytes` | 1024 字节 |
| `max_chunks` | 65536 |
| `max_informational_responses` | 8 |
| 服务器状态码范围 | 200–599 |

服务器和客户端选项各有默认值，见[服务器](server.zh.md#限制与默认值)和[客户端](client.zh.md#限制与默认值)页面。

## 参见

- [HTTP 消息](../http/README.zh.md)、[路由](../http/router.zh.md)、[跨协议的 URL 客户端](../http/clients.zh.md)
- [网关服务](../gateway/README.zh.md)
- [TLS](../tls/README.zh.md)、[TCP](../net/tcp.zh.md)
- 教程：[HTTP 客户端](../../tutorial/02-http-client.zh.md)、[HTTP 服务器](../../tutorial/03-http-server.zh.md)；指南：[HTTP 实用做法](../../guides/http-recipes.zh.md)
- [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html)，HTTP/1.1 消息语法
