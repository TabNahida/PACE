<!-- source-hash: d5fb661cd7df -->
# TUN 设备

[English](tun-devices.md) · [指南](README.zh.md) · [教程](../tutorial/README.zh.md) · [API 参考](../reference/README.zh.md)

TUN 设备把完整的 IP 数据包交给程序：内核路由进这个接口的数据包都可以从一个描述符读出，程序写进描述符的数据包则被内核当作
从该接口收到的包。VPN 客户端、用户态网络栈和隧道都建立在它之上。PACE 不创建也不配置设备；它像等待 socket 一样等待设备的
描述符，所以数据包循环就是 `io_context` 上的一个普通协程。

准确的约定见[句柄与信号](../reference/runtime/handles-and-signals.zh.md#非-socket-描述符上的就绪等待)。

## 数据包循环

所有平台都是同一个结构：等待描述符可读，一直读到 `EAGAIN`，处理数据包，写回应答；只有写操作会阻塞时才等待可写。

```cpp
#include <pace/runtime.hpp>

#include <array>
#include <cerrno>
#include <span>
#include <system_error>

#include <unistd.h>

// 返回写入 `packet` 的应答长度；0 表示不应答。
std::size_t handle_packet(std::span<unsigned char> packet, std::size_t size);

pace::task<void> packet_loop(pace::io_context& io, int device)
{
    std::array<unsigned char, 65536> packet{};
    for (;;)
    {
        co_await io.wait_readable(device);
        for (;;)
        {
            const auto received = ::read(device, packet.data(), packet.size()); // 每次 read 一个数据包
            if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                break; // 已读空：再次等待
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

描述符必须是非阻塞的（`O_NONBLOCK`），关闭之前必须先调用 `io.release_socket(device)`。像取消其他任务一样取消这个循环：
通过任务组、截止时间或 `io.stop()`；此时等待会抛出 `operation_canceled`。

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

// 创建（或附加到）名为 `name` 的设备；"tun%d" 让内核选一个空闲编号。需要 CAP_NET_ADMIN。
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

用 `ip addr add 10.0.0.1/24 dev tun0` 和 `ip link set tun0 up`（或等价的 `ioctl`）给接口分配地址并启用它。`IFF_NO_PI`
去掉四字节的包信息头，读写的就是裸 IPv4 或 IPv6 数据包。所有后端都适用，包括 io_uring 后端。

**多队列。** 使用 `IFF_MULTI_QUEUE` 时，以同一个名字为每个队列打开一次设备。每个队列是一个独立的描述符，内核把各条流分散
到这些队列上。每个队列运行一个循环，通常是 `pace::runtime` 的每个工作 context 一个，这样各队列并行处理。带
`IFF_DETACH_QUEUE` 或 `IFF_ATTACH_QUEUE` 的 `TUNSETQUEUE` 把某个队列移出或重新加入轮转。

**不用 root。** `sudo unshare --net`（在允许非特权用户命名空间的系统上也可用 `unshare -rn`）在私有网络命名空间里运行
程序，程序在其中可以创建 TUN 设备；PACE 自己的 TUN 测试在 CI 中就是这样运行的。

## Android

`VpnService.Builder.establish()` 返回一个 `ParcelFileDescriptor`，对应一个已经配置好的 Linux TUN 设备。用
`detachFd()` 取得所有权，把这个整数传给原生代码，用 `fcntl` 设置 `O_NONBLOCK`，然后运行同样的循环。VPN 自己打开的
socket 必须在连接之前用 `VpnService.protect()` 排除在隧道之外。

## macOS 与 iOS

`utun` 接口是一个 `PF_SYSTEM` 控制 socket，因此 kqueue 像等待任何 socket 一样等待它。创建它需要 root（在 iOS 上则由
Network Extension 提供描述符或数据包流）。

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
    address.sc_id = info.ctl_id; // sc_unit 为 0：使用下一个空闲的 utunN
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

从 `utun` 描述符读出或写入的每个数据包，在 IP 头之前都有一个网络字节序的四字节地址族（`AF_INET` 或 `AF_INET6`）；应答
时要保留它。用 `ifconfig utun4 10.0.0.1 10.0.0.2 up` 配置接口。描述符关闭后接口随之消失。

## FreeBSD

以 root 身份打开 `/dev/tun` 会创建下一个空闲的 `tunN`（`fstat` 之后用 `devname(st_rdev, S_IFCHR)` 取得名字）。除非用
`TUNSIFHEAD` 打开与 macOS 类似的四字节地址族头，读写的都是裸 IP 数据包。用 `ifconfig tun0 10.0.0.1 10.0.0.2 up` 配置
它。与 Linux 和 macOS 不同，关闭描述符不会销毁接口，要用 `ifconfig tun0 destroy`（`SIOCIFDESTROY`）。

## Windows

Windows 没有可轮询的 TUN 描述符。[Wintun](https://www.wintun.net/) 提供一个数据包环和一个在数据包到达时变为有信号的
事件；`io_context::wait_handle` 等待这个事件。

```cpp
pace::task<void> wintun_loop(pace::io_context& io, WINTUN_SESSION_HANDLE session)
{
    const HANDLE readable = WintunGetReadWaitEvent(session); // 归会话所有：不要关闭它
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
        handle_wintun_packet(session, packet, size); // 应答用 WintunAllocateSendPacket / WintunSendPacket
        WintunReleaseReceivePacket(session, packet);
    }
}
```

`wait_handle` 在 Windows 的两个后端（IOCP 和 WSAPoll）上都能工作：一个线程池等待观察这个事件并唤醒 context。只在循环结束
之后（例如它所在的任务组汇合之后）才结束会话。

## 参见

- [句柄与信号](../reference/runtime/handles-and-signals.zh.md)：各后端接受哪些描述符，`wait_handle`
- [`io_context`](../reference/runtime/io-context.zh.md)：`wait_readable`、`wait_writable`、`cancel`、`release_socket`
- [`runtime`](../reference/runtime/runtime.zh.md)：每个工作线程一个 context，适合多队列设备
- Linux 内核：[Universal TUN/TAP device driver](https://docs.kernel.org/networking/tuntap.html)
