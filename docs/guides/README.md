# Guides

[简体中文](README.zh.md) · [Documentation](../README.md)

Recipes for specific tasks. Each guide stands on its own and assumes you know the basics from the
[tutorial](../tutorial/README.md); exact signatures and defaults are in the [reference](../reference/README.md).

| Guide | When you need to |
|:--|:--|
| [HTTP configuration and recipes](http-recipes.md) | Control response completion and closing, keep an explicit listener, add HTTPS, reuse connections, set bounds, check headers before accepting a body |
| [Protocol tuning](protocol-tuning.md) | Reuse buffers, set deadlines and concurrency limits for TCP, UDP, DNS, TLS, WebSocket, HTTP/1, HTTP/2, HTTP/3 and QUIC, and place work on workers |

More guides arrive with the 1.0 protocol work: reverse proxying, SNI routing, custom congestion control, relays
and tunnels, SOCKS5, CONNECT-UDP and ECH.
