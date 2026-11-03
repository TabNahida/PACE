#include <pace/http.hpp>
#include <iostream>
int main(int argc, char** argv) {
    try {
        if (argc != 7) throw std::invalid_argument("usage: pace_http_gateway MODE PORT WORKERS RESOURCE CACHE_BYTES PAYLOAD_BYTES");
        const std::string mode = argv[1];
        const auto port = static_cast<std::uint16_t>(std::stoul(argv[2]));
        const auto workers = std::stoul(argv[3]);
        const auto bytes = std::stoull(argv[5]);
        pace::http::app app;
        if (mode == "static") {
            pace::http::static_options options;
            options.cache = {.max_bytes=bytes, .ttl=std::chrono::seconds(60)};
            options.cache_control = "public, max-age=60";
            app.mount("/", argv[4], options);
        } else if (mode == "proxy") {
            pace::http::proxy_options options;
            options.cache = {.max_bytes=bytes, .ttl=std::chrono::seconds(60)};
            options.max_connections = 1024; options.max_idle_connections = 256;
            app.proxy("/", std::string(argv[4]), options);
        } else if (mode == "origin") {
            const std::string payload(std::stoul(argv[6]), 'x');
            app.get("/data.bin", [payload] {
                return pace::http::response{.body=payload, .content_type="application/octet-stream",
                    .headers={{"Cache-Control", "public, max-age=60"}}};
            });
        } else throw std::invalid_argument("unknown gateway mode");
        pace::io_context probe;
        std::cout << pace::backend_name(probe.backend()) << std::endl;
        pace::http::limits limits; limits.max_requests_per_connection = 10000000;
        app.run_workers(workers, {"127.0.0.1", port}, limits);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
