# TLS 使用指南

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
证书身份分开传入。域名会设置 SNI 并检查证书主机名；数字 IP 则检查 IP SAN，
不会发送 DNS SNI。客户端必须提供非空身份，始终验证证书链与身份；没有跳过
验证的开关。默认客户端加载 OpenSSL 信任路径，`load_verify_file` 可追加 CA。
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
临时文件。`load_verify_file` 仍可加载显式文件。两种方式始终保留证书链、有效期
和 DNS / IP 身份验证，不暴露可绕过验证的 callback、原生可变句柄或借用的
`X509_STORE`。PACE 不自动导入 Windows 证书库或 macOS Keychain；应用可以将
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

配置在首次构造流或被注册为 SNI 目标时冻结，后续修改抛出 `logic_error`，即使
流已关闭也不会解冻。修改配置时请创建新的 context。配置修改与冻结在内部串行
化，冻结后的配置可供多个 reactor 创建各自独立的流。移动、销毁公开 context
对象不能与对同一对象的访问并发发生。

流共享配置及其整个 SNI 映射表的所有权，公开配置对象随后可以移动或销毁，
证书、信任根与回调状态一直保留到最后一个拥有它们的流或配置销毁。

ALPN 服务端按自身列表顺序选择与客户端的共同协议。客户端提供列表但没有交集
时握手失败；没有提供 ALPN 的对端可以成功且结果为空。HTTP/2 应主动检查
`selected_protocol() == "h2"`。ALPN 只负责协商，不实现对应应用协议。

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
调用 `handshake(peer_name)`，服务端调用 `handshake()`。移动后不再进行明文
操作；协商阶段应只读取明文响应字节，已经读进应用缓冲区的加密字节无法重新
喂给当前基于 socket 的 TLS 接口。

## 读写与并发

`read_some(span<char/byte>)` 读取解密数据；`write_all(string_view/span<const byte>)`
处理全部明文字节的加密写入。缓冲区和 view 在整个挂起期间都借用应用内存。
非空读取返回零只代表验证过的 `close_notify`，裸 TCP EOF 被视为截断错误。
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
`close_gracefully` 是同义方法。内部使用可取消的 reactor 定时器界定就绪等待，超时正常
关闭，TLS 错误和取消继续抛出。零或负超时会尝试一次后关闭，不能保证对端已
收到；协作式时限无法抢占阻塞应用代码。操作结束即注销定时器，不留下轮询任务
或延迟回调。

`close()` 立即取消等待并关闭 socket，不发送 TLS 关闭通知。之后仍需保留流
对象直到挂起的成员函数返回。`close()` 应在所属 reactor 上调用，或在它未运行
时调用。源对象被移动后，仅允许析构、赋值、`close()` 和 `is_open()`。
