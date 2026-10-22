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

示例地址保留用于文档，实际应替换为解析后的数字 IPv4 地址。TCP 地址和预期
证书身份分开传入。域名会设置 SNI 并检查证书主机名；数字 IP 则检查 IP SAN，
不会发送 DNS SNI。客户端必须提供非空身份，始终验证证书链与身份；没有跳过
验证的开关。客户端加载 OpenSSL 默认信任路径，`load_verify_file` 可追加 CA。
这不会自动导入 Windows 证书库或 macOS Keychain；默认根证书不可用时应显式
提供正确的 CA 文件。没有内置 DNS 解析器。

```cpp
pace::tls::context config(pace::tls::role::server);
config.use_certificate_chain("chain.pem", "private-key.pem");
pace::tls::stream socket(co_await listener.accept(), config);
co_await socket.handshake();
```

服务端 chain 文件以叶证书开头，后接中间证书；私钥必须匹配且为未加密 PEM。
加密私钥会失败，不会从终端提示输入密码。当前不支持要求客户端证书的 mTLS。
创建流之前完成配置，不要在流使用期间修改配置。流共享其配置的所有权，因此
配置对象创建流后可以移动或销毁。将 TCP 流移动进 TLS 后不能继续明文操作它。

ALPN 服务端按自身列表顺序选择与客户端的共同协议。客户端提供列表但没有交集
时握手失败；没有提供 ALPN 的对端可以成功且结果为空。HTTP/2 应主动检查
`selected_protocol() == "h2"`。ALPN 只负责协商，不实现对应应用协议。

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
`close_gracefully` 是同义方法。内部使用可取消的 1ms 计时器重试，超时正常
关闭，TLS 错误和取消继续抛出。零或负超时会尝试一次后关闭，不能保证对端已
收到；协作式时限无法抢占阻塞应用代码。

`close()` 立即取消等待并关闭 socket，不发送 TLS 关闭通知。之后仍需保留流
对象直到挂起的成员函数返回。`close()` 应在所属 reactor 上调用，或在它未运行
时调用。源对象被移动后，仅允许析构、赋值、`close()` 和 `is_open()`。
