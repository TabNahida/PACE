#include <pace/pace.hpp>
#include <pace/http_router.hpp>
#include <filesystem>
#include <thread>
#include <iostream>
#include <mutex>
#include <condition_variable>
using namespace std::chrono_literals;
void require(bool b, const char* text) { if (!b) throw std::runtime_error(text); }
pace::http::router routes() {
    pace::http::router result;
    result.get("/", [] { return pace::http::text("ok"); });
    result.post("/", [](pace::http::request request) {
        if (request.header("content-type") != "text/plain") throw std::runtime_error("POST content type");
        return pace::http::text(std::move(request.body));
    });
    result.get("/slow", []() -> pace::task<pace::http::response> {
        co_await pace::sleep_for(1h); co_return pace::http::text("late");
    });
    return result;
}
struct server_thread {
    pace::io_context& io;
    std::exception_ptr failure;
    std::thread thread;
    explicit server_thread(pace::io_context& context) : io(context), thread([this] { try { io.run(); } catch (...) { failure=std::current_exception(); } }) {}
    void finish() { io.stop(); thread.join(); if (failure) std::rethrow_exception(failure); }
    ~server_thread() { io.stop(); if (thread.joinable()) thread.join(); }
};
template<class Get, class Post, class Fetch, class AsyncGet>
void exercise(const std::string& url, Get get, Post post, Fetch fetch, AsyncGet async_get) {
    require(get(url).body == "ok", "direct GET contract");
    require(post(url, std::string("x\0y", 3)).body == std::string("x\0y", 3), "direct POST contract");
    pace::http::request request; request.method = "HEAD";
    auto head = fetch(url, std::move(request));
    require(head.status == 200 && head.body.empty(), "HEAD contract");
    require(pace::run(async_get(url)).body == "ok", "async URL contract");
    bool expired=false;
    try { pace::run(pace::with_deadline(pace::deadline::after(20ms), async_get(url+"slow"))); }
    catch (const std::system_error& e) { expired=e.code()==std::errc::timed_out; }
    require(expired, "shared deadline did not cancel HTTP request");
    require(get(url).body == "ok", "one timeout affected another request");
    request.method="GET"; request.headers={{"Host", "override.invalid"}};
    bool rejected=false;
    try { (void)fetch(url, std::move(request)); } catch (const std::invalid_argument&) { rejected=true; }
    require(rejected, "URL authority override allowed");
}
#if defined(PACE_HAS_HTTP1) || defined(PACE_HAS_HTTP2)
template<class Serve, class Get>
pace::task<void> listener_scope(Serve serve, Get get) {
    auto listener = pace::tcp::listen({"127.0.0.1", 0});
    auto url = "http://127.0.0.1:" + std::to_string(listener.local_endpoint().port) + "/";
    bool entered = false, finished = false;
    pace::http::handler handler = [&](pace::http::request) -> pace::task<pace::http::response> {
        struct finish { bool& value; ~finish() { value = true; } } done{finished};
        entered = true;
        co_await pace::sleep_for(1h);
        co_return pace::http::text("late");
    };
    co_await pace::with_task_group([&](pace::task_group& group) -> pace::task<void> {
        auto server = group.spawn(serve(listener, handler));
        (void)group.spawn([&]() -> pace::task<void> {
            try { (void)co_await get(url); } catch (const std::exception&) {}
        });
        while (!entered) co_await pace::yield();
        listener.close();
        co_await server;
        require(finished, "serve returned before canceled handler finished");
        require(!pace::this_context().stopped(), "listener close stopped unrelated context");
    }, {.deadline = pace::deadline::after(2s)});
}
#endif

int main() {
    std::mutex mutex; std::condition_variable cv; bool done = false;
    std::thread watchdog([&] { std::unique_lock lock(mutex); if (!cv.wait_for(lock, 40s, [&] { return done; })) std::abort(); });
    int status=0;
    try {
#ifdef PACE_HAS_HTTP1
        pace::run(listener_scope([](auto& l, auto h) { return pace::http::serve(l, h); },
            [](auto u) { return pace::http1::async_get(u); }));
        {
            pace::io_context io; pace::tcp::listener listener(io,{"127.0.0.1",0});
            auto url="http://127.0.0.1:"+std::to_string(listener.local_endpoint().port)+"/";
            io.spawn(pace::http::serve(listener,routes())); server_thread server(io);
            exercise(url, [](auto u){return pace::http::get(u);}, [](auto u,auto b){return pace::http::post(u,b,"text/plain");},
                [](auto u,auto r){return pace::http::fetch(u,r);}, [](auto u){return pace::http::async_get(u);});
            server.finish();
        }
#endif
#ifdef PACE_HAS_HTTP2
        pace::run(listener_scope([](auto& l, auto h) { return pace::http2::serve(l, h); },
            [](auto u) { return pace::http2::async_get(u); }));
        {
            pace::io_context io; pace::tcp::listener listener(io,{"127.0.0.1",0});
            auto url="http://127.0.0.1:"+std::to_string(listener.local_endpoint().port)+"/";
            io.spawn(pace::http2::serve(listener,routes())); server_thread server(io);
            exercise(url, [](auto u){return pace::http2::get(u);}, [](auto u,auto b){return pace::http2::post(u,b,"text/plain");},
                [](auto u,auto r){return pace::http2::fetch(u,r);}, [](auto u){return pace::http2::async_get(u);});
            server.finish();
        }
#ifdef PACE_HAS_TLS
        {
            const auto certs=std::filesystem::path(__FILE__).parent_path()/"certs";
            pace::tls::context security(pace::tls::role::server), trust;
            security.use_certificate_chain((certs/"localhost.pem").string(),(certs/"localhost-key.pem").string());
            security.set_alpn_protocols({"h2"}); trust.load_verify_file((certs/"ca.pem").string()); trust.set_alpn_protocols({"h2"});
            pace::io_context io; pace::tcp::listener listener(io,{"127.0.0.1",0});
            auto url="https://127.0.0.1:"+std::to_string(listener.local_endpoint().port)+"/";
            io.spawn(pace::http2::serve(listener,security,routes())); server_thread server(io);
            require(pace::http2::get(trust,url).body=="ok", "HTTP2 verified URL");
            bool refused=false; try{(void)pace::http2::get(url);}catch(const std::exception&){refused=true;}
            require(refused,"HTTP2 trust bypass"); server.finish();
        }
#endif
#endif
#ifdef PACE_HAS_HTTP3
        {
            const auto certs=std::filesystem::path(__FILE__).parent_path()/"certs";
            pace::quic::context security(pace::quic::role::server), trust;
            security.use_certificate_chain((certs/"localhost.pem").string(),(certs/"localhost-key.pem").string());
            security.set_alpn_protocols({"h3"}); trust.load_verify_file((certs/"ca.pem").string()); trust.set_alpn_protocols({"h3"});
            pace::io_context io; pace::quic::listener listener(io,{"127.0.0.1",0},security);
            auto url="https://127.0.0.1:"+std::to_string(listener.local_endpoint().port)+"/";
            io.spawn(pace::http3::serve(listener,routes())); server_thread server(io);
            exercise(url, [&](auto u){return pace::http3::get(trust,u);}, [&](auto u,auto b){
                pace::http::request r; r.method="POST"; r.body=std::move(b); r.headers={{"Content-Type","text/plain"}};
                return pace::http3::fetch(trust,u,std::move(r));},
                [&](auto u,auto r){return pace::http3::fetch(trust,u,r);}, [&](auto u)->pace::task<pace::http::response>{
                    co_return co_await pace::http3::async_get(pace::this_context(),trust,u);
                });
            bool refused=false; try{(void)pace::http3::get(url);}catch(const std::exception&){refused=true;}
            require(refused,"HTTP3 trust bypass");
            refused=false; try{(void)pace::http3::get("http://127.0.0.1/");}catch(const std::invalid_argument&){refused=true;}
            require(refused,"HTTP3 silent insecure downgrade"); server.finish();
        }
#endif
        std::cout<<"Unified HTTP URL methods, local deadlines, authority and verified protocols passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';status=1;}
    {std::lock_guard lock(mutex);done=true;}cv.notify_one();watchdog.join();return status;
}
