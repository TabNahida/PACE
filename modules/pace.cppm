// ABI-preserving facade: declarations belong to the global module, just as
// they do for #include consumers. Keep this export list in sync with the API.
module;
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 16
#error PACE_named_modules_require_Clang_or_GCC_16_with_coroutine_module_fixes
#endif
#include <pace/http_router.hpp>
#include <pace/pace.hpp>

export module pace;

export namespace pace
{
using ::pace::address_family;
using ::pace::backend_name;
using ::pace::blocking_options;
using ::pace::cancellation_source;
using ::pace::cancellation_token;
using ::pace::deadline;
using ::pace::invalid_socket;
using ::pace::io_backend;
using ::pace::io_context;
using ::pace::ipv4_endpoint;
using ::pace::join_handle;
using ::pace::listen_options;
using ::pace::native_socket_type;
using ::pace::operation_options;
using ::pace::read_exact;
using ::pace::run;
using ::pace::runtime;
using ::pace::shield;
using ::pace::sleep_for;
using ::pace::task;
using ::pace::task_group;
using ::pace::tcp_listener;
using ::pace::tcp_stream;
using ::pace::this_context;
using ::pace::with_cancellation;
using ::pace::with_deadline;
using ::pace::with_options;
using ::pace::with_task_group;
using ::pace::write;
using ::pace::yield;
} // namespace pace

export namespace pace::this_task
{
using ::pace::this_task::cancellation;
} // namespace pace::this_task

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

export namespace pace::http
{
using ::pace::http::handler;
using ::pace::http::header_field;
using ::pace::http::html;
using ::pace::http::json;
using ::pace::http::make_handler;
using ::pace::http::protocol_error;
using ::pace::http::request;
using ::pace::http::response;
using ::pace::http::router;
using ::pace::http::text;
} // namespace pace::http

export namespace pace
{
namespace http1 = http;
} // namespace pace

#ifdef PACE_HAS_HTTP1
export namespace pace::http
{
using ::pace::http::admission;
using ::pace::http::app;
using ::pace::http::async_fetch;
using ::pace::http::async_get;
using ::pace::http::async_post;
using ::pace::http::basic_client;
using ::pace::http::body_source;
using ::pace::http::cache_options;
using ::pace::http::cache_statistics;
using ::pace::http::client;
using ::pace::http::client_close_policy;
using ::pace::http::client_options;
using ::pace::http::fetch;
using ::pace::http::get;
using ::pace::http::limits;
using ::pace::http::load_balance;
using ::pace::http::post;
using ::pace::http::proxy_options;
using ::pace::http::request_head;
using ::pace::http::response_cache;
using ::pace::http::reverse_proxy;
using ::pace::http::serve;
using ::pace::http::serve_connection;
using ::pace::http::server_handler;
using ::pace::http::server_options;
using ::pace::http::server_response;
using ::pace::http::static_files;
using ::pace::http::static_options;
using ::pace::http::streaming_response;
} // namespace pace::http
#endif

#ifdef PACE_HAS_TLS
export namespace pace::tls
{
using ::pace::tls::accept;
using ::pace::tls::client_hello;
using ::pace::tls::client_hello_action;
using ::pace::tls::client_hello_callback;
using ::pace::tls::client_hello_handoff;
using ::pace::tls::client_hello_key_share;
using ::pace::tls::connect;
using ::pace::tls::context;
using ::pace::tls::pin_type;
using ::pace::tls::role;
using ::pace::tls::sha256_digest;
using ::pace::tls::sni_policy;
using ::pace::tls::stream;
using ::pace::tls::trust_source;
using ::pace::tls::verification_callback;
} // namespace pace::tls
#endif

#ifdef PACE_HAS_DNS
export namespace pace::dns
{
using ::pace::dns::address;
using ::pace::dns::error_category;
using ::pace::dns::family;
using ::pace::dns::options;
using ::pace::dns::resolve;
using ::pace::dns::resolver;
using ::pace::dns::result;
} // namespace pace::dns
#endif

#ifdef PACE_HAS_QUIC
export namespace pace::quic
{
using ::pace::quic::connect;
using ::pace::quic::connection;
using ::pace::quic::context;
using ::pace::quic::listen;
using ::pace::quic::listener;
using ::pace::quic::role;
using ::pace::quic::stream;
using ::pace::quic::stream_type;
} // namespace pace::quic
#endif

#ifdef PACE_HAS_HTTP2
export namespace pace::http2
{
using ::pace::http2::async_exchange;
using ::pace::http2::async_fetch;
using ::pace::http2::async_get;
using ::pace::http2::async_post;
using ::pace::http2::fetch;
using ::pace::http2::get;
using ::pace::http2::limits;
using ::pace::http2::post;
using ::pace::http2::serve;
using ::pace::http2::serve_connection;
} // namespace pace::http2
#endif

#ifdef PACE_HAS_HTTP3
export namespace pace::http3
{
using ::pace::http3::async_exchange;
using ::pace::http3::async_fetch;
using ::pace::http3::async_get;
using ::pace::http3::async_post;
using ::pace::http3::fetch;
using ::pace::http3::get;
using ::pace::http3::limits;
using ::pace::http3::post;
using ::pace::http3::serve;
using ::pace::http3::serve_connection;
} // namespace pace::http3
#endif

#ifdef PACE_HAS_WEBSOCKET
export namespace pace::websocket
{
using ::pace::websocket::accept;
using ::pace::websocket::basic_stream;
using ::pace::websocket::connect;
using ::pace::websocket::limits;
using ::pace::websocket::message;
using ::pace::websocket::message_type;
using ::pace::websocket::protocol_error;
using ::pace::websocket::role;
using ::pace::websocket::stream;
using ::pace::websocket::upgrade;
using ::pace::websocket::upgrade_request;
} // namespace pace::websocket
#endif

#if defined(PACE_HAS_HTTP1) && defined(PACE_HAS_TLS)
export namespace pace::http
{
using ::pace::http::secure_client;
} // namespace pace::http
#endif
#if defined(PACE_HAS_WEBSOCKET) && defined(PACE_HAS_TLS)
export namespace pace::websocket
{
using ::pace::websocket::secure_stream;
} // namespace pace::websocket
#endif
