#pragma once

#include <pace/http/types.hpp>

#include <algorithm>
#include <memory>
#include <type_traits>
#include <utility>

namespace pace::http
{
template <class Function>
auto call_handler(Function& function, request incoming)
{
    if constexpr (std::is_invocable_v<Function&, request>)
    {
        return std::invoke(function, std::move(incoming));
    }
    else
    {
        return std::invoke(function);
    }
}

template <class Function>
task<response> invoke_handler(std::shared_ptr<Function> function, request incoming)
{
    using result = decltype(call_handler(*function, std::move(incoming)));
    if constexpr (std::is_same_v<result, response>)
    {
        co_return call_handler(*function, std::move(incoming));
    }
    else
    {
        co_return co_await call_handler(*function, std::move(incoming));
    }
}

// Stateless trivial callables and function pointers need no shared owner.
// The coroutine frame owns its copy, including across a nested coroutine call.
// Keep invocation lazy and exceptions inside the returned task.
template <class Function>
task<response> invoke_function(Function function, request incoming)
{
    using result = decltype(call_handler(function, std::move(incoming)));
    if constexpr (std::is_same_v<result, response>)
    {
        co_return call_handler(function, std::move(incoming));
    }
    else
    {
        co_return co_await call_handler(function, std::move(incoming));
    }
}

inline task<response> immediate_response(response outgoing)
{
    co_return outgoing;
}

// Adapt a synchronous response or task<response> callable to the shared HTTP
// handler type. The callable (including move-only captures) is owned through
// every invocation, even if the adapter/router is destroyed while suspended.
template <class Function>
handler make_handler(Function&& function)
{
    using owned_type = std::decay_t<Function>;
    using result = decltype(call_handler(std::declval<owned_type&>(), std::declval<request>()));
    static_assert(
        std::is_same_v<result, response> || std::is_same_v<result, task<response>>,
        "an HTTP handler must return response or task<response>"
    );
    if constexpr (
        std::is_pointer_v<owned_type>
        || (std::is_empty_v<owned_type> && std::is_trivially_copy_constructible_v<owned_type>
            && std::is_trivially_destructible_v<owned_type>)
    )
    {
        return [owned = owned_type(std::forward<Function>(function))](request incoming)
        { return invoke_function(owned, std::move(incoming)); };
    }
    else
    {
        auto owned = std::make_shared<owned_type>(std::forward<Function>(function));
        return [owned = std::move(owned)](request incoming) { return invoke_handler(owned, std::move(incoming)); };
    }
}

// Exact, case-sensitive method/path routing for HTTP/1, HTTP/2 and HTTP/3.
// Queries are excluded from path matching; percent escapes are not decoded.
// Configure before serving. Copies share a table until registration detaches
// it; stateful callable instances stay shared. Synchronize state shared across workers.
class router
{
    struct route
    {
        std::string method, path;
        handler handle;
        bool prefix = false;
    };

    // HTTP servers copy their handler per connection. Share the read-only table
    // there instead of allocating/copying every route string on each accept.
    std::shared_ptr<std::vector<route>> routes_ = std::make_shared<std::vector<route>>();
    std::shared_ptr<std::vector<route>> mounts_;

    // Group paths in sorted order, retaining registration order within a path
    // for Allow. Registration is cold; requests perform one binary search.
    static auto first_path(const std::vector<route>& routes, std::string_view path)
    {
        return std::lower_bound(
            routes.begin(),
            routes.end(),
            path,
            [](const route& entry, std::string_view key) { return entry.path < key; }
        );
    }

public:
    template <class Function>
    router& add(std::string method, std::string path, Function&& function)
    {
        const auto token = [](unsigned char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                   || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
        };
        if (method.empty() || path.empty() || path.front() != '/')
        {
            throw std::invalid_argument("route needs a method and an absolute path");
        }
        for (unsigned char c : method)
        {
            if (!token(c))
            {
                throw std::invalid_argument("invalid route method");
            }
        }
        for (unsigned char c : path)
        {
            if (c <= 32 || c == 127 || c == '?' || c == '#')
            {
                throw std::invalid_argument("invalid route path");
            }
        }
        std::size_t position = 0;
        if (routes_)
        {
            auto it = first_path(*routes_, path);
            for (; it != routes_->end() && it->path == path; ++it)
            {
                if (it->method == method)
                {
                    throw std::invalid_argument("duplicate route");
                }
            }
            position = static_cast<std::size_t>(it - routes_->begin());
        }
        if (!routes_)
        {
            routes_ = std::make_shared<std::vector<route>>();
        }
        else if (routes_.use_count() != 1)
        {
            routes_ = std::make_shared<std::vector<route>>(*routes_);
        }
        routes_->insert(
            routes_->begin() + position,
            {std::move(method), std::move(path), make_handler(std::forward<Function>(function))}
        );
        return *this;
    }

    template <class Function>
    router& get(std::string path, Function&& function)
    {
        return add("GET", std::move(path), std::forward<Function>(function));
    }

    template <class Function>
    router& post(std::string path, Function&& function)
    {
        return add("POST", std::move(path), std::forward<Function>(function));
    }

    // Exact routes take priority; otherwise select the longest segment prefix.
    template <class Function>
    router& mount(std::string prefix, Function&& function)
    {
        if (prefix.empty() || prefix.front() != '/' || prefix.find_first_of("?#\\") != std::string::npos)
        {
            throw std::invalid_argument("invalid mount prefix");
        }
        for (unsigned char c : prefix)
        {
            if (c <= 32 || c == 127)
            {
                throw std::invalid_argument("invalid mount prefix");
            }
        }
        while (prefix.size() > 1 && prefix.back() == '/')
        {
            prefix.pop_back();
        }
        if (!routes_)
        {
            routes_ = std::make_shared<std::vector<route>>();
        }
        if (!mounts_)
        {
            mounts_ = std::make_shared<std::vector<route>>();
        }
        for (const auto& entry : *mounts_)
        {
            if (entry.prefix && entry.path == prefix)
            {
                throw std::invalid_argument("duplicate mount");
            }
        }
        if (mounts_.use_count() != 1)
        {
            mounts_ = std::make_shared<std::vector<route>>(*mounts_);
        }
        mounts_->push_back({{}, std::move(prefix), make_handler(std::forward<Function>(function)), true});
        return *this;
    }

    task<response> operator()(request incoming) const
    {
        if (!routes_)
        {
            return immediate_response({.status = 404, .body = "Not Found\n"});
        }
        const auto path = std::string_view(incoming.target).substr(0, incoming.target.find('?'));
        const handler* get_fallback = nullptr;
        bool has_head = false;
        const auto first = first_path(*routes_, path);
        auto last = first;
        for (; last != routes_->end() && last->path == path; ++last)
        {
            const auto& entry = *last;
            if (entry.method == incoming.method)
            {
                return entry.handle(std::move(incoming));
            }
            if (entry.method == "GET")
            {
                get_fallback = &entry.handle;
            }
            if (entry.method == "HEAD")
            {
                has_head = true;
            }
        }
        if (incoming.method == "HEAD" && get_fallback)
        {
            return (*get_fallback)(std::move(incoming));
        }
        if (first == last && mounts_)
        {
            const route* match = nullptr;
            for (const auto& entry : *mounts_)
            {
                if (path.starts_with(entry.path)
                    && (entry.path == "/" || path.size() == entry.path.size() || path[entry.path.size()] == '/')
                    && (!match || entry.path.size() > match->path.size()))
                {
                    match = &entry;
                }
            }
            if (match)
            {
                return match->handle(std::move(incoming));
            }
        }
        if (first == last)
        {
            return immediate_response({.status = 404, .body = "Not Found\n"});
        }
        std::string allow;
        for (auto it = first; it != last; ++it)
        {
            const auto& entry = *it;
            if (!allow.empty())
            {
                allow += ", ";
            }
            allow += entry.method;
            if (entry.method == "GET" && !has_head)
            {
                allow += ", HEAD";
            }
        }
        return immediate_response(
            {.status = 405, .body = "Method Not Allowed\n", .headers = {{"Allow", std::move(allow)}}}
        );
    }
};
} // namespace pace::http
