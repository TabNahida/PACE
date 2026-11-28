#include <pace/net/tcp.hpp>
#include "net/socket_address.hpp"
#include "net/socket_ops.hpp"

#if defined(__linux__)
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv6/ip6_tables.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <fcntl.h>
#include <net/if.h>
#include <sys/ioctl.h>
#if defined(__FreeBSD__)
#include <net/pfvar.h>
#endif
#endif

namespace pace::tcp
{
#if defined(__APPLE__) || defined(__FreeBSD__)
namespace
{
#if defined(__APPLE__)
// The macOS SDK omits net/pfvar.h. XNU bsd/net/pfvar.h defines the DIOCNATLOOK ABI.
struct nat_lookup
{
    in6_addr saddr, daddr, rsaddr, rdaddr;

    struct transport_port
    {
        std::uint16_t port, padding;
    };

    transport_port sxport, dxport, rsxport, rdxport;
    sa_family_t af;
    std::uint8_t proto, proto_variant, direction;
};

static_assert(sizeof(nat_lookup) == 84);
constexpr auto nat_request = _IOWR('D', 23, nat_lookup);
constexpr unsigned pf_in = 1, pf_out = 2;
#else
using nat_lookup = pfioc_natlook;
constexpr auto nat_request = DIOCNATLOOK;
constexpr unsigned pf_in = PF_IN, pf_out = PF_OUT;
#endif

endpoint pf_destination(const endpoint& peer, const endpoint& local)
{
    nat_lookup query{};
    // PF indexes the packet family. A dual-stack listener reports IPv4 peers as mapped IPv6.
    constexpr std::string_view mapped_prefix = "::ffff:";
    const bool mapped = local.family == address_family::ipv6 && local.address.starts_with(mapped_prefix)
                        && peer.address.starts_with(mapped_prefix);
    const auto family = mapped ? address_family::ipv4 : local.family;
    query.af = static_cast<sa_family_t>(net::native_address_family(family));
    query.proto = IPPROTO_TCP;
    query.direction = pf_out;
    const auto prefix_size = mapped ? mapped_prefix.size() : 0;
    if (::inet_pton(query.af, peer.address.c_str() + prefix_size, &query.saddr) != 1
        || ::inet_pton(query.af, local.address.c_str() + prefix_size, &query.daddr) != 1)
    {
        throw std::invalid_argument("invalid PF connection endpoints");
    }
#if defined(__APPLE__)
    query.sxport.port = htons(peer.port);
    query.dxport.port = htons(local.port);
#else
    query.sport = htons(peer.port);
    query.dport = htons(local.port);
#endif
    const int pf = ::open("/dev/pf", O_RDONLY | O_CLOEXEC);
    if (pf < 0)
    {
        net::throw_socket_error("open(/dev/pf)");
    }
    int status = ::ioctl(pf, nat_request, &query);
    if (status < 0 && errno == ENOENT)
    {
        query.direction = pf_in;
        status = ::ioctl(pf, nat_request, &query);
    }
    const int error = errno;
    ::close(pf);
    if (status < 0)
    {
        throw std::system_error(net::socket_error_code(error), "DIOCNATLOOK");
    }
    char address[INET6_ADDRSTRLEN]{};
    if (!::inet_ntop(query.af, &query.rdaddr, address, sizeof(address)))
    {
        net::throw_socket_error("inet_ntop");
    }
#if defined(__APPLE__)
    auto port = ntohs(query.rdxport.port);
#else
    auto port = ntohs(query.rdport);
#endif
    return {address, port, family, mapped ? 0 : local.scope_id};
}
} // namespace
#endif

endpoint stream::original_destination() const
{
    if (!is_open())
    {
        throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor));
    }
#if defined(__linux__)
    net::socket_address address;
    const bool v4 = family_ == address_family::ipv4;
    if (::getsockopt(
            fd_,
            v4 ? IPPROTO_IP : IPPROTO_IPV6,
            v4 ? SO_ORIGINAL_DST : IP6T_SO_ORIGINAL_DST,
            address.data(),
            &address.length
        )
        != 0)
    {
        const auto error = net::socket_error_code(net::last_socket_error());
        // A dual-stack listener may have accepted an IPv4 connection.
        if (v4 || !remote_endpoint().address.starts_with("::ffff:"))
        {
            throw std::system_error(error, "getsockopt(original destination)");
        }
        address.length = sizeof(address.storage);
        if (::getsockopt(fd_, IPPROTO_IP, SO_ORIGINAL_DST, address.data(), &address.length) != 0)
        {
            net::throw_socket_error("getsockopt(SO_ORIGINAL_DST)");
        }
    }
    return net::socket_endpoint<endpoint>(address.data(), address.length);
#elif defined(__APPLE__) || defined(__FreeBSD__)
    return pf_destination(remote_endpoint(), local_endpoint());
#else
    throw std::system_error(std::make_error_code(std::errc::operation_not_supported), "original destination");
#endif
}
} // namespace pace::tcp
