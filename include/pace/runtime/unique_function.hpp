#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace pace
{
/// Move-only callable. Small, suitably aligned, nothrow-movable targets stay inline.
/// Invoking an empty function throws std::bad_function_call.
template <class Signature>
class unique_function;

template <class R, class... Args>
class unique_function<R(Args...)>
{
    struct storage
    {
        alignas(void*) std::byte bytes[3 * sizeof(void*)];
    };

    template <class F>
    static constexpr bool fits =
        sizeof(F) <= sizeof(storage) && alignof(F) <= alignof(storage) && std::is_nothrow_move_constructible_v<F>;

    template <class F>
    using stored = std::conditional_t<fits<F>, F, std::unique_ptr<F>>;

    template <class F>
    static stored<F>& get(storage& s) noexcept
    {
        return *std::launder(reinterpret_cast<stored<F>*>(s.bytes));
    }

    template <class F>
    static F& target(storage& s) noexcept
    {
        if constexpr (fits<F>)
        {
            return get<F>(s);
        }
        else
        {
            return *get<F>(s);
        }
    }

    struct operations
    {
        R (*invoke)(storage&, Args&&...);
        void (*move)(storage&, storage&) noexcept;
        void (*destroy)(storage&) noexcept;
    };

    template <class F>
    static constexpr operations table{
        [](storage& s, Args&&... args) -> R
        {
            if constexpr (std::is_void_v<R>)
            {
                std::invoke(target<F>(s), std::forward<Args>(args)...);
            }
            else
            {
                return std::invoke(target<F>(s), std::forward<Args>(args)...);
            }
        },
        [](storage& from, storage& to) noexcept
        {
            auto& value = get<F>(from);
            ::new (static_cast<void*>(to.bytes)) stored<F>(std::move(value));
            std::destroy_at(std::addressof(value));
        },
        [](storage& s) noexcept { std::destroy_at(std::addressof(get<F>(s))); }
    };

    storage data_;
    const operations* ops_ = nullptr;

    void reset() noexcept
    {
        if (auto* ops = std::exchange(ops_, nullptr))
        {
            ops->destroy(data_);
        }
    }

public:
    unique_function() noexcept = default;

    unique_function(std::nullptr_t) noexcept
    {
    }

    template <class F>
    requires(
        !std::same_as<std::decay_t<F>, unique_function> && std::is_constructible_v<std::decay_t<F>, F>
        && std::is_invocable_r_v<R, std::decay_t<F>&, Args...>
        && (!std::is_reference_v<R>
            || std::is_convertible_v<
                std::add_pointer_t<std::remove_reference_t<std::invoke_result_t<std::decay_t<F>&, Args...>>>,
                std::add_pointer_t<std::remove_reference_t<R>>>)
        && (!std::is_reference_v<R> || std::is_reference_v<std::invoke_result_t<std::decay_t<F>&, Args...>>)
    )
    unique_function(F&& callable)
    {
        using value = std::decay_t<F>;
        if constexpr (
            std::is_pointer_v<value> || std::is_member_pointer_v<value>
            || std::same_as<value, std::function<R(Args...)>>
        )
        {
            if (!callable)
            {
                return;
            }
        }
        if constexpr (fits<value>)
        {
            ::new (static_cast<void*>(data_.bytes)) value(std::forward<F>(callable));
        }
        else
        {
            ::new (static_cast<void*>(data_.bytes)) stored<value>(std::make_unique<value>(std::forward<F>(callable)));
        }
        ops_ = &table<value>;
    }

    unique_function(const unique_function&) = delete;
    unique_function& operator=(const unique_function&) = delete;

    unique_function(unique_function&& other) noexcept
        : ops_(std::exchange(other.ops_, nullptr))
    {
        if (ops_)
        {
            ops_->move(other.data_, data_);
        }
    }

    unique_function& operator=(unique_function&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            ops_ = std::exchange(other.ops_, nullptr);
            if (ops_)
            {
                ops_->move(other.data_, data_);
            }
        }
        return *this;
    }

    ~unique_function()
    {
        reset();
    }

    explicit operator bool() const noexcept
    {
        return ops_ != nullptr;
    }

    R operator()(Args... args)
    {
        if (!ops_)
        {
            throw std::bad_function_call();
        }
        return ops_->invoke(data_, std::forward<Args>(args)...);
    }
};
} // namespace pace
