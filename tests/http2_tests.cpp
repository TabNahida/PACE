#include <pace/http2.hpp>
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
using namespace std::chrono_literals;
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
    check_responses(co_await pace::http2::exchange(std::move(stream), "localhost", requests()));
    done.arrive();
}
void roundtrip() {
    pace::io_context io;
    pace::tcp::listener listener(io,{"127.0.0.1",0});
    completion done{io};
    io.spawn(server(listener,done)); io.spawn(client(io,listener.local_endpoint(),done));
    io.run(); require(done.count==2,"roundtrip incomplete");
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
        auto replies=co_await pace::http2::exchange(std::move(stream),"localhost",std::move(batch),limits);
        require(replies.size()==1&&replies[0].status==200&&replies[0].body=="ok","independent server response");
    } catch(const std::exception&) { if(!failure) throw; failed=true; }
    require(failed==failure,"expected HTTP2 failure"); done.arrive();
}
void interop() {
    { pace::io_context io; pace::tcp::listener listener(io,{"127.0.0.1",0}); completion done{io};
      io.spawn(server(listener,done)); io.spawn(raw_client(io,listener.local_endpoint(),done));
      io.run(); require(done.count==2,"raw client incomplete"); }
    for (unsigned variant=0;variant<9;++variant) {
        pace::io_context io; pace::tcp::listener listener(io,{"127.0.0.1",0}); completion done{io};
        io.spawn(raw_server(listener,done,variant));
        io.spawn(simple_client(io,listener.local_endpoint(),done,variant));
        io.run(); require(done.count==2,"raw server incomplete");
    }
}
pace::task<void> validation(pace::io_context& io, bool& completed) {
    for(unsigned variant=0;variant<7;++variant) {
        std::vector<pace::http::request> batch(1); batch[0].method="GET"; batch[0].target="/";
        if(variant==0) batch[0].headers.push_back({"content-type","x\r\ny"});
        if(variant==1) batch[0].headers.push_back({"connection","close"});
        if(variant==2) batch[0].trailers.push_back({"content-length","1"});
        if(variant==3) batch.clear();
        if(variant==6) batch[0].target="*";
        pace::http2::limits limits;
        if(variant==4 || variant==5) {
            batch[0].headers.push_back({"x-header","one"});
            batch[0].trailers.push_back({"x-trailer","two"});
            if(variant==4) limits.max_header_fields=6;
            // Fields include the RFC's 32-byte overhead: 264 + 44 trailer bytes.
            if(variant==5) limits.max_header_bytes=280;
        }
        bool rejected=false;
        try { (void)co_await pace::http2::exchange(pace::tcp::stream(io),"localhost",std::move(batch),limits); }
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
    try { (void)co_await pace::http2::exchange(std::move(stream),"localhost",std::move(batch)); }
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
    { pace::io_context io; bool completed=false; io.spawn(validation(io,completed)); io.run(); require(completed,"validation incomplete"); }
    { pace::io_context io; pace::tcp::listener listener(io,{"127.0.0.1",0}); bool canceled=false;
      io.spawn(stalled_server(listener)); io.spawn(canceled_client(io,listener.local_endpoint(),canceled));
      io.run(); require(canceled,"cancellation incomplete"); }
    { pace::io_context io; pace::tcp::listener listener(io,{"127.0.0.1",0}); bool resumed=false;
      io.spawn(pace::http2::serve(listener,echo)); io.spawn(close_listener(listener,resumed));
      io.run(); require(resumed,"listener cancellation stopped unrelated task"); }
    { pace::io_context io; pace::tcp::listener listener(io,{"127.0.0.1",0}); bool completed=false;
      listener.close(); io.spawn(pace::http2::serve(listener,echo)); io.spawn(validation(io,completed));
      io.run(); require(completed,"already-closed listener stopped unrelated task"); }
}
#ifdef PACE_HAS_TLS
pace::task<void> secure_server(pace::tcp::listener& listener, const pace::tls::context& config, completion& done) {
    pace::tls::stream stream(co_await listener.accept(),config); co_await stream.handshake();
    co_await pace::http2::serve_connection(std::move(stream),echo); done.arrive();
}
pace::task<void> secure_client(pace::io_context& io, pace::tcp::endpoint endpoint, const pace::tls::context& config, completion& done) {
    pace::tls::stream stream(io,config); co_await stream.connect(endpoint,"localhost");
    check_responses(co_await pace::http2::exchange(std::move(stream),"localhost",requests())); done.arrive();
}
void secure_roundtrip(const std::filesystem::path& certs) {
    pace::io_context io; pace::tcp::listener listener(io,{"127.0.0.1",0}); completion done{io};
    pace::tls::context server_config(pace::tls::role::server), client_config;
    server_config.use_certificate_chain((certs/"localhost.pem").string(),(certs/"localhost-key.pem").string());
    client_config.load_verify_file((certs/"ca.pem").string());
    server_config.set_alpn_protocols({"h2"}); client_config.set_alpn_protocols({"h2"});
    io.spawn(secure_server(listener,server_config,done)); io.spawn(secure_client(io,listener.local_endpoint(),client_config,done));
    io.run(); require(done.count==2,"secure roundtrip incomplete");
}
#endif
}
int main(int argc, char** argv) {
    watchdog deadline;
    try {
        roundtrip(); interop(); errors();
#ifdef PACE_HAS_TLS
        secure_roundtrip(argc>1 ? std::filesystem::path(argv[1]) : std::filesystem::path(__FILE__).parent_path()/"certs");
#else
        (void)argc; (void)argv;
#endif
        std::cout<<"HTTP/2 multiplexing, flow control, trailers, raw interoperability and limits passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
