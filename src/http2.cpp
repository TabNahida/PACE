#include <pace/http2.hpp>
#ifdef PACE_HAS_HTTP2
#include <nghttp2/nghttp2.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <deque>
#include <map>
#include <stdexcept>
#include <utility>

namespace pace::http2 {
namespace {
void check(int result) {
    if (result < 0) throw std::runtime_error(std::string("HTTP/2: ") + nghttp2_strerror(result));
}
bool valid_value(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char c) { return c == '\t' || (c >= 32 && c != 127); });
}
bool valid_name(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos;
    });
}
std::string lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return text;
}
bool forbidden(std::string_view name) {
    return name == "connection" || name == "proxy-connection" || name == "keep-alive" ||
           name == "transfer-encoding" || name == "upgrade";
}
using fields = std::vector<http::header_field>;
std::vector<nghttp2_nv> wire_headers(fields& headers) {
    std::vector<nghttp2_nv> result;
    for (auto& h : headers) result.push_back({reinterpret_cast<uint8_t*>(h.name.data()),
        reinterpret_cast<uint8_t*>(h.value.data()), h.name.size(), h.value.size(), NGHTTP2_NV_FLAG_NONE});
    return result;
}
void add_headers(fields& target, const fields& source, limits bounds, bool trailers = false) {
    std::size_t total = 0;
    for (const auto& item : target) {
        if (bounds.max_header_bytes - total < 32) throw std::length_error("HTTP/2 outbound header limit");
        total += 32; // RFC 9113 SETTINGS_MAX_HEADER_LIST_SIZE accounting.
        if (!valid_value(item.value) || item.name.size() > bounds.max_header_bytes - total ||
            item.value.size() > bounds.max_header_bytes - total - item.name.size())
            throw std::length_error("HTTP/2 outbound header limit");
        total += item.name.size() + item.value.size();
    }
    if (target.size() > bounds.max_header_fields) throw std::length_error("HTTP/2 outbound header limit");
    for (const auto& item : source) {
        auto name = lower(item.name);
        if (!valid_name(name) || !valid_value(item.value) || forbidden(name) ||
            (name == "te" && item.value != "trailers") ||
            (trailers && (name == "content-length" || name == "host" || name == "te")))
            throw std::invalid_argument("invalid HTTP/2 header");
        if (!trailers && (name == "content-length" || name == "host" || name == "content-type")) continue;
        if (bounds.max_header_bytes - total < 32) throw std::length_error("HTTP/2 outbound header limit");
        total += 32;
        if (name.size() > bounds.max_header_bytes - total ||
            item.value.size() > bounds.max_header_bytes - total - name.size() || target.size() >= bounds.max_header_fields)
            throw std::length_error("HTTP/2 outbound header limit");
        total += name.size() + item.value.size();
        target.push_back({std::move(name), item.value});
    }
}
void check_combined_headers(const fields& headers, const fields& trailers, limits bounds) {
    std::size_t count = 0, bytes = 0;
    for (const auto* group : {&headers, &trailers}) for (const auto& field : *group) {
        if (bounds.max_header_bytes - bytes < 32) throw std::length_error("HTTP/2 combined header/trailer limit");
        bytes += 32;
        if (++count > bounds.max_header_fields || field.name.size() > bounds.max_header_bytes - bytes ||
            field.value.size() > bounds.max_header_bytes - bytes - field.name.size())
            throw std::length_error("HTTP/2 combined header/trailer limit");
        bytes += field.name.size() + field.value.size();
    }
}
struct stream_state {
    http::request request;
    http::response response;
    std::string outgoing;
    fields trailers;
    std::size_t offset = 0, header_bytes = 0, header_fields = 0, result_index = 0;
    bool incoming_complete = false, trailer_block = false, final_status = false, replied = false;
};
struct session {
    nghttp2_session* native = nullptr;
    limits bounds;
    bool server;
    std::map<int32_t, stream_state> streams;
    std::deque<int32_t> ready;
    std::vector<http::response> responses;
    std::exception_ptr failure;
    std::size_t accepted = 0, completed = 0;
    bool goaway = false;

    explicit session(bool is_server, limits value) : bounds(value), server(is_server) {
        if (!bounds.max_header_bytes || !bounds.max_header_fields || !bounds.max_concurrent_streams ||
            bounds.max_concurrent_streams > 10000 || !bounds.max_requests_per_connection)
            throw std::invalid_argument("invalid HTTP/2 limits");
        nghttp2_session_callbacks* callbacks = nullptr;
        check(nghttp2_session_callbacks_new(&callbacks));
        nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, begin_headers);
        nghttp2_session_callbacks_set_on_header_callback(callbacks, header);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, data);
        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, frame);
        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, closed);
        const int result = server ? nghttp2_session_server_new(&native, callbacks, this)
                                  : nghttp2_session_client_new(&native, callbacks, this);
        nghttp2_session_callbacks_del(callbacks);
        check(result);
        try {
            nghttp2_settings_entry settings[] = {
                {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, static_cast<uint32_t>(bounds.max_concurrent_streams)},
                {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, static_cast<uint32_t>(std::min<std::size_t>(bounds.max_header_bytes, UINT32_MAX))},
                {NGHTTP2_SETTINGS_ENABLE_PUSH, 0}};
            check(nghttp2_submit_settings(native, NGHTTP2_FLAG_NONE, settings, server ? 2 : 3));
        } catch (...) { nghttp2_session_del(native); throw; }
    }
    ~session() { nghttp2_session_del(native); }
    session(const session&) = delete;
    template<class F> static int callback(void* pointer, F&& action) noexcept {
        auto& self = *static_cast<session*>(pointer);
        try { action(self); return 0; }
        catch (...) { self.failure = std::current_exception(); return NGHTTP2_ERR_CALLBACK_FAILURE; }
    }
    void rethrow() { if (failure) std::rethrow_exception(failure); }
    static int begin_headers(nghttp2_session*, const nghttp2_frame* frame, void* pointer) {
        return callback(pointer, [&](session& self) {
            if (self.server && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
                if (++self.accepted > self.bounds.max_requests_per_connection ||
                    self.streams.size() >= self.bounds.max_concurrent_streams)
                    throw std::length_error("HTTP/2 connection stream limit");
                auto& state = self.streams[frame->hd.stream_id];
                state.request.version = "HTTP/2";
                state.request.method.clear(); state.request.target.clear();
            }
            auto found = self.streams.find(frame->hd.stream_id);
            if (found == self.streams.end()) throw std::runtime_error("unexpected HTTP/2 stream");
            auto& state = found->second;
            state.trailer_block = frame->headers.cat == NGHTTP2_HCAT_HEADERS &&
                                  (self.server || state.final_status);
            if (!self.server && !state.trailer_block) {
                state.response.headers.clear(); state.response.status = 0;
                state.response.version = "HTTP/2";
                state.response.content_type.clear();
            }
        });
    }
    static int header(nghttp2_session*, const nghttp2_frame* frame,
                      const uint8_t* name, size_t name_length,
                      const uint8_t* value, size_t value_length, uint8_t, void* pointer) {
        return callback(pointer, [&](session& self) {
            auto& state = self.streams.at(frame->hd.stream_id);
            if (self.bounds.max_header_bytes - state.header_bytes < 32)
                throw std::length_error("HTTP/2 incoming header limit");
            state.header_bytes += 32;
            if (name_length > self.bounds.max_header_bytes - std::min(state.header_bytes, self.bounds.max_header_bytes) ||
                value_length > self.bounds.max_header_bytes - state.header_bytes - name_length ||
                ++state.header_fields > self.bounds.max_header_fields)
                throw std::length_error("HTTP/2 incoming header limit");
            state.header_bytes += name_length + value_length;
            std::string key(reinterpret_cast<const char*>(name), name_length);
            std::string text(reinterpret_cast<const char*>(value), value_length);
            if (!valid_value(text) || forbidden(key)) throw std::runtime_error("invalid HTTP/2 header");
            if (state.trailer_block) {
                if (key.starts_with(':') || key == "content-length" || key == "host" || key == "te")
                    throw std::runtime_error("invalid HTTP/2 trailer");
                (self.server ? state.request.trailers : state.response.trailers).push_back({std::move(key), std::move(text)});
            } else if (key == ":method" && self.server) state.request.method = text;
            else if (key == ":path" && self.server) state.request.target = text;
            else if (key == ":authority" && self.server) state.request.headers.push_back({"host", text});
            else if (key == ":status" && !self.server) {
                unsigned status = 0;
                auto parsed = std::from_chars(text.data(), text.data() + text.size(), status);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size() || text.size()!=3 || status<100 || status>599)
                    throw std::runtime_error("invalid HTTP/2 response status");
                state.response.status = status; state.final_status = status >= 200;
            } else if (!key.starts_with(':')) {
                if (!valid_name(key)) throw std::runtime_error("invalid HTTP/2 header name");
                if (!self.server && key == "content-type") state.response.content_type = text;
                (self.server ? state.request.headers : state.response.headers).push_back({std::move(key), std::move(text)});
            }
        });
    }
    static int data(nghttp2_session*, uint8_t, int32_t id, const uint8_t* bytes, size_t size, void* pointer) {
        return callback(pointer, [&](session& self) {
            auto& state = self.streams.at(id);
            if (!self.server && state.response.status == 205 && size != 0)
                throw std::runtime_error("HTTP/2 205 response must not contain a body");
            auto& body = self.server ? state.request.body : state.response.body;
            if (size > self.bounds.max_body_bytes - body.size()) throw std::length_error("HTTP/2 body limit");
            body.append(reinterpret_cast<const char*>(bytes), size);
        });
    }
    static int frame(nghttp2_session*, const nghttp2_frame* frame, void* pointer) {
        return callback(pointer, [&](session& self) {
            if (frame->hd.type == NGHTTP2_GOAWAY) {
                self.goaway = true;
                if (frame->goaway.error_code != NGHTTP2_NO_ERROR)
                    throw std::runtime_error("HTTP/2 peer reported a connection error");
                if (!self.server) for (const auto& [id, state] : self.streams) {
                    (void)state;
                    if (id > frame->goaway.last_stream_id)
                        throw std::runtime_error("HTTP/2 GOAWAY refused an outstanding request");
                }
            }
            if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) &&
                (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
                auto& state = self.streams.at(frame->hd.stream_id);
                state.incoming_complete = true;
                if (self.server) self.ready.push_back(frame->hd.stream_id);
            }
        });
    }
    static int closed(nghttp2_session*, int32_t id, uint32_t error, void* pointer) {
        return callback(pointer, [&](session& self) {
            auto found = self.streams.find(id);
            if (found == self.streams.end()) return;
            if (!self.server) {
                auto& state = found->second;
                if (error != NGHTTP2_NO_ERROR || !state.incoming_complete || !state.final_status)
                    throw std::runtime_error("HTTP/2 stream reset or incomplete response");
                self.responses.at(state.result_index) = std::move(state.response);
                ++self.completed;
            }
            self.streams.erase(found);
        });
    }
    static ssize_t provide(nghttp2_session* native, int32_t id, uint8_t* buffer,
                           size_t length, uint32_t* flags, nghttp2_data_source* source, void* pointer) noexcept {
        auto& self = *static_cast<session*>(pointer);
        try {
            auto& state = *static_cast<stream_state*>(source->ptr);
            const auto count = std::min(length, state.outgoing.size() - state.offset);
            if (count) std::memcpy(buffer, state.outgoing.data()+state.offset, count);
            state.offset += count;
            if (state.offset == state.outgoing.size()) {
                *flags |= NGHTTP2_DATA_FLAG_EOF;
                if (!state.trailers.empty()) {
                    *flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
                    auto headers = wire_headers(state.trailers);
                    check(nghttp2_submit_trailer(native, id, headers.data(), headers.size()));
                }
            }
            return static_cast<ssize_t>(count);
        } catch (...) { self.failure = std::current_exception(); return NGHTTP2_ERR_CALLBACK_FAILURE; }
    }
    void submit(std::string authority, std::vector<http::request> requests, std::string scheme) {
        if (authority.empty() || !valid_value(authority) || authority.find_first_of(" /\t") != std::string::npos)
            throw std::invalid_argument("HTTP/2 requires an authority");
        if (requests.empty() || requests.size() > bounds.max_concurrent_streams || requests.size() > bounds.max_requests_per_connection)
            throw std::invalid_argument("HTTP/2 batch exceeds configured stream limit");
        responses.resize(requests.size());
        for (std::size_t i = 0; i < requests.size(); ++i) {
            auto& request = requests[i];
            if (request.method.empty() || !valid_name(lower(request.method)) || request.method == "CONNECT" ||
                request.target.empty() || (request.target.front() != '/' && request.target != "*") ||
                (request.target == "*" && request.method != "OPTIONS") ||
                !valid_value(request.target) || request.target.find_first_of(" \t") != std::string::npos ||
                request.body.size() > bounds.max_body_bytes)
                throw std::invalid_argument("invalid HTTP/2 request");
            fields headers{{":method",request.method},{":scheme",scheme},{":authority",authority},{":path",request.target},
                           {"content-length",std::to_string(request.body.size())}};
            // Preserve the request content type; it is not generated by request.
            for (const auto& h : request.headers) if (lower(h.name)=="content-type") headers.push_back({"content-type",h.value});
            add_headers(headers, request.headers, bounds);
            auto wire = wire_headers(headers);
            // Reserve the predicted client stream id so the provider's state has
            // a stable address before the first mem_send call.
            const int32_t predicted = static_cast<int32_t>(nghttp2_session_get_next_stream_id(native));
            auto& state = streams[predicted]; state.result_index = i;
            state.request.method = request.method;
            state.outgoing = std::move(request.body);
            add_headers(state.trailers, request.trailers, bounds, true);
            check_combined_headers(headers, state.trailers, bounds);
            nghttp2_data_provider provider{};
            provider.source.ptr = &state;
            provider.read_callback = provide;
            const auto id = nghttp2_submit_request(native,nullptr,wire.data(),wire.size(),
                state.outgoing.empty() && state.trailers.empty() ? nullptr : &provider,nullptr);
            check(id); if (id != predicted) throw std::runtime_error("HTTP/2 stream allocation mismatch");
        }
    }
    void respond(int32_t id, http::response result) {
        auto& state = streams.at(id);
        if (result.status < 200 || result.status > 599 || result.body.size() > bounds.max_body_bytes || !valid_value(result.content_type))
            throw std::invalid_argument("invalid HTTP/2 response");
        const bool no_body = state.request.method == "HEAD" || result.status == 204 || result.status == 205 || result.status == 304;
        fields headers{{":status",std::to_string(result.status)}};
        if (result.status != 204 && result.status != 304)
            headers.push_back({"content-length", std::to_string(result.status == 205 ? 0 : result.body.size())});
        if (!result.content_type.empty()) headers.push_back({"content-type",result.content_type});
        add_headers(headers,result.headers,bounds);
        if (!no_body) { state.outgoing = std::move(result.body); add_headers(state.trailers,result.trailers,bounds,true); }
        check_combined_headers(headers, state.trailers, bounds);
        auto wire = wire_headers(headers);
        nghttp2_data_provider provider{};
        provider.source.ptr = &state;
        provider.read_callback = provide;
        check(nghttp2_submit_response(native,id,wire.data(),wire.size(),state.outgoing.empty() && state.trailers.empty() ? nullptr : &provider));
        state.replied = true;
    }
    template<class Stream> task<void> flush(Stream& stream) {
        for (;;) {
            const uint8_t* data = nullptr;
            const auto size = nghttp2_session_mem_send(native,&data);
            rethrow(); if (size < 0) check(static_cast<int>(size));
            if (!size) co_return;
            co_await stream.write_all(std::string_view(reinterpret_cast<const char*>(data),static_cast<std::size_t>(size)));
        }
    }
    void receive(std::span<const char> input) {
        const auto count = nghttp2_session_mem_recv(native,reinterpret_cast<const uint8_t*>(input.data()),input.size());
        rethrow(); if (count < 0) check(static_cast<int>(count));
        if (static_cast<std::size_t>(count) != input.size()) throw std::runtime_error("incomplete HTTP/2 input consumption");
    }
};
template<class Stream> task<std::vector<http::response>> exchange_impl(Stream transport, std::string authority,
        std::vector<http::request> requests, limits bounds, std::string scheme) {
    session state(false,bounds);
    state.submit(std::move(authority),std::move(requests),std::move(scheme));
    std::array<char,16384> buffer{};
    while (state.completed < state.responses.size()) {
        co_await state.flush(transport);
        if (state.completed == state.responses.size()) break;
        if (!nghttp2_session_want_read(state.native))
            throw std::runtime_error("HTTP/2 peer ended session before responses completed");
        const auto size = co_await transport.read_some(buffer);
        if (!size) throw std::runtime_error("HTTP/2 truncated response");
        state.receive(std::span<const char>(buffer.data(),size));
    }
    check(nghttp2_session_terminate_session(state.native,NGHTTP2_NO_ERROR));
    co_await state.flush(transport);
    co_await transport.close_gracefully();
    co_return std::move(state.responses);
}
template<class Stream> task<void> serve_impl(Stream transport,http::handler handle,limits bounds) {
    session state(true,bounds);
    std::array<char,16384> buffer{};
    for (;;) {
        while (!state.ready.empty()) {
            const auto id = state.ready.front(); state.ready.pop_front();
            auto found = state.streams.find(id);
            if (found == state.streams.end()) continue; // A reset can follow END_STREAM in the same read.
            auto request = std::move(found->second.request);
            found->second.request.method = request.method;
            http::response response;
            if (request.method == "CONNECT") response = {.status=405,.body="CONNECT is not supported\n"};
            else {
                try { response = co_await handle(std::move(request)); }
                catch (const std::system_error& e) { if (e.code()==std::errc::operation_canceled) throw; response={.status=500,.body="Internal Server Error\n"}; }
                catch (...) { response={.status=500,.body="Internal Server Error\n"}; }
            }
            state.respond(id,std::move(response));
        }
        co_await state.flush(transport);
        if ((state.goaway && state.streams.empty()) || !nghttp2_session_want_read(state.native)) break;
        const auto size=co_await transport.read_some(buffer);
        if (!size) break;
        state.receive(std::span<const char>(buffer.data(),size));
    }
    co_await transport.close_gracefully();
}
template<class Stream> task<void> isolated(Stream stream,http::handler handle,limits bounds) {
    try { co_await serve_impl(std::move(stream),std::move(handle),bounds); }
    catch (const std::exception&) { /* One malformed/closed peer must not stop other connections. */ }
}
} // namespace
task<std::vector<http::response>> exchange(tcp::stream transport,std::string authority,std::vector<http::request> requests,limits bounds) {
    return exchange_impl(std::move(transport),std::move(authority),std::move(requests),bounds,"http");
}
task<void> serve_connection(tcp::stream transport,http::handler handle,limits bounds) {
    return serve_impl(std::move(transport),std::move(handle),bounds);
}
task<void> serve(tcp::listener& listener,http::handler handle,limits bounds) {
    try { for (;;) listener.context().spawn(isolated(co_await listener.accept(),handle,bounds)); }
    catch (const std::system_error& error) {
        if (error.code() != std::errc::operation_canceled) throw;
    }
}
#ifdef PACE_HAS_TLS
task<std::vector<http::response>> exchange(tls::stream transport,std::string authority,std::vector<http::request> requests,limits bounds) {
    if (transport.selected_protocol()!="h2") throw std::runtime_error("HTTP/2 requires negotiated ALPN h2");
    return exchange_impl(std::move(transport),std::move(authority),std::move(requests),bounds,"https");
}
task<void> serve_connection(tls::stream transport,http::handler handle,limits bounds) {
    if (transport.selected_protocol()!="h2") throw std::runtime_error("HTTP/2 requires negotiated ALPN h2");
    return serve_impl(std::move(transport),std::move(handle),bounds);
}
namespace {
task<void> secure_connection(tls::stream transport,http::handler handle,limits bounds) {
    try { co_await transport.handshake(); co_await serve_connection(std::move(transport),std::move(handle),bounds); }
    catch (const std::exception&) { }
}
}
task<void> serve(tcp::listener& listener,const tls::context& configuration,http::handler handle,limits bounds) {
    try { for (;;) listener.context().spawn(secure_connection(tls::stream(co_await listener.accept(),configuration),handle,bounds)); }
    catch (const std::system_error& error) {
        if (error.code() != std::errc::operation_canceled) throw;
    }
}
#endif
} // namespace pace::http2
#endif
