#pragma once

#include <pace/http_types.hpp>
#include <memory>
#include <type_traits>
#include <utility>

namespace pace::http {
namespace detail {
template<class Function>
task<response> invoke_handler(std::shared_ptr<Function> function, request incoming) {
    using result = std::invoke_result_t<Function&, request>;
    if constexpr (std::is_same_v<result, response>)
        co_return std::invoke(*function, std::move(incoming));
    else
        co_return co_await std::invoke(*function, std::move(incoming));
}
inline task<response> immediate_response(response outgoing) { co_return outgoing; }
} // namespace detail

// Adapt a synchronous response or task<response> callable to the shared HTTP
// handler type. The callable (including move-only captures) is owned through
// every invocation, even if the adapter/router is destroyed while suspended.
template<class Function>
handler make_handler(Function&& function) {
    using owned_type = std::decay_t<Function>;
    using result = std::invoke_result_t<owned_type&, request>;
    static_assert(std::is_same_v<result, response> || std::is_same_v<result, task<response>>,
                  "an HTTP handler must return response or task<response>");
    auto owned = std::make_shared<owned_type>(std::forward<Function>(function));
    return [owned = std::move(owned)](request incoming) {
        return detail::invoke_handler(owned, std::move(incoming));
    };
}

// Exact, case-sensitive method/path routing for HTTP/1, HTTP/2 and HTTP/3.
// Queries are excluded from path matching; percent escapes are not decoded.
// Configure before serving. Copies share a table until registration detaches
// it; callable instances stay shared. Synchronize state shared across workers.
class router {
    struct route { std::string method, path; handler handle; };
    // HTTP servers copy their handler per connection. Share the read-only table
    // there instead of allocating/copying every route string on each accept.
    std::shared_ptr<std::vector<route>> routes_ = std::make_shared<std::vector<route>>();
public:
    template<class Function>
    router& add(std::string method, std::string path, Function&& function) {
        const auto token = [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
        };
        if (method.empty() || path.empty() || path.front() != '/')
            throw std::invalid_argument("route needs a method and an absolute path");
        for (unsigned char c : method) if (!token(c)) throw std::invalid_argument("invalid route method");
        for (unsigned char c : path)
            if (c <= 32 || c == 127 || c == '?' || c == '#') throw std::invalid_argument("invalid route path");
        if (routes_)
            for (const auto& entry : *routes_)
                if (entry.method == method && entry.path == path) throw std::invalid_argument("duplicate route");
        if (!routes_) routes_ = std::make_shared<std::vector<route>>();
        else if (!routes_.unique()) routes_ = std::make_shared<std::vector<route>>(*routes_);
        routes_->push_back({std::move(method), std::move(path), make_handler(std::forward<Function>(function))});
        return *this;
    }
    template<class Function> router& get(std::string path, Function&& function) {
        return add("GET", std::move(path), std::forward<Function>(function));
    }
    template<class Function> router& post(std::string path, Function&& function) {
        return add("POST", std::move(path), std::forward<Function>(function));
    }
    task<response> operator()(request incoming) const {
        if (!routes_) return detail::immediate_response({.status = 404, .body = "Not Found\n"});
        const auto path = std::string_view(incoming.target).substr(0, incoming.target.find('?'));
        const handler* get_fallback = nullptr;
        bool path_found = false, has_head = false;
        for (const auto& entry : *routes_) {
            if (entry.path != path) continue;
            path_found = true;
            if (entry.method == incoming.method) return entry.handle(std::move(incoming));
            if (entry.method == "GET") get_fallback = &entry.handle;
            if (entry.method == "HEAD") has_head = true;
        }
        if (incoming.method == "HEAD" && get_fallback) return (*get_fallback)(std::move(incoming));
        if (!path_found) return detail::immediate_response({.status = 404, .body = "Not Found\n"});
        std::string allow;
        for (const auto& entry : *routes_) {
            if (entry.path != path) continue;
            if (!allow.empty()) allow += ", ";
            allow += entry.method;
            if (entry.method == "GET" && !has_head) allow += ", HEAD";
        }
        return detail::immediate_response({.status = 405, .body = "Method Not Allowed\n", .headers = {{"Allow", std::move(allow)}}});
    }
};
} // namespace pace::http
