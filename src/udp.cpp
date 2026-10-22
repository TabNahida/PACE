#include <pace/udp.hpp>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <cerrno>
#include <climits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace pace::udp {
namespace {
#if defined(_WIN32)
using socket_length = int;
int last_error() noexcept { return ::WSAGetLastError(); }
void release(native_socket_type fd) noexcept { ::closesocket(fd); }
bool interrupted(int error) { return error == WSAEINTR; }
bool would_block(int error) { return error == WSAEWOULDBLOCK; }
std::error_code error_code(int error) { return {error, std::system_category()}; }
#else
using socket_length = socklen_t;
int last_error() noexcept { return errno; }
void release(native_socket_type fd) noexcept { ::close(fd); }
bool interrupted(int error) { return error == EINTR; }
bool would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
std::error_code error_code(int error) { return {error, std::generic_category()}; }
#endif
[[noreturn]] void fail(const char* operation) { throw std::system_error(error_code(last_error()), operation); }

sockaddr_in address(const endpoint& value) {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_port = htons(value.port);
    if (value.address.find('\0') != std::string::npos ||
        ::inet_pton(AF_INET, value.address.c_str(), &result.sin_addr) != 1)
        throw std::invalid_argument("UDP endpoint requires a numeric IPv4 address");
    return result;
}
endpoint from_address(const sockaddr_in& value) {
    char text[INET_ADDRSTRLEN]{};
    if (!::inet_ntop(AF_INET, &value.sin_addr, text, sizeof(text))) fail("UDP inet_ntop");
    return {text, ntohs(value.sin_port)};
}
void check(io_context& context, native_socket_type fd, bool asynchronous = false) {
    if (context.stopped()) throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    if (fd == invalid_socket) throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor));
    auto* current = io_context::current();
    if ((asynchronous && current != &context) || (current && current != &context))
        throw std::logic_error("UDP operation must run on its owning io_context");
}
struct pending_operation {
    bool& active;
    explicit pending_operation(bool& value) : active(value) {
        if (active) throw std::logic_error("another UDP operation is pending in this direction");
        active = true;
    }
    ~pending_operation() { active = false; }
};
} // namespace

socket::socket(io_context& context) : context_(&context) {
    detail::initialize_sockets();
    fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ == invalid_socket) fail("UDP socket");
    try {
#if defined(_WIN32)
        u_long nonblocking = 1;
        if (::ioctlsocket(fd_, FIONBIO, &nonblocking) != 0) fail("UDP nonblocking");
#else
        const int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0 ||
            ::fcntl(fd_, F_SETFD, FD_CLOEXEC) < 0) fail("UDP nonblocking");
#endif
    } catch (...) { release(std::exchange(fd_, invalid_socket)); throw; }
}
socket::socket(io_context& context, endpoint local) : socket(context) { bind(std::move(local)); }
socket::~socket() { close(); }
socket::socket(socket&& other) noexcept
    : context_(other.context_), fd_(std::exchange(other.fd_, invalid_socket)) {}
socket& socket::operator=(socket&& other) noexcept {
    if (this != &other) { close(); context_ = other.context_; fd_ = std::exchange(other.fd_, invalid_socket); }
    return *this;
}
void socket::bind(endpoint local) {
    check(*context_, fd_);
    auto value = address(local);
    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&value), sizeof(value)) != 0) fail("UDP bind");
}
endpoint socket::local_endpoint() const {
    check(*context_, fd_);
    sockaddr_in local{};
    socket_length length = sizeof(local);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&local), &length) != 0) fail("UDP getsockname");
    return from_address(local);
}
void socket::set_broadcast(bool enabled) {
    check(*context_, fd_);
    const int value = enabled ? 1 : 0;
    if (::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&value), sizeof(value)) != 0)
        fail("UDP SO_BROADCAST");
}
task<std::size_t> socket::send_to(std::string_view bytes, endpoint destination) {
    check(*context_, fd_, true);
    pending_operation guard(sending_);
    if (bytes.size() > 65507) throw std::system_error(std::make_error_code(std::errc::message_size));
    const auto target = address(destination);
    for (;;) {
        check(*context_, fd_, true);
        const auto count = ::sendto(fd_, bytes.empty() ? "" : bytes.data(), static_cast<int>(bytes.size()), 0,
                                    reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        if (count >= 0) {
            if (static_cast<std::size_t>(count) != bytes.size())
                throw std::system_error(std::make_error_code(std::errc::message_size));
            co_return static_cast<std::size_t>(count);
        }
        const int error = last_error();
        if (interrupted(error)) continue;
        if (!would_block(error)) throw std::system_error(error_code(error), "UDP sendto");
        co_await context_->wait_writable(fd_);
    }
}
task<datagram> socket::receive_from(std::span<char> buffer) {
    check(*context_, fd_, true);
    pending_operation guard(receiving_);
    if (buffer.empty()) throw std::invalid_argument("UDP receive buffer must not be empty");
    for (;;) {
        check(*context_, fd_, true);
        sockaddr_in sender{};
        bool truncated = false;
        const auto capacity = std::min<std::size_t>(buffer.size(), INT_MAX);
#if defined(_WIN32)
        socket_length length = sizeof(sender);
        auto count = ::recvfrom(fd_, buffer.data(), static_cast<int>(capacity), 0,
                                reinterpret_cast<sockaddr*>(&sender), &length);
        if (count < 0 && last_error() == WSAEMSGSIZE) {
            count = static_cast<int>(capacity);
            truncated = true;
        }
#else
        iovec vector{buffer.data(), capacity};
        msghdr message{};
        message.msg_name = &sender;
        message.msg_namelen = sizeof(sender);
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        const auto count = ::recvmsg(fd_, &message, 0);
        truncated = (message.msg_flags & MSG_TRUNC) != 0;
#endif
        if (count >= 0) co_return datagram{std::min(static_cast<std::size_t>(count), capacity),
                                         from_address(sender), truncated};
        const int error = last_error();
        if (interrupted(error)) continue;
        if (!would_block(error)) throw std::system_error(error_code(error), "UDP receive");
        co_await context_->wait_readable(fd_);
    }
}
void socket::close() noexcept {
    if (fd_ != invalid_socket) { context_->cancel(fd_); release(std::exchange(fd_, invalid_socket)); }
}
} // namespace pace::udp
