#pragma once

#include <cstdint>
#include <optional>

namespace pace
{
/// Native TCP options for listeners and outbound connections. Unset fields keep the OS defaults.
/// An unavailable option throws operation_not_supported. Kernel and permission errors propagate.
struct socket_options
{
    /// Interface index (zero clears the binding). FreeBSD routing tables use routing_table instead.
    std::optional<unsigned> interface_index{};
    /// Linux SO_MARK or FreeBSD SO_USER_COOKIE.
    std::optional<std::uint32_t> mark{};
    /// FreeBSD SO_SETFIB.
    std::optional<unsigned> routing_table{};
    std::optional<bool> keepalive{};
    std::optional<int> keepalive_idle_seconds{};
    std::optional<int> keepalive_interval_seconds{};
    std::optional<int> keepalive_count{};
    std::optional<unsigned> user_timeout_ms{};
    std::optional<bool> dont_fragment{};
    /// Allow replayable TCP Fast Open data. TCP exchange supplies its request during connection setup.
    bool fast_open = false;
    /// Linux IP_TRANSPARENT / IPV6_TRANSPARENT for nonlocal binds.
    bool transparent = false;
};
} // namespace pace
