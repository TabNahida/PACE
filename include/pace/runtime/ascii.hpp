#pragma once

// Locale-independent ASCII case folding for protocol tokens: HTTP field names, WebSocket and HTTP
// header tokens, DNS names. Bytes outside A-Z are left unchanged, so UTF-8 passes through.

#include <algorithm>
#include <string>
#include <string_view>

namespace pace::ascii
{
[[nodiscard]] constexpr char to_lower(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] constexpr bool iequals(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size()
           && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return to_lower(x) == to_lower(y); });
}

[[nodiscard]] inline std::string to_lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](char c) { return to_lower(c); });
    return text;
}
} // namespace pace::ascii
