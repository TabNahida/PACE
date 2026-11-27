#pragma once

#include <pace/http/types.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace pace::http
{
/// Implementation support for make_handler: invoking a handler callable with or without the request. Not part of
/// the API.
struct handler_access
{
    /// Calls `function` with the request when it accepts one, else with no arguments.
    template <class Function>
    static auto call(Function& function, request incoming)
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
};

// Coroutines in public headers stay namespace-scope functions: GCC 16 cannot read a class-scope coroutine back
// from a named-module interface ("failed to read compiled module cluster"), so these are not members of the
// handler_access struct above.

/// Implementation support for make_handler: a stateful callable, kept alive by `function` until the task
/// completes. Not part of the API.
template <class Function>
task<response> invoke_shared_handler(std::shared_ptr<Function> function, request incoming)
{
    using result = decltype(handler_access::call(*function, std::move(incoming)));
    if constexpr (std::is_same_v<result, response>)
    {
        co_return handler_access::call(*function, std::move(incoming));
    }
    else
    {
        co_return co_await handler_access::call(*function, std::move(incoming));
    }
}

/// Implementation support for make_handler: a function pointer or stateless callable, copied into the task. Not
/// part of the API.
// Stateless trivial callables and function pointers need no shared owner.
// The coroutine frame owns its copy, including across a nested coroutine call.
// Keep invocation lazy and exceptions inside the returned task.
template <class Function>
task<response> invoke_copied_handler(Function function, request incoming)
{
    using result = decltype(handler_access::call(function, std::move(incoming)));
    if constexpr (std::is_same_v<result, response>)
    {
        co_return handler_access::call(function, std::move(incoming));
    }
    else
    {
        co_return co_await handler_access::call(function, std::move(incoming));
    }
}

/// Implementation support for router: a task that returns `outgoing`. Not part of the API.
inline task<response> immediate_response(response outgoing)
{
    co_return outgoing;
}

/// Adapts a callable taking `request` (or nothing) and returning `response` or `task<response>` to `handler`.
/// The callable, move-only captures included, stays alive through every started invocation, even if the
/// adapter is destroyed meanwhile. It runs when the task is first resumed; its exceptions are stored in the task.
template <class Function>
handler make_handler(Function&& function)
{
    using owned_type = std::decay_t<Function>;
    using result = decltype(handler_access::call(std::declval<owned_type&>(), std::declval<request>()));
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
        { return invoke_copied_handler(owned, std::move(incoming)); };
    }
    else
    {
        auto owned = std::make_shared<owned_type>(std::forward<Function>(function));
        return [owned = std::move(owned)](request incoming)
        { return invoke_shared_handler(owned, std::move(incoming)); };
    }
}

/// Exact, case-sensitive method and path routing for HTTP/1, HTTP/2 and HTTP/3; usable as a `handler`.
/// The query is ignored for matching and percent escapes are not decoded. Register every route before serving.
/// Copies share one table until a registration on a copy detaches it; the callables stay shared, so state they
/// share across worker threads must be synchronized.
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
    /// Registers `function` (adapted with make_handler) for an exact method and path. Throws
    /// std::invalid_argument for a method that is not a token, a path that does not start with '/' or contains
    /// spaces, controls, '?' or '#', or a duplicate method and path.
    template <class Function>
    router& add(std::string method, std::string path, Function&& function)
    {
        const auto token = [](unsigned char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                   || std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos;
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
            routes_->begin() + static_cast<std::ptrdiff_t>(position),
            {std::move(method), std::move(path), make_handler(std::forward<Function>(function))}
        );
        return *this;
    }

    /// add("GET", path, function).
    template <class Function>
    router& get(std::string path, Function&& function)
    {
        return add("GET", std::move(path), std::forward<Function>(function));
    }

    /// add("POST", path, function).
    template <class Function>
    router& post(std::string path, Function&& function)
    {
        return add("POST", std::move(path), std::forward<Function>(function));
    }

    /// Registers `function` for every method on paths under `prefix`, matched by whole segments. Exact paths win;
    /// otherwise the longest prefix wins. The handler receives the full target. Trailing slashes are dropped.
    /// Throws std::invalid_argument for an invalid or duplicate prefix.
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

    /// Dispatches one request. HEAD falls back to GET. No match is 404; a known path with another method is 405
    /// with an Allow field. Exceptions of the selected handler propagate from the task.
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
