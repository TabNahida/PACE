<!-- source-hash: 04af5c38846f -->
# 指南

[English](README.md) · [文档](../README.zh.md)

针对具体任务的做法。每篇指南都可以单独阅读，前提是你已经从[教程](../tutorial/README.zh.md)掌握了基础；准确的签名
和默认值见[参考](../reference/README.zh.md)。

| 指南 | 适用场景 |
|:--|:--|
| [HTTP 配置与常用做法](http-recipes.zh.md) | 控制响应何时完成与何时关闭连接，保留显式的监听器，启用 HTTPS，复用连接，设置上限，在接收消息体之前检查头部 |
| [协议调优](protocol-tuning.zh.md) | 复用缓冲区，为 TCP、UDP、DNS、TLS、WebSocket、HTTP/1、HTTP/2、HTTP/3 和 QUIC 设置截止时间与并发上限，把工作放到工作线程上 |

更多指南会随 1.0 的协议工作陆续加入：反向代理、SNI 路由、自定义拥塞控制、中继与隧道、SOCKS5、CONNECT-UDP 和
ECH。
