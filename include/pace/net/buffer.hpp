#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace pace
{
/// Views text as the bytes byte streams read and write; no copy. The view borrows `text`.
inline std::span<const std::byte> as_bytes(std::string_view text) noexcept
{
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// Views a character buffer as writable bytes, for reading into it; no copy. The view borrows `characters`.
inline std::span<std::byte> as_writable_bytes(std::span<char> characters) noexcept
{
    return {reinterpret_cast<std::byte*>(characters.data()), characters.size()};
}

/// Views the characters of `text` as writable bytes, for reading into an already-sized string; no copy.
inline std::span<std::byte> as_writable_bytes(std::string& text) noexcept
{
    return as_writable_bytes(std::span<char>(text));
}

/// Views bytes as text, for parsing what was read; no copy. The view borrows `bytes`.
inline std::string_view as_text(std::span<const std::byte> bytes) noexcept
{
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
} // namespace pace
