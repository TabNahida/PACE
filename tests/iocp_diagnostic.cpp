// Independent SDK/API probe: no PACE poller, used to diagnose Windows ABI support.
#include <winsock2.h>
#include <windows.h>
#include <iostream>
#include <cstddef>
int main() {
    WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
    std::cout << "registration size=" << sizeof(SOCK_NOTIFY_REGISTRATION)
              << " filter offset=" << offsetof(SOCK_NOTIFY_REGISTRATION, eventFilter) << '\n';
    using function = DWORD (WSAAPI*)(HANDLE, UINT32, SOCK_NOTIFY_REGISTRATION*, UINT32, ULONG, OVERLAPPED_ENTRY*, UINT32*);
    unsigned success = 0;
    for (auto module_name : {L"ws2_32.dll", L"mswsock.dll"}) {
        const auto module = LoadLibraryW(module_name);
        auto process = reinterpret_cast<function>(GetProcAddress(module, "ProcessSocketNotifications"));
        if (!process) { FreeLibrary(module); continue; }
        for (int receive : {0, 1}) for (UINT16 filter : {1, 5}) for (UINT8 trigger : {4, 5}) {
            auto port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
            SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(sock, 16)) return 2;
            SOCK_NOTIFY_REGISTRATION info{sock, reinterpret_cast<void*>(1), filter, SOCK_NOTIFY_OP_ENABLE, trigger, 0};
            OVERLAPPED_ENTRY entry{}; UINT32 received = 0;
            const auto result = process(port, 1, &info, 0, receive, receive ? &entry : nullptr, receive ? &received : nullptr);
            std::cout << (module_name[0] == 'w' ? "ws2" : "msw") << " receive=" << receive << " filter=" << filter
                      << " trigger=" << unsigned(trigger) << " call=" << result << " registration=" << info.registrationResult << '\n';
            if ((result == 0 || result == WAIT_TIMEOUT) && info.registrationResult == 0) ++success;
            closesocket(sock); CloseHandle(port);
        }
        FreeLibrary(module);
    }
    WSACleanup();
#ifdef _WIN64
    return success ? 0 : 1;
#else
    std::cout << "32-bit SDK probe: " << success << " supported variants; PACE uses WSAPoll on x86\n";
    return 0;
#endif
}
