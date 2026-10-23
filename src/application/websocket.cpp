#include <pace/websocket.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>
#if defined(_WIN32)
#include <bcrypt.h>
#elif defined(__linux__)
#include <sys/random.h>
#else
#include <cstdlib>
#endif

namespace pace::websocket {
namespace {
[[noreturn]] void invalid(const char* text, std::uint16_t code = 1002) { throw protocol_error(code, text); }
void random_bytes(std::span<unsigned char> bytes) {
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("WebSocket secure random generation failed");
#elif defined(__linux__)
    while (!bytes.empty()) {
        const auto count = ::getrandom(bytes.data(), bytes.size(), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::system_error(errno, std::generic_category(), "WebSocket getrandom");
        bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
#else
    ::arc4random_buf(bytes.data(), bytes.size());
#endif
}
std::string base64(std::span<const unsigned char> bytes) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto left = bytes.size() - i;
        const unsigned bits = static_cast<unsigned>(bytes[i]) << 16 |
            (left > 1 ? static_cast<unsigned>(bytes[i + 1]) << 8 : 0) | (left > 2 ? bytes[i + 2] : 0);
        output += alphabet[bits >> 18]; output += alphabet[(bits >> 12) & 63];
        output += left > 1 ? alphabet[(bits >> 6) & 63] : '=';
        output += left > 2 ? alphabet[bits & 63] : '=';
    }
    return output;
}
bool valid_key(std::string_view key) {
    if (key.size() != 24 || key.substr(22) != "==") return false;
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t i = 0; i < 22; ++i) if (alphabet.find(key[i]) == std::string_view::npos) return false;
    return (alphabet.find(key[21]) & 15) == 0; // Canonical encoding of exactly 16 bytes.
}
std::string accept_key(std::string_view key) {
    // SHA-1 is required by RFC 6455 for this public upgrade challenge. It is not
    // used for certificate verification or as a general security primitive.
    std::string bytes(key);
    bytes += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const std::uint64_t bits = bytes.size() * 8;
    bytes += static_cast<char>(0x80);
    while (bytes.size() % 64 != 56) bytes += '\0';
    for (int shift = 56; shift >= 0; shift -= 8) bytes += static_cast<char>(bits >> shift);
    std::array<std::uint32_t, 5> hash{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    for (std::size_t block = 0; block < bytes.size(); block += 64) {
        std::array<std::uint32_t, 80> words{};
        for (std::size_t i = 0; i < 16; ++i)
            for (std::size_t n = 0; n < 4; ++n)
                words[i] = (words[i] << 8) | static_cast<unsigned char>(bytes[block + i * 4 + n]);
        for (std::size_t i = 16; i < words.size(); ++i)
            words[i] = std::rotl(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
        auto [a, b, c, d, e] = hash;
        for (unsigned i = 0; i < 80; ++i) {
            const auto f = i < 20 ? (b & c) | (~b & d) : i < 40 ? b ^ c ^ d : i < 60 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
            const std::uint32_t k = i < 20 ? 0x5a827999 : i < 40 ? 0x6ed9eba1 : i < 60 ? 0x8f1bbcdc : 0xca62c1d6;
            const auto next = std::rotl(a, 5) + f + e + k + words[i];
            e = d; d = c; c = std::rotl(b, 30); b = a; a = next;
        }
        hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d; hash[4] += e;
    }
    std::array<unsigned char, 20> digest{};
    for (std::size_t i = 0; i < digest.size(); ++i) digest[i] = static_cast<unsigned char>(hash[i / 4] >> (24 - (i % 4) * 8));
    return base64(digest);
}
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
bool equal_ci(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}
std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}
bool token(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos;
    });
}
bool clean(std::string_view value, bool whitespace = false) {
    return std::all_of(value.begin(), value.end(), [whitespace](unsigned char c) {
        return (c >= (whitespace ? 0x20 : 0x21) && c != 0x7f) || (whitespace && c == '\t');
    });
}
std::vector<std::string> tokens(std::string_view list) {
    std::vector<std::string> result;
    if (list.empty()) return result;
    while (true) {
        const auto comma = list.find(',');
        const auto item = trim(list.substr(0, comma));
        if (!token(item)) invalid("invalid upgrade token list");
        result.emplace_back(item);
        if (comma == std::string_view::npos) return result;
        list.remove_prefix(comma + 1);
    }
}
bool contains_ci(std::string_view list, std::string_view value) {
    for (const auto& item : tokens(list)) if (equal_ci(item, value)) return true;
    return false;
}
void validate_protocols(const std::vector<std::string>& protocols) {
    for (std::size_t i = 0; i < protocols.size(); ++i) {
        if (!token(protocols[i])) throw std::invalid_argument("invalid WebSocket subprotocol");
        for (std::size_t n = 0; n < i; ++n)
            if (protocols[n] == protocols[i]) throw std::invalid_argument("duplicate WebSocket subprotocol");
    }
}
bool valid_utf8(std::string_view value) {
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i++]);
        if (first < 0x80) continue;
        unsigned length;
        std::uint32_t code;
        if (first >= 0xc2 && first <= 0xdf) { length = 1; code = first & 0x1f; }
        else if (first >= 0xe0 && first <= 0xef) { length = 2; code = first & 0x0f; }
        else if (first >= 0xf0 && first <= 0xf4) { length = 3; code = first & 0x07; }
        else return false;
        if (value.size() - i < length) return false;
        for (unsigned n = 0; n < length; ++n) {
            const auto next = static_cast<unsigned char>(value[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if ((length == 1 && code < 0x80) || (length == 2 && code < 0x800) ||
            (length == 3 && code < 0x10000) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
bool valid_close_code(std::uint16_t code) {
    return (code >= 3000 && code <= 4999) ||
        (code >= 1000 && code <= 1014 && code != 1004 && code != 1005 && code != 1006);
}
std::string close_payload(std::uint16_t code, std::string_view reason) {
    if (!valid_close_code(code) || reason.size() > 123 || !valid_utf8(reason))
        throw std::invalid_argument("invalid WebSocket close code/reason");
    std::string result{static_cast<char>(code >> 8), static_cast<char>(code)};
    result += reason;
    return result;
}
struct head {
    std::string first;
    std::vector<std::pair<std::string, std::string>> fields;
    bool has(std::string_view key) const {
        for (const auto& [name, value] : fields) { (void)value; if (equal_ci(name, key)) return true; }
        return false;
    }
    std::string get(std::string_view key, bool combine = false) const {
        std::string result;
        bool found = false;
        for (const auto& [name, value] : fields) if (equal_ci(name, key)) {
            if (found && !combine) invalid("duplicate WebSocket upgrade field");
            if (found) result += ",";
            result += value; found = true;
        }
        return result;
    }
};
head parse_head(std::string_view bytes) {
    head result;
    const auto line = bytes.find("\r\n");
    if (line == std::string_view::npos) invalid("invalid WebSocket HTTP head");
    result.first = bytes.substr(0, line);
    bytes.remove_prefix(line + 2);
    while (bytes != "\r\n") {
        const auto end = bytes.find("\r\n");
        if (end == std::string_view::npos || end == 0 || result.fields.size() >= 100) invalid("invalid/oversized upgrade headers");
        const auto field = bytes.substr(0, end);
        const auto colon = field.find(':');
        if (colon == std::string_view::npos || !token(field.substr(0, colon)) || !clean(field.substr(colon + 1), true))
            invalid("invalid upgrade header syntax");
        result.fields.emplace_back(field.substr(0, colon), trim(field.substr(colon + 1)));
        bytes.remove_prefix(end + 2);
    }
    const auto length = result.get("Content-Length");
    if ((result.has("Content-Length") && length != "0") || result.has("Transfer-Encoding"))
        invalid("WebSocket upgrade must not carry an HTTP body");
    return result;
}
} // namespace

template<class Transport>
struct basic_stream<Transport>::implementation {
    Transport transport;
    role mode;
    limits bounds;
    std::string pending, protocol, close_reason;
    std::optional<std::uint16_t> close_code;
    bool busy = false, upgraded = false, close_sent = false, close_received = false;
    implementation(Transport socket, role value, limits maximum)
        : transport(std::move(socket)), mode(value), bounds(maximum) {
        if ((mode != role::client && mode != role::server) || bounds.max_header_bytes < 64 ||
            bounds.max_header_bytes > 1024 * 1024 || bounds.max_frame_bytes == 0 ||
            bounds.max_message_bytes == 0 || bounds.max_fragments == 0 || bounds.max_control_frames == 0 ||
            bounds.max_frame_bytes > bounds.max_message_bytes)
            throw std::invalid_argument("invalid WebSocket role/limits");
    }
    void check(bool require_upgrade = true) const {
        if (transport.context().stopped()) throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        if (!transport.is_open()) throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor));
        if (io_context::current() != &transport.context()) throw std::logic_error("WebSocket operation must run on its owning context");
        if (require_upgrade && !upgraded) throw std::logic_error("WebSocket upgrade is incomplete");
#ifdef PACE_HAS_TLS
        if constexpr (std::is_same_v<Transport, tls::stream>) {
            if (!require_upgrade) {
                const auto selected = transport.selected_protocol();
                if (!selected.empty() && selected != "http/1.1")
                    throw std::logic_error("WebSocket HTTP/1 upgrade requires http/1.1 ALPN");
            }
        }
#endif
    }
    struct operation {
        implementation& state;
        explicit operation(implementation& value) : state(value) {
            if (state.busy) throw std::logic_error("WebSocket allows only one active operation");
            state.busy = true;
        }
        ~operation() { state.busy = false; }
    };
    task<void> exact(std::span<char> bytes) {
        if (!pending.empty()) {
            const auto count = std::min(pending.size(), bytes.size());
            std::copy_n(pending.data(), count, bytes.data());
            pending.erase(0, count); bytes = bytes.subspan(count);
        }
        while (!bytes.empty()) {
            const auto count = co_await transport.read_some(bytes);
            if (!count) invalid("WebSocket EOF without a complete closing frame");
            bytes = bytes.subspan(count);
        }
    }
    task<head> read_head() {
        for (;;) {
            const auto end = pending.find("\r\n\r\n");
            if (end != std::string::npos) {
                if (end + 4 > bounds.max_header_bytes) invalid("WebSocket upgrade head limit exceeded");
                auto result = parse_head(std::string_view(pending).substr(0, end + 4));
                pending.erase(0, end + 4);
                co_return result;
            }
            if (pending.size() >= bounds.max_header_bytes) invalid("WebSocket upgrade head limit exceeded");
            std::array<char, 2048> bytes{};
            const auto count = co_await transport.read_some(std::span<char>(bytes).first(
                std::min(bytes.size(), bounds.max_header_bytes - pending.size())));
            if (!count) invalid("EOF during WebSocket upgrade");
            pending.append(bytes.data(), count);
        }
    }
    task<void> frame(unsigned opcode, std::string_view data, bool final = true) {
        std::string output;
        output.reserve(data.size() + 14);
        output += static_cast<char>((final ? 0x80 : 0) | opcode);
        const auto mask = mode == role::client ? 0x80 : 0;
        if (data.size() < 126) output += static_cast<char>(mask | data.size());
        else if (data.size() <= 65535) {
            output += static_cast<char>(mask | 126);
            output += static_cast<char>(data.size() >> 8); output += static_cast<char>(data.size());
        } else {
            output += static_cast<char>(mask | 127);
            for (int shift = 56; shift >= 0; shift -= 8)
                output += static_cast<char>(static_cast<std::uint64_t>(data.size()) >> shift);
        }
        std::array<unsigned char, 4> key{};
        if (mask) { random_bytes(key); output.append(reinterpret_cast<const char*>(key.data()), key.size()); }
        const auto start = output.size();
        output.append(data);
        if (mask) for (std::size_t i = 0; i < data.size(); ++i) output[start + i] ^= static_cast<char>(key[i % 4]);
        co_await transport.write_all(output);
    }
    task<std::optional<message>> receive_message() {
        message result;
        bool fragmented = false;
        std::size_t fragments = 0, controls = 0;
        for (;;) {
            check();
            std::array<char, 2> prefix{};
            co_await exact(prefix);
            const auto first = static_cast<unsigned char>(prefix[0]), second = static_cast<unsigned char>(prefix[1]);
            const unsigned opcode = first & 15;
            const bool final = (first & 0x80) != 0, masked = (second & 0x80) != 0;
            const bool control = (opcode & 8) != 0;
            if (control && ++controls > bounds.max_control_frames) invalid("WebSocket control frame limit exceeded", 1008);
            if ((first & 0x70) || (opcode != 0 && opcode != 1 && opcode != 2 && opcode != 8 && opcode != 9 && opcode != 10))
                invalid("WebSocket reserved bits/opcode without negotiated extension");
            if (masked != (mode == role::server)) invalid("incorrect WebSocket masking direction");
            std::uint64_t length = second & 127;
            if (control && (!final || length > 125)) invalid("invalid fragmented/oversized WebSocket control frame");
            if (length == 126 || length == 127) {
                const auto extended = length == 126 ? 2U : 8U;
                std::array<char, 8> bytes{};
                co_await exact(std::span<char>(bytes).first(extended));
                if (extended == 8 && (static_cast<unsigned char>(bytes[0]) & 0x80)) invalid("invalid WebSocket 64-bit length");
                length = 0;
                for (unsigned i = 0; i < extended; ++i) length = (length << 8) | static_cast<unsigned char>(bytes[i]);
                if ((extended == 2 && length < 126) || (extended == 8 && length <= 65535)) invalid("nonminimal WebSocket length encoding");
            }
            if (!control) {
                if (length > bounds.max_frame_bytes || length > bounds.max_message_bytes - result.data.size())
                    invalid("WebSocket frame/message limit exceeded", 1009);
                if (++fragments > bounds.max_fragments) invalid("WebSocket fragment limit exceeded", 1009);
                if (opcode == 0 && !fragmented) invalid("unexpected WebSocket continuation");
                if (opcode != 0 && fragmented) invalid("new message inside fragmented WebSocket message");
                if (opcode != 0) result.type = opcode == 1 ? message_type::text : message_type::binary;
            }
            std::array<char, 4> key{};
            if (masked) co_await exact(key);
            std::string data(static_cast<std::size_t>(length), '\0');
            co_await exact(std::span<char>(data.data(), data.size()));
            if (masked) for (std::size_t i = 0; i < data.size(); ++i) data[i] ^= key[i % 4];
            if (opcode == 8) {
                if (data.size() == 1) invalid("invalid WebSocket close payload");
                if (!data.empty()) {
                    const auto code = static_cast<std::uint16_t>(static_cast<unsigned char>(data[0]) * 256 + static_cast<unsigned char>(data[1]));
                    if (!valid_close_code(code)) invalid("invalid WebSocket peer close code");
                    if (!valid_utf8(std::string_view(data).substr(2))) invalid("invalid UTF-8 close reason", 1007);
                    close_code = code; close_reason = data.substr(2);
                }
                close_received = true;
                if (!close_sent) { co_await frame(8, data); close_sent = true; }
                co_await transport.close_gracefully();
                co_return std::nullopt;
            }
            if (opcode == 9) { co_await frame(10, data); continue; }
            if (opcode == 10) continue;
            result.data += data;
            if (final) {
                if (result.type == message_type::text && !valid_utf8(result.data)) invalid("invalid WebSocket text UTF-8", 1007);
                co_return result;
            }
            fragmented = true;
        }
    }

};

template<class T> basic_stream<T>::basic_stream(T transport, role mode, limits bounds)
    : impl_(std::make_unique<implementation>(std::move(transport), mode, bounds)) {}
template<class T> basic_stream<T>::~basic_stream() = default;
template<class T> basic_stream<T>::basic_stream(basic_stream&&) noexcept = default;
template<class T> basic_stream<T>& basic_stream<T>::operator=(basic_stream&&) noexcept = default;
template<class T> io_context& basic_stream<T>::context() const noexcept { return impl_->transport.context(); }
template<class T> bool basic_stream<T>::is_open() const noexcept { return impl_ && impl_->transport.is_open() && !impl_->close_received; }
template<class T> void basic_stream<T>::close() noexcept { if (impl_) impl_->transport.close(); }
template<class T> std::string basic_stream<T>::selected_protocol() const { return impl_->protocol; }
template<class T> std::optional<std::uint16_t> basic_stream<T>::peer_close_code() const { return impl_->close_code; }
template<class T> std::string basic_stream<T>::peer_close_reason() const { return impl_->close_reason; }

template<class T>
task<void> basic_stream<T>::handshake(std::string host, std::string target, std::vector<std::string> protocols) {
    if (!impl_) throw std::logic_error("moved-from WebSocket stream");
    auto& p = *impl_;
    typename implementation::operation operation(p);
    try {
        p.check(false);
        if (p.mode != role::client || p.upgraded) throw std::logic_error("invalid WebSocket client handshake state");
        if (host.empty() || !clean(host) || target.empty() || target.front() != '/' || !clean(target) || target.find('#') != std::string::npos)
            throw std::invalid_argument("invalid WebSocket Host/target");
        validate_protocols(protocols);
        std::array<unsigned char, 16> nonce{}; random_bytes(nonce);
        const auto key = base64(nonce);
        std::string request = "GET " + target + " HTTP/1.1\r\nHost: " + host +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key + "\r\n";
        if (!protocols.empty()) {
            request += "Sec-WebSocket-Protocol: ";
            for (std::size_t i = 0; i < protocols.size(); ++i) { if (i) request += ", "; request += protocols[i]; }
            request += "\r\n";
        }
        request += "\r\n";
        if (request.size() > p.bounds.max_header_bytes) throw std::invalid_argument("WebSocket upgrade request exceeds header limit");
        co_await p.transport.write_all(request);
        const auto response = co_await p.read_head();
        if (!clean(response.first, true) || !(response.first == "HTTP/1.1 101" || response.first.starts_with("HTTP/1.1 101 ")) ||
            !contains_ci(response.get("Upgrade", true), "websocket") || !contains_ci(response.get("Connection", true), "upgrade") ||
            response.get("Sec-WebSocket-Accept") != accept_key(key)) invalid("invalid WebSocket upgrade response");
        if (response.has("Sec-WebSocket-Extensions")) invalid("unrequested WebSocket extension");
        const auto selected = response.get("Sec-WebSocket-Protocol");
        if (response.has("Sec-WebSocket-Protocol") && (selected.empty() || std::find(protocols.begin(), protocols.end(), selected) == protocols.end()))
            invalid("server selected an unoffered WebSocket subprotocol");
        p.protocol = selected; p.upgraded = true;
    } catch (...) { close(); throw; }
}

template<class T>
task<upgrade_request> basic_stream<T>::accept(std::vector<std::string> protocols,
    std::function<bool(const upgrade_request&)> authorize) {
    if (!impl_) throw std::logic_error("moved-from WebSocket stream");
    auto& p = *impl_;
    typename implementation::operation operation(p);
    std::exception_ptr failure;
    unsigned reject_status = 400;
    upgrade_request request;
    std::string key;
    try {
        p.check(false);
        if (p.mode != role::server || p.upgraded) throw std::logic_error("invalid WebSocket server handshake state");
        validate_protocols(protocols);
        const auto incoming = co_await p.read_head();
        if (!incoming.first.starts_with("GET ") || !incoming.first.ends_with(" HTTP/1.1")) invalid("WebSocket requires HTTP/1.1 GET");
        request.target = incoming.first.substr(4, incoming.first.size() - 13);
        if (request.target.empty() || request.target.front() != '/' || !clean(request.target) || request.target.find('#') != std::string::npos)
            invalid("invalid WebSocket request target");
        request.host = incoming.get("Host"); request.origin = incoming.get("Origin");
        if (request.host.empty() || !clean(request.host)) invalid("missing/invalid WebSocket Host");
        if (!contains_ci(incoming.get("Upgrade", true), "websocket") || !contains_ci(incoming.get("Connection", true), "upgrade"))
            invalid("missing WebSocket upgrade headers");
        if (incoming.get("Sec-WebSocket-Version") != "13") { reject_status = 426; invalid("unsupported WebSocket version"); }
        key = incoming.get("Sec-WebSocket-Key");
        if (!valid_key(key)) invalid("invalid WebSocket key");
        request.protocols = tokens(incoming.get("Sec-WebSocket-Protocol", true));
        for (std::size_t i = 0; i < request.protocols.size(); ++i)
            for (std::size_t n = 0; n < i; ++n)
                if (request.protocols[i] == request.protocols[n]) invalid("duplicate offered WebSocket subprotocol");
        if (authorize && !authorize(request)) { reject_status = 403; invalid("WebSocket upgrade rejected by application", 1008); }
        for (const auto& protocol : protocols)
            if (std::find(request.protocols.begin(), request.protocols.end(), protocol) != request.protocols.end()) { p.protocol = protocol; break; }
    } catch (...) { failure = std::current_exception(); }
    if (failure) {
        try {
            std::string response = "HTTP/1.1 " + std::to_string(reject_status) + " Rejected\r\nConnection: close\r\nContent-Length: 0\r\n";
            if (reject_status == 426) response += "Sec-WebSocket-Version: 13\r\n";
            response += "\r\n";
            co_await p.transport.write_all(response);
        } catch (...) {}
        close(); std::rethrow_exception(failure);
    }
    try {
        std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept_key(key) + "\r\n";
        if (!p.protocol.empty()) response += "Sec-WebSocket-Protocol: " + p.protocol + "\r\n";
        response += "\r\n";
        co_await p.transport.write_all(response);
        p.upgraded = true;
        co_return request;
    } catch (...) { close(); throw; }
}

template<class T> task<std::optional<message>> basic_stream<T>::receive() {
    if (!impl_) throw std::logic_error("moved-from WebSocket stream");
    auto& p = *impl_;
    typename implementation::operation operation(p);
    if (p.close_received) co_return std::nullopt;
    std::exception_ptr failure;
    std::uint16_t code = 0;
    try { co_return co_await p.receive_message(); }
    catch (const protocol_error& error) { failure = std::current_exception(); code = error.close_code(); }
    catch (...) { close(); throw; }
    if (code && p.upgraded && !p.close_sent) {
        try { const auto payload = close_payload(code, {}); co_await p.frame(8, payload); p.close_sent = true; }
        catch (...) {}
    }
    close(); std::rethrow_exception(failure);
}
template<class T> task<void> basic_stream<T>::send(message value, std::size_t fragment_bytes) {
    if (!impl_) throw std::logic_error("moved-from WebSocket stream");
    auto& p = *impl_;
    typename implementation::operation operation(p);
    try {
        p.check();
        if (p.close_sent) throw std::logic_error("cannot send WebSocket data after close");
        if (value.type != message_type::text && value.type != message_type::binary) throw std::invalid_argument("invalid WebSocket message type");
        if (value.data.size() > p.bounds.max_message_bytes || (value.type == message_type::text && !valid_utf8(value.data)))
            throw std::invalid_argument("invalid or oversized outgoing WebSocket message");
        const auto chunk = fragment_bytes ? std::min(fragment_bytes, p.bounds.max_frame_bytes) : p.bounds.max_frame_bytes;
        if (!value.data.empty() && (value.data.size() - 1) / chunk + 1 > p.bounds.max_fragments)
            throw std::invalid_argument("outgoing WebSocket fragment limit exceeded");
        std::size_t offset = 0;
        do {
            const auto count = std::min(chunk, value.data.size() - offset);
            co_await p.frame(offset == 0 ? (value.type == message_type::text ? 1 : 2) : 0,
                std::string_view(value.data).substr(offset, count), offset + count == value.data.size());
            offset += count;
        } while (offset < value.data.size());
    } catch (...) { close(); throw; }
}
template<class T> task<void> basic_stream<T>::ping(std::string payload) {
    if (!impl_) throw std::logic_error("moved-from WebSocket stream");
    auto& p = *impl_; typename implementation::operation operation(p);
    try {
        p.check();
        if (p.close_sent) throw std::logic_error("cannot ping after WebSocket close");
        if (payload.size() > 125) throw std::invalid_argument("WebSocket ping payload exceeds 125 bytes");
        co_await p.frame(9, payload);
    } catch (...) { close(); throw; }
}
template<class T> task<void> basic_stream<T>::pong(std::string payload) {
    if (!impl_) throw std::logic_error("moved-from WebSocket stream");
    auto& p = *impl_; typename implementation::operation operation(p);
    try {
        p.check();
        if (payload.size() > 125) throw std::invalid_argument("WebSocket pong payload exceeds 125 bytes");
        co_await p.frame(10, payload);
    } catch (...) { close(); throw; }
}
template<class T> task<void> basic_stream<T>::send_close(std::uint16_t code, std::string reason) {
    if (!impl_) throw std::logic_error("moved-from WebSocket stream");
    auto& p = *impl_; typename implementation::operation operation(p);
    try {
        p.check();
        if (!p.close_sent) { const auto payload = close_payload(code, reason); co_await p.frame(8, payload); p.close_sent = true; }
    } catch (...) { close(); throw; }
}
template<class T> task<bool> basic_stream<T>::close_gracefully(std::uint16_t code, std::string reason,
                                                             std::chrono::milliseconds timeout) {
    if (!impl_) throw std::logic_error("moved-from WebSocket stream");
    auto& p = *impl_; typename implementation::operation operation(p);
    if (p.close_received) { close(); co_return true; }
    bool expired = false;
    io_context::timer_handle deadline;
    try {
        p.check();
        const auto payload = close_payload(code, reason);
        deadline = p.transport.context().call_after(timeout, [&] { expired = true; p.transport.close(); });
        if (!p.close_sent) { co_await p.frame(8, payload); p.close_sent = true; }
        while (!p.close_received) (void)co_await p.receive_message();
    } catch (...) {
        close();
        if (!expired) throw;
    }
    close();
    co_return p.close_received;
}

template class basic_stream<tcp::stream>;
#ifdef PACE_HAS_TLS
template class basic_stream<tls::stream>;
#endif
} // namespace pace::websocket
