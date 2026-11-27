// Readiness on descriptors that are not sockets (pipes, pseudo-terminals, TUN devices) and, on Windows, waits on
// kernel object handles. Every test synchronizes on events: the wait is registered before the peer acts.
#include <pace/runtime.hpp>
#include "support/pace_test.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <termios.h>
#endif
#if defined(__linux__)
#include <linux/if_tun.h>
#elif defined(__APPLE__)
#include <net/if_utun.h>
#include <netinet/in_var.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#elif defined(__FreeBSD__)
#include <net/if_tun.h>
#include <netinet/in_var.h>
#include <sys/sockio.h>
#include <sys/stat.h>
#endif

using namespace std::chrono_literals;

namespace
{
using pace::test::require;

bool canceled(const std::system_error& error)
{
    return error.code() == std::errc::operation_canceled;
}

#ifndef _WIN32
void set_nonblocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL);
    require(flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0, "set O_NONBLOCK");
}

// A descriptor closed on scope exit. release() runs first so a reused number never matches a stale registration.
struct owned_fd
{
    pace::io_context* io = nullptr;
    int fd = -1;

    owned_fd() = default;

    owned_fd(pace::io_context& context, int descriptor)
        : io(&context)
        , fd(descriptor)
    {
    }

    owned_fd(const owned_fd&) = delete;
    owned_fd& operator=(const owned_fd&) = delete;

    ~owned_fd()
    {
        close();
    }

    void close() noexcept
    {
        if (fd >= 0)
        {
            io->release_socket(fd);
            ::close(fd);
            fd = -1;
        }
    }
};

struct pipe_pair
{
    owned_fd read_end;
    owned_fd write_end;

    explicit pipe_pair(pace::io_context& io)
    {
        std::array<int, 2> ends{};
        require(::pipe(ends.data()) == 0, "pipe");
        read_end.io = write_end.io = &io;
        read_end.fd = ends[0];
        write_end.fd = ends[1];
        set_nonblocking(ends[0]);
        set_nonblocking(ends[1]);
    }
};

pace::task<void> pipe_readiness()
{
    auto& io = pace::this_context();
    pipe_pair pipe(io);
    // A reader that suspends before any byte exists is woken by a later write.
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            bool waiting = false;
            auto reader = group.spawn(
                [&]() -> pace::task<char>
                {
                    waiting = true;
                    co_await io.wait_readable(pipe.read_end.fd);
                    char byte = 0;
                    require(::read(pipe.read_end.fd, &byte, 1) == 1, "read after readiness");
                    co_return byte;
                }
            );
            while (!waiting)
            {
                co_await io.yield();
            }
            require(::write(pipe.write_end.fd, "p", 1) == 1, "pipe write");
            require(co_await reader == 'p', "pipe byte");
        }
    );

    // A full pipe is not writable until the reader drains it.
    std::array<char, 4096> chunk{};
    while (::write(pipe.write_end.fd, chunk.data(), chunk.size()) > 0)
    {
    }
    require(errno == EAGAIN || errno == EWOULDBLOCK, "fill pipe until EAGAIN");
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            bool waiting = false;
            auto writer = group.spawn(
                [&]() -> pace::task<void>
                {
                    waiting = true;
                    co_await io.wait_writable(pipe.write_end.fd);
                }
            );
            while (!waiting)
            {
                co_await io.yield();
            }
            while (::read(pipe.read_end.fd, chunk.data(), chunk.size()) > 0)
            {
            }
            co_await writer;
        }
    );

    // cancel(fd) resumes a pending wait with operation_canceled and leaves the pipe usable.
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            bool waiting = false;
            auto reader = group.spawn(
                [&]() -> pace::task<bool>
                {
                    waiting = true;
                    try
                    {
                        co_await io.wait_readable(pipe.read_end.fd);
                    }
                    catch (const std::system_error& error)
                    {
                        co_return canceled(error);
                    }
                    co_return false;
                }
            );
            while (!waiting)
            {
                co_await io.yield();
            }
            io.cancel(pipe.read_end.fd);
            require(co_await reader, "cancel(fd) on a pipe");
        }
    );

    // Closing the write end is end of stream: the reader wakes and read() returns 0.
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            bool waiting = false;
            auto reader = group.spawn(
                [&]() -> pace::task<long>
                {
                    waiting = true;
                    co_await io.wait_readable(pipe.read_end.fd);
                    char byte = 0;
                    co_return static_cast<long>(::read(pipe.read_end.fd, &byte, 1));
                }
            );
            while (!waiting)
            {
                co_await io.yield();
            }
            pipe.write_end.close();
            require(co_await reader == 0, "end of stream after the writer closed");
        }
    );
}

pace::task<void> terminal_readiness()
{
    auto& io = pace::this_context();
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    require(master >= 0, "posix_openpt");
    owned_fd primary(io, master);
    require(::grantpt(master) == 0 && ::unlockpt(master) == 0, "grantpt/unlockpt");
    const char* name = ::ptsname(master);
    require(name != nullptr, "ptsname");
    owned_fd secondary(io, ::open(name, O_RDWR | O_NOCTTY));
    require(secondary.fd >= 0, "open the terminal");
    termios mode{};
    require(::tcgetattr(secondary.fd, &mode) == 0, "tcgetattr");
    ::cfmakeraw(&mode);
    require(::tcsetattr(secondary.fd, TCSANOW, &mode) == 0, "raw mode");
    set_nonblocking(primary.fd);
    set_nonblocking(secondary.fd);

    const auto exchange = [&](int from, int to, char sent) -> pace::task<void>
    {
        co_await pace::with_task_group(
            [&](pace::task_group& group) -> pace::task<void>
            {
                bool waiting = false;
                auto reader = group.spawn(
                    [&]() -> pace::task<char>
                    {
                        waiting = true;
                        co_await io.wait_readable(to);
                        char byte = 0;
                        require(::read(to, &byte, 1) == 1, "terminal read");
                        co_return byte;
                    }
                );
                while (!waiting)
                {
                    co_await io.yield();
                }
                co_await io.wait_writable(from);
                require(::write(from, &sent, 1) == 1, "terminal write");
                require(co_await reader == sent, "terminal byte");
            }
        );
    };
    co_await exchange(primary.fd, secondary.fd, 'm');
    co_await exchange(secondary.fd, primary.fd, 's');
    // wait_ready occupies both directions of a character device and resumes once.
    co_await io.wait_ready(primary.fd);
}

// TUN devices. Creating one needs privileges (CAP_NET_ADMIN on Linux, root on macOS and FreeBSD); without them the
// test reports a skip, unless PACE_REQUIRE_TUN is set (CI runs it under sudo in a network namespace).
bool tun_required()
{
    const char* value = std::getenv("PACE_REQUIRE_TUN");
    return value && *value && std::string_view(value) != "0";
}

void skip_tun(const std::string& why)
{
    if (tun_required())
    {
        FAIL("PACE_REQUIRE_TUN is set but " << why);
    }
    MESSAGE("TUN test skipped: " << why);
}

// The peer address is routed into the device; the local address is the device's own.
constexpr std::uint32_t tun_local = (198U << 24) | (18U << 16) | (211U << 8) | 1U;
constexpr std::uint32_t tun_peer = (198U << 24) | (18U << 16) | (211U << 8) | 2U;

std::uint16_t ipv4_checksum(const unsigned char* header, std::size_t length)
{
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i + 1 < length; i += 2)
    {
        sum += static_cast<std::uint32_t>(header[i] << 8 | header[i + 1]);
    }
    while (sum >> 16)
    {
        sum = (sum & 0xffffU) + (sum >> 16);
    }
    return static_cast<std::uint16_t>(~sum);
}

// Turns an IPv4/UDP packet addressed to the peer into the peer's reply: swaps addresses and ports, drops the UDP
// checksum (allowed for IPv4) and recomputes the header checksum. Anything else (IPv6 neighbour discovery, MLD)
// is ignored.
bool make_reply(unsigned char* packet, std::size_t length)
{
    if (length < 28 || (packet[0] >> 4) != 4 || packet[9] != 17)
    {
        return false;
    }
    const std::size_t header = static_cast<std::size_t>(packet[0] & 0x0fU) * 4;
    std::uint32_t destination = 0;
    std::memcpy(&destination, packet + 16, 4);
    if (header < 20 || length < header + 8 || ntohl(destination) != tun_peer)
    {
        return false;
    }
    std::array<unsigned char, 4> address{};
    std::memcpy(address.data(), packet + 12, 4);
    std::memcpy(packet + 12, packet + 16, 4);
    std::memcpy(packet + 16, address.data(), 4);
    std::array<unsigned char, 2> port{};
    std::memcpy(port.data(), packet + header, 2);
    std::memcpy(packet + header, packet + header + 2, 2);
    std::memcpy(packet + header + 2, port.data(), 2);
    packet[header + 6] = packet[header + 7] = 0;
    packet[10] = packet[11] = 0;
    const auto sum = ipv4_checksum(packet, header);
    packet[10] = static_cast<unsigned char>(sum >> 8);
    packet[11] = static_cast<unsigned char>(sum & 0xffU);
    return true;
}

// Bytes before the IP packet on reads and writes: macOS utun prefixes the address family.
#if defined(__APPLE__)
constexpr std::size_t tun_prefix = 4;
#else
constexpr std::size_t tun_prefix = 0;
#endif

// Reads packets from the device until the probe datagram arrives, then writes the peer's reply back into it.
pace::task<void> echo_one(pace::io_context& io, int device, bool& waiting)
{
    std::array<unsigned char, 2048> packet{};
    for (;;)
    {
        waiting = true;
        co_await io.wait_readable(device);
        for (;;)
        {
            const auto received = ::read(device, packet.data(), packet.size());
            if (received <= 0)
            {
                require(received == 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR, "TUN read");
                break;
            }
            const auto size = static_cast<std::size_t>(received);
            if (size <= tun_prefix || !make_reply(packet.data() + tun_prefix, size - tun_prefix))
            {
                continue;
            }
            for (;;)
            {
                co_await io.wait_writable(device);
                if (::write(device, packet.data(), size) == received)
                {
                    co_return;
                }
                require(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR, "TUN write");
            }
        }
    }
}

// Sends a datagram to the peer through the device, answers it from the device, and receives the answer.
pace::task<void> tun_round_trip(int device)
{
    auto& io = pace::this_context();
    owned_fd udp(io, ::socket(AF_INET, SOCK_DGRAM, 0));
    require(udp.fd >= 0, "UDP socket");
    set_nonblocking(udp.fd);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(tun_local);
    require(::bind(udp.fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0, "bind the TUN address");
    co_await pace::with_deadline(
        pace::deadline::after(10s),
        [&]() -> pace::task<void>
        {
            co_await pace::with_task_group(
                [&](pace::task_group& group) -> pace::task<void>
                {
                    bool waiting = false;
                    auto echo = group.spawn(echo_one(io, device, waiting));
                    while (!waiting)
                    {
                        co_await io.yield();
                    }
                    sockaddr_in peer{};
                    peer.sin_family = AF_INET;
                    peer.sin_addr.s_addr = htonl(tun_peer);
                    peer.sin_port = htons(9);
                    constexpr std::string_view probe = "pace-tun";
                    require(
                        ::sendto(
                            udp.fd,
                            probe.data(),
                            probe.size(),
                            0,
                            reinterpret_cast<const sockaddr*>(&peer),
                            sizeof(peer)
                        ) == static_cast<long>(probe.size()),
                        "send through the TUN route"
                    );
                    co_await echo;
                    co_await io.wait_readable(udp.fd);
                    std::array<char, 64> reply{};
                    sockaddr_in from{};
                    socklen_t length = sizeof(from);
                    const auto size =
                        ::recvfrom(udp.fd, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr*>(&from), &length);
                    require(size == static_cast<long>(probe.size()), "reply size");
                    require(std::string_view(reply.data(), probe.size()) == probe, "reply payload");
                    require(ntohl(from.sin_addr.s_addr) == tun_peer && ntohs(from.sin_port) == 9, "reply source");
                }
            );
        }
    );
}

#if defined(__linux__)
// Opens a queue of a TUN device named `name` ("pacetun%d" picks a free one); returns -1 with errno set on failure.
int open_tun_queue(std::string& name, bool multi_queue)
{
    const int fd = ::open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        return -1;
    }
    ifreq request{};
    std::strncpy(request.ifr_name, name.c_str(), IFNAMSIZ - 1);
    request.ifr_flags = static_cast<short>(IFF_TUN | IFF_NO_PI | (multi_queue ? IFF_MULTI_QUEUE : 0));
    if (::ioctl(fd, TUNSETIFF, &request) < 0)
    {
        const int error = errno;
        ::close(fd);
        errno = error;
        return -1;
    }
    name = request.ifr_name;
    return fd;
}

void configure_tun(const std::string& name)
{
    const int control = ::socket(AF_INET, SOCK_DGRAM, 0);
    require(control >= 0, "control socket");
    ifreq request{};
    std::strncpy(request.ifr_name, name.c_str(), IFNAMSIZ - 1);
    auto* address = reinterpret_cast<sockaddr_in*>(&request.ifr_addr);
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = htonl(tun_local);
    const bool assigned = ::ioctl(control, SIOCSIFADDR, &request) == 0;
    address->sin_addr.s_addr = htonl(0xffffff00U);
    const bool masked = ::ioctl(control, SIOCSIFNETMASK, &request) == 0;
    const bool read_flags = ::ioctl(control, SIOCGIFFLAGS, &request) == 0;
    request.ifr_flags = static_cast<short>(request.ifr_flags | IFF_UP | IFF_RUNNING);
    const bool up = read_flags && ::ioctl(control, SIOCSIFFLAGS, &request) == 0;
    ::close(control);
    require(assigned && masked && up, "configure the TUN interface");
}

bool open_tun_or_skip(std::string& name, bool multi_queue, int& fd)
{
    fd = open_tun_queue(name, multi_queue);
    if (fd < 0)
    {
        skip_tun(std::string("cannot create a TUN device: ") + std::strerror(errno));
        return false;
    }
    return true;
}
#elif defined(__APPLE__)
int open_utun(std::string& name)
{
    const int fd = ::socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (fd < 0)
    {
        return -1;
    }
    ctl_info info{};
    std::strncpy(info.ctl_name, UTUN_CONTROL_NAME, sizeof(info.ctl_name) - 1);
    sockaddr_ctl address{};
    address.sc_len = sizeof(address);
    address.sc_family = AF_SYSTEM;
    address.ss_sysaddr = AF_SYS_CONTROL;
    std::array<char, IFNAMSIZ> buffer{};
    socklen_t length = buffer.size();
    const auto fail = [fd]
    {
        const int error = errno;
        ::close(fd);
        errno = error;
        return -1;
    };
    if (::ioctl(fd, CTLIOCGINFO, &info) < 0)
    {
        return fail();
    }
    address.sc_id = info.ctl_id; // sc_unit 0: the kernel picks the next free utunN
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0
        || ::getsockopt(fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, buffer.data(), &length) < 0)
    {
        return fail();
    }
    name = buffer.data();
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}
#elif defined(__FreeBSD__)
int open_bsd_tun(std::string& name)
{
    const int fd = ::open("/dev/tun", O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        return -1;
    }
    struct stat status{};
    const char* device = ::fstat(fd, &status) == 0 ? ::devname(status.st_rdev, S_IFCHR) : nullptr;
    if (!device)
    {
        ::close(fd);
        errno = ENODEV;
        return -1;
    }
    name = device;
    return fd;
}

void destroy_bsd_tun(const std::string& name)
{
    const int control = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (control >= 0)
    {
        ifreq request{};
        std::strncpy(request.ifr_name, name.c_str(), IFNAMSIZ - 1);
        ::ioctl(control, SIOCIFDESTROY, &request);
        ::close(control);
    }
}
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
// Point-to-point: the local address with the peer as destination, and the interface up.
void configure_point_to_point(const std::string& name)
{
    const int control = ::socket(AF_INET, SOCK_DGRAM, 0);
    require(control >= 0, "control socket");
    in_aliasreq alias{};
    std::strncpy(alias.ifra_name, name.c_str(), IFNAMSIZ - 1);
    const auto set = [](sockaddr_in& address, std::uint32_t value)
    {
        address.sin_len = sizeof(address);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(value);
    };
    set(alias.ifra_addr, tun_local);
    set(alias.ifra_dstaddr, tun_peer);
    set(alias.ifra_mask, 0xffffffffU);
    const bool assigned = ::ioctl(control, SIOCAIFADDR, &alias) == 0;
    ifreq request{};
    std::strncpy(request.ifr_name, name.c_str(), IFNAMSIZ - 1);
    const bool read_flags = ::ioctl(control, SIOCGIFFLAGS, &request) == 0;
    request.ifr_flags = static_cast<short>(request.ifr_flags | IFF_UP);
    const bool up = read_flags && ::ioctl(control, SIOCSIFFLAGS, &request) == 0;
    ::close(control);
    require(assigned && up, "configure the TUN interface");
}
#endif
#endif
} // namespace

#ifndef _WIN32
TEST_CASE("pipes wake readiness waits for data, space, cancellation and end of stream")
{
    pace::run(pipe_readiness);
}

TEST_CASE("a pseudo-terminal is awaited like a socket in both directions")
{
    pace::run(terminal_readiness);
}

TEST_CASE("a TUN device carries a read/write loop through the reactor")
{
#if defined(__linux__)
    std::string name = "pacetun%d";
    int device = -1;
    if (!open_tun_or_skip(name, false, device))
    {
        return;
    }
    configure_tun(name);
    pace::run(
        [device]() -> pace::task<void>
        {
            owned_fd queue(pace::this_context(), device);
            co_await tun_round_trip(device);
        }
    );
#elif defined(__APPLE__)
    std::string name;
    const int device = open_utun(name);
    if (device < 0)
    {
        skip_tun(std::string("cannot create a utun interface: ") + std::strerror(errno));
        return;
    }
    set_nonblocking(device);
    configure_point_to_point(name);
    pace::run(
        [device]() -> pace::task<void>
        {
            owned_fd queue(pace::this_context(), device);
            co_await tun_round_trip(device);
        }
    );
#elif defined(__FreeBSD__)
    std::string name;
    const int device = open_bsd_tun(name);
    if (device < 0)
    {
        skip_tun(std::string("cannot open /dev/tun: ") + std::strerror(errno));
        return;
    }
    set_nonblocking(device);

    struct destroy_on_exit
    {
        std::string name;

        ~destroy_on_exit()
        {
            destroy_bsd_tun(name);
        }
    } destroy{name};

    configure_point_to_point(name);
    pace::run(
        [device]() -> pace::task<void>
        {
            owned_fd queue(pace::this_context(), device);
            co_await tun_round_trip(device);
        }
    );
#else
    MESSAGE("TUN test skipped: no TUN support on this platform");
#endif
}

#if defined(__linux__)
TEST_CASE("each IFF_MULTI_QUEUE queue is a descriptor of its own")
{
    std::string name = "pacetun%d";
    int first = -1;
    if (!open_tun_or_skip(name, true, first))
    {
        return;
    }
    const int second = open_tun_queue(name, true);
    if (second < 0)
    {
        const int error = errno;
        ::close(first);
        skip_tun(std::string("cannot open a second queue: ") + std::strerror(error));
        return;
    }
    configure_tun(name);
    // With one queue detached, the kernel delivers every packet to the other: each queue is exercised in turn.
    const auto attach = [](int fd, bool attached)
    {
        ifreq request{};
        request.ifr_flags = static_cast<short>(attached ? IFF_ATTACH_QUEUE : IFF_DETACH_QUEUE);
        require(::ioctl(fd, TUNSETQUEUE, &request) == 0, "TUNSETQUEUE");
    };
    pace::run(
        [&]() -> pace::task<void>
        {
            auto& io = pace::this_context();
            owned_fd a(io, first);
            owned_fd b(io, second);
            attach(b.fd, false);
            co_await tun_round_trip(a.fd);
            attach(b.fd, true);
            attach(a.fd, false);
            co_await tun_round_trip(b.fd);
        }
    );
}
#endif
#endif

#ifdef _WIN32
namespace
{
struct owned_handle
{
    HANDLE value = nullptr;

    explicit owned_handle(HANDLE handle)
        : value(handle)
    {
        require(value != nullptr, "create a kernel object");
    }

    owned_handle(const owned_handle&) = delete;
    owned_handle& operator=(const owned_handle&) = delete;

    ~owned_handle()
    {
        CloseHandle(value);
    }
};

pace::task<void> event_waits()
{
    auto& io = pace::this_context();
    // An event set by another thread after the wait is registered resumes it.
    owned_handle manual(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            bool waiting = false;
            auto first = group.spawn(
                [&]() -> pace::task<void>
                {
                    waiting = true;
                    co_await io.wait_handle(manual.value);
                }
            );
            // Any number of waits may target one handle; a manual-reset event releases them all.
            auto second = group.spawn([&]() -> pace::task<void> { co_await io.wait_handle(manual.value); });
            while (!waiting)
            {
                co_await io.yield();
            }
            co_await io.yield();
            std::thread setter([&] { SetEvent(manual.value); });
            co_await first;
            co_await second;
            setter.join();
        }
    );
    // A signaled object completes the wait without a cross-thread signal.
    co_await io.wait_handle(manual.value);

    // An auto-reset event's signal is consumed by the wait that observes it.
    owned_handle automatic(CreateEventW(nullptr, FALSE, TRUE, nullptr));
    co_await io.wait_handle(automatic.value);
    require(WaitForSingleObject(automatic.value, 0) == WAIT_TIMEOUT, "auto-reset signal consumed");

    // Cancellation unregisters the pool wait: a later SetEvent finds nothing to resume.
    owned_handle never(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    pace::cancellation_source source;
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            bool waiting = false;
            auto waiter = group.spawn(
                [&]() -> pace::task<bool>
                {
                    try
                    {
                        co_await pace::with_cancellation(
                            source.token(),
                            [&]() -> pace::task<void>
                            {
                                waiting = true;
                                co_await io.wait_handle(never.value);
                            }
                        );
                    }
                    catch (const std::system_error& error)
                    {
                        co_return canceled(error);
                    }
                    co_return false;
                }
            );
            while (!waiting)
            {
                co_await io.yield();
            }
            source.request_cancellation();
            require(co_await waiter, "cancelled handle wait");
        }
    );
    SetEvent(never.value);
    require(WaitForSingleObject(never.value, 0) == WAIT_OBJECT_0, "cancelled wait did not consume the signal");

    // A process handle is signaled when the process exits.
    std::wstring command = L"cmd.exe /c exit 7";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    require(
        CreateProcessW(
            nullptr,
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process
        ) != 0,
        "CreateProcessW"
    );
    owned_handle child(process.hProcess);
    CloseHandle(process.hThread);
    co_await io.wait_handle(child.value);
    DWORD code = 0;
    require(GetExitCodeProcess(child.value, &code) && code == 7, "process exit code");

    bool rejected = false;
    try
    {
        co_await io.wait_handle(nullptr);
    }
    catch (const std::system_error& error)
    {
        rejected = error.code() == std::errc::bad_file_descriptor;
    }
    require(rejected, "a null handle is rejected");
}
} // namespace

TEST_CASE("wait_handle resumes on signaled events and processes and unregisters on cancellation")
{
    pace::run(event_waits);
}

TEST_CASE("stopping the context cancels a pending handle wait")
{
    owned_handle never(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    pace::io_context io;
    bool waiting = false;
    bool cancelled = false;
    io.spawn(
        [](pace::io_context& context, HANDLE object, bool& registered, bool& result) -> pace::task<void>
        {
            try
            {
                registered = true;
                co_await context.wait_handle(object);
            }
            catch (const std::system_error& error)
            {
                result = canceled(error);
            }
        }(io, never.value, waiting, cancelled)
    );
    io.spawn(
        [](pace::io_context& context, const bool& registered) -> pace::task<void>
        {
            while (!registered)
            {
                co_await context.yield();
            }
            context.stop();
        }(io, waiting)
    );
    io.run();
    require(cancelled, "stop cancels the handle wait");
}
#endif
