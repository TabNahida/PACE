# TLS 使用指南

简化写法与拥有数据的 helper：[组件便捷接口](component-conveniences.zh-CN.md)。

[English API reference](tls.md) · [WebSocket](websocket.zh-CN.md) · [完整示例](../../examples/tutorial_tls.cpp)

启用 `--tls=y` 后包含 `<pace/tls.hpp>`，使用 OpenSSL 1.1.1 以上版本；部署时
建议采用仍受支持的 OpenSSL 3.x。构建会导出 `PACE_HAS_TLS` 并链接 ssl/crypto。
关闭该选项时，基础网络库不依赖 OpenSSL。最低协议为 TLS 1.2，禁用压缩和重新
协商。Linux、macOS、Windows、FreeBSD 共用非阻塞 TLS 状态机。

```sh
xmake f -m release --tls=y --examples=y --tests=y -y
xmake -y
xmake test -v
xmake run tutorial_tls
```

示例证书和私钥是公开测试素材，只用于回环演示，不能作为部署凭据。

## 配置与连接

```cpp
pace::tls::context config;                         // 默认是客户端配置
config.load_verify_file("private-ca.pem");         // 可选：附加受信任 CA
config.set_alpn_protocols({"http/1.1"});            // 可选：应用协议协商
pace::tls::stream socket(io, config);
co_await socket.connect({"192.0.2.1", 443}, "api.example.com");
```

示例地址保留用于文档，实际应替换为解析后的数字 IPv4 / IPv6 地址。TCP 地址和预期
证书身份分开传入。默认验证证书链和非空的 DNS/IP 身份。DNS 身份默认也用于
SNI，数字 IP 则检查 IP SAN。可通过 `connect(endpoint, peer_name, server_name)`
或 `handshake(peer_name, server_name)` 独立指定 ASCII DNS SNI，传入空的
`server_name` 可关闭 SNI。默认客户端加载 OpenSSL 信任路径，`load_verify_file`
可追加 CA。
TLS 接口不会自动解析域名，可先使用独立的 [DNS 扩展](dns.md)。

应用自己管理信任根时，可以完全跳过默认路径加载：

```cpp
pace::tls::context client(pace::tls::role::client,
                          pace::tls::trust_source::application);
client.load_verify_pem(application_ca_bundle);
pace::tls::stream socket(io, client);
co_await socket.connect({"192.0.2.1", 443}, "api.example.com");
```

`trust_source::application` 从空信任库开始，**不会调用默认信任路径加载**，即使
OpenSSL 默认路径配置失败也可构造客户端，再添加明确指定的 CA。缺少适用的
信任根时握手仍会失败。默认 `trust_source::system` 则保持加载失败即抛错。

`load_verify_pem` 接受一个或多个 `CERTIFICATE` PEM 块，以空白分隔，解析整个
输入后才添加证书。空输入、损坏或混入其他内容的 bundle 会抛错；重复证书可
重复添加。证书保存在配置拥有的信任库中，调用返回后输入内存即可释放，无需
临时文件。`load_verify_file` 仍可加载显式文件。两种方式都为默认的 CA 验证提供
信任根。PACE 不自动导入 Windows 证书库或 macOS Keychain；应用可以将
所选平台证书编码为内存 PEM 后传入。

```cpp
pace::tls::context config(pace::tls::role::server);
config.use_certificate_chain("chain.pem", "private-key.pem");
pace::tls::stream socket(co_await listener.accept(), config);
co_await socket.handshake();
```

服务端 chain 文件以叶证书开头，后接中间证书；私钥必须匹配且为未加密 PEM。
证书和私钥会先完整检查再安装，缺失或不匹配的私钥不会替换已有的有效证书。
加密私钥会失败，不会从终端提示输入密码。当前不支持要求客户端证书的 mTLS。

配置在首次构造流、被注册为 SNI 目标或被 ClientHello 回调选中时冻结，后续修改
抛出 `logic_error`，即使流已关闭也不会解冻。修改配置时请创建新的 context。配置修改与冻结在内部串行
化，冻结后的配置可供多个 reactor 创建各自独立的流。移动、销毁公开 context
对象不能与对同一对象的访问并发发生。

流共享配置及其整个 SNI 映射表的所有权，公开配置对象随后可以移动或销毁，
证书、信任根与回调状态一直保留到最后一个拥有它们的流或配置销毁。

ALPN 服务端按自身列表顺序选择与客户端的共同协议。客户端提供列表但没有交集
时握手失败；没有提供 ALPN 的对端可以成功且结果为空。HTTP/2 应主动检查
`selected_protocol() == "h2"`。ALPN 只负责协商，不实现对应应用协议。

## 客户端验证模式

在构造 TLS 流前选择验证模式。每次设置都会选定一种模式。无需系统信任库时，
可用 `trust_source::application` 构造配置。

- `set_verify_pin(pin_type::leaf_certificate, digest)` 比较叶证书 DER 的 SHA-256。
  `pin_type::subject_public_key_info` 比较完整 DER SPKI 的 SHA-256。两者均接收
  32 个原始摘要字节，以匹配的固定值替代证书链和有效期校验，可用于自签名证书。
- `set_verify_callback(verifier)` 将对端提供的 DER 证书按发送顺序传给应用，
  首项为叶证书。应用决定是否信任该链，可以组合自己的 CA 和公钥检查。返回
  `false` 或抛出异常会拒绝握手。证书数据只在调用期间有效。回调在所属 reactor
  同步执行，同一配置供多个 reactor 使用时，回调必须支持并发调用。
- `insecure_skip_verify()` 关闭证书和身份校验，用于测试和互操作。连接加密，
  对端身份未经认证。

固定摘要和自定义验证模式也会检查显式提供的 DNS/IP `peer_name`。传入空串可由
固定摘要或应用策略决定身份。

```cpp
pace::tls::context client(pace::tls::role::client,
                          pace::tls::trust_source::application);
client.set_verify_pin(pace::tls::pin_type::subject_public_key_info, expected_spki_sha256);
pace::tls::stream connection(io, client);
co_await connection.connect({"192.0.2.1", 443}, "", "relay.example.com");
```

## 多域名独立证书与 SNI

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
server.add_server_name("a.example", domain_a); // 保活并冻结 domain_a。
server.add_server_name("b.example", domain_b);
server.set_server_name_policy(pace::tls::sni_policy::use_default,
                              pace::tls::sni_policy::reject);
// 缺少 SNI 使用默认配置；未知 SNI 拒绝该握手。
```

域名精确匹配，不区分 ASCII 大小写；国际化域名应先转换成 A-label。通配符、IP
字面值、结尾点、空或无效 label、重复名称、客户端目标和自引用均被拒绝。目标
不能再包含 SNI 映射，但同一目标可被多个名称或入口配置共享。

缺失和未知 SNI 的策略分别设置，默认均为 `use_default`；数字 IP 客户端身份不
发送 SNI，按缺失策略处理。格式非法的 SNI 被拒绝。回退时客户端仍检查默认
证书是否匹配其预期身份。**已明确选中的目标握手失败只关闭该连接，绝不回退**；
服务端应捕获此连接的异常，继续接收其它连接。

选择发生在所属 reactor 的握手中，只查冻结映射并切换当前连接的 SSL 上下文，
不会修改共享默认配置或在握手时读取证书文件。所选目标提供自己的证书链和
ALPN 策略；隐式 TLS 与 STARTTLS 使用完全相同的选择机制。

配置了 SNI 映射或策略的入口禁用会话缓存和 ticket（包括 TLS 1.3），避免会话
恢复绕过连接的域名选择。OpenSSL 的 servername callback 晚于会话恢复、早于
ALPN 选择，详见 [OpenSSL 官方语义](https://docs.openssl.org/3.0/man3/SSL_CTX_set_tlsext_servername_callback/)。

STARTTLS 应先在 TCP 流完成明文协议协商，再将其移动进 `tls::stream`：客户端
调用 `handshake(peer_name)`，服务端调用 `handshake()`。如果协议识别或协商已经
从 TCP 流读走了一些 TLS 字节，可以按线上顺序将它们作为第三个参数 `pre_read`
传给 `tls::stream(std::move(transport), config, pre_read)`。构造函数返回前会复制
这些字节，TLS 流会先回放它们，再从 TCP 读取。前缀可以包含完整 ClientHello，
也可以在 TLS record 中间结束。

## 检查 ClientHello 并路由

服务端在构造流前通过 `context::set_client_hello_callback` 注册回调。回调在所属
reactor 上同步执行，每条连接仅检查首次 ClientHello，早于证书选择。共享 context
时，多个 reactor 可以并发调用。回调可读取 SNI、ALPN 列表、支持版本、密码套件 ID、
密钥份额、旧版协议字段、会话 ID，以及带四字节握手头的原始握手消息。列表保留
线上顺序，所有 view 仅在回调期间有效。原始消息跨 TLS record 拼接，省略各 record 头。

- `client_hello_action::proceed()` 继续使用入口配置的 SNI 策略。
- `client_hello_action::select(context)` 冻结并持有所选服务端配置，使用其证书和
  ALPN。此选择优先于 SNI 路由，握手失败时关闭该连接。
- `client_hello_action::reject()` 发送致命握手告警。回调抛出异常也会使握手失败。
- `client_hello_action::handoff()` 在发出响应前暂停 TLS。通过
  `handshake_with_handoff()` 取回原传输。

```cpp
server.set_client_hello_callback([](const pace::tls::client_hello& hello) {
    if (hello.server_name == "upstream.example.com")
        return pace::tls::client_hello_action::handoff();
    return pace::tls::client_hello_action::proceed();
});

pace::tls::stream session(co_await listener.accept(), server);
if (auto handoff = co_await session.handshake_with_handoff()) {
    // 先按线上顺序处理 pre_read，再继续从底层传输读取。
    co_await route_to_upstream(std::move(handoff->transport), std::move(handoff->pre_read));
} else {
    co_await handle_https(session);
}
```

`handshake_with_handoff<Transport>()` 默认传输类型为 `tcp::stream`，传入 QUIC 流或
自定义传输时须指定构造时的类型。空结果表示 TLS 握手已完成。交还结果接管仍然
打开的原传输及全部已读字节，包括构造时传入的前缀和越过 ClientHello 的预读字节，
TLS 流进入移出状态。回调执行前额外缓存的字节上限为 1 MiB。若使用已有的
`handshake()` 或 `accept()`，收到交还请求时会静默关闭传输并报告协议错误。
配置此回调的入口禁用会话恢复和票据，每条连接分别选择证书。

## 在异步字节流上运行 TLS

`tls::stream(transport, configuration, pre_read = {})` 也可以接管已连接的有序
双向字节流。可移动传入 QUIC 双向流、已完成外层握手的 `tls::stream`，或应用
自己的字节流适配器。随后客户端调用 `handshake(peer_name)`，服务端调用
`handshake()`。如果已读走部分 TLS 字节，可通过 `pre_read` 按发送顺序回放。

```cpp
auto bytes = co_await quic_connection.open_stream(pace::quic::stream_type::bidirectional);
pace::tls::stream inner(std::move(bytes), tls_configuration);
co_await inner.handshake("relay.example.com");
```

QUIC 父连接必须活到 TLS 流使用结束。自定义传输提供
`context() const noexcept` 并返回 `io_context&`，通过
`read_some(std::span<char>) -> task<std::size_t>` 读取、
`write_all(std::string_view) -> task<void>` 写入。读到字节流末尾时返回零，写入
接受全部字节后返回。传输还需提供 `close() noexcept`，或同时提供 QUIC 风格的
`finish_write()` 和 `reset() noexcept`。关闭或重置应以 `operation_canceled`
取消挂起操作。可选的 `is_open() const noexcept` 让 TLS 观察底层关闭。
关闭期间，TLS 可以同时进行一次底层读取和一次底层写入，使缓冲区满时仍能交换
关闭通知。要求串行 I/O 的适配器可实现 `bool supports_concurrent_io() const noexcept`
并返回 `false`。嵌套 TLS 的每一层都按顺序处理 TLS 记录。最底层传输可以在各层读取对端关闭通知时并发发送密文。

TLS 使用有界缓冲区收发密文，传输和 TLS 操作均在同一 reactor 上执行。
使用异步传输的服务端会关闭 TLS 1.3 会话票据，让有界传输在握手后能直接交换应用数据。
`connect()`、`set_no_delay()` 和 HTTP/2 的 TLS 多路复用驱动要求直接使用 TCP。

## 读写与并发

`read_some(span<char/byte>)` 读取解密数据；`write_all(string_view/span<const byte>)`
处理全部明文字节的加密写入。缓冲区和 view 在整个挂起期间都借用应用内存。
非空读取返回零只代表验证过的 `close_notify`，传输直接到达 EOF 时会视为截断错误。
空缓冲区直接返回零。

**同一 TLS 流同时只能执行一个异步操作，读写不能重叠。** 握手和关闭也遵守
这一限制，重叠抛出 `logic_error`，不影响原操作。这是当前明确的 API 边界，
适用于顺序请求/响应；不能将独立 reader/writer 协程同时挂在一个 TLS 流上。
OpenSSL 的读写操作都可能要求等待任一方向的就绪，不能直接沿用 TCP 的双向
独立操作约定。对象必须保持稳定地址并活到操作结束，仅在空闲时移动。

所有异步操作在所属 `io_context` 执行。TLS 错误使用带 OpenSSL 信息的
`system_error(protocol_error)`，取消保留 `operation_canceled`。握手失败、
读写失败或取消都会关闭底层传输，拒绝重叠操作除外。握手本身没有独立超时，
应用可以停止 reactor 或在确保对象生命周期的前提下关闭流。

## 关闭

`shutdown(timeout = 100ms)` 发送并等待 `close_notify` 后关闭；
`close_gracefully` 是同义方法。完成交换后，提供 `close()` 的自定义传输会先执行
可选的 `shutdown(std::chrono::milliseconds)` 做清理，再调用 `close()`。
QUIC 风格的传输会发送并等待 FIN。内部使用可取消的 reactor 定时器限制操作时间，超时正常关闭，TLS
错误和取消继续抛出。零或负超时会尝试一次 TCP 关闭，其他传输立即关闭。协作式
时限无法抢占阻塞应用代码。操作结束即注销定时器，不留下轮询任务
或延迟回调。

`close()` 立即取消等待并关闭或重置底层传输，不发送 TLS 关闭通知。之后仍需保留流
对象直到挂起的成员函数返回。`close()` 应在所属 reactor 上调用，或在它未运行
时调用。源对象被移动后，仅允许析构、赋值、`close()` 和 `is_open()`。
