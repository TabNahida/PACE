// One PACE process that exposes every enabled server-side protocol for external interop peers
// (curl, h2spec, h3spec, openssl s_client, websocket clients...). tests/interop/run_interop.py
// starts it, reads the endpoint map from the first line of stdout, and drives the peers.
//
//     pace_interop_server <certificate directory>
//
// The first stdout line is JSON such as {"http1": 40001, "h2c": 40002, "h2": 40003, ...}. The
// process serves until it is killed or stdin reaches EOF.
//
// Every HTTP endpoint answers:
//     GET  /              200 "hello from PACE\n"
//     POST /echo          200 with the request body
//     GET  /status/<n>    status n with an empty body
//     anything else       404

#include <pace/net/tcp.hpp>
#include <pace/runtime/runtime.hpp>
#ifdef PACE_HAS_HTTP1
#include <pace/http1/protocol.hpp>
#endif
#ifdef PACE_HAS_HTTP2
#include <pace/http2/protocol.hpp>
#endif
#ifdef PACE_HAS_HTTP3
#include <pace/http3/protocol.hpp>
#include <pace/quic/connection.hpp>
#endif
#ifdef PACE_HAS_TLS
#include <pace/tls/stream.hpp>
#endif
#ifdef PACE_HAS_WEBSOCKET
#include <pace/websocket/stream.hpp>
#endif

#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace
{
#if defined(PACE_HAS_HTTP1) || defined(PACE_HAS_HTTP2) || defined(PACE_HAS_HTTP3)
pace::task<pace::http::response> respond(pace::http::request request)
{
    pace::http::response response;
    if (request.target == "/" && (request.method == "GET" || request.method == "HEAD"))
    {
        response.body = "hello from PACE\n";
    }
    else if (request.target == "/echo" && request.method == "POST")
    {
        response.body = std::move(request.body);
    }
    else if (request.target.starts_with("/status/"))
    {
        try
        {
            response.status = std::stoi(request.target.substr(8));
        }
        catch (const std::exception&)
        {
            response.status = 400;
        }
    }
    else
    {
        response.status = 404;
        response.body = "not found\n";
    }
    co_return response;
}
#endif

#ifdef PACE_HAS_TLS
// Raw TLS echo: one handshake, then echo bytes until the peer closes.
pace::task<void> tls_echo(pace::tcp::listener& listener, const pace::tls::context& configuration)
{
    while (true)
    {
        auto transport = co_await listener.accept();
        auto session = [](pace::tcp::stream transport, const pace::tls::context& configuration) -> pace::task<void>
        {
            try
            {
                auto stream = co_await pace::tls::accept(std::move(transport), configuration);
                char buffer[16 * 1024];
                while (const auto read = co_await stream.read_some(pace::as_writable_bytes(std::span<char>(buffer))))
                {
                    co_await stream.write_all(pace::as_bytes(std::string_view(buffer, read)));
                }
                co_await stream.shutdown();
            }
            catch (const std::exception&)
            {
            }
        };
        listener.context().spawn(session(std::move(transport), configuration));
    }
}
#endif

#ifdef PACE_HAS_WEBSOCKET
pace::task<void> websocket_echo(pace::tcp::listener& listener)
{
    while (true)
    {
        auto transport = co_await listener.accept();
        auto session = [](pace::tcp::stream transport) -> pace::task<void>
        {
            try
            {
                auto socket = co_await pace::websocket::accept(std::move(transport));
                while (auto message = co_await socket.receive())
                {
                    co_await socket.send(std::move(*message));
                }
            }
            catch (const std::exception&)
            {
            }
        };
        listener.context().spawn(session(std::move(transport)));
    }
}
#endif
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: pace_interop_server <certificate directory>\n";
        return 2;
    }
    try
    {
        const std::filesystem::path certs(argv[1]);
        const auto certificate = (certs / "localhost.pem").string();
        const auto key = (certs / "localhost-key.pem").string();
        pace::io_context io;
        std::map<std::string, unsigned> ports;
        std::vector<std::unique_ptr<pace::tcp::listener>> listeners;
        [[maybe_unused]] auto listen = [&](const std::string& name)
        {
            listeners.push_back(std::make_unique<pace::tcp::listener>(io, pace::tcp::endpoint{"127.0.0.1", 0}));
            ports[name] = listeners.back()->local_endpoint().port;
            return std::ref(*listeners.back());
        };

#ifdef PACE_HAS_HTTP1
        io.spawn(pace::http::serve(listen("http1"), respond));
#endif
#ifdef PACE_HAS_HTTP2
        io.spawn(pace::http2::serve(listen("h2c"), respond));
#endif
#ifdef PACE_HAS_TLS
        std::vector<std::unique_ptr<pace::tls::context>> contexts;
        auto server_context = [&](std::vector<std::string> alpn)
        {
            contexts.push_back(std::make_unique<pace::tls::context>(pace::tls::role::server));
            contexts.back()->use_certificate_chain(certificate, key);
            if (!alpn.empty())
            {
                contexts.back()->set_alpn_protocols(alpn);
            }
            return std::cref(*contexts.back());
        };
        io.spawn(tls_echo(listen("tls_echo"), server_context({})));
#ifdef PACE_HAS_HTTP1
        io.spawn(pace::http::serve(listen("https1"), server_context({"http/1.1"}), respond));
#endif
#ifdef PACE_HAS_HTTP2
        io.spawn(pace::http2::serve(listen("h2"), server_context({"h2"}), respond));
#endif
#endif
#ifdef PACE_HAS_WEBSOCKET
        io.spawn(websocket_echo(listen("websocket")));
#endif
#ifdef PACE_HAS_HTTP3
        pace::quic::context quic_context(pace::quic::role::server);
        quic_context.use_certificate_chain(certificate, key);
        quic_context.set_alpn_protocols({"h3"});
        auto quic_listener = pace::quic::listen(io, {"127.0.0.1", 0}, quic_context);
        ports["h3"] = quic_listener.local_endpoint().port;
        io.spawn(pace::http3::serve(quic_listener, respond));
#endif

        std::string line = "{";
        for (const auto& [name, port] : ports)
        {
            line += (line.size() > 1 ? ", \"" : "\"") + name + "\": " + std::to_string(port);
        }
        std::cout << line << "}" << std::endl;

        // Stop when the runner closes stdin, so a killed runner never leaves a server behind.
        std::thread stdin_watch(
            [&io]
            {
                while (std::getchar() != EOF)
                {
                }
                io.stop();
            }
        );
        stdin_watch.detach();
        io.run();
    }
    catch (const std::exception& error)
    {
        std::cerr << "pace_interop_server: " << error.what() << '\n';
        return 1;
    }
}
