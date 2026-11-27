<!-- source-hash: 0685e11d8875 -->
# TLS（`pace/tls.hpp`）

[English](README.md) · [参考手册](../README.zh.md)

## 概述

TLS 模块借助 OpenSSL 在 TCP 流或任何其他 PACE 字节流上运行 TLS，支持带验证的客户端、按域名选择证书的服务器、
STARTTLS 以及按 ClientHello 路由。

| 头文件 | 目标 | 构建选项 |
|:--|:--|:--|
| `<pace/tls.hpp>` | `pace_tls` | `--tls=y` |

`<pace/tls.hpp>` 包含 `<pace/tls/stream.hpp>`，后者又包含 `<pace/net/tcp.hpp>`。该构建选项定义 `PACE_HAS_TLS` 并
链接 OpenSSL 的 `ssl` 和 `crypto`；关闭时头文件不声明任何内容，其他模块也不会因 TLS 而依赖 OpenSSL。启用
`--modules=y` 时，这些名字由 `import pace.tls;` 导出。

需要 OpenSSL 1.1.1 或更新版本；部署时请使用仍受支持的 OpenSSL 3.x。最低协议版本为 TLS 1.2；SSL、TLS 1.0 和 1.1、
压缩以及重新协商均被禁用。Linux、macOS、Windows 和 FreeBSD 共用同一个非阻塞 OpenSSL 状态机。

仓库中的示例 `examples/tutorial_tls.cpp` 使用 `tests/certs` 中的测试证书验证一条回环 TLS 连接。这些证书和私钥是
公开的测试素材，绝不能用作部署凭据；测试 CA 只加载到示例的客户端配置中。

```sh
xmake f -m release --tls=y --examples=y --tests=y -y
xmake -y
xmake run tutorial_tls
```

## 概要

```cpp
#include <pace/net.hpp>
#include <pace/runtime.hpp>
#include <pace/tls.hpp>

#include <iostream>

// 服务器：包装每个接受的 TCP 流并完成握手。
pace::task<void> serve_one(pace::tcp::listener& listener, const pace::tls::context& server)
{
    auto connection = co_await pace::tls::accept(co_await listener.accept(), server);
    co_await pace::write(connection, "hello\n");
    co_await connection.shutdown();
}

// 客户端：用系统信任库验证 "api.example.com" 的证书。
pace::task<void> client(pace::tcp::endpoint address)
{
    pace::tls::context configuration;
    configuration.set_alpn_protocols({"http/1.1"});
    auto connection = co_await pace::tls::connect(configuration, address, "api.example.com");
    std::cout << "ALPN: " << connection.selected_protocol() << '\n';
    std::cout << co_await pace::read_exact(connection, 6);
    co_await connection.shutdown();
}
```

## 类型

### 枚举与别名

```cpp
enum class role
{
    client,
    server
};
enum class trust_source
{
    system,
    application
};
enum class sni_policy
{
    use_default,
    reject
};
enum class pin_type
{
    leaf_certificate,
    subject_public_key_info
};
using sha256_digest = std::array<std::byte, 32>;
using verification_callback = std::function<bool(std::span<const std::string>)>;
using client_hello_callback = std::function<client_hello_action(const client_hello&)>;
```

| 名字 | 含义 |
|:--|:--|
| `role` | `context` 用于配置客户端还是服务器 |
| `trust_source` | `system` 加载 OpenSSL 默认信任路径；`application` 从空信任库开始 |
| `sni_policy` | 用于带 SNI 路由的服务器：`use_default` 提供默认证书，`reject` 使握手失败 |
| `pin_type` | 固定（pin）哪一项的哈希：叶证书的 DER 编码，或其 DER SubjectPublicKeyInfo |
| `sha256_digest` | 32 字节原始 SHA-256 值 |
| `verification_callback` | 按线上顺序接收对端的 DER 证书（叶证书在前）；返回 `true` 表示接受 |
| `client_hello_callback` | 检查服务器收到的 ClientHello 并决定如何处理 |

### `context`

```cpp
class context
{
public:
    explicit context(role mode = role::client, trust_source trust = trust_source::system);
    ~context();
    context(context&&) noexcept;
    context& operator=(context&&) noexcept;
    context(const context&) = delete;
    context& operator=(const context&) = delete;

    void load_verify_file(const std::string& pem_file);
    void load_verify_pem(std::string_view certificates);
    void set_verify_pin(pin_type type, sha256_digest digest);
    void set_verify_callback(verification_callback verifier);
    void insecure_skip_verify();
    void use_certificate_chain(const std::string& certificate_chain_file, const std::string& private_key_file);
    void set_alpn_protocols(const std::vector<std::string>& protocols);
    void add_server_name(std::string name, const context& configuration);
    void set_server_name_policy(sni_policy missing, sni_policy unrecognized);
    void set_client_hello_callback(client_hello_callback callback);
};
```

TLS 配置：角色、信任、验证策略、证书、ALPN 和按服务器名路由。它与 `io_context` 反应器无关，一份配置可以同时服务
多个反应器上的流。

**冻结。** 当第一个流由它构造、它被注册为 SNI 目标、或被 ClientHello 回调选中时，context 即被冻结。此后所有
setter 都抛出 `std::logic_error`，即使这些流已经关闭也一样；要修改配置请新建一个 context。setter 与冻结在内部是
串行化的。移动或销毁 `context` 对象不能与对该对象的其他使用竞争；被移走的 context 的 setter 会抛出
`std::logic_error`，也不能再创建流。

**共享所有权。** 流共享其配置以及整张 SNI 表的所有权。流创建之后，`context` 对象可以被移动或销毁：证书、信任根和
回调会一直存活，直到最后一个使用它们的流或 context 消失。

#### 信任

使用 `trust_source::system`（默认）构造客户端 context 时，构造函数会加载 OpenSSL 默认信任路径，失败则抛出
`std::system_error`。`trust_source::application` 从没有任何受信任根的状态开始，从不加载默认路径，因此即使 OpenSSL
默认路径不可用，也能配置客户端。客户端默认验证证书链以及预期的 DNS 名或 IP 地址。

- `load_verify_file(pem_file)` 添加某个 PEM 文件中的根证书。
- `load_verify_pem(certificates)` 添加一个或多个以空白分隔的内存 `CERTIFICATE` PEM 块。它先解析整个输入再添加，并把
  证书复制到 context 中，因此调用返回后即可释放输入。空输入、格式错误或混入其他内容的输入抛出
  `std::invalid_argument`（OpenSSL 拒绝某个块时抛出 `std::system_error`）；重复的证书会被接受。

```cpp
pace::tls::context client(pace::tls::role::client, pace::tls::trust_source::application);
client.load_verify_pem(application_ca_bundle); // 根证书被复制；证书链仍会验证。
```

PACE 不会导入 Windows 证书库或 macOS 钥匙串。请把要信任的平台证书导出为 PEM 后传入，或提供 CA 文件。无论哪种信任
来源，名字不符、根不受信任和证书过期都是握手错误。

#### 客户端验证模式

下面每个 setter 都用另一种模式取代 CA 验证。它们只用于客户端 context（服务器 context 会抛出
`std::invalid_argument`）；模式不需要系统信任时，请使用 `trust_source::application`。

- `set_verify_pin(pin_type::leaf_certificate, digest)` 在对端叶证书 DER 编码的 SHA-256 等于 `digest` 时接受对端；
  `pin_type::subject_public_key_info` 则对叶证书的 DER SubjectPublicKeyInfo 求哈希。pin 匹配时，不受信任或自签名的
  叶证书也会被接受：pin 取代了证书链和有效期检查。
- `set_verify_callback(verifier)` 把对端未经验证的 DER 证书（叶证书在前）交给 `verifier`，由它决定是否信任该证书链，
  并可实现自己的 CA 和 pin 检查。返回 `false` 或抛出异常会使握手失败。证书只在调用期间借用。回调在流的反应器上同步
  执行；context 被多个反应器共享时会并发执行。空函数抛出 `std::invalid_argument`。
- `insecure_skip_verify()` 关闭证书和名字验证，仅用于测试和互通：连接是加密的，但对端未经认证。

向 `connect` 或 `handshake` 传入非空对端名时，pin 和回调模式仍会检查证书的 DNS 或 IP 身份；传入空的对端名则只依赖
pin 或回调。

```cpp
pace::tls::context client(pace::tls::role::client, pace::tls::trust_source::application);
client.set_verify_pin(pace::tls::pin_type::subject_public_key_info, expected_spki_sha256);
pace::tls::stream connection(io, client);
co_await connection.connect({"192.0.2.1", 443}, "", "relay.example.com");
```

#### 服务器证书

`use_certificate_chain(certificate_chain_file, private_key_file)` 加载一个 PEM 证书链（叶证书在前，然后是中间证书）
及其未加密的 PEM 私钥。加载是同步的，会先检查私钥与证书匹配再安装，因此私钥缺失或不匹配时，之前有效的证书对保持
不变。加密私钥会直接失败，不会提示输入口令。服务器提供这张证书，除非 SNI 路由或 ClientHello 回调选择了另一个
context。本 API 不请求客户端证书。

#### ALPN

`set_alpn_protocols(protocols)` 设置客户端要提供的协议，或服务器按偏好顺序接受的协议。每个标识为 1–255 字节，编码
后的列表最多 65535 字节，否则抛出 `std::invalid_argument`。服务器选择自己列表中第一个客户端也提供的协议；双方没有
交集时握手失败。空列表关闭 ALPN；不发送 ALPN 的对端可以完成握手，但不会选中任何协议。客户端在使用某个协议之前必须
检查 `selected_protocol()`（例如 `== "h2"`）；ALPN 本身并不实现该协议。

#### 按服务器名路由（SNI）

```cpp
pace::tls::context domain_a(pace::tls::role::server);
domain_a.use_certificate_chain("a-fullchain.pem", "a-key.pem");
domain_a.set_alpn_protocols({"http/1.1"});
pace::tls::context domain_b(pace::tls::role::server);
domain_b.use_certificate_chain("b-fullchain.pem", "b-key.pem");
domain_b.set_alpn_protocols({"http/1.1"});

pace::tls::context server(pace::tls::role::server);
server.use_certificate_chain("default-fullchain.pem", "default-key.pem");
server.set_alpn_protocols({"http/1.1"});
server.add_server_name("a.example", domain_a); // 冻结并持有 domain_a。
server.add_server_name("b.example", domain_b);
server.set_server_name_policy(pace::tls::sni_policy::use_default, pace::tls::sni_policy::reject);
// 缺少 SNI 时使用默认证书；未知 SNI 使本次握手失败。
```

`add_server_name(name, configuration)` 把 SNI 等于 `name` 的 ClientHello 路由到服务器 context `configuration`，由它为
该连接提供证书链和 ALPN 策略。名字精确匹配，忽略 ASCII 大小写；请使用 ASCII 标签（国际化域名使用 A-label）。通配符、
IP 字面量、末尾的点、空标签或无效标签、重复的名字、客户端 context、context 自身，以及自带 SNI 表的目标都会抛出
`std::invalid_argument`。一个目标可以服务多个名字或多个路由 context。

`set_server_name_policy(missing, unrecognized)` 分别决定没有 SNI 和名字未知时的处理方式，两者初始都是
`use_default`。对端名为 IP 地址的客户端不发送 SNI，按 `missing` 处理。格式错误的 SNI 会被拒绝。在 `use_default` 下，
客户端仍会用它请求的名字检查默认证书。选中的目标无法完成握手时，该连接失败，绝不会回退到默认证书。仅用于服务器
context。

选择在所属反应器的握手过程中同步进行。它只查询已冻结的表，握手期间从不修改共享的 context，也不加载文件。它同样适用
于隐式 TLS 和通过 STARTTLS 升级的 TCP 流。带 SNI 表的 context 会关闭会话缓存和会话票据（包括 TLS 1.3 票据），因为
OpenSSL 的 servername 回调在会话恢复之后才运行，会话恢复会绕过按连接的选择。

#### ClientHello 回调

`set_client_hello_callback(callback)` 安装一个服务器端回调，它对每个连接只在第一个 ClientHello 上运行一次，早于 SNI
和证书选择。它在反应器上同步执行；context 被多个反应器共享时会并发执行。返回的 `client_hello_action` 决定下一步；
抛出异常会中止握手。空函数或客户端 context 抛出 `std::invalid_argument`。带此回调的 context 会关闭会话恢复和票据，
使每个连接都重新选择证书。

```cpp
server.set_client_hello_callback(
    [](const pace::tls::client_hello& hello)
    {
        if (hello.server_name == "upstream.example.com")
        {
            return pace::tls::client_hello_action::handoff();
        }
        return pace::tls::client_hello_action::proceed();
    }
);
```

### `client_hello` 与 `client_hello_key_share`

```cpp
struct client_hello_key_share
{
    std::uint16_t group = 0;
    std::string_view key_exchange;
};

struct client_hello
{
    std::string_view server_name;
    std::vector<std::string_view> alpn_protocols;
    std::vector<std::uint16_t> supported_versions;
    std::vector<std::uint16_t> cipher_suites;
    std::vector<client_hello_key_share> key_shares;
    std::uint16_t legacy_version = 0;
    std::string_view session_id;
    std::string_view raw_message;
};
```

传给回调的已解析 ClientHello。各列表保持线上顺序和线上 ID。缺少 SNI、ALPN 或 supported_versions 扩展时，对应字段为
空。`raw_message` 是完整的握手消息，包含四字节的握手头；它可能跨越多个 TLS 记录，不含记录头。所有视图借用的内存
只在回调返回前有效。

### `client_hello_action`

```cpp
class client_hello_action
{
public:
    static client_hello_action proceed() noexcept;
    static client_hello_action select(const context& configuration);
    static client_hello_action reject() noexcept;
    static client_hello_action handoff() noexcept;
};
```

| 动作 | 效果 |
|:--|:--|
| `proceed()` | 按此 context 正常的 SNI 策略和证书继续 |
| `select(configuration)` | 用服务器 context `configuration`（被冻结并持有）提供证书和 ALPN。优先于 SNI 路由；握手失败会关闭连接 |
| `reject()` | 发送致命的握手告警 |
| `handoff()` | 在发送任何内容之前停下，以便 `handshake_with_handoff()` 交还传输层 |

以客户端 context 调用 `select` 会抛出 `std::invalid_argument`。

### `client_hello_handoff`

```cpp
template <class Transport>
struct client_hello_handoff
{
    Transport transport;
    std::string pre_read;
};
```

移交（handoff）的结果：原始的、仍然打开的传输层，以及到目前为止从中读到的全部字节（按线上顺序），包括构造函数的
`pre_read` 前缀和 ClientHello 之后多读的字节。先处理 `pre_read`，再从 `transport` 继续读取。

### `stream`

```cpp
class stream
{
public:
    stream(io_context& io, const tls::context& configuration);
    stream(tcp::stream transport, const tls::context& configuration);
    stream(tcp::stream transport, const tls::context& configuration, std::string_view pre_read);
    template <tls_byte_stream Transport>
    requires(!std::same_as<Transport, tcp::stream>)
    stream(Transport transport, const tls::context& configuration, std::string_view pre_read = {});
    ~stream();
    stream(stream&&) noexcept;
    stream& operator=(stream&&) noexcept;
    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    task<void> connect(tcp::endpoint endpoint, std::string peer_name);
    task<void> connect(tcp::endpoint endpoint, std::string peer_name, std::string server_name);
    task<void> handshake(std::string peer_name = {});
    task<void> handshake(std::string peer_name, std::string server_name);
    template <tls_byte_stream Transport = tcp::stream>
    task<std::optional<client_hello_handoff<Transport>>> handshake_with_handoff();

    task<std::size_t> read_some(std::span<char> buffer);
    task<std::size_t> read_some(std::span<std::byte> buffer);
    task<void> write_all(std::string_view bytes);
    task<void> write_all(std::span<const std::byte> bytes);

    task<void> shutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
    task<void> close_gracefully(std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
    void close() noexcept;
    void set_no_delay(bool enabled = true);
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] io_context& context() const noexcept;
    [[nodiscard]] std::string selected_protocol() const;
};
```

在其拥有的传输层之上运行的、只能移动的 TLS 会话。构造会冻结配置。

#### 构造

| 构造函数 | 传输层 |
|:--|:--|
| `stream(io, configuration)` | `io` 上一个新的、未连接的 TCP 套接字；随后调用 `connect` |
| `stream(tcp_stream, configuration)` | 移入一个已接受或已连接的 TCP 流；随后调用 `handshake` |
| `stream(tcp_stream, configuration, pre_read)` | 同上，用于已从套接字读走部分 TLS 字节的情况 |
| `stream(transport, configuration, pre_read = {})` | 移入任何其他满足 [`tls_byte_stream`](#自定义传输层) 的已连接字节流 |

传输层移入之后，不要再用它做明文 I/O。`pre_read` 用于协议探测或 STARTTLS 协商时已经读走的 TLS 字节：流在构造函数
返回前复制这些字节，并在从传输层读取之前按线上顺序重放它们。这段前缀可以包含完整的 ClientHello，也可以在某个记录
中间结束。

#### 握手

```cpp
task<void> connect(tcp::endpoint endpoint, std::string peer_name);
task<void> connect(tcp::endpoint endpoint, std::string peer_name, std::string server_name);
task<void> handshake(std::string peer_name = {});
task<void> handshake(std::string peer_name, std::string server_name);
```

`connect`（客户端，仅限基于 TCP 的流）把 TCP 套接字连接到数字地址 `endpoint` 并完成握手。`handshake` 只在已连接的
传输层上完成握手：客户端传入对端名，服务器不传。

`peer_name` 是证书中预期的身份，与地址无关：DNS 名会与证书中的 DNS 名比对，同时作为 SNI 发送；IP 地址会与证书中的
IP 地址比对，不发送 SNI。CA 验证要求 `peer_name` 非空。带 `server_name` 的重载单独指定作为 SNI 发送的 ASCII DNS 名，
`server_name` 为空时不发送 SNI。主机名请先用 [DNS 模块](../dns/README.zh.md)解析。

```cpp
pace::tls::context configuration; // 默认是进行验证的客户端。
pace::tls::stream connection(io, configuration);
co_await connection.connect({"192.0.2.1", 443}, "api.example.com");
```

对于 STARTTLS，先在 `tcp::stream` 上完成明文协商，再把它移入 `tls::stream`（已读走的 TLS 字节作为 `pre_read`），然后
客户端调用 `handshake(peer_name)`，服务器调用 `handshake()`：

```cpp
pace::tls::stream connection(co_await listener.accept(), server_configuration);
co_await connection.handshake();
```

握手在传输层就绪时挂起。握手失败或被取消都会关闭传输层。握手本身没有超时，请使用
[截止时间](../runtime/structured-concurrency.zh.md)或关闭流。

#### `handshake_with_handoff`

```cpp
template <tls_byte_stream Transport = tcp::stream>
task<std::optional<client_hello_handoff<Transport>>> handshake_with_handoff();
```

支持 `client_hello_action::handoff()` 的服务器握手。TLS 握手完成时返回空的 optional。发生移交时，返回原始传输层和
到目前为止读到的全部字节，并使本流成为被移走的状态。`Transport` 默认为 `tcp::stream`；其他传输层请写出构造时传入的
类型。在客户端 context 上，或 `Transport` 类型不符时，抛出 `std::logic_error`。在 `handshake()` 和 `tls::accept()`
中，移交请求则会关闭传输层并抛出协议错误。只要 ClientHello 回调还可能要求移交，读到的数据最多捕获 1 MiB；超过则
握手失败。

```cpp
pace::tls::stream session(co_await listener.accept(), server);
if (auto handoff = co_await session.handshake_with_handoff())
{
    // 先按线上顺序处理 pre_read，再从传输层继续读取。
    co_await route_to_upstream(std::move(handoff->transport), std::move(handoff->pre_read));
}
else
{
    co_await handle_https(session);
}
```

#### 读取与写入

```cpp
task<std::size_t> read_some(std::span<char> buffer);
task<std::size_t> read_some(std::span<std::byte> buffer);
task<void> write_all(std::string_view bytes);
task<void> write_all(std::span<const std::byte> bytes);
```

握手成功后才可使用（之前调用抛出 `std::logic_error`）。`read_some` 返回解密后的字节，可能少于请求的数量。缓冲区
非空时，`0` 表示对端发送了经过验证的 `close_notify`；没有该告警的传输层 EOF 属于协议错误，因此被截断的 TLS 流量
不会被当作正常结束。缓冲区为空时不读取，直接返回 `0`。`write_all` 加密并发送整个视图，处理部分写入，在密文交给
传输层后完成。缓冲区在操作完成前一直被借用。通用的 [`pace::write` 和 `pace::read_exact`](../net/README.zh.md#函数)
可用于 `tls::stream`。

#### 关闭

```cpp
task<void> shutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
task<void> close_gracefully(std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
void close() noexcept;
```

`shutdown` 发送 `close_notify`，等待对端的 `close_notify`，然后关闭传输层。`close_gracefully` 是同一个操作，只是沿用
`tcp::stream` 的名字。它要求握手已完成（否则抛出 `std::logic_error`；要放弃一个从未完成握手的连接，请用 `close()`）。

- 一个可取消的反应器定时器约束整个操作；达到 `timeout` 时正常关闭，但不保证对端收到任何内容。TLS 错误或取消会关闭
  传输层并传播。
- `timeout` 不为正数时，在 TCP 传输层上尝试一次，其他传输层则立即关闭。
- 对于带 `close()` 的自定义传输层，如果它有 `shutdown(std::chrono::milliseconds) -> task<void>` 成员，会在 `close()`
  之前调用。QUIC 风格的传输层用 `finish_write()` 发送 FIN，并等待对端的 FIN。
- 关闭期间不能有其他 TLS 操作。关闭结束后定时器立即移除。

`close()` 立即关闭或复位传输层并取消等待；它不发送 `close_notify`，也不等待。挂起的操作随后以错误结束，因此在它们
结束之前保持 `stream` 存活。析构函数也会立即关闭。

#### 访问器

| 成员 | 结果 |
|:--|:--|
| `context()` | 传输层的反应器；被移动后的流返回它原先所属的反应器 |
| `is_open()` | 传输层是否打开；不表示握手是否成功。被移走的流返回 `false` |
| `selected_protocol()` | 协商出的 ALPN 协议的自有副本，或空字符串 |
| `set_no_delay(enabled)` | 在 TCP 传输层上设置 `TCP_NODELAY`；其他传输层抛出 `std::logic_error` |

移动之后，源对象只支持析构、赋值、`close()` 和 `is_open()`；其他成员抛出 `std::logic_error`（且不能调用
`context()`）。

### 自定义传输层

```cpp
template <class Transport>
concept tls_closeable = requires(Transport& transport) {
    { transport.close() } noexcept -> std::same_as<void>;
};

template <class Transport>
concept tls_resettable = requires(Transport& transport) {
    { transport.finish_write() } -> std::same_as<void>;
    { transport.reset() } noexcept -> std::same_as<void>;
};

template <class Transport>
concept tls_byte_stream =
    (tls_closeable<Transport> || tls_resettable<Transport>)
    && requires(Transport& transport, const Transport& owner, std::span<char> input, std::string_view output) {
           { owner.context() } noexcept -> std::same_as<io_context&>;
           { transport.read_some(input) } -> std::same_as<task<std::size_t>>;
           { transport.write_all(output) } -> std::same_as<task<void>>;
       };
```

`tls::stream` 可以运行在任何满足 `tls_byte_stream` 的已连接、有序、双向字节流之上：双向的 `quic::stream`、另一个
`tls::stream`（嵌套 TLS），或应用自己的适配器。流拥有传给它的传输层。要求如下：

- `context()` 返回反应器；传输层在整个生命周期内都留在该反应器上。
- `read_some` 只在 EOF 时返回 `0`；`write_all` 在接受全部字节后才完成。
- 提供 `close() noexcept`，或者 QUIC 风格的一对 `finish_write()` 和 `reset() noexcept`。关闭或复位必须以
  `operation_canceled` 取消未完成的操作。
- 可选：`is_open() const noexcept` 让 TLS 能感知传输层已关闭。
- 关闭期间，TLS 可能同时进行一次传输层读和一次传输层写，以便在缓冲区已满时交换告警。串行化 I/O 的适配器可以声明
  返回 `false` 的 `bool supports_concurrent_io() const noexcept`。

```cpp
auto bytes = co_await quic_connection.open_stream(pace::quic::stream_type::bidirectional);
pace::tls::stream inner(std::move(bytes), tls_configuration);
co_await inner.handshake("relay.example.com");
```

在 TLS 流的整个生命周期内，保持 QUIC 传输层的父连接存活。密文经由有界的 64 KiB 缓冲区传递。这类传输层上的服务器
不发送 TLS 1.3 会话票据，使有界的传输层在握手后即可传输应用数据。`connect()` 和 `set_no_delay()` 需要 TCP 传输层，
PACE 基于 TLS 的 HTTP/2 驱动也需要。嵌套 TLS 中，每个会话依次处理记录。

头文件中的 `tls_transport` 和 `tls_transport_model` 是该构造函数背后的类型擦除实现，不属于 API，也不由
`import pace.tls;` 导出。

## 函数

### `connect`

```cpp
task<stream> connect(io_context& io, const context& configuration, tcp::endpoint peer, std::string expected_name);
task<stream> connect(const context& configuration, tcp::endpoint peer, std::string expected_name);
task<stream> connect(io_context& io, tcp::endpoint peer, std::string expected_name);
task<stream> connect(tcp::endpoint peer, std::string expected_name);
```

创建一个流，连接到数字地址 `peer`，完成握手并检查 `expected_name`（含义同上文的 `peer_name`），然后返回该流。不带
`configuration` 的重载使用一个系统信任、进行 CA 与名字验证、不设 ALPN 的客户端 context。传入的 `configuration` 在
任务完成前被借用，因此保存起来的惰性任务不能引用已销毁的 context；返回的流共享该配置的状态。不带 `io` 的重载使用
运行该任务的上下文，在任务开始时读取。

### `accept`

```cpp
task<stream> accept(tcp::stream transport, const context& configuration);
```

把一个已接受的 TCP 流（由任务拥有）包装成服务器流并完成握手。`configuration` 在任务完成前被借用。

## 错误

| 情形 | 异常 |
|:--|:--|
| 握手失败、证书或名字不符、非正常 EOF、OpenSSL 错误 | `std::system_error`，错误码 `std::errc::protocol_error`，附 OpenSSL 诊断文本 |
| 加载默认信任路径、信任文件或证书失败 | `std::system_error`（`protocol_error`） |
| `connect` 中的 TCP 连接失败 | TCP 的 `std::system_error`，原样传播 |
| 参数无效：路径为空、PEM 错误、ALPN 列表错误、SNI 名无效、回调为空、CA 客户端没有对端名、服务器握手传入了对端名 | `std::invalid_argument` |
| 在服务器 context 上设置验证模式；在客户端 context 上设置 SNI 或 ClientHello | `std::invalid_argument` |
| 在已冻结或被移走的 context 上调用 setter | `std::logic_error` |
| 已有操作进行时启动第二个操作 | `std::logic_error`；正在进行的操作不受影响 |
| 握手前读、写或关闭；第二次握手；在非 TCP 流上或以服务器 context `connect`；在非 TCP 流上 `set_no_delay`；移交用法错误；被移走的流 | `std::logic_error` |
| 在传输层所属上下文之外执行操作 | `std::logic_error` |
| 上下文停止、流被关闭、任务被取消 | `std::system_error`（`operation_canceled`） |
| 等待方任务的截止时间到期 | `std::system_error`（`timed_out`） |
| 在已关闭的传输层上操作 | `std::system_error`（`bad_file_descriptor`） |

操作失败会关闭传输层，被拒绝的重叠操作除外。请在连接自己的任务里捕获每个连接的错误，避免一个对端结束整个服务器。

## 线程与并发

- 所有流操作都在拥有传输层的反应器上执行。`close()` 和析构也必须在那里执行，或在它未运行时执行。
- **每个流同一时间只能有一个异步操作**，读、写、握手和关闭都算在内。OpenSSL 的任何操作都可能需要另一个方向，因此
  TCP 那种“一读一写”的规则在这里不适用；独立的读任务和写任务不能共享一个 TLS 流。
- 一个 `context` 可以同时被多个反应器上的流使用，此时其验证回调和 ClientHello 回调会并发执行，必须是线程安全的。
  请在第一个流创建之前，在一个线程上完成配置。

## 生命周期与所有权

- 流拥有其传输层，并共享其配置的所有权。流创建之后，`context` 对象本身可以销毁，但借用它的 `tls::connect` 或
  `tls::accept` 任务仍在运行时除外。
- 在每个操作完成之前，保持流的地址不变、其反应器存活、缓冲区有效。只在空闲时移动流。
- `pre_read` 由构造函数复制。ClientHello 视图和验证回调收到的证书只在回调期间有效。
- 移交会把传输层及其字节交给调用方，并使流成为被移走的状态。

## 限制与默认值

| 项目 | 默认值或上限 |
|:--|:--|
| `context` 角色 / 信任来源 | `client` / `system` |
| 客户端验证 | CA 证书链加 DNS 名或 IP |
| 协议版本 | TLS 1.2 和 1.3 |
| SNI 策略 | 缺失和未知均为 `use_default` |
| ALPN 标识长度 / 编码后列表 | 1–255 字节 / 最多 65535 字节 |
| SNI 名长度 | 最多 253 字节，每个标签最多 63 字节 |
| `shutdown` / `close_gracefully` 超时 | 100 ms |
| ClientHello 决定之前的移交读取捕获 | 1 MiB |
| 非 TCP 传输层上的密文缓冲区 | 每个方向 64 KiB |
| 每个流同时进行的操作 | 1 |
| 握手超时 | 无；请使用截止时间 |

## 参见

- [网络](../net/README.zh.md)、[TCP](../net/tcp.zh.md)、[DNS](../dns/README.zh.md)
- [QUIC](../quic/README.zh.md)、[WebSocket](../websocket/README.zh.md)、[HTTP/1](../http1/README.zh.md)、
  [HTTP/2](../http2/README.zh.md)
- 截止时间见[结构化并发](../runtime/structured-concurrency.zh.md)
- 教程：[TLS 与 DNS](../../tutorial/10-tls-and-dns.zh.md)
- 指南：[HTTP 实践](../../guides/http-recipes.zh.md)
- OpenSSL：[SNI 回调约定](https://docs.openssl.org/3.0/man3/SSL_CTX_set_tlsext_servername_callback/)
