// Standard-library headers precede the import; no PACE headers in this TU.
#include <chrono>
#include <coroutine>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
import pace;

pace::task<int> module_header_task(pace::io_context&);
pace::http::response module_header_response(pace::http::request);

int main()
{
    static_assert(!std::is_copy_constructible_v<pace::task<int>>);
    static_assert(std::is_same_v<pace::http1::request, pace::http::request>);
    pace::runtime workers(2);
    workers.stop();
    workers.run();
    return pace::run(
        [](pace::io_context& io) -> pace::task<int>
        {
            if (&pace::this_context() != &io)
            {
                throw std::runtime_error("context affinity");
            }
            if (co_await module_header_task(io) != 42)
            {
                throw std::runtime_error("task ABI");
            }
            (void)pace::backend_name(io.backend());
            (void)co_await pace::this_task::cancellation();
            co_await pace::sleep_for(std::chrono::milliseconds(0));
            co_await pace::yield();
            pace::tcp::listener listener(io, {"127.0.0.1", 0});
            auto client = co_await pace::tcp::connect(io, listener.local_endpoint());
            auto accepted = co_await listener.accept();
            if (!client.is_open() || !accepted.is_open())
            {
                throw std::runtime_error("TCP connection");
            }
            pace::udp::socket datagrams(io, {"127.0.0.1", 0});
            pace::http::router routes;
            routes.get("/module", module_header_response);
            pace::http::request request;
            request.method = "GET";
            request.target = "/module";
            const auto response = co_await routes(std::move(request));
            if (response.body != "/module")
            {
                throw std::runtime_error("response ABI");
            }
#ifdef PACE_HAS_HTTP1
            pace::http::app app;
            app.get("/", [] { return pace::http::text("module"); });
            pace::http::client http(std::move(client));
#endif
#ifdef PACE_HAS_WEBSOCKET
            pace::websocket::stream websocket(std::move(accepted), pace::websocket::role::server);
#endif
#ifdef PACE_HAS_TLS
            pace::tls::context tls(pace::tls::role::client, pace::tls::trust_source::application);
#endif
#ifdef PACE_HAS_DNS
            pace::dns::resolver dns(io);
            (void)pace::dns::error_category();
#endif
#ifdef PACE_HAS_QUIC
            pace::quic::context quic(pace::quic::role::client);
#endif
#ifdef PACE_HAS_HTTP2
            pace::http2::limits h2;
            (void)h2;
#endif
#ifdef PACE_HAS_HTTP3
            pace::http3::limits h3;
            (void)h3;
#endif
            co_return 0;
        }
    );
}
