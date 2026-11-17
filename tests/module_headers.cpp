// Deliberately compiled through headers to check cross-TU ABI compatibility.
#include <pace/networking.hpp>
#include <pace/http_types.hpp>

pace::task<int> module_header_task(pace::io_context& io) {
    co_await io.yield();
    co_return 42;
}
pace::http::response module_header_response(pace::http::request request) {
    return pace::http::text(std::move(request.target));
}
