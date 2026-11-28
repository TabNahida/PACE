#include <pace/net/relay.hpp>

#if defined(__linux__)
#include "net/socket_ops.hpp"

#include <cerrno>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

namespace pace::tcp
{
struct splice_access
{
    static bool& reading(stream& s) noexcept
    {
        return s.reading_;
    }

    static bool& writing(stream& s) noexcept
    {
        return s.writing_;
    }
};
} // namespace pace::tcp

namespace pace
{
namespace
{
struct splice_pipe
{
    int ends[2] = {-1, -1};

    ~splice_pipe()
    {
        if (ends[0] >= 0)
        {
            ::close(ends[0]);
            ::close(ends[1]);
        }
    }
};

// splice has no MSG_NOSIGNAL flag. Block SIGPIPE only around the syscall and consume the signal we generated.
ssize_t splice_to_socket(int pipe, int socket, std::size_t size)
{
    sigset_t set, old_mask, pending;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    const int mask_error = pthread_sigmask(SIG_BLOCK, &set, &old_mask);
    if (mask_error)
    {
        throw std::system_error(mask_error, std::generic_category(), "pthread_sigmask");
    }
    sigpending(&pending);
    const auto count = ::splice(pipe, nullptr, socket, nullptr, size, SPLICE_F_NONBLOCK);
    const auto error = errno;
    if (count < 0 && error == EPIPE && !sigismember(&pending, SIGPIPE))
    {
        const timespec zero{};
        while (sigtimedwait(&set, nullptr, &zero) < 0 && errno == EINTR)
        {
        }
    }
    pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
    errno = error;
    return count;
}

bool unsupported_splice(int error)
{
    return error == EINVAL || error == EOPNOTSUPP || error == ENOSYS;
}
} // namespace

task<bool> relay_splice(tcp::stream& from, tcp::stream& to, std::uint64_t& transferred, relay_access::state& state)
{
    splice_pipe pipe;
    if (::pipe2(pipe.ends, O_NONBLOCK | O_CLOEXEC) != 0)
    {
        co_return false;
    }
    std::size_t pending = 0;
    {
        net::pending_operation reader(tcp::splice_access::reading(from), "another TCP read is pending");
        net::pending_operation writer(tcp::splice_access::writing(to), "another TCP write is pending");
        std::size_t quantum = 0;
        for (;;)
        {
            const auto count = ::splice(from.native_handle(), nullptr, pipe.ends[1], nullptr, 65536, SPLICE_F_NONBLOCK);
            if (count == 0)
            {
                state.eof();
                co_await to.shutdown_write();
                co_return true;
            }
            if (count < 0)
            {
                const int error = errno;
                if (unsupported_splice(error))
                {
                    co_return false;
                }
                if (error == EINTR)
                {
                    continue;
                }
                if (!net::would_block(error))
                {
                    throw std::system_error(error, std::generic_category(), "splice from TCP");
                }
                co_await state.io.wait_readable(from.native_handle());
                continue;
            }
            state.progress();
            pending = static_cast<std::size_t>(count);
            while (pending)
            {
                const auto wrote = splice_to_socket(pipe.ends[0], to.native_handle(), pending);
                if (wrote > 0)
                {
                    pending -= static_cast<std::size_t>(wrote);
                    transferred += static_cast<std::size_t>(wrote);
                    state.progress();
                    continue;
                }
                const auto error = wrote == 0 ? EPIPE : errno;
                if (unsupported_splice(error))
                {
                    break;
                }
                if (error == EINTR)
                {
                    continue;
                }
                if (!net::would_block(error))
                {
                    throw std::system_error(error, std::generic_category(), "splice to TCP");
                }
                co_await state.io.wait_writable(to.native_handle());
            }
            if (pending)
            {
                break;
            }
            quantum += static_cast<std::size_t>(count);
            if (quantum >= 256 * 1024)
            {
                quantum = 0;
                co_await state.io.yield();
            }
        }
    }
    // An unsupported pipe-to-socket operation can leave already consumed input in the pipe.
    relay_access::lease buffer(relay_access::buffers(state.io));
    while (pending)
    {
        const auto count = ::read(pipe.ends[0], buffer.data->data(), std::min(pending, buffer.data->size()));
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            throw std::system_error(count == 0 ? EIO : errno, std::generic_category(), "read relay pipe");
        }
        const auto size = static_cast<std::size_t>(count);
        co_await to.write_all(std::span(*buffer.data).first(size));
        pending -= size;
        transferred += size;
        state.progress();
    }
    co_return false;
}
} // namespace pace
#endif
