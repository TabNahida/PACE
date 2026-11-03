# HTTP 网关接口与调优

[循序渐进教程](../tutorial/13-gateway.zh-CN.md) · [完整英文契约](http-gateway.md)

包含 `<pace/http.hpp>`，链接 `pace_http1`。这是 0.4.0 之后新增的开发分支接口。
`app.mount(prefix, directory, options)` 挂载目录，`app.proxy(prefix, upstreams, options)`
代理到一个或多个固定上游。最长路径前缀优先，精确路由优先于前缀路由；匹配以路径段为界。

## 生命周期与 workers

配置完成后再启动服务。已有 handler 持有路由快照，后续改路由不会改变该快照。
`static_files` / `reverse_proxy` 是可复制的共享所有权 handler，返回的 task 持有服务与请求。
缓存可跨线程共享，连接池、活跃计数和分配游标只属于各自 reactor。
context 会保留已经使用的代理状态至自身销毁，因此不要每个请求创建一个新的代理对象。

`run_workers(n, endpoint)` 创建固定 runtime 和每 worker 一个监听器。
多 worker 依赖 `SO_REUSEPORT`，支持 Linux/macOS/FreeBSD；Windows 目前只能用该入口运行一个 worker。
需要应用控制停止时，在自己的 runtime 中运行 `app.serve(listener)`。

需要显式清缓存或读取统计时，持有 `static_files` / `reverse_proxy` 对象，通过
`router.mount(prefix, service)` 注册，调用 `service.clear_cache()` / `service.cache_stats()`。
清除不会撤销正在发送的快照，也不是阻止并发 miss 回填的事务屏障。

## 静态服务

`static_options` 默认：`index="index.html"`、`max_file_bytes=8 MiB`、
`allow_dotfiles=false`、`cache_control="no-cache"`，本地缓存关闭。
GET/HEAD 支持 MIME、ETag、Last-Modified、条件请求、单段及后缀 Range、If-Range。
If-Modified-Since 目前只比较规范格式的相等日期；多段 Range 被忽略。
没有目录列表，没有目录尾斜杠重定向；目录链接应带 `/` 以正确解析浏览器相对资源。

路径只解码一次，拒绝路径穿越、控制字符、反斜杠、非法转义、Windows ADS 及尾点/尾空格。
拒绝符号链接、包含符号链接的中间目录和 Windows reparse point；Windows 还检查打开句柄的最终路径。
配置根目录本身是可信输入，仅允许常规文件。

**磁盘读取在 reactor 上同步完成，响应有界缓冲。** 这里没有 sendfile、异步磁盘执行器或大文件流式发送。
超过大小上限返回 413；缓存命中仍需复制到独立响应。大文件、冷文件场景应使用专用静态服务器。

本地缓存以解码后的文件路径为键，不包含 query。TTL 内不重新 stat，文件更新/删除可能暂不可见，
需要等待 TTL 或清缓存。请求含 Cache-Control / Pragma 时绕过本地缓存。
`cache_control` 控制下游 HTTP 缓存，与 PACE 的本地缓存是否启用相互独立。

## 代理与均衡

上游仅使用 HTTP/1.1，允许固定 HTTP/HTTPS base URL，不允许 query/fragment。
域名需要 DNS 模块，HTTPS 需要 TLS 模块并验证系统信任链。
默认去除挂载前缀，再拼接上游 base path，保留方法、body 和 query。
替换 Host，移除逐跳头、Connection 指定的头、原始 framing/trailer，以及传入的 Forwarded/X-Forwarded-*。
不会自动填写客户端 IP。Upgrade/CONNECT 返回 501，body 有界缓冲，不转发 trailer。

| 参数 | 默认值 | 含义 |
|---|---:|---|
| `balance` | `round_robin` | 或 `least_connections`，按 worker 计数，平局轮转 |
| `strip_prefix` | true | 去掉挂载前缀 |
| `max_connections` | 64 | 每上游、每 reactor 的活跃交换上限 |
| `max_idle_connections` | 16 | 每上游、每 reactor 的空闲连接上限 |
| `max_response_bytes` | 8 MiB | 上游 body 大小上限 |
| `timeout` | 30 秒 | DNS/连接/TLS/交换共用截止时间 |
| `idle_timeout` | 30 秒 | 取出连接时清理过期空闲项 |
| `fail_timeout` | 1 秒 | 传输失败后的被动冷却 |

饱和或没有可用上游返回 503，传输/协议失败返回 502，超时返回 504。
局部取消向上传播。没有排队接纳、主动健康检查、自动重试或 stale-on-error。
空闲连接已经失效时，该次请求可能失败，GET 也不会自动重放。
这些 helper 面向 HTTP/1，不隐式做 HTTP/2/3 协议转换或 WebSocket 隧道。

代理缓存只接纳不带 body 的 GET、目标长度不超过 4096 字节；认证、Cookie、Range、条件头、
Cache-Control、Pragma 请求绕过缓存。只有显式 public 或 s-maxage 的 200 响应可存储，
Set-Cookie、Vary、private/no-cache/no-store、Content-Range 和不认识的缓存指令都阻止存储。
键包含下游 Host 与完整 target/query；TTL 取配置和上游策略约束，Date/Age 参与新鲜度判断，命中更新 Age。
这是保守的 HTTP 缓存子集，不是完整 RFC 缓存实现。

## 缓存预算与局部性

`cache_options` 默认 `max_bytes=0`（关闭）、`max_entries=1024`、`max_object_bytes=1 MiB`、
`ttl=60s`、`shards=16`。预算平均分配给各 shard；热点偏斜可能导致预算利用不满。
统计包含命中、未命中、驱逐、条目数和核算字节数。

`max_bytes` 是存储条目的核算预算，slot/hash 存储另由 max_entries 约束；发送中的旧快照和分配器元数据
意味着它不是进程 RSS 上限。clear 清除条目，保留累计计数。没有合并并发缓存 miss 的机制。

采用分片锁、连续 slot 的 CLOCK 替换、不可变快照和锁外响应复制，减少每次命中修改链式 LRU 的开销。
连接池按 reactor 隔离；事件分发用带 generation 的密集描述符 slot，省掉第二次哈希查找并过滤复用后的旧事件。
没有绑定本机 CPU cache 大小或亲和性。[性能报告](../gateway-performance.md)给出全部本地场景和局限。
