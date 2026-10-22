#pragma once

#include <pace/tcp.hpp>
#ifdef PACE_HAS_TLS
#include <pace/tls.hpp>
#endif

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace pace::websocket {
enum class role { client, server };
enum class message_type { text, binary };
struct message {
    message_type type = message_type::text;
    std::string data;
};
struct limits {
    std::size_t max_header_bytes = 16 * 1024;
    std::size_t max_frame_bytes = 64 * 1024;
    std::size_t max_message_bytes = 1024 * 1024;
    std::size_t max_fragments = 1024;
    std::size_t max_control_frames = 1024; // Per receive call, bounds ping/pong floods.
};
struct upgrade_request {
    std::string target;
    std::string host;
    std::string origin;
    std::vector<std::string> protocols;
};
class protocol_error : public std::runtime_error {
public:
    protocol_error(std::uint16_t code, const std::string& message)
        : std::runtime_error(message), code_(code) {}
    std::uint16_t close_code() const noexcept { return code_; }
private:
    std::uint16_t code_;
};

// Owns a connected TCP or handshaken TLS transport. One operation at a time,
// including sends vs receive; move only while idle. All methods are context-affine.
template<class Transport>
class basic_stream {
public:
    explicit basic_stream(Transport transport, role mode, limits bounds = {});
    ~basic_stream();
    basic_stream(basic_stream&&) noexcept;
    basic_stream& operator=(basic_stream&&) noexcept;
    basic_stream(const basic_stream&) = delete;
    basic_stream& operator=(const basic_stream&) = delete;

    task<void> handshake(std::string host, std::string target = "/",
                         std::vector<std::string> protocols = {});
    task<upgrade_request> accept(std::vector<std::string> supported_protocols = {},
        std::function<bool(const upgrade_request&)> authorize = {});

    // Receive assembles fragments, answers ping, consumes pong, and returns nullopt
    // after a peer close (with the closing response sent). Bare EOF is an error.
    task<std::optional<message>> receive();
    // Data is owned across suspension. fragment_bytes=0 uses max_frame_bytes.
    task<void> send(message value, std::size_t fragment_bytes = 0);
    task<void> send_text(std::string text, std::size_t fragment_bytes = 0) {
        return send({message_type::text, std::move(text)}, fragment_bytes);
    }
    task<void> send_binary(std::string bytes, std::size_t fragment_bytes = 0) {
        return send({message_type::binary, std::move(bytes)}, fragment_bytes);
    }
    task<void> ping(std::string payload = {});
    task<void> pong(std::string payload = {});
    task<void> send_close(std::uint16_t code = 1000, std::string reason = {});
    // Returns true for a peer closing frame, false if the cooperative bound expires.
    // Always closes transport. No application messages may be sent after close.
    task<bool> close_gracefully(std::uint16_t code = 1000, std::string reason = {},
                               std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] io_context& context() const noexcept;
    [[nodiscard]] std::string selected_protocol() const;
    [[nodiscard]] std::optional<std::uint16_t> peer_close_code() const;
    [[nodiscard]] std::string peer_close_reason() const;
private:
    struct implementation;
    std::unique_ptr<implementation> impl_;
};
using stream = basic_stream<tcp::stream>;
extern template class basic_stream<tcp::stream>;
#ifdef PACE_HAS_TLS
using secure_stream = basic_stream<tls::stream>;
extern template class basic_stream<tls::stream>;
#endif
} // namespace pace::websocket
