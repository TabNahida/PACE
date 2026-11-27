#pragma once

// Drives one fuzz input through PACE's public API over a loopback TCP connection.
//
// Fuzz targets go through the same entry points applications use (for example
// http::serve_connection), so they keep working when the parsers behind those entry points are
// rewritten. Protocol errors are expected outcomes and are swallowed; only crashes, sanitizer
// reports, hangs and leaks count as findings.

#include <pace/net/tcp.hpp>
#include <pace/runtime/run.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace pace::fuzz
{
// Upper bound on what a target sends or reads back, so one input stays fast.
inline constexpr std::size_t max_reply_bytes = 256 * 1024;

inline std::string_view as_text(const std::uint8_t* data, std::size_t size)
{
    return {reinterpret_cast<const char*>(data), size};
}

// Reads until EOF or until `limit` bytes, ignoring errors.
inline task<void> drain(tcp::stream& stream, std::size_t limit = max_reply_bytes)
{
    char buffer[4096];
    std::size_t total = 0;
    try
    {
        while (total < limit)
        {
            const auto read = co_await stream.read_some(pace::as_writable_bytes(std::span<char>(buffer)));
            if (read == 0)
            {
                break;
            }
            total += read;
        }
    }
    catch (const std::exception&)
    {
    }
}

// The spawned server coroutines below take everything they use by value. pace::run returns once the
// entry coroutine finishes, but the reactor still resumes a server that is closing its connection, so a
// server must never refer to the entry coroutine's locals.

// The input is what a client sends to a server-side handler: `serve(stream)` owns the accepted
// connection. The client writes the input, half-closes, and drains whatever the server answers.
template <class Serve>
void feed_server(std::string_view input, Serve serve)
{
    pace::run(
        [&](io_context& io) -> task<void>
        {
            tcp::listener listener(io, {"127.0.0.1", 0});
            const auto endpoint = listener.local_endpoint();
            auto server = [](tcp::listener listener, Serve serve) -> task<void>
            {
                try
                {
                    co_await serve(co_await listener.accept());
                }
                catch (const std::exception&)
                {
                }
            };
            io.spawn(server(std::move(listener), serve));
            auto client = co_await tcp::connect(io, endpoint);
            try
            {
                co_await client.write_all(pace::as_bytes(input));
                co_await client.close_gracefully(std::chrono::milliseconds(200), max_reply_bytes);
            }
            catch (const std::exception&)
            {
            }
        }
    );
}

// The input is what a server answers: `talk(endpoint)` is client code that connects to the
// returned endpoint. The fake server reads the request headers, writes the input and closes.
template <class Talk>
void feed_client(std::string_view input, Talk talk)
{
    pace::run(
        [&](io_context& io) -> task<void>
        {
            tcp::listener listener(io, {"127.0.0.1", 0});
            const auto endpoint = listener.local_endpoint();
            auto server = [](tcp::listener listener, std::string answer) -> task<void>
            {
                try
                {
                    auto peer = co_await listener.accept();
                    std::string request;
                    char buffer[4096];
                    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 64 * 1024)
                    {
                        const auto read = co_await peer.read_some(pace::as_writable_bytes(std::span<char>(buffer)));
                        if (read == 0)
                        {
                            break;
                        }
                        request.append(buffer, read);
                    }
                    co_await peer.write_all(pace::as_bytes(answer));
                    co_await peer.close_gracefully(std::chrono::milliseconds(200), max_reply_bytes);
                }
                catch (const std::exception&)
                {
                }
            };
            io.spawn(server(std::move(listener), std::string(input)));
            try
            {
                co_await talk(io, endpoint);
            }
            catch (const std::exception&)
            {
            }
        }
    );
}
} // namespace pace::fuzz
