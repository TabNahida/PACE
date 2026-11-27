#include <pace/quic/connection.hpp>
#ifdef PACE_HAS_QUIC
#include "net/socket_address.hpp"
#include "quic/driver.hpp"
#include "runtime/reactor_signal.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <unordered_map>
#include <utility>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

namespace pace::quic
{
namespace
{
using clock = std::chrono::steady_clock;
using address = net::socket_address;
constexpr std::size_t stream_window = std::size_t{256} * 1024, connection_window = std::size_t{4} * 1024 * 1024;
constexpr std::size_t cid_size = 18, packet_size = 1200, batch_size = 64;

ngtcp2_tstamp timestamp()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
}

[[noreturn]] void fail(const char* where)
{
    std::string message(where);
    while (auto code = ERR_get_error())
    {
        char buffer[256];
        ERR_error_string_n(code, buffer, sizeof(buffer));
        message += ": ";
        message += buffer;
    }
    throw std::system_error(std::make_error_code(std::errc::protocol_error), message);
}

void required(int result, const char* where)
{
    if (result != 1)
    {
        fail(where);
    }
}

void affinity(io_context& io)
{
    if (io.stopped())
    {
        throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    }
    if (io_context::current() != &io)
    {
        throw std::logic_error("QUIC operation must run on its owning context");
    }
}

struct guard
{
    bool& flag;

    explicit guard(bool& value)
        : flag(value)
    {
        if (flag)
        {
            throw std::logic_error("overlapping QUIC operation");
        }
        flag = true;
    }

    guard(const guard&) = delete;
    guard& operator=(const guard&) = delete;

    ~guard()
    {
        flag = false;
    }
};

clock::time_point deadline_after(std::chrono::milliseconds timeout)
{
    const auto now = clock::now();
    if (timeout.count() <= 0)
    {
        return now;
    }
    if (timeout >= std::chrono::duration_cast<std::chrono::milliseconds>(clock::time_point::max() - now))
    {
        return clock::time_point::max();
    }
    return now + timeout;
}

std::string cid_key(const uint8_t* data, std::size_t size)
{
    return {reinterpret_cast<const char*>(data), size};
}

ngtcp2_cid random_cid()
{
    ngtcp2_cid result{};
    result.datalen = cid_size;
    required(RAND_bytes(result.data, cid_size), "QUIC connection ID");
    return result;
}
} // namespace

struct configuration
{
    SSL_CTX* ssl = nullptr;
    role mode;
    std::vector<unsigned char> alpn;

    explicit configuration(role value)
        : mode(value)
    {
        if (mode != role::client && mode != role::server)
        {
            throw std::invalid_argument("invalid QUIC context role");
        }
        ssl = SSL_CTX_new(TLS_method());
        if (!ssl)
        {
            fail("QUIC TLS context");
        }
        SSL_CTX_set_min_proto_version(ssl, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ssl, TLS1_3_VERSION);
        SSL_CTX_set_default_passwd_cb(ssl, [](char*, int, int, void*) { return 0; });
        if (mode == role::client)
        {
            SSL_CTX_set_verify(ssl, SSL_VERIFY_PEER, nullptr);
            SSL_CTX_set_default_verify_paths(ssl);
        }
        else
        {
            SSL_CTX_set_alpn_select_cb(
                ssl,
                [](SSL*,
                   const unsigned char** out,
                   unsigned char* size,
                   const unsigned char* in,
                   unsigned int length,
                   void* arg)
                {
                    const auto& protocols = static_cast<configuration*>(arg)->alpn;
                    unsigned char* selected = nullptr;
                    if (SSL_select_next_proto(
                            &selected,
                            size,
                            protocols.data(),
                            static_cast<unsigned>(protocols.size()),
                            in,
                            length
                        )
                        != OPENSSL_NPN_NEGOTIATED)
                    {
                        return SSL_TLSEXT_ERR_ALERT_FATAL;
                    }
                    *out = selected;
                    return SSL_TLSEXT_ERR_OK;
                },
                this
            );
        }
    }

    configuration(const configuration&) = delete;
    configuration& operator=(const configuration&) = delete;

    ~configuration()
    {
        SSL_CTX_free(ssl);
    }
};

namespace
{
struct stream_buffer
{
    struct chunk
    {
        std::string bytes;
        uint64_t end;
    };

    int64_t id;
    std::deque<std::string> received;
    std::size_t read_offset = 0;
    std::deque<chunk> sending;
    uint64_t total = 0, submitted = 0, acknowledged = 0;
    bool fin = false, finish = false, fin_sent = false, reset = false, closed = false;
    std::size_t unread = 0;
    bool retained = false, queued = false;

    explicit stream_buffer(int64_t value)
        : id(value)
    {
    }
};
} // namespace

struct connection_state;

// Socket ownership and readiness belong to PACE. The routing table only borrows
// connections; accepted connections keep the socket alive, never the reverse.
struct engine : std::enable_shared_from_this<engine>
{
    io_context& io;
    udp::socket socket;
    std::shared_ptr<configuration> config;
    reactor_signal events;
    // Reactor-affine borrowed pointers: state destruction removes every CID,
    // and packet processing never suspends or resumes application code inline.
    std::unordered_map<std::string, connection_state*> routes;
    std::deque<std::shared_ptr<connection_state>> pending;
    std::deque<std::weak_ptr<connection_state>> blocked;
    std::array<uint8_t, 32> retry_secret{};
    bool open = true, started = false, accepting = false, writing = false;
    std::exception_ptr failure;

    engine(io_context& value, udp::endpoint local, std::shared_ptr<configuration> cfg)
        : io(value)
        , socket(value, std::move(local))
        , config(std::move(cfg))
        , events(value)
    {
        if (config->mode == role::server)
        {
            required(RAND_bytes(retry_secret.data(), static_cast<int>(retry_secret.size())), "QUIC Retry secret");
        }
    }

    ~engine()
    {
        close();
    }

    void check()
    {
        affinity(io);
        if (failure)
        {
            std::rethrow_exception(failure);
        }
        if (!open)
        {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
    }

    void close() noexcept;
    void start();
    void receive();
    void retry(const ngtcp2_pkt_hd& initial, const address& peer);
    void wait_writable(const std::shared_ptr<connection_state>& connection);
    static task<void> read_pump(std::weak_ptr<engine> weak);
    static task<void> write_pump(std::weak_ptr<engine> weak);
};

struct connection_state : std::enable_shared_from_this<connection_state>
{
    std::shared_ptr<engine> owner;
    reactor_signal events;
    ngtcp2_conn* conn = nullptr;
    ngtcp2_crypto_ossl_ctx* crypto = nullptr;
    ngtcp2_crypto_conn_ref reference{};
    SSL* ssl = nullptr;
    address local, remote;
    std::vector<std::string> cids;
    std::map<int64_t, std::unique_ptr<stream_buffer>> streams;
    std::deque<int64_t> incoming;
    uint64_t pending_bytes = 0;
    int64_t last_stream = -1;
    io_context::timer_handle timer;
    ngtcp2_tstamp timer_expiry = UINT64_MAX;
    std::array<uint8_t, packet_size> packet{};
    std::size_t packet_length = 0;
    address packet_remote;
    bool open = true, connected = false, connecting = false, accepting = false, opening = false, queued = false,
         write_blocked = false;
    std::exception_ptr failure;

    explicit connection_state(std::shared_ptr<engine> transport)
        : owner(std::move(transport))
        , events(owner->io)
    {
    }

    ~connection_state()
    {
        timer.cancel();
        // ngtcp2's TLS callbacks borrow this connection through SSL app_data.
        if (ssl)
        {
            SSL_set_app_data(ssl, nullptr);
            SSL_free(ssl);
        }
        ngtcp2_conn_del(conn);
        ngtcp2_crypto_ossl_ctx_del(crypto);
        for (const auto& cid : cids)
        {
            owner->routes.erase(cid);
        }
    }

    bool server() const
    {
        return owner->config->mode == role::server;
    }

    void check(bool handshake = true)
    {
        owner->check();
        if (failure)
        {
            std::rethrow_exception(failure);
        }
        if (!open)
        {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
        if (handshake && !connected)
        {
            throw std::logic_error("QUIC handshake incomplete");
        }
    }

    void stop()
    {
        open = false;
        timer.cancel();
        events.notify();
    }

    void error(int code);
    void init(
        const address& peer,
        const ngtcp2_pkt_hd* initial,
        const std::string& name = {},
        const ngtcp2_cid* original = nullptr
    );

    void register_cid(const ngtcp2_cid& cid)
    {
        auto key = cid_key(cid.data, cid.datalen);
        cids.push_back(key);
        owner->routes.emplace(std::move(key), this);
    }

    stream_buffer& get_stream(int64_t id, bool remote_stream = false)
    {
        auto [it, inserted] = streams.try_emplace(id);
        if (inserted)
        {
            it->second = std::make_unique<stream_buffer>(id);
            if (remote_stream)
            {
                it->second->queued = true;
                incoming.push_back(id);
            }
        }
        return *it->second;
    }

    void collect()
    {
        for (auto it = streams.begin(); it != streams.end();)
        {
            if (it->second->closed && !it->second->retained && !it->second->queued)
            {
                if (conn && open)
                {
                    ngtcp2_conn_extend_max_offset(conn, it->second->unread);
                    if ((it->first & 1) != server())
                    {
                        if (it->first & 2)
                        {
                            ngtcp2_conn_extend_max_streams_uni(conn, 1);
                        }
                        else
                        {
                            ngtcp2_conn_extend_max_streams_bidi(conn, 1);
                        }
                    }
                }
                it = streams.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void schedule();
    void flush();
    void refresh();
    bool transmit();

    void close_packet(const ngtcp2_ccerr& error) noexcept
    {
        if (!conn || !owner->open)
        {
            return;
        }
        ngtcp2_path_storage path{};
        ngtcp2_path_storage_zero(&path);
        std::array<uint8_t, packet_size> data{};
        const auto length = ngtcp2_conn_write_connection_close(
            conn,
            &path.path,
            nullptr,
            data.data(),
            data.size(),
            &error,
            timestamp()
        );
        if (length > 0)
        {
            ::sendto(
                owner->socket.native_handle(),
                data.data(),
                length,
                0,
                path.path.remote.addr,
                path.path.remote.addrlen
            );
        }
    }

    template <class F>
    static int callback(void* data, F&& work) noexcept
    {
        auto& self = *static_cast<connection_state*>(data);
        try
        {
            work(self);
            return 0;
        }
        catch (...)
        {
            self.failure = std::current_exception();
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
    }

    static ngtcp2_callbacks callbacks();
};

struct stream_state
{
    std::shared_ptr<connection_state> owner;
    stream_buffer& buffer;
    bool reading = false, writing = false;

    stream_state(std::shared_ptr<connection_state> connection, stream_buffer& value)
        : owner(std::move(connection))
        , buffer(value)
    {
        buffer.retained = true;
    }

    stream_state(const stream_state&) = delete;
    stream_state& operator=(const stream_state&) = delete;

    ~stream_state()
    {
        buffer.retained = false;
        // Dropping a handle must not leave an unread stream consuming the
        // connection window forever. ngtcp2 retains send buffers until closure.
        if (!buffer.closed && owner->open && owner->conn && !owner->owner->io.stopped())
        {
            const bool local = (buffer.id & 1) == owner->server();
            if (!(buffer.id & 2) || local)
            {
                if (!buffer.finish)
                {
                    ngtcp2_conn_shutdown_stream_write(owner->conn, 0, buffer.id, 0);
                }
            }
            if (!(buffer.id & 2) || !local)
            {
                if (!buffer.fin || buffer.unread)
                {
                    ngtcp2_conn_shutdown_stream_read(owner->conn, 0, buffer.id, 0);
                }
            }
            try
            {
                owner->schedule();
            }
            catch (...)
            {
                owner->failure = std::current_exception();
                owner->stop();
            }
        }
        owner->collect();
    }

    void check()
    {
        owner->check();
        if (buffer.reset)
        {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
    }
};

ngtcp2_callbacks connection_state::callbacks()
{
    ngtcp2_callbacks cb{};
    cb.client_initial = ngtcp2_crypto_client_initial_cb;
    cb.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
    cb.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    cb.encrypt = ngtcp2_crypto_encrypt_cb;
    cb.decrypt = ngtcp2_crypto_decrypt_cb;
    cb.hp_mask = ngtcp2_crypto_hp_mask_cb;
    cb.recv_retry = ngtcp2_crypto_recv_retry_cb;
    cb.update_key = ngtcp2_crypto_update_key_cb;
    cb.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    cb.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    cb.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    cb.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
    cb.rand = [](uint8_t* dest, size_t size, const ngtcp2_rand_ctx*)
    {
        // This callback has no error return. Fail closed on RNG failure.
        if (RAND_bytes(dest, static_cast<int>(size)) != 1)
        {
            std::terminate();
        }
    };
    cb.handshake_completed = [](ngtcp2_conn*, void* data)
    { return callback(data, [](auto& self) { self.connected = true; }); };
    cb.get_new_connection_id2 =
        [](ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token, size_t size, void* data)
    {
        return callback(
            data,
            [&](auto& self)
            {
                cid->datalen = size;
                required(RAND_bytes(cid->data, static_cast<int>(size)), "QUIC CID");
                required(RAND_bytes(token->data, sizeof(token->data)), "QUIC reset token");
                self.register_cid(*cid);
            }
        );
    };
    cb.remove_connection_id = [](ngtcp2_conn*, const ngtcp2_cid* cid, void* data)
    {
        return callback(
            data,
            [&](auto& self)
            {
                auto key = cid_key(cid->data, cid->datalen);
                self.owner->routes.erase(key);
                std::erase(self.cids, key);
            }
        );
    };
    cb.stream_open = [](ngtcp2_conn*, int64_t id, void* data)
    { return callback(data, [&](auto& self) { self.get_stream(id, true); }); };
    cb.recv_stream_data =
        [](ngtcp2_conn*, uint32_t flags, int64_t id, uint64_t, const uint8_t* bytes, size_t size, void* data, void*)
    {
        return callback(
            data,
            [&](auto& self)
            {
                auto& stream = self.get_stream(id, (id & 1) != self.server());
                if (size)
                {
                    stream.received.emplace_back(reinterpret_cast<const char*>(bytes), size);
                    stream.unread += size;
                }
                if (flags & NGTCP2_STREAM_DATA_FLAG_FIN)
                {
                    stream.fin = true;
                }
            }
        );
    };
    cb.acked_stream_data_offset = [](ngtcp2_conn*, int64_t id, uint64_t offset, uint64_t size, void* data, void*)
    {
        return callback(
            data,
            [&](auto& self)
            {
                auto& stream = self.get_stream(id);
                const auto end = offset + size;
                self.pending_bytes -= end - stream.acknowledged;
                stream.acknowledged = end;
                while (!stream.sending.empty() && stream.sending.front().end <= end)
                {
                    stream.sending.pop_front();
                }
            }
        );
    };
    cb.stream_reset = [](ngtcp2_conn*, int64_t id, uint64_t, uint64_t, void* data, void*)
    { return callback(data, [&](auto& self) { self.get_stream(id, (id & 1) != self.server()).reset = true; }); };
    cb.stream_close2 = [](ngtcp2_conn*, uint32_t flags, int64_t id, uint64_t, uint64_t, void* data, void*)
    {
        return callback(
            data,
            [&](auto& self)
            {
                auto& stream = self.get_stream(id);
                stream.closed = true;
                if (flags)
                {
                    stream.reset = true;
                }
                self.pending_bytes -= stream.total - stream.acknowledged;
                stream.acknowledged = stream.total;
                stream.sending.clear();
            }
        );
    };
    return cb;
}

void connection_state::init(
    const address& peer,
    const ngtcp2_pkt_hd* initial,
    const std::string& name,
    const ngtcp2_cid* original
)
{
    remote = peer;
    const auto endpoint = owner->socket.local_endpoint();
    local = net::parse_socket_address(endpoint.address, endpoint.port, endpoint.family, endpoint.scope_id);
    auto scid = random_cid();
    auto dcid = initial ? initial->scid : random_cid();
    ngtcp2_path path{{local.data(), local.length}, {remote.data(), remote.length}, nullptr};
    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = timestamp();
    settings.max_tx_udp_payload_size = packet_size;
    settings.no_tx_udp_payload_size_shaping = 1;
    settings.no_pmtud = 1;
    // Bound incomplete handshakes even when the application is not accepting.
    settings.handshake_timeout = initial ? 10 * NGTCP2_SECONDS : UINT64_MAX;
    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local = params.initial_max_stream_data_bidi_remote =
        params.initial_max_stream_data_uni = stream_window;
    params.initial_max_data = connection_window;
    params.initial_max_streams_bidi = params.initial_max_streams_uni = 64;
    params.active_connection_id_limit = 2;
    params.disable_active_migration = 1;
    params.max_idle_timeout = 30 * NGTCP2_SECONDS;
    if (initial)
    {
        params.original_dcid = original ? *original : initial->dcid;
        params.original_dcid_present = 1;
        if (original)
        {
            params.retry_scid = initial->dcid;
            params.retry_scid_present = 1;
            settings.token = initial->token;
            settings.tokenlen = initial->tokenlen;
            settings.token_type = NGTCP2_TOKEN_TYPE_RETRY;
        }
    }
    const auto cb = callbacks();
    const int result = initial ? ngtcp2_conn_server_new(
                                     &conn,
                                     &dcid,
                                     &scid,
                                     &path,
                                     initial->version,
                                     &cb,
                                     &settings,
                                     &params,
                                     nullptr,
                                     this
                                 )
                               : ngtcp2_conn_client_new(
                                     &conn,
                                     &dcid,
                                     &scid,
                                     &path,
                                     NGTCP2_PROTO_VER_V1,
                                     &cb,
                                     &settings,
                                     &params,
                                     nullptr,
                                     this
                                 );
    if (result != 0)
    {
        fail("ngtcp2 connection");
    }
    ssl = SSL_new(owner->config->ssl);
    if (!ssl)
    {
        fail("QUIC TLS session");
    }
    if (ngtcp2_crypto_ossl_ctx_new(&crypto, ssl) != 0)
    {
        fail("ngtcp2 TLS context");
    }
    reference.get_conn = [](ngtcp2_crypto_conn_ref* ref)
    { return static_cast<connection_state*>(ref->user_data)->conn; };
    reference.user_data = this;
    SSL_set_app_data(ssl, &reference);
    ngtcp2_conn_set_tls_native_handle(conn, crypto);
    if (initial)
    {
        if (ngtcp2_crypto_ossl_configure_server_session(ssl) != 0)
        {
            fail("ngtcp2 server TLS");
        }
        SSL_set_accept_state(ssl);
    }
    else
    {
        if (ngtcp2_crypto_ossl_configure_client_session(ssl) != 0)
        {
            fail("ngtcp2 client TLS");
        }
        SSL_set_connect_state(ssl);
        in_addr ip{};
        in6_addr ip6{};
        if (inet_pton(AF_INET, name.c_str(), &ip) == 1 || inet_pton(AF_INET6, name.c_str(), &ip6) == 1)
        {
            required(X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), name.c_str()), "QUIC peer IP");
        }
        else
        {
            required(SSL_set_tlsext_host_name(ssl, name.c_str()), "QUIC SNI");
            required(SSL_set1_host(ssl, name.c_str()), "QUIC hostname");
        }
    }
    register_cid(scid);
    if (initial)
    {
        register_cid(initial->dcid);
    }
}

void connection_state::error(int code)
{
    if (!code)
    {
        return;
    }
    ngtcp2_ccerr reason{};
    ngtcp2_ccerr_default(&reason);
    if (code == NGTCP2_ERR_CRYPTO)
    {
        ngtcp2_ccerr_set_tls_alert(&reason, ngtcp2_conn_get_tls_alert(conn), nullptr, 0);
    }
    else
    {
        ngtcp2_ccerr_set_liberr(&reason, code, nullptr, 0);
    }
    close_packet(reason);
    if (!failure)
    {
        failure = std::make_exception_ptr(
            std::system_error(
                std::make_error_code(
                    code == NGTCP2_ERR_DRAINING || code == NGTCP2_ERR_IDLE_CLOSE ? std::errc::connection_aborted
                                                                                 : std::errc::protocol_error
                ),
                ngtcp2_strerror(code)
            )
        );
    }
    stop();
    owner->events.notify();
}

void connection_state::schedule()
{
    if (!open || !owner->open || owner->io.stopped() || queued)
    {
        return;
    }
    queued = true;
    owner->io.post(
        [weak = weak_from_this()]
        {
            if (auto self = weak.lock(); self && self->open && self->owner->open)
            {
                self->queued = false;
                try
                {
                    self->flush();
                    self->events.notify();
                }
                catch (...)
                {
                    self->failure = std::current_exception();
                    self->stop();
                }
            }
        }
    );
}

bool connection_state::transmit()
{
    while (packet_length)
    {
        const auto count = ::sendto(
            owner->socket.native_handle(),
            packet.data(),
            packet_length,
            0,
            packet_remote.data(),
            packet_remote.length
        );
        if (count >= 0)
        {
            packet_length = 0;
            return true;
        }
        if (errno == EINTR)
        {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            owner->wait_writable(shared_from_this());
            return false;
        }
        throw std::system_error(errno, std::generic_category(), "QUIC UDP send");
    }
    return true;
}

void connection_state::flush()
{
    if (!open || !owner->open || owner->io.stopped() || !conn)
    {
        return;
    }
    if (!transmit())
    {
        refresh();
        return;
    }
    std::size_t produced = 0;
    for (; produced < batch_size; ++produced)
    {
        ngtcp2_path_storage path{};
        ngtcp2_path_storage_zero(&path);
        ngtcp2_ssize length = 0;
        auto it = streams.upper_bound(last_stream);
        std::size_t candidates = streams.size();
        while (connected && candidates--)
        {
            if (it == streams.end())
            {
                it = streams.begin();
            }
            auto& stream = *it->second;
            last_stream = it->first;
            ++it;
            if (stream.reset || stream.closed
                || (stream.submitted == stream.total && (!stream.finish || stream.fin_sent)))
            {
                continue;
            }
            std::array<ngtcp2_vec, 16> vectors{};
            std::size_t count = 0;
            uint64_t end = stream.submitted;
            for (auto& chunk : stream.sending)
            {
                if (chunk.end <= stream.submitted)
                {
                    continue;
                }
                const auto start = chunk.end - chunk.bytes.size();
                const auto offset = stream.submitted > start ? stream.submitted - start : 0;
                vectors[count++] = {
                    reinterpret_cast<uint8_t*>(chunk.bytes.data()) + offset,
                    chunk.bytes.size() - offset
                };
                end = chunk.end;
                if (count == vectors.size())
                {
                    break;
                }
            }
            const bool fin = stream.finish && end == stream.total;
            ngtcp2_ssize consumed = -1;
            length = ngtcp2_conn_writev_stream(
                conn,
                &path.path,
                nullptr,
                packet.data(),
                packet.size(),
                &consumed,
                fin ? NGTCP2_WRITE_STREAM_FLAG_FIN : 0,
                stream.id,
                vectors.data(),
                count,
                timestamp()
            );
            if (consumed >= 0)
            {
                stream.submitted += consumed;
                if (fin && stream.submitted == stream.total)
                {
                    stream.fin_sent = true;
                }
            }
            if (length == NGTCP2_ERR_STREAM_DATA_BLOCKED || length == NGTCP2_ERR_STREAM_SHUT_WR
                || length == NGTCP2_ERR_STREAM_NOT_FOUND)
            {
                length = 0;
                continue;
            }
            break;
        }
        if (!length)
        {
            length = ngtcp2_conn_write_pkt(conn, &path.path, nullptr, packet.data(), packet.size(), timestamp());
        }
        if (length < 0)
        {
            error(static_cast<int>(length));
            return;
        }
        if (!length)
        {
            break;
        }
        packet_length = static_cast<std::size_t>(length);
        packet_remote.length = path.path.remote.addrlen;
        std::memcpy(packet_remote.data(), path.path.remote.addr, packet_remote.length);
        if (!transmit())
        {
            break;
        }
    }
    ngtcp2_conn_update_pkt_tx_time(conn, timestamp());
    collect();
    refresh();
    if (produced == batch_size)
    {
        schedule();
    }
}

void connection_state::refresh()
{
    if (!open || owner->io.stopped())
    {
        return;
    }
    const auto expiry = ngtcp2_conn_get_expiry(conn);
    if (expiry == timer_expiry)
    {
        return;
    }
    timer.cancel();
    timer_expiry = expiry;
    if (expiry == UINT64_MAX)
    {
        return;
    }
    timer = owner->io.call_at(
        clock::time_point(std::chrono::duration_cast<clock::duration>(std::chrono::nanoseconds(expiry))),
        [weak = weak_from_this()]
        {
            if (auto self = weak.lock(); self && self->open && self->owner->open)
            {
                self->timer_expiry = UINT64_MAX;
                try
                {
                    self->error(ngtcp2_conn_handle_expiry(self->conn, timestamp()));
                    self->flush();
                    self->events.notify();
                }
                catch (...)
                {
                    self->failure = std::current_exception();
                    self->stop();
                }
            }
        }
    );
}

void engine::start()
{
    if (!started)
    {
        started = true;
        io.spawn(read_pump(weak_from_this()));
    }
}

void engine::close() noexcept
{
    if (!open)
    {
        return;
    }
    open = false;
    socket.close();
    while (!routes.empty())
    {
        auto connection = routes.begin()->second->shared_from_this();
        routes.erase(routes.begin());
        connection->stop();
    }
    pending.clear();
    routes.clear();
    blocked.clear();
    events.notify();
}

void engine::retry(const ngtcp2_pkt_hd& initial, const address& peer)
{
    const auto scid = random_cid();
    std::array<uint8_t, NGTCP2_CRYPTO_MAX_RETRY_TOKENLEN2> token{};
    const auto size = ngtcp2_crypto_generate_retry_token2(
        token.data(),
        retry_secret.data(),
        retry_secret.size(),
        initial.version,
        peer.data(),
        peer.length,
        &scid,
        &initial.dcid,
        timestamp()
    );
    if (size < 0)
    {
        fail("QUIC Retry token");
    }
    std::array<uint8_t, packet_size> data{};
    const auto length = ngtcp2_crypto_write_retry(
        data.data(),
        data.size(),
        initial.version,
        &initial.scid,
        &scid,
        &initial.dcid,
        token.data(),
        size
    );
    if (length < 0)
    {
        fail("QUIC Retry packet");
    }
    // Stateless Retry is best-effort: a lost response is regenerated when the
    // client retransmits. It never allocates a per-peer writable queue.
    ::sendto(socket.native_handle(), data.data(), length, 0, peer.data(), peer.length);
}

void engine::receive()
{
    std::array<uint8_t, 65536> bytes{};
    for (std::size_t count = 0; count < batch_size; ++count)
    {
        address peer;
        const auto length =
            ::recvfrom(socket.native_handle(), bytes.data(), bytes.size(), 0, peer.data(), &peer.length);
        if (length < 0)
        {
            if (errno == EINTR)
            {
                --count;
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            throw std::system_error(errno, std::generic_category(), "QUIC UDP receive");
        }
        ngtcp2_version_cid decoded{};
        if (ngtcp2_pkt_decode_version_cid(&decoded, bytes.data(), length, cid_size) != 0)
        {
            continue;
        }
        auto it = routes.find(cid_key(decoded.dcid, decoded.dcidlen));
        auto* connection = it == routes.end() ? nullptr : it->second;
        std::shared_ptr<connection_state> created;
        const bool fresh = !connection;
        ngtcp2_pkt_hd initial{};
        if (!connection)
        {
            if (config->mode != role::server)
            {
                continue;
            }
            std::erase_if(pending, [](const auto& state) { return !state->open; });
            if (pending.size() >= 128)
            {
                continue;
            }
            if (ngtcp2_accept(&initial, bytes.data(), length) != 0)
            {
                continue;
            }
            ngtcp2_cid original{};
            if (initial.tokenlen
                && ngtcp2_crypto_verify_retry_token2(
                       &original,
                       initial.token,
                       initial.tokenlen,
                       retry_secret.data(),
                       retry_secret.size(),
                       initial.version,
                       peer.data(),
                       peer.length,
                       &initial.dcid,
                       10 * NGTCP2_SECONDS,
                       timestamp()
                   ) != 0)
            {
                continue;
            }
            created = std::make_shared<connection_state>(shared_from_this());
            connection = created.get();
            connection->init(peer, &initial, {}, initial.tokenlen ? &original : nullptr);
        }
        if (!connection->open)
        {
            continue;
        }
        ngtcp2_path path{{connection->local.data(), connection->local.length}, {peer.data(), peer.length}, nullptr};
        ngtcp2_pkt_info info{};
        const int result = ngtcp2_conn_read_pkt(connection->conn, &path, &info, bytes.data(), length, timestamp());
        // Out-of-order ClientHello fragments require address validation before
        // ngtcp2 will buffer them. Retry tokens bind address, CID and expiry.
        if (fresh && (result == NGTCP2_ERR_RETRY || result == NGTCP2_ERR_DROP_CONN))
        {
            if (result == NGTCP2_ERR_RETRY)
            {
                retry(initial, peer);
            }
            connection->stop();
            continue;
        }
        connection->error(result);
        if (fresh && connection->open)
        {
            pending.push_back(std::move(created));
            events.notify();
        }
        connection->schedule();
        connection->refresh();
        connection->events.notify();
        connection->collect();
    }
}

task<void> engine::read_pump(std::weak_ptr<engine> weak)
{
    for (;;)
    {
        io_context* io;
        native_socket_type fd;
        {
            auto self = weak.lock();
            if (!self || !self->open)
            {
                co_return;
            }
            io = &self->io;
            fd = self->socket.native_handle();
        }
        try
        {
            co_await io->wait_readable(fd);
            auto self = weak.lock();
            if (!self || !self->open)
            {
                co_return;
            }
            self->receive();
            // Bounded batches let other sockets and runnable tasks make progress.
            co_await io->yield();
        }
        catch (...)
        {
            if (auto self = weak.lock(); self && self->open)
            {
                self->failure = std::current_exception();
                self->close();
            }
            co_return;
        }
    }
}

void engine::wait_writable(const std::shared_ptr<connection_state>& connection)
{
    if (!connection->write_blocked)
    {
        connection->write_blocked = true;
        blocked.push_back(connection);
    }
    if (!writing)
    {
        writing = true;
        io.spawn(write_pump(weak_from_this()));
    }
}

task<void> engine::write_pump(std::weak_ptr<engine> weak)
{
    for (;;)
    {
        io_context* io;
        native_socket_type fd;
        {
            auto self = weak.lock();
            if (!self || !self->open)
            {
                co_return;
            }
            io = &self->io;
            fd = self->socket.native_handle();
        }
        try
        {
            co_await io->wait_writable(fd);
            auto self = weak.lock();
            if (!self || !self->open)
            {
                co_return;
            }
            auto batch = std::move(self->blocked);
            self->blocked.clear();
            for (auto& entry : batch)
            {
                if (auto connection = entry.lock())
                {
                    connection->write_blocked = false;
                    connection->schedule();
                }
            }
            self->writing = false;
            co_return;
        }
        catch (...)
        {
            if (auto self = weak.lock(); self && self->open)
            {
                self->failure = std::current_exception();
                self->close();
            }
            co_return;
        }
    }
}

context::context(role mode)
    : state_(std::make_shared<configuration>(mode))
{
    set_alpn_protocols({"pace-quic"});
}

void context::load_verify_file(const std::string& path)
{
    required(SSL_CTX_load_verify_locations(state_->ssl, path.c_str(), nullptr), "QUIC trust file");
}

void context::use_certificate_chain(const std::string& chain, const std::string& key)
{
    required(SSL_CTX_use_certificate_chain_file(state_->ssl, chain.c_str()), "QUIC certificate");
    required(SSL_CTX_use_PrivateKey_file(state_->ssl, key.c_str(), SSL_FILETYPE_PEM), "QUIC private key");
    required(SSL_CTX_check_private_key(state_->ssl), "QUIC certificate/key mismatch");
}

void context::set_alpn_protocols(const std::vector<std::string>& protocols)
{
    std::vector<unsigned char> encoded;
    for (const auto& value : protocols)
    {
        if (value.empty() || value.size() > 255)
        {
            throw std::invalid_argument("invalid QUIC ALPN");
        }
        encoded.push_back(static_cast<unsigned char>(value.size()));
        encoded.insert(encoded.end(), value.begin(), value.end());
    }
    if (encoded.empty() || encoded.size() > 65535)
    {
        throw std::invalid_argument("QUIC requires ALPN");
    }
    if (state_->mode == role::client
        && SSL_CTX_set_alpn_protos(state_->ssl, encoded.data(), static_cast<unsigned>(encoded.size())) != 0)
    {
        fail("QUIC ALPN");
    }
    state_->alpn = std::move(encoded);
}

namespace
{
task<void> handshake(const std::shared_ptr<connection_state>& state, std::chrono::milliseconds timeout)
{
    const auto deadline = deadline_after(timeout);
    while (!state->connected)
    {
        state->check(false);
        if (clock::now() >= deadline)
        {
            throw std::system_error(std::make_error_code(std::errc::timed_out), "QUIC handshake");
        }
        co_await state->events.wait(deadline);
    }
    state->check();
}

std::shared_ptr<stream_state> new_stream(const std::shared_ptr<connection_state>& state, stream_type type)
{
    state->check();
    if (type != stream_type::bidirectional && type != stream_type::unidirectional)
    {
        throw std::invalid_argument("QUIC stream type");
    }
    int64_t id;
    const int result = type == stream_type::unidirectional ? ngtcp2_conn_open_uni_stream(state->conn, &id, nullptr)
                                                           : ngtcp2_conn_open_bidi_stream(state->conn, &id, nullptr);
    if (result == NGTCP2_ERR_STREAM_ID_BLOCKED)
    {
        return {};
    }
    if (result != 0)
    {
        state->error(result);
        state->check();
    }
    return std::make_shared<stream_state>(state, state->get_stream(id));
}

std::shared_ptr<stream_state> accept_stream(const std::shared_ptr<connection_state>& state)
{
    state->check();
    if (state->incoming.empty())
    {
        return {};
    }
    const auto id = state->incoming.front();
    auto& buffer = state->get_stream(id);
    auto result = std::make_shared<stream_state>(state, buffer);
    state->incoming.pop_front();
    buffer.queued = false;
    return result;
}

std::optional<std::size_t> read(stream_state& state, std::span<char> bytes)
{
    state.check();
    auto& stream = state.buffer;
    if ((stream.id & 2) && ((stream.id & 1) == state.owner->server()))
    {
        throw std::logic_error("cannot read local unidirectional stream");
    }
    if (bytes.empty())
    {
        return 0;
    }
    if (stream.received.empty())
    {
        return stream.fin ? std::optional<std::size_t>{0} : std::nullopt;
    }
    auto& front = stream.received.front();
    const auto count = std::min(bytes.size(), front.size() - stream.read_offset);
    std::memcpy(bytes.data(), front.data() + stream.read_offset, count);
    stream.read_offset += count;
    stream.unread -= count;
    if (stream.read_offset == front.size())
    {
        stream.received.pop_front();
        stream.read_offset = 0;
    }
    ngtcp2_conn_extend_max_stream_offset(state.owner->conn, stream.id, count);
    ngtcp2_conn_extend_max_offset(state.owner->conn, count);
    state.owner->schedule();
    return count;
}

std::size_t write(stream_state& state, std::string_view bytes)
{
    state.check();
    auto& stream = state.buffer;
    if ((stream.id & 2) && ((stream.id & 1) != state.owner->server()))
    {
        throw std::logic_error("cannot write remote unidirectional stream");
    }
    if (stream.finish || stream.closed)
    {
        throw std::logic_error("QUIC write after FIN");
    }
    const auto size = std::min<uint64_t>(
        {bytes.size(),
         stream_window - (stream.total - stream.acknowledged),
         connection_window - state.owner->pending_bytes,
         uint64_t{16} * 1024}
    );
    if (!size)
    {
        return 0;
    }
    stream.sending.push_back({std::string(bytes.substr(0, size)), stream.total + size});
    stream.total += size;
    state.owner->pending_bytes += size;
    state.owner->schedule();
    return size;
}
} // namespace

connection::connection(io_context& io, const quic::context& config)
{
    if (config.state_->mode != role::client)
    {
        throw std::invalid_argument("QUIC client requires client configuration");
    }
    state_ =
        std::make_shared<connection_state>(std::make_shared<engine>(io, udp::endpoint{"0.0.0.0", 0}, config.state_));
}

connection::connection(std::shared_ptr<connection_state> state)
    : state_(std::move(state))
{
}

connection::connection(connection&&) noexcept = default;
connection& connection::operator=(connection&&) noexcept = default;
connection::~connection() = default;

io_context& connection::context() const noexcept
{
    return state_->owner->io;
}

bool connection::is_open() const noexcept
{
    return state_ && state_->open && state_->owner->open && !state_->owner->io.stopped();
}

std::string connection::selected_protocol() const
{
    if (!state_->ssl)
    {
        return {};
    }
    const unsigned char* bytes = nullptr;
    unsigned size = 0;
    SSL_get0_alpn_selected(state_->ssl, &bytes, &size);
    return size ? std::string(reinterpret_cast<const char*>(bytes), size) : std::string{};
}

void connection::close(std::uint64_t application_error) noexcept
{
    auto state = state_;
    if (!state || !state->open)
    {
        return;
    }
    ngtcp2_ccerr reason{};
    ngtcp2_ccerr_default(&reason);
    ngtcp2_ccerr_set_application_error(&reason, application_error, nullptr, 0);
    state->close_packet(reason);
    state->stop();
    if (!state->server())
    {
        state->owner->close();
    }
}

task<void> connection::connect(udp::endpoint peer, std::string name, std::chrono::milliseconds timeout)
{
    auto state = state_;
    guard operation(state->connecting);
    state->check(false);
    if (state->conn || state->server())
    {
        throw std::logic_error("QUIC connect state");
    }
    try
    {
        if (name.empty() || name.find('\0') != std::string::npos)
        {
            throw std::invalid_argument("QUIC client requires certificate identity");
        }
        const auto remote = net::parse_socket_address(peer.address, peer.port, peer.family, peer.scope_id);
        if (!peer.port)
        {
            throw std::invalid_argument("QUIC remote port must be nonzero");
        }
        if (remote.family() == address_family::ipv6)
        {
            state->owner->socket = udp::socket(state->owner->io, udp::endpoint{"::", 0});
        }
        state->init(remote, nullptr, name);
        state->owner->start();
        state->schedule();
        co_await handshake(state, timeout);
    }
    catch (...)
    {
        close();
        throw;
    }
}

task<stream> connection::open_stream(stream_type type)
{
    auto state = state_;
    guard operation(state->opening);
    for (;;)
    {
        if (auto result = new_stream(state, type))
        {
            co_return stream(std::move(result));
        }
        co_await state->events.wait();
    }
}

std::optional<stream> connection::try_open_stream(stream_type type)
{
    guard operation(state_->opening);
    if (auto result = new_stream(state_, type))
    {
        return stream(std::move(result));
    }
    return std::nullopt;
}

std::optional<stream> connection::try_accept_stream()
{
    guard operation(state_->accepting);
    if (auto result = quic::accept_stream(state_))
    {
        return stream(std::move(result));
    }
    return std::nullopt;
}

task<stream> connection::accept_stream()
{
    auto state = state_;
    guard operation(state->accepting);
    for (;;)
    {
        if (auto result = quic::accept_stream(state))
        {
            co_return stream(std::move(result));
        }
        co_await state->events.wait();
    }
}

stream::stream(std::shared_ptr<stream_state> state)
    : state_(std::move(state))
{
}

stream::stream(stream&&) noexcept = default;
stream& stream::operator=(stream&&) noexcept = default;
stream::~stream() = default;

std::uint64_t stream::id() const noexcept
{
    return state_->buffer.id;
}

bool stream::is_unidirectional() const noexcept
{
    return (id() & 2) != 0;
}

bool stream::is_local() const noexcept
{
    return (id() & 1) == state_->owner->server();
}

io_context& stream::context() const noexcept
{
    return state_->owner->owner->io;
}

std::optional<std::size_t> stream::try_read(std::span<char> buffer)
{
    guard operation(state_->reading);
    return read(*state_, buffer);
}

std::size_t stream::try_write(std::string_view bytes)
{
    guard operation(state_->writing);
    return write(*state_, bytes);
}

task<std::size_t> stream::read_some(std::span<char> buffer)
{
    auto state = state_;
    guard operation(state->reading);
    for (;;)
    {
        if (auto count = read(*state, buffer))
        {
            co_return *count;
        }
        co_await state->owner->events.wait();
    }
}

task<void> stream::write_all(std::string_view bytes)
{
    auto state = state_;
    guard operation(state->writing);
    while (!bytes.empty())
    {
        const auto count = write(*state, bytes);
        bytes.remove_prefix(count);
        if (!count)
        {
            co_await state->owner->events.wait();
        }
    }
}

task<void> stream::wait_events()
{
    auto state = state_;
    state->check();
    co_await state->owner->events.wait();
    state->check();
}

task<void> connection::wait_events()
{
    auto state = state_;
    state->check();
    co_await state->events.wait();
    state->check();
}

std::uint64_t stream::pending_write_bytes() const
{
    state_->check();
    return state_->buffer.total - state_->buffer.acknowledged;
}

void stream::finish_write()
{
    guard operation(state_->writing);
    state_->check();
    if (is_unidirectional() && !is_local())
    {
        throw std::logic_error("cannot finish remote unidirectional stream");
    }
    state_->buffer.finish = true;
    state_->owner->schedule();
}

void stream::reset(std::uint64_t application_error) noexcept
{
    auto state = state_;
    if (!state)
    {
        return;
    }
    state->buffer.reset = true;
    if (state->owner->conn && state->owner->open)
    {
        ngtcp2_conn_shutdown_stream(state->owner->conn, 0, state->buffer.id, application_error);
    }
    try
    {
        state->owner->schedule();
    }
    catch (...)
    {
        state->owner->failure = std::current_exception();
        state->owner->stop();
    }
    state->owner->events.notify();
}

listener::listener(io_context& io, udp::endpoint local, const quic::context& config)
{
    if (config.state_->mode != role::server)
    {
        throw std::invalid_argument("QUIC listener requires server configuration");
    }
    state_ = std::make_shared<engine>(io, std::move(local), config.state_);
    state_->start();
}

listener::~listener()
{
    close();
}

void listener::close() noexcept
{
    auto state = state_;
    if (state)
    {
        state->close();
    }
}

udp::endpoint listener::local_endpoint() const
{
    return state_->socket.local_endpoint();
}

io_context& listener::context() const noexcept
{
    return state_->io;
}

bool listener::is_open() const noexcept
{
    return state_ && state_->open && !state_->io.stopped();
}

task<connection> listener::accept(std::chrono::milliseconds timeout)
{
    auto engine = state_;
    guard operation(engine->accepting);
    for (;;)
    {
        engine->check();
        if (!engine->pending.empty())
        {
            auto state = std::move(engine->pending.front());
            engine->pending.pop_front();
            connection result(state);
            try
            {
                co_await handshake(state, timeout);
            }
            catch (...)
            {
                result.close();
                throw;
            }
            co_return result;
        }
        co_await engine->events.wait();
    }
}
} // namespace pace::quic

namespace pace::quic
{
void quic_driver::wake(quic::connection& connection)
{
    if (connection.state_)
    {
        connection.state_->schedule();
    }
}
} // namespace pace::quic
#endif
