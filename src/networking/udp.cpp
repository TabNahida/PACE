#include <pace/udp.hpp>
#include <pace/detail/socket_address.hpp>

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
native_socket_type create_socket(address_family family) {
    detail::initialize_sockets();
    const auto fd = static_cast<native_socket_type>(::socket(detail::native_address_family(family), SOCK_DGRAM, IPPROTO_UDP));
    if (fd == invalid_socket) fail("UDP socket");
    try {
#if defined(_WIN32)
        u_long nonblocking = 1;
        if (::ioctlsocket(fd, FIONBIO, &nonblocking) != 0) fail("UDP nonblocking");
#else
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
            ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) fail("UDP nonblocking");
#endif
        if (family == address_family::ipv6) {
            const int yes = 1;
            if (::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&yes), sizeof(yes)) != 0)
                fail("UDP IPV6_V6ONLY");
        }
    } catch (...) { release(fd); throw; }
    return fd;
}
} // namespace

socket::socket(io_context& context) : socket(context, address_family::unspecified) {}
socket::socket(io_context& context, address_family family)
    : context_(&context), fd_(create_socket(family == address_family::unspecified ? address_family::ipv4 : family)),
      family_(family == address_family::unspecified ? address_family::ipv4 : family),
      automatic_family_(family == address_family::unspecified) {}
socket::socket(io_context& context, endpoint local, bind_options options) : socket(context) { bind(std::move(local), options); }
socket::~socket() { close(); }
socket::socket(socket&& other) noexcept
    : context_(other.context_), fd_(std::exchange(other.fd_, invalid_socket)), family_(other.family_),
      automatic_family_(other.automatic_family_), family_selected_(other.family_selected_), ipv6_only_(other.ipv6_only_) {}
socket& socket::operator=(socket&& other) noexcept {
    if (this != &other) {
        close(); context_ = other.context_; fd_ = std::exchange(other.fd_, invalid_socket);
        family_ = other.family_; automatic_family_ = other.automatic_family_;
        family_selected_ = other.family_selected_; ipv6_only_ = other.ipv6_only_;
    }
    return *this;
}
void socket::select_family(address_family family) {
    if (family == family_) return;
    if (!automatic_family_ || family_selected_ || sending_ || receiving_)
        throw std::system_error(std::make_error_code(std::errc::address_family_not_supported), "UDP socket family");
    const auto replacement = create_socket(family);
    context_->cancel(fd_);
    release(std::exchange(fd_, replacement));
    family_ = family;
}
void socket::bind(endpoint local, bind_options options) {
    check(*context_, fd_);
    if (sending_ || receiving_) throw std::logic_error("UDP bind requires an idle socket");
    const auto value = detail::parse_socket_address(local.address, local.port, local.family, local.scope_id);
    if (value.family() == address_family::ipv4 && !options.ipv6_only)
        throw std::invalid_argument("dual-stack UDP bind requires an IPv6 endpoint");
    select_family(value.family());
    if (family_ == address_family::ipv6) {
        const int only = options.ipv6_only ? 1 : 0;
        if (::setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&only), sizeof(only)) != 0)
            fail("UDP IPV6_V6ONLY");
        ipv6_only_ = options.ipv6_only;
    }
    if (::bind(fd_, value.data(), value.length) != 0) fail("UDP bind");
    family_selected_ = true;
}
endpoint socket::local_endpoint() const {
    check(*context_, fd_);
    detail::socket_address local;
    if (::getsockname(fd_, local.data(), &local.length) != 0) fail("UDP getsockname");
    return detail::socket_endpoint<endpoint>(local.data(), local.length);
}
void socket::set_broadcast(bool enabled) {
    check(*context_, fd_);
    if (family_ != address_family::ipv4)
        throw std::system_error(std::make_error_code(std::errc::operation_not_supported), "IPv6 does not support broadcast");
    const int value = enabled ? 1 : 0;
    if (::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&value), sizeof(value)) != 0)
        fail("UDP SO_BROADCAST");
    family_selected_ = true;
}
task<std::size_t> socket::send_to(std::string_view bytes, endpoint destination) {
    check(*context_, fd_, true);
    if (bytes.size() > 65507) throw std::system_error(std::make_error_code(std::errc::message_size));
    auto target = detail::parse_socket_address(destination.address, destination.port, destination.family, destination.scope_id);
    if (family_ == address_family::ipv6 && !ipv6_only_ && target.family() == address_family::ipv4)
        target = detail::map_ipv4_address(target);
    select_family(target.family());
    pending_operation guard(sending_);
    family_selected_ = true;
    for (;;) {
        check(*context_, fd_, true);
        const auto count = ::sendto(fd_, bytes.empty() ? "" : bytes.data(), static_cast<int>(bytes.size()), 0,
                                    target.data(), target.length);
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
    family_selected_ = true;
    for (;;) {
        check(*context_, fd_, true);
        detail::socket_address sender;
        bool truncated = false;
        const auto capacity = std::min<std::size_t>(buffer.size(), INT_MAX);
#if defined(_WIN32)
        auto count = ::recvfrom(fd_, buffer.data(), static_cast<int>(capacity), 0,
                                sender.data(), &sender.length);
        if (count < 0 && last_error() == WSAEMSGSIZE) {
            count = static_cast<int>(capacity);
            truncated = true;
        }
#else
        iovec vector{buffer.data(), capacity};
        msghdr message{};
        message.msg_name = sender.data();
        message.msg_namelen = sender.length;
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        const auto count = ::recvmsg(fd_, &message, 0);
        sender.length = message.msg_namelen;
        truncated = (message.msg_flags & MSG_TRUNC) != 0;
#endif
        if (count >= 0) co_return datagram{std::min(static_cast<std::size_t>(count), capacity),
                                         detail::socket_endpoint<endpoint>(sender.data(), sender.length), truncated};
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
