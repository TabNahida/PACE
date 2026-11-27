# TUN devices

[简体中文](tun-devices.zh.md) · [Guides](README.md) · [Tutorial](../tutorial/README.md) · [API reference](../reference/README.md)

A TUN device hands the program whole IP packets: whatever the kernel routes into the interface is read from a
descriptor, and whatever the program writes to it is received by the kernel as if it had arrived on that interface.
VPN clients, user-space network stacks and tunnels are built on it. PACE does not create or configure devices; it
awaits their descriptors like sockets, so a packet loop is an ordinary coroutine on an `io_context`.

The exact contract is in [Handles and signals](../reference/runtime/handles-and-signals.md#readiness-on-non-socket-descriptors).

## A packet loop

Every platform uses the same shape: wait until the descriptor is readable, read packets until `EAGAIN`, handle them,
and write replies, waiting for writability only when a write would block.

```cpp
#include <pace/runtime.hpp>

#include <array>
#include <cerrno>
#include <span>
#include <system_error>

#include <unistd.h>

// Returns the size of the reply written into `packet`, or 0 for no reply.
std::size_t handle_packet(std::span<unsigned char> packet, std::size_t size);

pace::task<void> packet_loop(pace::io_context& io, int device)
{
    std::array<unsigned char, 65536> packet{};
    for (;;)
    {
        co_await io.wait_readable(device);
        for (;;)
        {
            const auto received = ::read(device, packet.data(), packet.size()); // one packet per read
            if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                break; // drained: wait again
            }
            if (received < 0 && errno == EINTR)
            {
                continue;
            }
            if (received <= 0)
            {
                throw std::system_error(errno, std::generic_category(), "read from the TUN device");
            }
            const auto reply = handle_packet(packet, static_cast<std::size_t>(received));
            while (reply && ::write(device, packet.data(), reply) < 0)
            {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                {
                    throw std::system_error(errno, std::generic_category(), "write to the TUN device");
                }
                co_await io.wait_writable(device);
            }
        }
    }
}
```

The descriptor must be nonblocking (`O_NONBLOCK`), and `io.release_socket(device)` must run before it is closed.
Cancel the loop like any other task: through a task group, a deadline, or `io.stop()`. The wait then throws
`operation_canceled`.

## Linux

```cpp
#include <cstring>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>

// Creates (or attaches to) the device `name`; "tun%d" lets the kernel pick a free number. Needs CAP_NET_ADMIN.
int open_tun(std::string& name, bool multi_queue = false)
{
    const int fd = ::open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        throw std::system_error(errno, std::generic_category(), "open /dev/net/tun");
    }
    ifreq request{};
    std::strncpy(request.ifr_name, name.c_str(), IFNAMSIZ - 1);
    request.ifr_flags = IFF_TUN | IFF_NO_PI | (multi_queue ? IFF_MULTI_QUEUE : 0);
    if (::ioctl(fd, TUNSETIFF, &request) < 0)
    {
        const int error = errno;
        ::close(fd);
        throw std::system_error(error, std::generic_category(), "TUNSETIFF");
    }
    name = request.ifr_name;
    return fd;
}
```

Give the interface an address and bring it up with `ip addr add 10.0.0.1/24 dev tun0` and `ip link set tun0 up`
(or the equivalent `ioctl`s). `IFF_NO_PI` leaves the four-byte packet information header out, so reads and writes are
bare IPv4 or IPv6 packets. Every backend works, the io_uring backend included.

**Multi-queue.** With `IFF_MULTI_QUEUE`, open the device once per queue with the same name. Each queue is its own
descriptor; the kernel spreads flows across them. Run one loop per queue, typically one per worker context of a
`pace::runtime`, so the queues are served in parallel. `TUNSETQUEUE` with `IFF_DETACH_QUEUE` or `IFF_ATTACH_QUEUE`
takes a queue out of or back into the rotation.

**Without root.** `sudo unshare --net` (or `unshare -rn` where unprivileged user namespaces are allowed) runs a
program in a private network namespace in which it may create TUN devices; PACE's own TUN tests run that way in CI.

## Android

`VpnService.Builder.establish()` returns a `ParcelFileDescriptor` for a Linux TUN device that is already configured.
Take ownership with `detachFd()`, pass the integer to native code, set `O_NONBLOCK` with `fcntl`, and run the same
loop. Sockets the VPN itself opens must be excluded from the tunnel with `VpnService.protect()` before they connect.

## macOS and iOS

A `utun` interface is a `PF_SYSTEM` control socket, so kqueue awaits it like any socket. Creating one needs root (on
iOS, a Network Extension provides the descriptor or a packet flow instead).

```cpp
#include <array>
#include <cstring>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <net/if_utun.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/socket.h>
#include <sys/sys_domain.h>
#include <unistd.h>

int open_utun(std::string& name)
{
    const int fd = ::socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (fd < 0)
    {
        throw std::system_error(errno, std::generic_category(), "utun control socket");
    }
    const auto fail = [fd](const char* what)
    {
        const int error = errno;
        ::close(fd);
        throw std::system_error(error, std::generic_category(), what);
    };
    ctl_info info{};
    std::strncpy(info.ctl_name, UTUN_CONTROL_NAME, sizeof(info.ctl_name) - 1);
    if (::ioctl(fd, CTLIOCGINFO, &info) < 0)
    {
        fail("CTLIOCGINFO");
    }
    sockaddr_ctl address{};
    address.sc_len = sizeof(address);
    address.sc_family = AF_SYSTEM;
    address.ss_sysaddr = AF_SYS_CONTROL;
    address.sc_id = info.ctl_id; // sc_unit 0: the next free utunN
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0)
    {
        fail("connect utun");
    }
    std::array<char, IFNAMSIZ> buffer{};
    socklen_t length = buffer.size();
    if (::getsockopt(fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, buffer.data(), &length) < 0)
    {
        fail("UTUN_OPT_IFNAME");
    }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    name = buffer.data();
    return fd;
}
```

Every packet read from or written to a `utun` descriptor starts with a four-byte address family in network byte order
(`AF_INET` or `AF_INET6`) before the IP header; keep it on replies. Configure the interface with
`ifconfig utun4 10.0.0.1 10.0.0.2 up`. The interface disappears when the descriptor is closed.

## FreeBSD

Opening `/dev/tun` as root creates the next free `tunN` (its name is `devname(st_rdev, S_IFCHR)` after `fstat`).
Reads and writes are bare IP packets unless `TUNSIFHEAD` turns on a four-byte address family header like macOS's.
Configure it with `ifconfig tun0 10.0.0.1 10.0.0.2 up`. Unlike Linux and macOS, closing the descriptor does not
destroy the interface: `ifconfig tun0 destroy` (`SIOCIFDESTROY`) does.

## Windows

Windows has no pollable TUN descriptor. [Wintun](https://www.wintun.net/) exposes a ring of packets and an event
that is signaled when packets arrive; `io_context::wait_handle` awaits that event.

```cpp
pace::task<void> wintun_loop(pace::io_context& io, WINTUN_SESSION_HANDLE session)
{
    const HANDLE readable = WintunGetReadWaitEvent(session); // owned by the session: do not close it
    for (;;)
    {
        DWORD size = 0;
        BYTE* packet = WintunReceivePacket(session, &size);
        if (!packet)
        {
            if (GetLastError() != ERROR_NO_MORE_ITEMS)
            {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "WintunReceivePacket");
            }
            co_await io.wait_handle(readable);
            continue;
        }
        handle_wintun_packet(session, packet, size); // WintunAllocateSendPacket / WintunSendPacket for replies
        WintunReleaseReceivePacket(session, packet);
    }
}
```

`wait_handle` works on both Windows backends (IOCP and WSAPoll): a thread-pool wait observes the event and wakes the
context. End the session only after the loop has finished, for example after its task group has joined it.

## See also

- [Handles and signals](../reference/runtime/handles-and-signals.md): which descriptors each backend accepts, `wait_handle`
- [`io_context`](../reference/runtime/io-context.md): `wait_readable`, `wait_writable`, `cancel`, `release_socket`
- [`runtime`](../reference/runtime/runtime.md): one context per worker, for multi-queue devices
- Linux kernel: [Universal TUN/TAP device driver](https://docs.kernel.org/networking/tuntap.html)
