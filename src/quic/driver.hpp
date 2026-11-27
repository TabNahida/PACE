#pragma once
#include <pace/quic/connection.hpp>

namespace pace::quic
{
// Internal: lets a multiplexed protocol (HTTP/3) wake a connection's driver task.
struct quic_driver
{
    static void wake(quic::connection& connection);
};
} // namespace pace::quic
