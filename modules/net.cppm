// ABI-preserving facade: declarations belong to the global module, just as they do for #include
// consumers. Keep this export list in sync with the module's public headers.
module;
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 16
#error PACE_named_modules_require_Clang_or_GCC_16_with_coroutine_module_fixes
#endif
#include <pace/net.hpp>

export module pace.net;
export import pace.runtime;

export namespace pace
{
using ::pace::address_family;
using ::pace::ipv4_endpoint;
using ::pace::listen_options;
using ::pace::read_exact;
using ::pace::tcp_listener;
using ::pace::tcp_stream;
using ::pace::write;
} // namespace pace

export namespace pace::tcp
{
using ::pace::tcp::async_exchange;
using ::pace::tcp::connect;
using ::pace::tcp::endpoint;
using ::pace::tcp::exchange;
using ::pace::tcp::listen;
using ::pace::tcp::listen_options;
using ::pace::tcp::listener;
using ::pace::tcp::stream;
} // namespace pace::tcp

export namespace pace::udp
{
using ::pace::udp::async_exchange;
using ::pace::udp::bind;
using ::pace::udp::bind_options;
using ::pace::udp::datagram;
using ::pace::udp::endpoint;
using ::pace::udp::exchange;
using ::pace::udp::open;
using ::pace::udp::packet;
using ::pace::udp::send;
using ::pace::udp::socket;
} // namespace pace::udp
