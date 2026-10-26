# HTTP handler adapters and exact routing

Header: `<pace/http_router.hpp>`; included by `<pace/http.hpp>`. The header uses
shared HTTP messages and works with HTTP/1, HTTP/2 and HTTP/3 handlers. It adds no
third-party dependency and does not change their wire parsers.

```cpp
template<class Function> handler make_handler(Function&& function);

class router {
public:
    template<class Function> router& add(std::string method, std::string path, Function&&);
    template<class Function> router& get(std::string path, Function&&);
    template<class Function> router& post(std::string path, Function&&);
    task<response> operator()(request incoming) const;
};
```

A callable accepts `request` (by value or compatible argument) and returns exactly
`response` or `task<response>`. `make_handler` owns it through an allocated shared
object; the resulting copyable `handler` supports move-only captures. Each pending
invocation owns a reference, so destroying a router/adapter does not invalidate
its suspended coroutine lambda. Borrowed captures still need external lifetime
management. Synchronous handlers run on the reactor and must not block.

Methods and paths match case-sensitively. `add` accepts any nonempty HTTP token
as a method. A path starts with `/` and cannot contain whitespace, control
characters, `?` or `#`. Duplicate method/path registration throws
`std::invalid_argument`. Query text is ignored for matching, but the original
request target is retained. Percent escapes are not decoded or normalized.
Only exact origin-form paths are routed; there are no wildcards, parameters,
regexes, URL decoding, middleware or automatic OPTIONS responses.

An explicit HEAD route takes priority. Otherwise HEAD falls back to GET; the
protocol serializer suppresses its body, preserving normal HEAD metadata.
An unmatched path returns 404. A known path with another method returns 405 and
an `Allow` header, including HEAD for GET routes. Exceptions propagate from direct
invocations; ordinary server adapters retain their existing 500-response behavior.

Configure routes before serving. Route lookup is linear in the number of routes;
this is a small dispatch utility, not a performance claim or a large routing
framework. Copies have independent route tables and share callable instances.
Concurrent mutation/use of the same router is unsupported. Mutable handler state
shared across worker threads requires application synchronization.

HTTP/1 also provides an owning-listener overload:

```cpp
task<void> serve(io_context&, tcp::endpoint, handler, limits = {});
```

It binds when execution starts, owns the listener until the accept loop ends,
and forwards to the existing buffered server. It stops with its context; it is
not a separate thread or a graceful-drain handle. Use `serve(listener, ...)` for
immediate bind errors, an ephemeral port's actual endpoint, TLS, admission,
streaming responses or separately closing acceptance. All original overloads
remain available.

[Build a service step by step](../tutorial/04-http.md) · [中文教程](../tutorial/04-http.zh-CN.md)
