#pragma once

#include <new>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace pace
{
template <class T>
class result;
template <class T>
class task;

/// Selects native error reporting on sockets. Also adapts an existing protocol task.
struct as_result_t
{
    // Require an explicit tag so existing braced socket arguments keep their overload.
    explicit constexpr as_result_t(std::in_place_t) noexcept
    {
    }

    template <class T>
    task<result<T>> operator()(task<T> operation) const;
};

inline constexpr as_result_t as_result{std::in_place};

/// A successful value (including a move-only socket), or the error that prevented completion.
template <class T>
class [[nodiscard]] result
{
public:
    result(T value)
        : storage_(std::in_place_index<0>, std::move(value))
    {
    }

    static result failure(std::error_code error) noexcept
    {
        return result(std::in_place_index<1>, error ? error : std::make_error_code(std::errc::io_error));
    }

    explicit operator bool() const noexcept
    {
        return storage_.index() == 0;
    }

    std::error_code error() const noexcept
    {
        if (const auto* error = std::get_if<1>(&storage_))
        {
            return *error;
        }
        return storage_.valueless_by_exception() ? std::make_error_code(std::errc::state_not_recoverable)
                                                 : std::error_code{};
    }

    T& value() &
    {
        check();
        return std::get<0>(storage_);
    }

    const T& value() const&
    {
        check();
        return std::get<0>(storage_);
    }

    T&& value() &&
    {
        check();
        return std::move(std::get<0>(storage_));
    }

private:
    result(std::in_place_index_t<1>, std::error_code error)
        : storage_(std::in_place_index<1>, error)
    {
    }

    void check() const
    {
        if (!*this)
        {
            throw std::system_error(error());
        }
    }

    std::variant<T, std::error_code> storage_;
};

template <>
class [[nodiscard]] result<void>
{
public:
    result() = default;

    static result failure(std::error_code error) noexcept
    {
        return result(error ? error : std::make_error_code(std::errc::io_error));
    }

    explicit operator bool() const noexcept
    {
        return !error_;
    }

    std::error_code error() const noexcept
    {
        return error_;
    }

    void value() const
    {
        if (error_)
        {
            throw std::system_error(error_);
        }
    }

private:
    explicit result(std::error_code error)
        : error_(error)
    {
    }

    std::error_code error_;
};

/// Internal support shared by result-returning tasks and the two socket API forms.
struct result_access
{
    template <class>
    struct result_type : std::false_type
    {
    };

    template <class T>
    struct result_type<result<T>> : std::true_type
    {
    };

    template <class T>
    static constexpr bool is_result = result_type<T>::value;

    template <bool Report, class T>
    using value = std::conditional_t<Report, result<T>, T>;

    template <bool Report, class T = void>
    static value<Report, T> fail(std::error_code error, const char* operation)
    {
        if constexpr (Report)
        {
            return result<T>::failure(error);
        }
        else
        {
            throw std::system_error(error, operation);
        }
    }

    template <bool Report>
    static value<Report, void> success()
    {
        if constexpr (Report)
        {
            return result<void>{};
        }
    }

    static std::error_code current_error() noexcept
    {
        try
        {
            throw;
        }
        catch (const std::system_error& e)
        {
            return e.code();
        }
        catch (const std::bad_alloc&)
        {
            return std::make_error_code(std::errc::not_enough_memory);
        }
        catch (const std::length_error&)
        {
            return std::make_error_code(std::errc::message_size);
        }
        catch (const std::invalid_argument&)
        {
            return std::make_error_code(std::errc::invalid_argument);
        }
        catch (const std::logic_error&)
        {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        catch (...)
        {
            return std::make_error_code(std::errc::io_error);
        }
    }
};
} // namespace pace
