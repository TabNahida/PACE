#include <pace/runtime/file_watch.hpp>
#include <pace/runtime/io_context.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/inotify.h>
#else
#include <sys/event.h>
#include <sys/stat.h>
#endif
#endif

namespace pace
{
namespace
{
[[noreturn]] void watch_error(const char* operation)
{
#ifdef _WIN32
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
#else
    throw std::system_error(errno, std::generic_category(), operation);
#endif
}

#ifndef _WIN32
void close_fd(int& fd) noexcept
{
    if (fd >= 0)
    {
        ::close(std::exchange(fd, -1));
    }
}
#endif
} // namespace

struct file_watch::implementation
{
    io_context& io;
    std::filesystem::path name;
    bool closed = false;
    bool waiting = false;

    implementation(io_context& context, std::filesystem::path filename)
        : io(context)
        , name(std::move(filename))
    {
    }

#ifdef _WIN32
    HANDLE directory = INVALID_HANDLE_VALUE;
    OVERLAPPED request{};
    alignas(DWORD) std::array<std::byte, 16384> buffer{};
    bool pending = false;

    void arm()
    {
        const HANDLE event = request.hEvent;
        request = {};
        request.hEvent = event;
        if (!ResetEvent(event))
        {
            watch_error("reset file watch event");
        }
        constexpr DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE
                                 | FILE_NOTIFY_CHANGE_ATTRIBUTES;
        if (!ReadDirectoryChangesW(
                directory,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                FALSE,
                filter,
                nullptr,
                &request,
                nullptr
            ))
        {
            watch_error("ReadDirectoryChangesW");
        }
        pending = true;
    }

    void open(const std::filesystem::path& parent)
    {
        directory = CreateFileW(
            parent.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr
        );
        if (directory == INVALID_HANDLE_VALUE)
        {
            watch_error("open watched directory");
        }
        request.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!request.hEvent)
        {
            watch_error("create file watch event");
        }
        arm();
    }

    void close_native() noexcept
    {
        if (pending)
        {
            CancelIoEx(directory, &request);
            DWORD bytes{};
            // The kernel owns both request and buffer until completion, including after cancellation.
            GetOverlappedResult(directory, &request, &bytes, TRUE);
            pending = false;
        }
        if (directory != INVALID_HANDLE_VALUE)
        {
            CloseHandle(std::exchange(directory, INVALID_HANDLE_VALUE));
        }
        if (request.hEvent)
        {
            SetEvent(request.hEvent);
        }
    }

    bool consume()
    {
        DWORD size{};
        const BOOL success = GetOverlappedResult(directory, &request, &size, FALSE);
        pending = false;
        if (!success && GetLastError() != ERROR_NOTIFY_ENUM_DIR)
        {
            watch_error("complete file watch");
        }
        bool changed = !success || size == 0; // Lost records require a rescan.
        for (DWORD offset = 0; !changed && offset < size;)
        {
            const auto* entry = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
            changed = CompareStringOrdinal(
                          entry->FileName,
                          static_cast<int>(entry->FileNameLength / sizeof(wchar_t)),
                          name.c_str(),
                          static_cast<int>(name.native().size()),
                          TRUE
                      )
                      == CSTR_EQUAL;
            if (!entry->NextEntryOffset)
            {
                break;
            }
            offset += entry->NextEntryOffset;
        }
        arm();
        return changed;
    }
#else
    int queue = -1;
#if defined(__linux__)
    void open(const std::filesystem::path& parent)
    {
        queue = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (queue < 0)
        {
            watch_error("inotify_init1");
        }
        constexpr auto mask = IN_ONLYDIR | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_MODIFY
                              | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF;
        if (inotify_add_watch(queue, parent.c_str(), mask) < 0)
        {
            watch_error("inotify_add_watch");
        }
    }

    bool consume()
    {
        alignas(inotify_event) std::array<char, 4096> buffer;
        const auto bytes = ::read(queue, buffer.data(), buffer.size());
        if (bytes < 0)
        {
            if (errno == EINTR || errno == EAGAIN)
            {
                return false;
            }
            watch_error("read inotify events");
        }
        bool changed = false;
        for (std::size_t offset = 0; offset < static_cast<std::size_t>(bytes);)
        {
            const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
            if (event->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT))
            {
                throw std::system_error(
                    std::make_error_code(std::errc::no_such_file_or_directory),
                    "watched directory"
                );
            }
            changed |= (event->mask & IN_Q_OVERFLOW) || (event->len && name.native() == event->name);
            offset += sizeof(inotify_event) + event->len;
        }
        return changed;
    }
#else
    int directory = -1;
    int file = -1;
#if defined(O_EVTONLY)
    static constexpr int watch_flags = O_EVTONLY | O_CLOEXEC;
#else
    static constexpr int watch_flags = O_RDONLY | O_NONBLOCK | O_CLOEXEC;
#endif

    void subscribe(int fd, unsigned flags)
    {
        struct kevent event;
        EV_SET(&event, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, flags, 0, nullptr);
        if (kevent(queue, &event, 1, nullptr, 0, nullptr) < 0)
        {
            watch_error("subscribe vnode");
        }
    }

    bool refresh_file()
    {
        int next = openat(directory, name.c_str(), watch_flags);
        if (next < 0)
        {
            if (errno != ENOENT)
            {
                watch_error("open watched file");
            }
            const bool disappeared = file >= 0;
            close_fd(file);
            return disappeared;
        }
        try
        {
            struct stat current{}, previous{};
            if (fstat(next, &current) < 0 || (file >= 0 && fstat(file, &previous) < 0))
            {
                watch_error("stat watched file");
            }
            if (file >= 0 && current.st_dev == previous.st_dev && current.st_ino == previous.st_ino)
            {
                close_fd(next);
                return false;
            }
            subscribe(next, NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE);
        }
        catch (...)
        {
            close_fd(next);
            throw;
        }
        close_fd(file);
        file = next;
        return true;
    }

    void open(const std::filesystem::path& parent)
    {
        queue = kqueue();
        if (queue < 0 || fcntl(queue, F_SETFD, FD_CLOEXEC) < 0)
        {
            watch_error("create file watch kqueue");
        }
        directory = ::open(parent.c_str(), watch_flags | O_DIRECTORY);
        if (directory < 0)
        {
            watch_error("open watched directory");
        }
        subscribe(directory, NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE);
        refresh_file();
    }

    bool consume()
    {
        struct kevent events[2];
        timespec now{};
        const int count = kevent(queue, nullptr, 0, events, 2, &now);
        if (count < 0)
        {
            if (errno == EINTR)
            {
                return false;
            }
            watch_error("read vnode events");
        }
        bool refresh = false, changed = false;
        for (int i = 0; i < count; ++i)
        {
            const auto& event = events[i];
            if (event.flags & EV_ERROR)
            {
                throw std::system_error(static_cast<int>(event.data), std::generic_category(), "vnode event");
            }
            if (event.ident == static_cast<std::uintptr_t>(directory))
            {
                if (event.fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE))
                {
                    throw std::system_error(
                        std::make_error_code(std::errc::no_such_file_or_directory),
                        "watched directory"
                    );
                }
                refresh = true;
            }
            else
            {
                changed = true;
                refresh |= (event.fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE)) != 0;
            }
        }
        return (refresh && refresh_file()) || changed;
    }
#endif
    void close_native() noexcept
    {
        if (queue >= 0)
        {
            io.release_socket(queue);
            close_fd(queue);
        }
#if !defined(__linux__)
        close_fd(file);
        close_fd(directory);
#endif
    }
#endif

    void close() noexcept
    {
        if (!std::exchange(closed, true))
        {
            close_native();
        }
    }

    ~implementation()
    {
        close();
#ifdef _WIN32
        if (request.hEvent)
        {
            CloseHandle(request.hEvent);
        }
#endif
    }

    static task<void> wait(std::shared_ptr<implementation> owner)
    {
        auto& watch = *owner;
        if (io_context::current() != &watch.io)
        {
            throw std::logic_error("file watch must run on its owning io_context");
        }
        if (watch.waiting)
        {
            throw std::logic_error("another file watch wait is pending");
        }

        struct guard
        {
            bool& flag;

            ~guard()
            {
                flag = false;
            }
        } reset{watch.waiting};

        watch.waiting = true;
        while (!watch.closed)
        {
#ifdef _WIN32
            co_await watch.io.wait_handle(watch.request.hEvent);
#else
            co_await watch.io.wait_readable(watch.queue);
#endif
            if (!watch.closed)
            {
                try
                {
                    if (watch.consume())
                    {
                        co_return;
                    }
                }
                catch (...)
                {
                    watch.close();
                    throw;
                }
            }
        }
        throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    }
};

file_watch::file_watch(io_context& io, const std::filesystem::path& file)
    : impl_(std::make_shared<implementation>(io, file.filename()))
{
    if (impl_->name.empty() || impl_->name == "." || impl_->name == ".."
        || file.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
    {
        throw std::invalid_argument("file watch requires a filename");
    }
    impl_->open(file.has_parent_path() ? file.parent_path() : std::filesystem::path("."));
}

file_watch::~file_watch()
{
    close();
}

void file_watch::close() noexcept
{
    impl_->close();
}

task<void> file_watch::wait()
{
    return implementation::wait(impl_);
}
} // namespace pace
