// Linux fault injection, linked with --wrap=SSL_CTX_set_default_verify_paths.
// The library itself contains no test hook or verification-disable mechanism.
#include <pace/tls.hpp>
#include <openssl/ssl.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
unsigned default_path_calls = 0;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct watchdog {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    std::thread thread{[this] {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(30), [&] { return done; })) std::abort();
    }};
    ~watchdog() { { std::lock_guard lock(mutex); done = true; } changed.notify_one(); thread.join(); }
};
struct finished {
    pace::io_context& io;
    unsigned count = 0;
    void arrive() { if (++count == 2) io.stop(); }
};
template<class Stream>
pace::task<void> read_exact(Stream& stream, std::span<char> bytes) {
    while (!bytes.empty()) {
        const auto count = co_await stream.read_some(bytes);
        require(count != 0, "early EOF in trust failure regression");
        bytes = bytes.subspan(count);
    }
}

pace::task<void> server(pace::tcp::listener& listener, const pace::tls::context& configuration,
                        bool starttls, bool should_accept, finished& done) {
    auto transport = co_await listener.accept();
    if (starttls) {
        std::array<char, 10> command{};
        co_await read_exact(transport, command);
        require(std::string_view(command.data(), command.size()) == "STARTTLS\r\n", "bad STARTTLS command");
        co_await transport.write_all("220 ready\r\n");
    }
    pace::tls::stream stream(std::move(transport), configuration);
    bool accepted = false;
    try { co_await stream.handshake(); accepted = true; }
    catch (const std::system_error& error) {
        require(error.code() == std::errc::protocol_error && !stream.is_open(), "unexpected server failure");
    }
    require(accepted == should_accept, "unexpected server trust decision");
    if (accepted) {
        co_await stream.write_all("verified");
        co_await stream.shutdown();
    }
    done.arrive();
}

pace::task<void> client(pace::io_context& io, pace::tcp::endpoint endpoint,
                        std::filesystem::path fixtures, std::string ca, std::string name,
                        bool starttls, bool should_accept, finished& done) {
    pace::tcp::stream transport(io);
    if (starttls) {
        co_await transport.connect(endpoint);
        co_await transport.write_all("STARTTLS\r\n");
        std::array<char, 11> ready{};
        co_await read_exact(transport, ready);
        require(std::string_view(ready.data(), ready.size()) == "220 ready\r\n", "bad STARTTLS reply");
    }
    auto stream = [&] {
        pace::tls::context config(pace::tls::role::client, pace::tls::trust_source::application);
        if (!ca.empty()) {
            std::ifstream input(fixtures / ca, std::ios::binary);
            require(static_cast<bool>(input), "missing trust regression fixture");
            const std::string pem{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            config.load_verify_pem(pem);
        }
        return pace::tls::stream(std::move(transport), config);
    }(); // Config and PEM have both been destroyed before the handshake.
    bool accepted = false;
    try {
        if (starttls) co_await stream.handshake(name);
        else co_await stream.connect(endpoint, name);
        accepted = true;
    } catch (const std::system_error& error) {
        require(error.code() == std::errc::protocol_error && !stream.is_open(), "unexpected client failure");
    }
    require(accepted == should_accept, "application trust bypassed chain/hostname verification");
    if (accepted) {
        std::array<char, 8> bytes{};
        co_await read_exact(stream, bytes);
        require(std::string_view(bytes.data(), bytes.size()) == "verified", "wrong authenticated payload");
        co_await stream.shutdown();
    }
    done.arrive();
}
void run_case(const std::filesystem::path& fixtures, bool starttls, std::string ca,
              std::string name, bool should_accept) {
    pace::io_context io;
    pace::tls::context config(pace::tls::role::server);
    config.use_certificate_chain((fixtures / "localhost.pem").string(), (fixtures / "localhost-key.pem").string());
    pace::tcp::listener listener(io, {"127.0.0.1", 0});
    finished done{io};
    io.spawn(server(listener, config, starttls, should_accept, done));
    io.spawn(client(io, listener.local_endpoint(), fixtures, std::move(ca), std::move(name),
                    starttls, should_accept, done));
    io.run();
    require(done.count == 2, "trust failure regression did not finish both peers");
}
} // namespace

extern "C" int __wrap_SSL_CTX_set_default_verify_paths(SSL_CTX*) {
    ++default_path_calls;
    return 0;
}

int main(int argc, char** argv) {
    watchdog guard;
    try {
        const auto fixtures = argc > 1 ? std::filesystem::path(argv[1]) :
            std::filesystem::path(__FILE__).parent_path() / "certs";
        bool failed = false;
        try { pace::tls::context system; } catch (const std::system_error&) { failed = true; }
        require(failed && default_path_calls == 1, "default-path fault injection was not exercised");
        for (bool starttls : {false, true}) {
            run_case(fixtures, starttls, "ca.pem", "localhost", true);
            run_case(fixtures, starttls, "ca.pem", "wrong.example", false);
            run_case(fixtures, starttls, "sni-ca.pem", "localhost", false);
            run_case(fixtures, starttls, "", "localhost", false);
        }
        require(default_path_calls == 1, "application trust unexpectedly consulted default paths");
        std::cout << "TLS unavailable-default-trust tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
