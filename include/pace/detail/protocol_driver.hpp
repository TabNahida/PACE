#pragma once
#include <pace/tls.hpp>
#include <pace/quic.hpp>
#include <optional>

namespace pace::detail {
#ifdef PACE_HAS_TLS
// Internal single-owner adapter. It holds TLS's normal operation guard while a
// multiplexed protocol drives nonblocking progress and waits outside OpenSSL.
// It does not permit simultaneous public TLS operations.
class tls_driver {
public:
    explicit tls_driver(tls::stream& stream);
    ~tls_driver();
    tls_driver(const tls_driver&) = delete;
    std::optional<std::size_t> read(std::span<char> bytes);
    std::size_t write(std::string_view bytes);
    native_socket_type native_handle() const noexcept;
    bool write_wait() const noexcept;
    bool either_wait() const noexcept { return false; }
    bool read_allowed() const noexcept { return !write_retry_; }
    bool write_allowed() const noexcept { return !read_wants_write_; }
private:
    tls::stream& stream_;
    const char* write_data_ = nullptr;
    std::size_t write_size_ = 0;
    bool write_retry_ = false, write_wants_write_ = false, read_wants_write_ = false;
};
#endif
#ifdef PACE_HAS_QUIC
struct quic_driver {
    static void wake(quic::connection& connection);
};
#endif
} // namespace pace::detail
