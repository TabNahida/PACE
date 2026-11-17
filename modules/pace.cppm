// ABI-preserving facade: declarations belong to the global module, just as
// they do for #include consumers. Keep this export list in sync with the API.
module;
#include <pace/pace.hpp>
#include <pace/http_router.hpp>

export module pace;

export namespace pace {
    using ::pace::task;
    using ::pace::io_context;
    using ::pace::runtime;
    using ::pace::run;
    using ::pace::this_context;
    using ::pace::sleep_for;
    using ::pace::yield;
    using ::pace::native_socket_type;
    using ::pace::invalid_socket;
    using ::pace::address_family;
    using ::pace::tcp_stream;
    using ::pace::tcp_listener;
    using ::pace::ipv4_endpoint;
    using ::pace::listen_options;
}

export namespace pace::tcp {
    using ::pace::tcp::endpoint;
    using ::pace::tcp::listen_options;
    using ::pace::tcp::stream;
    using ::pace::tcp::listener;
    using ::pace::tcp::connect;
}

export namespace pace::udp {
    using ::pace::udp::endpoint;
    using ::pace::udp::bind_options;
    using ::pace::udp::datagram;
    using ::pace::udp::socket;
}

export namespace pace::http {
    using ::pace::http::header_field;
    using ::pace::http::request;
    using ::pace::http::response;
    using ::pace::http::protocol_error;
    using ::pace::http::handler;
    using ::pace::http::text;
    using ::pace::http::html;
    using ::pace::http::json;
    using ::pace::http::make_handler;
    using ::pace::http::router;
}

#ifdef PACE_HAS_HTTP1
export namespace pace::http {
    using ::pace::http::limits;
    using ::pace::http::client_close_policy;
    using ::pace::http::client_options;
    using ::pace::http::request_head;
    using ::pace::http::admission;
    using ::pace::http::body_source;
    using ::pace::http::streaming_response;
    using ::pace::http::server_response;
    using ::pace::http::server_handler;
    using ::pace::http::server_options;
    using ::pace::http::serve_connection;
    using ::pace::http::serve;
    using ::pace::http::basic_client;
    using ::pace::http::client;
    using ::pace::http::fetch;
    using ::pace::http::get;
    using ::pace::http::post;
    using ::pace::http::app;
}
#endif

#ifdef PACE_HAS_TLS
export namespace pace::tls {
    using ::pace::tls::role;
    using ::pace::tls::trust_source;
    using ::pace::tls::sni_policy;
    using ::pace::tls::context;
    using ::pace::tls::stream;
}
#endif

#ifdef PACE_HAS_DNS
export namespace pace::dns {
    using ::pace::dns::error_category;
    using ::pace::dns::family;
    using ::pace::dns::address;
    using ::pace::dns::result;
    using ::pace::dns::options;
    using ::pace::dns::resolver;
}
#endif

#ifdef PACE_HAS_QUIC
export namespace pace::quic {
    using ::pace::quic::role;
    using ::pace::quic::stream_type;
    using ::pace::quic::context;
    using ::pace::quic::stream;
    using ::pace::quic::connection;
    using ::pace::quic::listener;
}
#endif

#ifdef PACE_HAS_HTTP2
export namespace pace::http2 {
    using ::pace::http2::limits;
    using ::pace::http2::exchange;
    using ::pace::http2::serve_connection;
    using ::pace::http2::serve;
}
#endif

#ifdef PACE_HAS_HTTP3
export namespace pace::http3 {
    using ::pace::http3::limits;
    using ::pace::http3::exchange;
    using ::pace::http3::serve_connection;
    using ::pace::http3::serve;
}
#endif

#ifdef PACE_HAS_WEBSOCKET
export namespace pace::websocket {
    using ::pace::websocket::role;
    using ::pace::websocket::message_type;
    using ::pace::websocket::message;
    using ::pace::websocket::limits;
    using ::pace::websocket::upgrade_request;
    using ::pace::websocket::protocol_error;
    using ::pace::websocket::basic_stream;
    using ::pace::websocket::stream;
}
#endif

#if defined(PACE_HAS_HTTP1) && defined(PACE_HAS_TLS)
export namespace pace::http { using ::pace::http::secure_client; }
#endif
#if defined(PACE_HAS_WEBSOCKET) && defined(PACE_HAS_TLS)
export namespace pace::websocket { using ::pace::websocket::secure_stream; }
#endif
