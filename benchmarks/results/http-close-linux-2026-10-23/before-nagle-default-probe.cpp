#include <pace/http.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

// Compile unchanged against the pre-fix tree with PACE_HTTP_CLOSE_BASELINE.
// That switch only adapts the constructor; the server and timing are identical.
namespace {
using clock_type = std::chrono::steady_clock;
struct options {
    std::string transport = "tcp", framing = "length", connection = "both", policy = "default";
    std::filesystem::path certs = std::filesystem::path(__FILE__).parent_path().parent_path() / "tests/certs";
    unsigned trials = 10;
    int close_timeout_ms = 100;
    std::vector<int> delays{0, 1, 10, 50, 200};
};
int number(const std::string& value, int maximum) {
    std::size_t end = 0;
    const int result = std::stoi(value, &end);
    if (end != value.size() || result < 0 || result > maximum) throw std::invalid_argument("invalid numeric option");
    return result;
}
options parse(int argc, char** argv) {
    options result;
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 == argc) throw std::invalid_argument("options require values");
        const std::string key = argv[index], value = argv[index + 1];
        if (key == "--transport") result.transport = value;
        else if (key == "--framing") result.framing = value;
        else if (key == "--connection") result.connection = value;
        else if (key == "--close-policy") result.policy = value;
        else if (key == "--certs") result.certs = value;
        else if (key == "--trials") result.trials = static_cast<unsigned>(number(value, 1000));
        else if (key == "--close-timeout-ms") result.close_timeout_ms = number(value, 10000);
        else if (key == "--delays-ms") {
            result.delays.clear();
            std::stringstream input(value); std::string item;
            while (std::getline(input, item, ',')) result.delays.push_back(number(item, 10000));
        } else throw std::invalid_argument("unknown option: " + key);
    }
    if ((result.transport != "tcp" && result.transport != "tls") ||
        (result.framing != "length" && result.framing != "chunked" && result.framing != "head" &&
         result.framing != "204" && result.framing != "eof") ||
        (result.connection != "both" && result.connection != "close" && result.connection != "keepalive") ||
        (result.policy != "default" && result.policy != "graceful") || !result.trials || result.delays.empty())
        throw std::invalid_argument("invalid probe configuration");
#ifdef PACE_HTTP_CLOSE_BASELINE
    if (result.policy != "default") throw std::invalid_argument("baseline only supports default close policy");
#endif
    return result;
}
struct sample {
    pace::io_context io;
    pace::tcp::listener listener{io, {"127.0.0.1", 0}};
    const options& config;
    bool persistent;
    int delay;
    unsigned completed = 0;
    clock_type::time_point begin{}, response_written{}, client_done{}, close_started{};
    explicit sample(const options& value, bool keepalive, int milliseconds)
        : config(value), persistent(keepalive), delay(milliseconds) {}
    void arrive() { if (++completed == 2) io.stop(); }
};

template<class Stream>
pace::task<void> serve(Stream socket, sample& test) {
    std::string request;
    std::array<char, 4096> bytes{};
    while (request.find("\r\n\r\n") == std::string::npos) {
        const auto count = co_await socket.read_some(bytes);
        if (!count || request.size() + count > 16384) throw std::runtime_error("invalid probe request");
        request.append(bytes.data(), count);
    }
    std::string response = test.config.framing == "204" ? "HTTP/1.1 204 No Content\r\n" : "HTTP/1.1 200 OK\r\n";
    if (test.config.framing == "length" || test.config.framing == "head") response += "Content-Length: 2\r\n";
    if (test.config.framing == "chunked") response += "Transfer-Encoding: chunked\r\nTrailer: X-Check\r\n";
    response += test.persistent ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
    if (test.config.framing == "length" || test.config.framing == "eof") response += "ok";
    else if (test.config.framing == "chunked") response += "2\r\nok\r\n0\r\nX-Check: yes\r\n\r\n";
    co_await socket.write_all(response);
    test.response_written = clock_type::now();
    if (test.delay) co_await test.io.sleep_for(std::chrono::milliseconds(test.delay));
    test.close_started = clock_type::now();
    try { co_await socket.close_gracefully(); }
    catch (const std::system_error&) { /* A completed client is allowed to have left. */ }
    test.arrive();
}
template<class Stream>
pace::task<void> exchange(Stream transport, sample& test) {
#ifdef PACE_HTTP_CLOSE_BASELINE
    pace::http::basic_client<Stream> client(std::move(transport));
#else
    pace::http::client_options closing;
    if (test.config.policy == "graceful") closing.close_policy = pace::http::client_close_policy::graceful;
    closing.close_timeout = std::chrono::milliseconds(test.config.close_timeout_ms);
    pace::http::basic_client<Stream> client(std::move(transport), {}, closing);
#endif
    pace::http::request outgoing;
    outgoing.method = test.config.framing == "head" ? "HEAD" : "GET";
    outgoing.target = "/";
    outgoing.headers = {{"Host", "localhost"}};
    outgoing.keep_alive = test.persistent;
    test.begin = clock_type::now();
    const auto response = co_await client.send(std::move(outgoing));
    test.client_done = clock_type::now();
    const bool bodyless = test.config.framing == "head" || test.config.framing == "204";
    if (response.status != (test.config.framing == "204" ? 204U : 200U) ||
        response.body != (bodyless ? "" : "ok") ||
        (test.config.framing == "chunked" && (response.trailers.size() != 1 || response.trailers[0].value != "yes")) ||
        (!test.persistent && client.is_open())) throw std::runtime_error("invalid probe response");
    client.close();
    test.arrive();
}
pace::task<void> tcp_server(sample& test) { co_await serve(co_await test.listener.accept(), test); }
pace::task<void> tcp_client(sample& test) {
    pace::tcp::stream socket(test.io);
    co_await socket.connect(test.listener.local_endpoint());
    co_await exchange(std::move(socket), test);
}
#ifdef PACE_HAS_TLS
pace::task<void> tls_server(sample& test, const pace::tls::context& configuration) {
    pace::tls::stream socket(co_await test.listener.accept(), configuration);
    co_await socket.handshake();
    co_await serve(std::move(socket), test);
}
pace::task<void> tls_client(sample& test, const pace::tls::context& configuration) {
    pace::tls::stream socket(test.io, configuration);
    co_await socket.connect(test.listener.local_endpoint(), "localhost");
    co_await exchange(std::move(socket), test);
}
#endif
double milliseconds(clock_type::time_point end, clock_type::time_point start) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}
void run(const options& config) {
#ifdef PACE_HAS_TLS
    pace::tls::context server(pace::tls::role::server), client(pace::tls::role::client, pace::tls::trust_source::application);
    if (config.transport == "tls") {
        server.use_certificate_chain((config.certs / "localhost.pem").string(), (config.certs / "localhost-key.pem").string());
        client.load_verify_file((config.certs / "ca.pem").string());
    }
#else
    if (config.transport == "tls") throw std::invalid_argument("TLS is not enabled in this probe");
#endif
    std::cout << "transport,framing,close_policy,close_timeout_ms,keep_alive,peer_close_delay_ms,trial,request_ms,after_response_written_ms,actual_peer_delay_ms,completed_before_peer_close,validated\n";
    std::cout << std::fixed << std::setprecision(6);
    for (unsigned trial = 1; trial <= config.trials; ++trial) {
        // Alternate connection-order between trials to reduce ordering bias.
        for (unsigned position = 0; position < 2; ++position) {
            const bool persistent = static_cast<bool>((position + trial) % 2);
            if ((config.connection == "close" && persistent) || (config.connection == "keepalive" && !persistent) ||
                (config.framing == "eof" && persistent)) continue;
            for (const int delay : config.delays) {
                sample test(config, persistent, delay);
#ifdef PACE_HAS_TLS
                if (config.transport == "tls") {
                    test.io.spawn(tls_server(test, server));
                    test.io.spawn(tls_client(test, client));
                } else
#endif
                {
                    test.io.spawn(tcp_server(test));
                    test.io.spawn(tcp_client(test));
                }
                test.io.run();
                if (test.completed != 2 || test.client_done < test.response_written)
                    throw std::runtime_error("incomplete or inconsistent probe sample");
                std::cout << config.transport << ',' << config.framing << ',' << config.policy << ',' << config.close_timeout_ms << ','
                          << persistent << ',' << delay << ',' << trial << ',' << milliseconds(test.client_done, test.begin) << ','
                          << milliseconds(test.client_done, test.response_written) << ',' << milliseconds(test.close_started, test.response_written) << ','
                          << (test.client_done < test.close_started) << ",1\n";
            }
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    try { run(parse(argc, argv)); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
