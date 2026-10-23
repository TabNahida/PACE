#pragma once
#ifdef PACE_HAS_QUIC
#include <pace/udp.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

namespace pace::detail { struct quic_driver; }
namespace pace::quic {
enum class role { client, server };
enum class stream_type { bidirectional, unidirectional };
namespace detail { struct configuration; struct engine; struct connection_state; struct stream_state; }
// OpenSSL 3.5+ native QUIC, TLS 1.3 with mandatory peer verification on clients.
// POSIX transport (Linux, macOS and FreeBSD). Windows QUIC is not supported.
class context {
public:
    explicit context(role mode = role::client);
    void load_verify_file(const std::string& path);
    void use_certificate_chain(const std::string& chain, const std::string& key);
    void set_alpn_protocols(const std::vector<std::string>& protocols);
private:
    std::shared_ptr<detail::configuration> state_;
    friend class connection;
    friend class listener;
};
class stream {
public:
    stream(stream&&) noexcept;
    stream& operator=(stream&&) noexcept;
    ~stream();
    stream(const stream&) = delete;
    std::uint64_t id() const noexcept;
    bool is_unidirectional() const noexcept;
    bool is_local() const noexcept;
    io_context& context() const noexcept;
    task<std::size_t> read_some(std::span<char> buffer);
    task<void> write_all(std::string_view bytes);
    task<std::size_t> read_some(std::span<std::byte> buffer) {
        return read_some(std::span<char>(reinterpret_cast<char*>(buffer.data()), buffer.size()));
    }
    task<void> write_all(std::span<const std::byte> bytes) {
        return write_all(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
    task<void> wait_events(); // Wait for transport progress; spurious wakeups are permitted.
    // Nonblocking primitives for multiplexed application protocol drivers.
    // nullopt means would-block; zero bytes means authenticated stream FIN.
    std::optional<std::size_t> try_read(std::span<char> buffer);
    std::size_t try_write(std::string_view bytes);
    std::uint64_t pending_write_bytes() const; // Bytes retained for QUIC acknowledgement.
    void finish_write(); // Queue stream FIN; receive direction remains usable.
    void reset(std::uint64_t application_error = 0) noexcept;
private:
    explicit stream(std::shared_ptr<detail::stream_state> state);
    std::shared_ptr<detail::stream_state> state_;
    friend class connection;
};
class connection {
public:
    connection(io_context& io, const quic::context& config);
    connection(connection&&) noexcept;
    connection& operator=(connection&&) noexcept;
    ~connection();
    connection(const connection&) = delete;
    task<void> connect(udp::endpoint peer, std::string expected_name,
                       std::chrono::milliseconds timeout = std::chrono::seconds(10));
    task<stream> open_stream(stream_type type = stream_type::bidirectional);
    std::optional<stream> try_open_stream(stream_type type = stream_type::bidirectional);
    task<stream> accept_stream();
    std::optional<stream> try_accept_stream();
    task<void> wait_events(); // Shared event-driven wait for multiplexed protocol drivers.
    io_context& context() const noexcept;
    std::string selected_protocol() const;
    bool is_open() const noexcept;
    // Sends CONNECTION_CLOSE best-effort and invalidates all owned stream handles.
    void close(std::uint64_t application_error = 0) noexcept;
private:
    explicit connection(std::shared_ptr<detail::connection_state> state);
    std::shared_ptr<detail::connection_state> state_;
    friend class listener;
    friend struct pace::detail::quic_driver;
};
class listener {
public:
    listener(io_context& io, udp::endpoint local, const quic::context& config);
    ~listener();
    listener(const listener&) = delete;
    task<connection> accept(std::chrono::milliseconds handshake_timeout = std::chrono::seconds(10));
    udp::endpoint local_endpoint() const;
    io_context& context() const noexcept;
    bool is_open() const noexcept;
    void close() noexcept; // Also invalidates accepted connections on this listener.
private:
    std::shared_ptr<detail::engine> state_;
};
} // namespace pace::quic
#endif
