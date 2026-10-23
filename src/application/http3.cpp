#include <pace/http3.hpp>
#include <pace/detail/http_dispatch.hpp>
#include <pace/detail/protocol_driver.hpp>
#ifdef PACE_HAS_HTTP3
#include <nghttp3/nghttp3.h>
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <deque>
#include <map>
#include <stdexcept>
#include <utility>

namespace pace::http3 {
namespace {
void check(nghttp3_ssize result) {
    if (result < 0) throw http::protocol_error(std::string("HTTP/3: ") + nghttp3_strerror(static_cast<int>(result)));
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
std::vector<nghttp3_nv> wire_headers(fields& headers) {
    std::vector<nghttp3_nv> result;
    for (auto& h : headers) result.push_back({reinterpret_cast<uint8_t*>(h.name.data()),
        reinterpret_cast<uint8_t*>(h.value.data()), h.name.size(), h.value.size(), NGHTTP3_NV_FLAG_NONE});
    return result;
}
void add_headers(fields& target, const fields& source, limits bounds, bool trailers = false) {
    std::size_t total = 0;
    for (const auto& item : target) {
        if (bounds.max_header_bytes - total < 32) throw std::length_error("HTTP/3 outbound header limit");
        total += 32; // RFC 9114 SETTINGS_MAX_FIELD_SECTION_SIZE accounting.
        if (!valid_value(item.value) || item.name.size() > bounds.max_header_bytes - total ||
            item.value.size() > bounds.max_header_bytes - total - item.name.size())
            throw std::length_error("HTTP/3 outbound header limit");
        total += item.name.size() + item.value.size();
    }
    if (target.size() > bounds.max_header_fields) throw std::length_error("HTTP/3 outbound header limit");
    for (const auto& item : source) {
        auto name = lower(item.name);
        if (!valid_name(name) || !valid_value(item.value) || forbidden(name) ||
            (name == "te" && item.value != "trailers") ||
            (trailers && (name == "content-length" || name == "host" || name == "te")))
            throw std::invalid_argument("invalid HTTP/3 header");
        if (!trailers && (name == "content-length" || name == "host" || name == "content-type")) continue;
        if (bounds.max_header_bytes - total < 32) throw std::length_error("HTTP/3 outbound header limit");
        total += 32;
        if (name.size() > bounds.max_header_bytes - total ||
            item.value.size() > bounds.max_header_bytes - total - name.size() || target.size() >= bounds.max_header_fields)
            throw std::length_error("HTTP/3 outbound header limit");
        total += name.size() + item.value.size();
        target.push_back({std::move(name), item.value});
    }
}
void check_combined_headers(const fields& headers, const fields& trailers, limits bounds) {
    std::size_t count = 0, bytes = 0;
    for (const auto* group : {&headers, &trailers}) for (const auto& field : *group) {
        if (bounds.max_header_bytes - bytes < 32) throw std::length_error("HTTP/3 combined header/trailer limit");
        bytes += 32;
        if (++count > bounds.max_header_fields || field.name.size() > bounds.max_header_bytes - bytes ||
            field.value.size() > bounds.max_header_bytes - bytes - field.name.size())
            throw std::length_error("HTTP/3 combined header/trailer limit");
        bytes += field.name.size() + field.value.size();
    }
}
struct stream_state {
    http::request request;
    http::response response;
    std::string outgoing;
    fields trailers;
    std::size_t header_bytes = 0, header_fields = 0, result_index = 0;
    bool incoming_complete = false, trailer_block = false, final_status = false, supplied = false;
};
struct transport_state {
    quic::stream stream;
    std::uint64_t written = 0, acknowledged = 0;
    bool read_fin = false, write_fin = false, blocked = false;
    explicit transport_state(quic::stream value) : stream(std::move(value)) {}
    bool readable() const { return !stream.is_unidirectional() || !stream.is_local(); }
    bool writable() const { return !stream.is_unidirectional() || stream.is_local(); }
};
struct session {
    nghttp3_conn* native = nullptr;
    limits bounds;
    bool server;
    std::map<int64_t, stream_state> streams;
    std::map<int64_t, transport_state> transports;
    std::deque<int64_t> ready;
    std::vector<http::response> responses;
    std::exception_ptr failure;
    std::size_t accepted = 0, completed = 0;

    explicit session(bool is_server, limits value) : bounds(value), server(is_server) {
        if (!bounds.max_header_bytes || !bounds.max_header_fields || !bounds.max_concurrent_streams ||
            bounds.max_concurrent_streams > 10000 || !bounds.max_requests_per_connection ||
            !bounds.max_concurrent_handlers || bounds.max_concurrent_handlers > 10000)
            throw std::invalid_argument("invalid HTTP/3 limits");
        nghttp3_callbacks callbacks{};
        callbacks.begin_headers = begin_headers;
        callbacks.recv_header = header;
        callbacks.begin_trailers = begin_trailers;
        callbacks.recv_trailer = header;
        callbacks.recv_data = data;
        callbacks.end_stream = end_stream;
        callbacks.stop_sending = reset_stream;
        callbacks.reset_stream = reset_stream;
        callbacks.shutdown = shutdown;
        callbacks.rand = [](uint8_t* bytes, size_t length) {
            // nghttp3's random callback cannot report failures.
            if (RAND_bytes(bytes, static_cast<int>(length)) != 1) std::terminate();
        };
        nghttp3_settings settings;
        nghttp3_settings_default(&settings);
        settings.max_field_section_size = bounds.max_header_bytes;
        // Dynamic QPACK decoding is deliberately disabled. Static-table and
        // literal QPACK remain interoperable and avoid blocked-header buffering.
        settings.qpack_max_dtable_capacity = 0;
        settings.qpack_blocked_streams = 0;
        check(server ? nghttp3_conn_server_new(&native, &callbacks, &settings, nullptr, this)
                     : nghttp3_conn_client_new(&native, &callbacks, &settings, nullptr, this));
        nghttp3_conn_set_max_concurrent_streams(native, bounds.max_concurrent_streams);
        if (server) nghttp3_conn_set_max_client_streams_bidi(native, bounds.max_requests_per_connection);
    }
    ~session() { nghttp3_conn_del(native); }
    session(const session&) = delete;
    template<class F> static int callback(void* pointer, F&& action) noexcept {
        auto& self = *static_cast<session*>(pointer);
        try { action(self); return 0; }
        catch (...) { self.failure = std::current_exception(); return NGHTTP3_ERR_CALLBACK_FAILURE; }
    }
    void checked(nghttp3_ssize result) { if (failure) std::rethrow_exception(failure); check(result); }
    static int begin_headers(nghttp3_conn*, int64_t id, void* pointer, void*) {
        return callback(pointer, [&](session& self) {
            if (self.server) {
                if (++self.accepted > self.bounds.max_requests_per_connection ||
                    self.streams.size() >= self.bounds.max_concurrent_streams)
                    throw std::length_error("HTTP/3 connection stream limit");
                self.streams[id].request.version = "HTTP/3";
            }
            auto& state = self.streams.at(id);
            state.trailer_block = false;
            if (!self.server) {
                state.response.headers.clear(); state.response.status = 0;
                state.response.version = "HTTP/3"; state.response.content_type.clear();
            }
        });
    }
    static int begin_trailers(nghttp3_conn*, int64_t id, void* pointer, void*) {
        return callback(pointer, [&](session& self) { self.streams.at(id).trailer_block = true; });
    }
    static int header(nghttp3_conn*, int64_t id, int32_t, nghttp3_rcbuf* name, nghttp3_rcbuf* value,
                      uint8_t, void* pointer, void*) {
        return callback(pointer, [&](session& self) {
            auto& state = self.streams.at(id);
            const auto key_bytes = nghttp3_rcbuf_get_buf(name), text_bytes = nghttp3_rcbuf_get_buf(value);
            if (self.bounds.max_header_bytes - state.header_bytes < 32)
                throw std::length_error("HTTP/3 incoming header limit");
            state.header_bytes += 32;
            if (key_bytes.len > self.bounds.max_header_bytes - state.header_bytes ||
                text_bytes.len > self.bounds.max_header_bytes - state.header_bytes - key_bytes.len ||
                ++state.header_fields > self.bounds.max_header_fields)
                throw std::length_error("HTTP/3 incoming header limit");
            state.header_bytes += key_bytes.len + text_bytes.len;
            std::string key(reinterpret_cast<char*>(key_bytes.base), key_bytes.len);
            std::string text(reinterpret_cast<char*>(text_bytes.base), text_bytes.len);
            if (!valid_value(text) || forbidden(key)) throw http::protocol_error("invalid HTTP/3 header");
            if (state.trailer_block) {
                if (!valid_name(key) || key == "content-length" || key == "host" || key == "te")
                    throw http::protocol_error("invalid HTTP/3 trailer");
                (self.server ? state.request.trailers : state.response.trailers).push_back({std::move(key), std::move(text)});
            } else if (key == ":method" && self.server) state.request.method = text;
            else if (key == ":path" && self.server) state.request.target = text;
            else if (key == ":authority" && self.server) state.request.headers.push_back({"host", text});
            else if (key == ":status" && !self.server) {
                unsigned status = 0;
                auto parsed = std::from_chars(text.data(), text.data() + text.size(), status);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || text.size() != 3 || status < 100 || status > 599)
                    throw http::protocol_error("invalid HTTP/3 status");
                state.response.status = status; state.final_status = status >= 200;
            } else if (!key.starts_with(':')) {
                if (!valid_name(key)) throw http::protocol_error("invalid HTTP/3 header name");
                if (!self.server && key == "content-type") state.response.content_type = text;
                (self.server ? state.request.headers : state.response.headers).push_back({std::move(key), std::move(text)});
            }
        });
    }
    static int data(nghttp3_conn*, int64_t id, const uint8_t* bytes, size_t size, void* pointer, void*) {
        return callback(pointer, [&](session& self) {
            auto& state = self.streams.at(id);
            if (!self.server && state.response.status == 205 && size)
                throw http::protocol_error("HTTP/3 205 response must not contain a body");
            auto& body = self.server ? state.request.body : state.response.body;
            if (size > self.bounds.max_body_bytes - body.size()) throw std::length_error("HTTP/3 body limit");
            body.append(reinterpret_cast<const char*>(bytes), size);
        });
    }
    static int end_stream(nghttp3_conn*, int64_t id, void* pointer, void*) {
        return callback(pointer, [&](session& self) {
            auto& state = self.streams.at(id);
            if (state.incoming_complete) throw http::protocol_error("duplicate HTTP/3 stream completion");
            state.incoming_complete = true;
            if (self.server) self.ready.push_back(id);
            else {
                if (!state.final_status) throw http::protocol_error("HTTP/3 response lacks final status");
                self.responses.at(state.result_index) = std::move(state.response);
                ++self.completed;
            }
        });
    }
    static int reset_stream(nghttp3_conn*, int64_t id, uint64_t code, void* pointer, void*) {
        return callback(pointer, [&](session& self) {
            if (auto it = self.transports.find(id); it != self.transports.end()) it->second.stream.reset(code);
            // This bounded batch API reports a refused/malformed stream as a
            // failed connection; it never returns a fabricated partial response.
            throw http::protocol_error("HTTP/3 stream rejected or malformed");
        });
    }
    static int shutdown(nghttp3_conn*, int64_t id, void* pointer) {
        return callback(pointer, [&](session& self) {
            if (!self.server) for (const auto& [stream_id, state] : self.streams)
                if (stream_id >= id && !state.incoming_complete)
                    throw http::protocol_error("HTTP/3 GOAWAY refused an outstanding request");
        });
    }
    static nghttp3_ssize provide(nghttp3_conn* native, int64_t id, nghttp3_vec* vec, size_t count,
                                 uint32_t* flags, void* pointer, void*) noexcept {
        auto& self = *static_cast<session*>(pointer);
        try {
            auto& state = self.streams.at(id);
            if (!count) return 0;
            *flags |= NGHTTP3_DATA_FLAG_EOF;
            if (state.supplied) return 0;
            state.supplied = true;
            if (!state.trailers.empty()) {
                *flags |= NGHTTP3_DATA_FLAG_NO_END_STREAM;
                auto trailers = wire_headers(state.trailers);
                check(nghttp3_conn_submit_trailers(native, id, trailers.data(), trailers.size()));
            }
            if (state.outgoing.empty()) return 0;
            vec[0] = {reinterpret_cast<uint8_t*>(state.outgoing.data()), state.outgoing.size()};
            return 1;
        } catch (...) { self.failure = std::current_exception(); return NGHTTP3_ERR_CALLBACK_FAILURE; }
    }
    task<void> initialize(quic::connection& connection) {
        std::array<int64_t, 3> ids{};
        for (auto& id : ids) {
            auto stream = co_await connection.open_stream(quic::stream_type::unidirectional);
            id = static_cast<int64_t>(stream.id());
            transports.try_emplace(id, std::move(stream));
        }
        checked(nghttp3_conn_bind_control_stream(native, ids[0]));
        checked(nghttp3_conn_bind_qpack_streams(native, ids[1], ids[2]));
    }
    void validate_batch(const std::string& authority, const std::vector<http::request>& requests) {
        if (authority.empty() || !valid_value(authority) || authority.find_first_of(" /\t") != std::string::npos)
            throw std::invalid_argument("HTTP/3 requires an authority");
        if (requests.empty() || requests.size() > bounds.max_concurrent_streams || requests.size() > bounds.max_requests_per_connection)
            throw std::invalid_argument("HTTP/3 batch exceeds configured stream limit");
        for (const auto& request : requests) (void)request_headers(authority, request);
    }
    fields request_headers(const std::string& authority, const http::request& request) {
        if (request.method.empty() || !valid_name(lower(request.method)) || request.method == "CONNECT" ||
            request.target.empty() || (request.target.front() != '/' && request.target != "*") ||
            (request.target == "*" && request.method != "OPTIONS") || !valid_value(request.target) ||
            request.target.find_first_of(" \t") != std::string::npos || request.body.size() > bounds.max_body_bytes)
            throw std::invalid_argument("invalid HTTP/3 request");
        fields headers{{":method", request.method}, {":scheme", "https"}, {":authority", authority}, {":path", request.target},
                       {"content-length", std::to_string(request.body.size())}};
        for (const auto& h : request.headers) if (lower(h.name) == "content-type") headers.push_back({"content-type", h.value});
        add_headers(headers, request.headers, bounds);
        fields trailers; add_headers(trailers, request.trailers, bounds, true);
        check_combined_headers(headers, trailers, bounds);
        return headers;
    }
    bool submit(quic::connection& connection, const std::string& authority, std::vector<http::request>& requests, std::size_t& next) {
        bool progress = false;
        for (; next < requests.size(); ++next) {
            auto stream = connection.try_open_stream();
            if (!stream) break;
            progress = true;
            const auto i = next;
            auto headers = request_headers(authority, requests[i]);
            const auto id = static_cast<int64_t>(stream->id());
            transports.try_emplace(id, std::move(*stream));
            auto& state = streams[id]; state.result_index = i; state.request.method = requests[i].method;
            state.outgoing = std::move(requests[i].body);
            add_headers(state.trailers, requests[i].trailers, bounds, true);
            auto wire = wire_headers(headers);
            const nghttp3_data_reader reader{provide};
            checked(nghttp3_conn_submit_request(native, id, wire.data(), wire.size(),
                state.outgoing.empty() && state.trailers.empty() ? nullptr : &reader, nullptr));
        }
        return progress;
    }
    void respond(int64_t id, http::response result) {
        auto& state = streams.at(id);
        if (result.status < 200 || result.status > 599 || result.body.size() > bounds.max_body_bytes || !valid_value(result.content_type))
            throw std::invalid_argument("invalid HTTP/3 response");
        const bool no_body = state.request.method == "HEAD" || result.status == 204 || result.status == 205 || result.status == 304;
        fields headers{{":status", std::to_string(result.status)}};
        if (result.status != 204 && result.status != 304)
            headers.push_back({"content-length", std::to_string(result.status == 205 ? 0 : result.body.size())});
        if (!result.content_type.empty()) headers.push_back({"content-type", result.content_type});
        add_headers(headers, result.headers, bounds);
        if (!no_body) { state.outgoing = std::move(result.body); add_headers(state.trailers, result.trailers, bounds, true); }
        check_combined_headers(headers, state.trailers, bounds);
        auto wire = wire_headers(headers);
        const nghttp3_data_reader reader{provide};
        checked(nghttp3_conn_submit_response(native, id, wire.data(), wire.size(),
            state.outgoing.empty() && state.trailers.empty() ? nullptr : &reader));
    }
    bool receive(quic::connection& connection) {
        bool progress = false;
        while (auto stream = connection.try_accept_stream()) {
            if ((!server && !stream->is_unidirectional()) ||
                transports.size() >= bounds.max_concurrent_streams + 16)
                throw http::protocol_error("HTTP/3 unexpected or excessive peer streams");
            const auto id = static_cast<int64_t>(stream->id());
            transports.try_emplace(id, std::move(*stream)); progress = true;
        }
        std::array<char, 16384> bytes{};
        for (auto& [id, wire] : transports) {
            if (!wire.readable() || wire.read_fin) continue;
            // A busy stream must not starve control/QPACK or other requests.
            for (unsigned batch = 0; batch < 16; ++batch) {
                const auto size = wire.stream.try_read(bytes);
                if (!size) break;
                const auto now = static_cast<nghttp3_tstamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                checked(nghttp3_conn_read_stream2(native, id, reinterpret_cast<uint8_t*>(bytes.data()), *size, !*size, now));
                // OpenSSL extends QUIC receive credit when try_read consumes the
                // plaintext. QPACK dynamic decoding is disabled above.
                progress = true;
                if (!*size) { wire.read_fin = true; break; }
            }
        }
        return progress;
    }
    bool flush() {
        bool progress = false;
        for (auto& [id, wire] : transports) if (wire.blocked) {
            checked(nghttp3_conn_unblock_stream(native, id)); wire.blocked = false;
        }
        for (unsigned batch = 0; batch < 256; ++batch) {
            std::array<nghttp3_vec, 16> vectors{};
            int64_t id = -1; int fin = 0;
            const auto count = nghttp3_conn_writev_stream(native, &id, &fin, vectors.data(), vectors.size());
            checked(count); if (id == -1) break;
            auto& wire = transports.at(id);
            bool all = true;
            std::size_t written = 0;
            for (nghttp3_ssize i = 0; i < count; ++i) {
                if (!vectors[i].len) continue;
                const auto n = wire.stream.try_write({reinterpret_cast<char*>(vectors[i].base), vectors[i].len});
                written += n;
                if (n < vectors[i].len) { all = false; break; }
            }
            wire.written += written;
            checked(nghttp3_conn_add_write_offset(native, id, written));
            progress = progress || written != 0;
            if (all && fin) { wire.stream.finish_write(); wire.write_fin = true; progress = true; }
            if (!all) { nghttp3_conn_block_stream(native, id); wire.blocked = true; }
        }
        return progress;
    }
    void acknowledge() {
        for (auto it = transports.begin(); it != transports.end();) {
            auto& [id, wire] = *it;
            if (!wire.writable()) { ++it; continue; }
            const auto pending = wire.stream.pending_write_bytes();
            // OpenSSL retains its send buffer through the contiguous ACK
            // frontier. nghttp3 retains body references until this frontier.
            const auto acknowledged = wire.written - pending;
            if (acknowledged > wire.acknowledged) {
                checked(nghttp3_conn_update_ack_offset(native, id, acknowledged));
                wire.acknowledged = acknowledged;
            }
            if (!wire.stream.is_unidirectional() && wire.read_fin && wire.write_fin && !pending) {
                checked(nghttp3_conn_close_stream(native, id, NGHTTP3_H3_NO_ERROR));
                streams.erase(id); it = transports.erase(it);
            } else ++it;
        }
    }
};
void require_h3(const quic::connection& transport) {
    if (transport.selected_protocol() != "h3") throw std::invalid_argument("HTTP/3 requires negotiated ALPN h3");
}
task<void> isolated(quic::connection connection, http::handler handle, limits bounds) {
    try { co_await serve_connection(std::move(connection), std::move(handle), bounds); }
    catch (const std::exception&) { /* A malformed peer does not stop the listener. */ }
}
} // namespace

task<std::vector<http::response>> exchange(quic::connection transport, std::string authority,
                                          std::vector<http::request> requests, limits bounds) {
    session state(false, bounds);
    state.validate_batch(authority, requests);
    require_h3(transport);
    try {
        co_await state.initialize(transport);
        state.responses.resize(requests.size());
        std::size_t next = 0;
        while (state.completed < state.responses.size()) {
            bool progress = state.submit(transport, authority, requests, next);
            progress = state.receive(transport) || progress;
            progress = state.flush() || progress;
            state.acknowledge();
            if (state.completed == state.responses.size()) break;
            if (progress) co_await transport.context().yield();
            else co_await transport.wait_events();
        }
        transport.close(NGHTTP3_H3_NO_ERROR);
        co_return std::move(state.responses);
    } catch (...) { transport.close(NGHTTP3_H3_INTERNAL_ERROR); throw; }
}
task<void> serve_connection(quic::connection transport, http::handler handle, limits bounds) {
    require_h3(transport);
    session state(true, bounds);
    auto dispatch = std::make_shared<detail::http_dispatch>(transport.context(), std::move(handle));
    dispatch->wake = [&] { detail::quic_driver::wake(transport); };
    detail::close_http_dispatch close{dispatch};
    try {
        co_await state.initialize(transport);
        for (;;) {
            bool progress = state.receive(transport);
            if (dispatch->failure) std::rethrow_exception(dispatch->failure);
            while (!dispatch->completed.empty()) {
                auto result = std::move(dispatch->completed.front()); dispatch->completed.pop_front();
                if (result.error) std::rethrow_exception(result.error);
                if (state.streams.contains(result.id)) state.respond(result.id, std::move(result.response));
                progress = true;
            }
            while (!state.ready.empty() && dispatch->active < std::min(bounds.max_concurrent_handlers, bounds.max_concurrent_streams)) {
                const auto id = state.ready.front(); state.ready.pop_front();
                auto found = state.streams.find(id);
                if (found == state.streams.end()) continue;
                auto request = std::move(found->second.request); found->second.request.method = request.method;
                dispatch->start(id, std::move(request)); progress = true;
            }
            progress = state.flush() || progress;
            state.acknowledge();
            if (progress) co_await transport.context().yield();
            else co_await transport.wait_events();
        }
    } catch (const std::system_error& error) {
        transport.close(NGHTTP3_H3_NO_ERROR);
        if (error.code() != std::errc::connection_aborted) throw;
    } catch (...) { transport.close(NGHTTP3_H3_INTERNAL_ERROR); throw; }
}
task<void> serve(quic::listener& listener, http::handler handle, limits bounds) {
    for (;;) {
        try { listener.context().spawn(isolated(co_await listener.accept(), handle, bounds)); }
        catch (const std::system_error& error) {
            if (error.code() == std::errc::operation_canceled || error.code() == std::errc::bad_file_descriptor) co_return;
            // accept includes the TLS handshake. A failed peer must not stop
            // the listener, while an actual engine failure must propagate.
            if (listener.is_open() && (error.code() == std::errc::protocol_error ||
                error.code() == std::errc::connection_aborted || error.code() == std::errc::timed_out)) continue;
            throw;
        }
    }
}
} // namespace pace::http3
#endif
