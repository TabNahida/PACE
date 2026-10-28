#include <pace/http2.hpp>
#include <pace/detail/protocol_driver.hpp>
#include "http2_diagnostics.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#if !defined(_WIN32)
#include <sys/socket.h>
#endif

namespace {
using namespace std::chrono_literals;
const auto suite_started = std::chrono::steady_clock::now();
void stage(std::string_view label) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - suite_started).count();
    std::cerr << "HTTP2 stage: " << label << " elapsed_ms=" << elapsed << '\n';
}
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
struct watchdog {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    std::thread thread{[this] {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, 30s, [&] { return done; })) std::abort();
    }};
    ~watchdog() { { std::lock_guard lock(mutex); done = true; } changed.notify_one(); thread.join(); }
};
void run_scenario(pace::io_context& io) {
    // Each independent connection scenario retains the same deadlock bound.
    // Tiny socket buffers can incur delayed ACKs: successful earlier scenarios
    // must not consume the time available to later ones on another platform.
    watchdog deadline;
    io.run();
}
struct completion {
    pace::io_context& io;
    unsigned count = 0;
    void arrive() { if (++count == 2) io.stop(); }
};
std::string payload() {
    std::string value(130 * 1024 + 7, '\0');
    for (std::size_t i=0; i<value.size(); ++i) value[i] = static_cast<char>(i % 251);
    return value;
}
pace::task<pace::http::response> echo(pace::http::request request) {
    require(request.version == "HTTP/2", "request version");
    pace::http::response result;
    if (request.target == "/fail") throw std::runtime_error("application failure");
    if (request.target == "/reset") { result.status = 205; result.body = "must be suppressed"; }
    else if (request.target == "/head") result.body = "HEAD metadata";
    else if (request.target == "/large") {
        require(request.body == payload(), "large request flow control");
        require(request.trailers.size()==1 && request.trailers[0].value=="request", "request trailers");
        result.body = request.body;
        result.trailers.push_back({"x-trailer", "response"});
    } else result.body = "ok";
    co_return result;
}
std::vector<pace::http::request> requests() {
    std::vector<pace::http::request> batch;
    pace::http::request large;
    large.method="POST"; large.target="/large"; large.body=payload();
    large.headers.push_back({"content-type","application/octet-stream"});
    large.trailers.push_back({"x-trailer","request"});
    batch.push_back(std::move(large));
    for (const auto* path : {"/head", "/reset", "/fail", "/"}) {
        pace::http::request request;
        request.method = std::string_view(path)=="/head" ? "HEAD" : "GET";
        request.target=path;
        batch.push_back(std::move(request));
    }
    return batch;
}
void check_responses(const std::vector<pace::http::response>& replies) {
    require(replies.size()==5, "batch result count");
    for (const auto& response : replies) require(response.version=="HTTP/2", "response version");
    require(replies[0].body==payload(), "large response flow control");
    require(replies[0].trailers.size()==1 && replies[0].trailers[0].value=="response", "response trailers");
    require(replies[1].status==200 && replies[1].body.empty() && replies[1].header("content-length")=="13", "HEAD response");
    require(replies[2].status==205 && replies[2].body.empty(), "205 body suppression");
    require(replies[3].status==500, "handler failure isolation");
    require(replies[4].body=="ok", "batch input ordering");
}
pace::task<void> server(pace::tcp::listener& listener, completion& done) {
    co_await pace::http2::serve_connection(co_await listener.accept(), echo);
    done.arrive();
}
pace::task<void> client(pace::io_context& io, pace::tcp::endpoint endpoint, completion& done) {
    pace::tcp::stream stream(io);
    co_await stream.connect(endpoint);
    check_responses(co_await pace::http2::async_exchange(std::move(stream), "localhost", requests()));
    done.arrive();
}
pace::task<void> single_get(pace::tcp::endpoint endpoint, completion& done) {
    auto transport = co_await pace::tcp::connect(std::move(endpoint));
    const auto reply = co_await pace::http2::async_get(std::move(transport), "localhost");
    require(reply.status == 200 && reply.body == "ok" && reply.version == "HTTP/2", "HTTP2 single GET");
    done.arrive();
}
void roundtrip() {
    for (bool single : {false, true}) {
        pace::io_context io;
        pace::tcp::listener listener(io,{"127.0.0.1",0});
        completion done{io};
        io.spawn(server(listener,done));
        io.spawn(single ? single_get(listener.local_endpoint(), done) : client(io,listener.local_endpoint(),done));
        run_scenario(io); require(done.count==2,"roundtrip incomplete");
    }
}

// Independent RFC 9113 frames and RFC 7541 static-table HPACK: no nghttp2
// encoder/decoder on the raw peer. This catches shared-wrapper assumptions.
std::string frame(unsigned type, unsigned flags, unsigned id, std::string_view body={}) {
    std::string bytes(9,'\0');
    bytes[0]=static_cast<char>(body.size()>>16); bytes[1]=static_cast<char>(body.size()>>8); bytes[2]=static_cast<char>(body.size());
    bytes[3]=static_cast<char>(type); bytes[4]=static_cast<char>(flags);
    for (unsigned i=0;i<4;++i) bytes[5+i]=static_cast<char>(id>>(24-i*8));
    bytes.append(body); return bytes;
}
pace::task<std::string> read_exact(pace::tcp::stream& stream, std::size_t size) {
    std::string bytes(size,'\0'); std::size_t offset=0;
    while(offset<size) {
        const auto count=co_await stream.read_some(std::span<char>(bytes.data()+offset,size-offset));
        require(count!=0,"raw peer early EOF"); offset+=count;
    }
    co_return bytes;
}
pace::task<void> raw_client(pace::io_context& io, pace::tcp::endpoint endpoint, completion& done) {
    pace::tcp::stream stream(io); co_await stream.connect(endpoint);
    std::string headers("\x82\x86\x84\x01\x09",5); headers+="localhost";
    const auto request = std::string("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n")+frame(4,0,0)+frame(1,5,1,headers);
    // Deliberately fragment every preface and frame byte across write calls.
    for (const char& byte : request) co_await stream.write_all(std::string_view(&byte,1));
    std::string body; bool ended=false;
    while (!ended) {
        const auto head=co_await read_exact(stream,9);
        const auto size=(static_cast<unsigned char>(head[0])<<16)|(static_cast<unsigned char>(head[1])<<8)|static_cast<unsigned char>(head[2]);
        const auto data=co_await read_exact(stream,size);
        if (head[3]==4 && head[4]==0) co_await stream.write_all(frame(4,1,0));
        if (head[3]==0) body+=data;
        if ((head[3]==0||head[3]==1) && (head[4]&1)) ended=true;
    }
    require(body=="ok","independent client body");
    const std::string goaway(8,'\0'); co_await stream.write_all(frame(7,0,0,goaway));
    co_await stream.close_gracefully(); done.arrive();
}
pace::task<void> raw_server(pace::tcp::listener& listener, completion& done, unsigned variant) {
    auto stream=co_await listener.accept();
    require(co_await read_exact(stream,24)=="PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n","client preface");
    if (variant==1) { co_await stream.write_all(std::string_view("\0\0",2)); stream.close(); done.arrive(); co_return; }
    bool request=false;
    while(!request) {
        const auto head=co_await read_exact(stream,9);
        const auto size=(static_cast<unsigned char>(head[0])<<16)|(static_cast<unsigned char>(head[1])<<8)|static_cast<unsigned char>(head[2]);
        (void)co_await read_exact(stream,size);
        request=(head[3]==1 && (head[4]&1));
    }
    std::string reply=frame(4,0,0);
    if (variant==3) reply+=frame(3,0,1,std::string("\0\0\0\x08",4)); // RST_STREAM(CANCEL)
    else {
        if (variant==4 || variant==5) {
            std::string goaway(8,'\0'); goaway[3]=variant==4 ? 1 : 0;
            reply+=frame(7,0,0,goaway); // Graceful vs refused active stream.
        }
        if (variant==6) reply+=frame(1,4,1,std::string("\x08\x03" "103",5));
        std::string headers=variant==8 ? std::string("\x08\x03" "205",5) : std::string("\x88",1);
        if (variant==7) headers+=std::string("\0\x01x\x7f\x49",5)+std::string(200,'y');
        reply+=frame(1,4,1,headers)+frame(0,1,1,variant==2 ? "too large" : "ok");
    }
    co_await stream.write_all(reply);
    std::array<char,1024> bytes{};
    try { while (co_await stream.read_some(bytes)) {} }
    catch(const std::system_error&) { }
    co_await stream.close_gracefully(); done.arrive();
}
pace::task<void> simple_client(pace::io_context& io, pace::tcp::endpoint endpoint, completion& done, unsigned variant) {
    pace::tcp::stream stream(io); co_await stream.connect(endpoint);
    std::vector<pace::http::request> batch(1); batch[0].method="GET"; batch[0].target="/";
    pace::http2::limits limits; if(variant==2) limits.max_body_bytes=2;
    if(variant==7) limits.max_header_bytes=256;
    const bool failure = variant==1 || variant==2 || variant==3 || variant==5 || variant==7 || variant==8;
    bool failed=false;
    try {
        auto replies=co_await pace::http2::async_exchange(std::move(stream),"localhost",std::move(batch),limits);
        require(replies.size()==1&&replies[0].status==200&&replies[0].body=="ok","independent server response");
    } catch(const std::exception&) { if(!failure) throw; failed=true; }
    require(failed==failure,"expected HTTP2 failure"); done.arrive();
}
void interop() {
    { pace::io_context io; pace::tcp::listener listener(io,{"127.0.0.1",0}); completion done{io};
      io.spawn(server(listener,done)); io.spawn(raw_client(io,listener.local_endpoint(),done));
      run_scenario(io); require(done.count==2,"raw client incomplete"); }
    for (unsigned variant=0;variant<9;++variant) {
        pace::io_context io; pace::tcp::listener listener(io,{"127.0.0.1",0}); completion done{io};
        io.spawn(raw_server(listener,done,variant));
        io.spawn(simple_client(io,listener.local_endpoint(),done,variant));
        run_scenario(io); require(done.count==2,"raw server incomplete");
    }
}
pace::task<void> validation(pace::io_context& io, bool& completed) {
    for(unsigned variant=0;variant<8;++variant) {
        std::vector<pace::http::request> batch(1); batch[0].method="GET"; batch[0].target="/";
        if(variant==0) batch[0].headers.push_back({"content-type","x\r\ny"});
        if(variant==1) batch[0].headers.push_back({"connection","close"});
        if(variant==2) batch[0].trailers.push_back({"content-length","1"});
        if(variant==3) batch.clear();
        if(variant==6) batch[0].target="*";
        pace::http2::limits limits;
        if(variant==7) limits.max_concurrent_handlers=0;
        if(variant==4 || variant==5) {
            batch[0].headers.push_back({"x-header","one"});
            batch[0].trailers.push_back({"x-trailer","two"});
            if(variant==4) limits.max_header_fields=6;
            // Fields include the RFC's 32-byte overhead: 264 + 44 trailer bytes.
            if(variant==5) limits.max_header_bytes=280;
        }
        bool rejected=false;
        try { (void)co_await pace::http2::async_exchange(pace::tcp::stream(io),"localhost",std::move(batch),limits); }
        catch(const std::invalid_argument&) { rejected=true; }
        catch(const std::length_error&) { rejected=true; }
        require(rejected,"invalid HTTP2 request reached transport");
    }
    completed=true; io.stop();
}
pace::task<void> stalled_server(pace::tcp::listener& listener) {
    auto stream=co_await listener.accept();
    (void)co_await read_exact(stream,24);
    co_await listener.context().sleep_for(5ms);
    listener.context().stop();
}
pace::task<void> canceled_client(pace::io_context& io, pace::tcp::endpoint endpoint, bool& canceled) {
    pace::tcp::stream stream(io); co_await stream.connect(endpoint);
    std::vector<pace::http::request> batch(1); batch[0].method="GET"; batch[0].target="/";
    try { (void)co_await pace::http2::async_exchange(std::move(stream),"localhost",std::move(batch)); }
    catch(const std::system_error& e) { canceled=e.code()==std::errc::operation_canceled; }
    require(canceled,"HTTP2 exchange cancellation");
}
pace::task<void> close_listener(pace::tcp::listener& listener, bool& resumed) {
    auto& io=listener.context();
    co_await io.yield();
    listener.close();
    co_await io.yield();
    require(!io.stopped(),"closing HTTP2 listener stopped unrelated work");
    resumed=true; io.stop();
}
void errors() {
    { pace::io_context io; bool completed=false; io.spawn(validation(io,completed)); run_scenario(io); require(completed,"validation incomplete"); }
    { pace::io_context io; pace::tcp::listener listener(io,{"127.0.0.1",0}); bool canceled=false;
      io.spawn(stalled_server(listener)); io.spawn(canceled_client(io,listener.local_endpoint(),canceled));
      run_scenario(io); require(canceled,"cancellation incomplete"); }
    { pace::io_context io; pace::tcp::listener listener(io,{"127.0.0.1",0}); bool resumed=false;
      io.spawn(pace::http2::serve(listener,echo)); io.spawn(close_listener(listener,resumed));
      run_scenario(io); require(resumed,"listener cancellation stopped unrelated task"); }
    { pace::io_context io; pace::tcp::listener listener(io,{"127.0.0.1",0}); bool completed=false;
      listener.close(); io.spawn(pace::http2::serve(listener,echo)); io.spawn(validation(io,completed));
      run_scenario(io); require(completed,"already-closed listener stopped unrelated task"); }
}
// Handler completion order is independent from stream and batch order. A
// small handler cap must also bound active work while the driver keeps reading.
struct concurrent_handlers {
    pace::io_context& io;
    unsigned active = 0, peak = 0;
    pace::native_socket_type server_fd = pace::invalid_socket, client_fd = pace::invalid_socket;
    std::vector<unsigned> completed;
    explicit concurrent_handlers(pace::io_context& context) : io(context) {}
    pace::task<pace::http::response> operator()(pace::http::request request) {
        const auto id = static_cast<unsigned>(std::stoul(request.target.substr(1)));
        require(request.body == std::string(64 * 1024, 'q'), "duplex concurrent request body");
        peak = std::max(peak, ++active);
        co_await io.sleep_for(id == 0 ? 80ms : 2ms);
        --active; completed.push_back(id);
        if (id == 4) throw std::runtime_error("isolated concurrent handler");
        co_return pace::http::response{.body = std::string(150 * 1024, static_cast<char>('a' + id))};
    }
};
std::vector<pace::http::request> concurrent_requests() {
    std::vector<pace::http::request> result(6);
    for (unsigned i = 0; i < result.size(); ++i) { result[i].method = "GET"; result[i].target = "/" + std::to_string(i); result[i].body.assign(64 * 1024, 'q'); }
    return result;
}
void concurrent_results(const std::vector<pace::http::response>& result) {
    require(result.size() == 6, "concurrent result count");
    for (unsigned i = 0; i < result.size(); ++i) {
        if (i == 4) require(result[i].status == 500, "concurrent exception isolation");
        else require(result[i].body == std::string(150 * 1024, static_cast<char>('a' + i)), "concurrent result order/body");
    }
}
template<class Stream> pace::task<void> concurrent_server(Stream stream, concurrent_handlers& handler, unsigned cap, completion& done) {
    pace::http2::limits limits; limits.max_concurrent_handlers = cap;
    co_await pace::http2::serve_connection(std::move(stream), std::ref(handler), limits); done.arrive();
}
pace::task<void> concurrent_tcp_server(pace::tcp::listener& listener, concurrent_handlers& handler, unsigned cap, completion& done) {
    auto stream = co_await listener.accept(); stream.set_no_delay(); const int small = 4096;
    require(::setsockopt(stream.native_handle(), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&small), sizeof(small)) == 0, "small TCP send buffer");
    co_await concurrent_server(std::move(stream), handler, cap, done);
}
pace::task<void> concurrent_tcp_client(pace::io_context& io, pace::tcp::endpoint endpoint, completion& done) {
    pace::tcp::stream stream(io); stream.set_no_delay(); const int small = 4096;
    require(::setsockopt(stream.native_handle(), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&small), sizeof(small)) == 0, "small TCP client send buffer");
    co_await stream.connect(endpoint);
    concurrent_results(co_await pace::http2::async_exchange(std::move(stream), "localhost", concurrent_requests())); done.arrive();
}
void concurrency() {
    for (unsigned cap : {1u, 2u}) {
        pace::io_context io; pace::tcp::listener listener(io, {"127.0.0.1", 0}); completion done{io};
        concurrent_handlers handler{io};
        io.spawn(concurrent_tcp_server(listener, handler, cap, done));
        io.spawn(concurrent_tcp_client(io, listener.local_endpoint(), done)); run_scenario(io);
        require(done.count == 2 && handler.active == 0 && handler.peak == cap, "HTTP2 handler concurrency bound");
        require(handler.completed.front() == (cap == 1 ? 0u : 1u), "slow handler blocked another stream");
    }
}
struct delayed_handler {
    pace::io_context& io;
    bool started = false, finished = false, canceled = false;
    pace::task<pace::http::response> operator()(pace::http::request) {
        if (!started) {
            started = true;
            try { co_await io.sleep_for(200ms); finished = true; }
            catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; throw; }
        }
        co_return pace::http::response{.body = "ok"};
    }
};
pace::task<void> delayed_server(pace::tcp::listener& listener, delayed_handler& handler, bool& ended) {
    try { co_await pace::http2::serve_connection(co_await listener.accept(), std::ref(handler)); }
    catch (const std::system_error&) {}
    ended = true;
}
pace::task<void> delayed_peer(pace::io_context& io, pace::tcp::endpoint endpoint, delayed_handler& handler, bool& ended, unsigned mode) {
    pace::tcp::stream stream(io); co_await stream.connect(endpoint);
    const std::string headers = std::string("\x82\x86\x84\x01\x09", 5) + "localhost";
    co_await stream.write_all(std::string("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n") + frame(4, 0, 0) + frame(1, 5, 1, headers));
    while (!handler.started) co_await io.yield();
    if (mode == 2) { io.stop(); co_return; }
    if (mode == 0 || mode == 3) {
        // This request arrives after the first handler suspended; processing
        // only requests from the original read would deadlock this exchange.
        if (mode == 3) co_await stream.write_all(frame(3, 0, 1, std::string("\0\0\0\x08", 4)));
        co_await stream.write_all(frame(1, 5, 3, headers));
        bool fast_done = false;
        while (!fast_done) {
            const auto head = co_await read_exact(stream, 9);
            const auto size = (static_cast<unsigned char>(head[0]) << 16) | (static_cast<unsigned char>(head[1]) << 8) | static_cast<unsigned char>(head[2]);
            (void)co_await read_exact(stream, size);
            if (head[3] == 4 && head[4] == 0) co_await stream.write_all(frame(4, 1, 0));
            fast_done = head[8] == 3 && (head[3] == 0 || head[3] == 1) && (head[4] & 1);
        }
        require(!handler.finished, "HTTP2 driver stopped reading during a handler");
    }
    stream.close();
    co_await io.sleep_for(250ms);
    require(ended && (handler.finished != handler.canceled), "HTTP2 connection returned before joining its handler");
    io.stop();
}
void delayed_streams() {
    for (unsigned mode : {0u, 1u, 2u, 3u}) {
        pace::io_context io; pace::tcp::listener listener(io, {"127.0.0.1", 0}); delayed_handler handler{io}; bool ended = false;
        io.spawn(delayed_server(listener, handler, ended)); io.spawn(delayed_peer(io, listener.local_endpoint(), handler, ended, mode));
        run_scenario(io); require(ended && (mode == 2 ? handler.canceled && !handler.finished : (handler.finished != handler.canceled)), "delayed stream incomplete");
    }
}
struct eof_case {
    pace::io_context& io;
    completion done{io};
    unsigned variant, calls = 0;
    bool error = false, canceled = false;
    pace::task<pace::http::response> handle(pace::http::request) {
        ++calls;
        try { co_await io.sleep_for(variant == 3 ? 1s : 20ms); }
        catch (const std::system_error& e) { canceled = e.code() == std::errc::operation_canceled; throw; }
        co_return pace::http::response{.body = variant == 1 ? std::string(100000, 'x') : "half closed"};
    }
};
pace::task<void> eof_server(pace::tcp::listener& listener, eof_case& test) {
    try { co_await pace::http2::serve_connection(co_await listener.accept(), [&](pace::http::request request) { return test.handle(std::move(request)); }); }
    catch (const std::exception&) { test.error = true; }
    test.done.arrive();
}
pace::task<void> eof_peer(pace::tcp::endpoint endpoint, eof_case& test) {
    pace::tcp::stream stream(test.io); co_await stream.connect(endpoint);
    const std::string headers = std::string("\x82\x86\x84\x01\x09", 5) + "localhost";
    co_await stream.write_all(std::string("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n") + frame(4, 0, 0) + frame(1, test.variant == 2 ? 4 : 5, 1, headers));
#ifdef _WIN32
    require(::shutdown(stream.native_handle(), SD_SEND) == 0, "HTTP2 peer write shutdown");
#else
    require(::shutdown(stream.native_handle(), SHUT_WR) == 0, "HTTP2 peer write shutdown");
#endif
    if (test.variant == 3) {
        while (!test.calls) co_await test.io.yield();
        // Let the connection enter its handler-completion wait after EOF.
        co_await test.io.sleep_for(5ms); test.io.stop(); test.done.arrive(); co_return;
    }
    std::string wire; std::array<char, 16384> buffer{};
    while (const auto count = co_await stream.read_some(buffer)) wire.append(buffer.data(), count);
    std::size_t cursor = 0; std::string body; bool ended = false;
    while (cursor + 9 <= wire.size()) {
        const auto size = (static_cast<unsigned char>(wire[cursor]) << 16) | (static_cast<unsigned char>(wire[cursor + 1]) << 8) | static_cast<unsigned char>(wire[cursor + 2]);
        require(static_cast<std::size_t>(size) <= wire.size() - cursor - 9, "HTTP2 EOF output partial frame");
        if (wire[cursor + 3] == 0) { body.append(wire, cursor + 9, size); ended = wire[cursor + 4] & 1; }
        cursor += size + 9;
    }
    if (test.variant == 0) require(body == "half closed" && ended, "complete request lost after TCP half close");
    else if (test.variant == 1) require(body.size() < 100000 && !ended, "EOF response exceeded unavailable flow credit");
    else require(test.calls == 0 && body.empty(), "truncated EOF request reached handler");
    test.done.arrive();
}
void half_close() {
    for (unsigned variant : {0u, 1u, 2u, 3u}) {
        pace::io_context io; pace::tcp::listener listener(io, {"127.0.0.1", 0}); eof_case test{io, {io}, variant};
        auto deadline = io.call_after(2s, [&] { io.stop(); });
        io.spawn(eof_server(listener, test)); io.spawn(eof_peer(listener.local_endpoint(), test)); run_scenario(io);
        require(test.done.count == 2, "HTTP2 EOF stalled on handler/flow control");
        require(test.error == (variant != 0), "HTTP2 EOF error classification");
        if (variant == 3) require(test.canceled, "EOF waiter prevented active handler cancellation");
    }
}
#ifdef PACE_HAS_TLS
pace::task<void> secure_server(pace::tcp::listener& listener, const pace::tls::context& config, completion& done) {
    pace::tls::stream stream(co_await listener.accept(),config); co_await stream.handshake();
    co_await pace::http2::serve_connection(std::move(stream),echo); done.arrive();
}
pace::task<void> secure_client(pace::io_context& io, pace::tcp::endpoint endpoint, const pace::tls::context& config, completion& done) {
    pace::tls::stream stream(io,config); co_await stream.connect(endpoint,"localhost");
    check_responses(co_await pace::http2::async_exchange(std::move(stream),"localhost",requests())); done.arrive();
}
pace::task<void> concurrent_tls_server(pace::tcp::listener& listener, const pace::tls::context& config,
                                        concurrent_handlers& handler, completion& done, bool small_receive) {
    auto tcp = co_await listener.accept();
    handler.server_fd = tcp.native_handle();
    tcp.set_no_delay(); const int small = 4096;
    require(::setsockopt(tcp.native_handle(), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&small), sizeof(small)) == 0, "small TLS send buffer");
    if (small_receive) require(::setsockopt(tcp.native_handle(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&small), sizeof(small)) == 0, "small TLS receive buffer");
    pace::tls::stream stream(std::move(tcp), config); co_await stream.handshake();
    co_await concurrent_server(std::move(stream), handler, 2, done);
}
pace::task<void> concurrent_tls_client(pace::io_context& io, pace::tcp::endpoint endpoint,
                                      const pace::tls::context& config, completion& done, bool small_receive, concurrent_handlers& handler) {
    pace::tcp::stream tcp(io); tcp.set_no_delay(); const int small = 4096;
    require(::setsockopt(tcp.native_handle(), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&small), sizeof(small)) == 0, "small TLS client send buffer");
    if (small_receive) require(::setsockopt(tcp.native_handle(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&small), sizeof(small)) == 0, "small TLS client receive buffer");
    co_await tcp.connect(endpoint); handler.client_fd = tcp.native_handle();
    pace::tls::stream stream(std::move(tcp), config); co_await stream.handshake("localhost");
    concurrent_results(co_await pace::http2::async_exchange(std::move(stream), "localhost", concurrent_requests())); done.arrive();
}
pace::task<void> tls_driver_server(pace::tcp::listener& listener, const pace::tls::context& config, completion& done, bool withhold_close) {
    pace::tls::stream stream(co_await listener.accept(), config); co_await stream.handshake();
    co_await listener.context().sleep_for(20ms);
    std::array<char, 16384> buffer{}; std::size_t received = 0;
    while (received < 1024 * 1024) {
        const auto count = co_await stream.read_some(buffer); require(count != 0, "TLS driver truncated retry");
        require(std::all_of(buffer.begin(), buffer.begin() + count, [](char c) { return c == 'x'; }), "TLS retry corrupted data"); received += count;
    }
    co_await stream.write_all("ok");
    if (withhold_close) {
        require(co_await stream.read_some(buffer) == 0, "TLS driver shutdown omitted close_notify");
        co_await listener.context().sleep_for(20ms); stream.close();
    } else co_await stream.close_gracefully();
    done.arrive();
}
pace::task<void> tls_driver_client(pace::io_context& io, pace::tcp::endpoint endpoint, const pace::tls::context& config, completion& done, bool withhold_close) {
    pace::tcp::stream tcp(io); tcp.set_no_delay(); const int small = 4096;
    require(::setsockopt(tcp.native_handle(), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&small), sizeof(small)) == 0, "TLS driver small send buffer");
    co_await tcp.connect(endpoint); pace::tls::stream stream(std::move(tcp), config); co_await stream.handshake("localhost");
    {
        pace::detail::tls_driver driver(stream); std::array<char, 16> input{};
        require(!driver.read(input), "TLS nonblocking read should want input");
        bool guarded = false;
        try { (void)co_await stream.read_some(input); } catch (const std::logic_error&) { guarded = true; }
        require(guarded, "TLS protocol driver did not retain single-operation guard");
        const std::string payload(1024 * 1024, 'x'); std::size_t offset = 0; bool blocked = false;
        while (offset < payload.size() || !driver.drained()) {
            bool progress = driver.progress();
            if (offset < payload.size() && driver.write_allowed()) {
                const auto count = driver.write(std::string_view(payload).substr(offset));
                offset += count; progress = progress || count != 0;
            }
            if (!driver.drained()) {
                // Backpressure now lives below SSL_write: completing one
                // bounded record leaves application reads free to make progress.
                blocked = true;
                require(driver.read_allowed(), "queued ciphertext blocked application reads");
                require(!driver.write_allowed(), "queued ciphertext allowed unbounded writes");
            }
            progress = driver.progress() || progress;
            if (offset == payload.size() && driver.drained()) break;
            if (progress) { co_await io.yield(); continue; }
            if (driver.either_wait()) co_await io.wait_ready(driver.native_handle());
            else if (driver.write_wait()) co_await io.wait_writable(driver.native_handle());
            else co_await io.wait_readable(driver.native_handle());
        }
        require(blocked, "TLS backpressure did not exercise bounded ciphertext staging");
        std::string response;
        while (response.size() < 2) {
            const auto count = driver.read(input);
            if (count) { require(*count != 0, "TLS response early EOF"); response.append(input.data(), *count); }
            else if (driver.write_wait()) co_await io.wait_writable(driver.native_handle());
            else co_await io.wait_readable(driver.native_handle());
        }
        require(response == "ok", "TLS retry response");
    }
    require(stream.is_open(), "TLS driver closed a successfully drained transport");
    co_await stream.close_gracefully(withhold_close ? 5ms : 100ms);
    require(!stream.is_open(), "TLS driver graceful close deadline did not close transport"); done.arrive();
}
pace::task<void> tls_abort_server(pace::tcp::listener& listener, const pace::tls::context& config, completion& done) {
    pace::tls::stream stream(co_await listener.accept(), config); co_await stream.handshake();
    // A client's handshake can finish before the server consumes its Finished
    // message. Confirm server completion before the client deliberately aborts.
    co_await stream.write_all("ready");
    // Leave the peer's tiny send buffer blocked while it abandons queued output.
    co_await listener.context().sleep_for(10ms);
    std::array<char, 16384> input{};
    bool truncated = false;
    try { while (co_await stream.read_some(input)) {} }
    catch (const std::system_error& error) { truncated = error.code() == std::errc::protocol_error; }
    require(truncated, "exceptional TLS driver exit sent a clean close after dropping ciphertext"); done.arrive();
}
pace::task<void> tls_abort_client(pace::io_context& io, pace::tcp::endpoint endpoint, const pace::tls::context& config, completion& done) {
    pace::tcp::stream tcp(io); tcp.set_no_delay(); const int small = 4096;
    require(::setsockopt(tcp.native_handle(), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&small), sizeof(small)) == 0, "TLS abort small send buffer");
    co_await tcp.connect(endpoint); pace::tls::stream stream(std::move(tcp), config); co_await stream.handshake("localhost");
    std::array<char, 5> ready{}; std::size_t received = 0;
    while (received < ready.size()) {
        const auto count = co_await stream.read_some(std::span<char>(ready).subspan(received));
        require(count != 0, "TLS abort peer closed before handshake barrier"); received += count;
    }
    require(std::string_view(ready.data(), ready.size()) == "ready", "TLS abort handshake barrier");
    struct abandon {};
    try {
        pace::detail::tls_driver driver(stream); const std::string bytes(16384, 'x');
        unsigned count = 0;
        while (driver.drained() && count++ < 1024) require(driver.write(bytes) != 0, "TLS abort staging made no progress");
        require(!driver.drained(), "TLS abort did not queue ciphertext");
        throw abandon{};
    } catch (const abandon&) {}
    require(!stream.is_open(), "TLS driver exception retained unsent ciphertext on an open stream"); done.arrive();
}
void secure_roundtrip(const std::filesystem::path& certs) {
    pace::io_context io; pace::tcp::listener listener(io,{"127.0.0.1",0}); completion done{io};
    pace::tls::context server_config(pace::tls::role::server), client_config;
    server_config.use_certificate_chain((certs/"localhost.pem").string(),(certs/"localhost-key.pem").string());
    client_config.load_verify_file((certs/"ca.pem").string());
    server_config.set_alpn_protocols({"h2"}); client_config.set_alpn_protocols({"h2"});
    io.spawn(secure_server(listener,server_config,done)); io.spawn(secure_client(io,listener.local_endpoint(),client_config,done));
    stage("TLS roundtrip");
    run_scenario(io); require(done.count==2,"secure roundtrip incomplete");
    for (const bool small_receive : {false, true}) {
        pace::io_context concurrent_io; pace::tcp::listener concurrent_listener(concurrent_io, {"127.0.0.1", 0});
        completion concurrent_done{concurrent_io}; concurrent_handlers handler{concurrent_io};
        concurrent_io.spawn(concurrent_tls_server(concurrent_listener, server_config, handler, concurrent_done, small_receive));
        concurrent_io.spawn(concurrent_tls_client(concurrent_io, concurrent_listener.local_endpoint(), client_config, concurrent_done, small_receive, handler));
        std::array<pace::io_context::timer_handle, 3> diagnostic_timers;
#if defined(__FreeBSD__)
        if (const auto* trace = std::getenv("PACE_HTTP2_TRACE"); trace && *trace) {
            std::size_t timer = 0;
            for (const auto elapsed : {2s, 8s, 20s}) diagnostic_timers[timer++] = concurrent_io.call_after(elapsed, [&, elapsed] {
                std::cerr << "HTTP2 diagnostic: stage_seconds=" << elapsed.count() << " small_receive=" << small_receive
                          << " active=" << handler.active << " peak=" << handler.peak << " handlers_completed=" << handler.completed.size()
                          << " peers_completed=" << concurrent_done.count << '\n';
                pace_test_http2_snapshot("server", handler.server_fd);
                pace_test_http2_snapshot("client", handler.client_fd);
            });
        }
#endif
        stage(small_receive ? "TLS small receive-window duplex" : "TLS concurrent duplex/backpressure");
        run_scenario(concurrent_io);
        for (auto& timer : diagnostic_timers) timer.cancel();
        require(concurrent_done.count == 2 && handler.active == 0 && handler.peak <= 2, "TLS HTTP2 duplex/backpressure");
        // Tiny TCP receive windows can delay the second complete request until
        // after the first handler finishes; use normal windows to test overlap.
        if (!small_receive) require(handler.peak == 2 && handler.completed.front() == 1, "TLS HTTP2 concurrent handlers");
    }
    for (const bool withhold_close : {false, true}) {
        pace::io_context driver_io; pace::tcp::listener driver_listener(driver_io, {"127.0.0.1", 0}); completion driver_done{driver_io};
        driver_io.spawn(tls_driver_server(driver_listener, server_config, driver_done, withhold_close));
        driver_io.spawn(tls_driver_client(driver_io, driver_listener.local_endpoint(), client_config, driver_done, withhold_close));
        stage(withhold_close ? "TLS driver close deadline" : "TLS driver ciphertext backpressure"); run_scenario(driver_io);
        require(driver_done.count == 2, "TLS protocol driver backpressure test incomplete");
    }
    pace::io_context abort_io; pace::tcp::listener abort_listener(abort_io, {"127.0.0.1", 0}); completion abort_done{abort_io};
    abort_io.spawn(tls_abort_server(abort_listener, server_config, abort_done));
    abort_io.spawn(tls_abort_client(abort_io, abort_listener.local_endpoint(), client_config, abort_done));
    stage("TLS driver abandoned ciphertext"); run_scenario(abort_io);
    require(abort_done.count == 2, "TLS protocol driver abandonment test incomplete");
}
#endif
}
int main(int argc, char** argv) {
    try {
        stage("roundtrip"); roundtrip();
        stage("raw interoperability"); interop();
        stage("errors"); errors();
        stage("TCP concurrency/backpressure"); concurrency();
        stage("delayed/reset/cancel"); delayed_streams();
        stage("TCP half close"); half_close();
#ifdef PACE_HAS_TLS
        secure_roundtrip(argc>1 ? std::filesystem::path(argv[1]) : std::filesystem::path(__FILE__).parent_path()/"certs");
#else
        (void)argc; (void)argv;
#endif
        std::cout<<"HTTP/2 multiplexing, flow control, trailers, raw interoperability and limits passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
