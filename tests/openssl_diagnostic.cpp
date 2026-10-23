// Isolate the OpenSSL dependency from PACE sockets/coroutines during platform CI.
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
void stage(const char* text) { std::fprintf(stderr, "OPENSSL_DIAGNOSTIC %s\n", text); std::fflush(stderr); }
void require(bool value, const char* message) {
    if (!value) {
        // Avoid OpenSSL FILE* APIs: only the application CRT owns stderr.
        for (unsigned long error; (error = ERR_get_error()) != 0;) {
            char buffer[256]{}; ERR_error_string_n(error, buffer, sizeof(buffer));
            std::fprintf(stderr, "%s\n", buffer);
        }
        throw std::runtime_error(message);
    }
}
template<class T, auto Free> using owner = std::unique_ptr<T, decltype(Free)>;
using context = owner<SSL_CTX, SSL_CTX_free>;
using session = owner<SSL, SSL_free>;

void crypto() {
    stage("RAND_bytes begin");
    std::array<unsigned char, 32> random{};
    require(RAND_bytes(random.data(), static_cast<int>(random.size())) == 1, "RAND_bytes");
    stage("SHA256 begin");
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{}; unsigned size = 0;
    require(EVP_Digest("abc", 3, digest.data(), &size, EVP_sha256(), nullptr) == 1 && size == 32, "SHA256");
    constexpr std::array<unsigned char, 32> expected{
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    require(std::memcmp(digest.data(), expected.data(), expected.size()) == 0, "SHA256 known answer");
    stage("AES-128-GCM begin");
    const std::array<unsigned char, 16> key{};
    const std::array<unsigned char, 12> iv{};
    std::array<unsigned char, 48> encrypted{}, plain{};
    std::array<unsigned char, 16> tag{};
    owner<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free> enc(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    owner<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free> dec(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    require(enc && dec, "cipher context");
    int count = 0, final = 0;
    require(EVP_EncryptInit_ex(enc.get(), EVP_aes_128_gcm(), nullptr, key.data(), iv.data()) == 1 &&
            EVP_EncryptUpdate(enc.get(), encrypted.data(), &count, random.data(), static_cast<int>(random.size())) == 1 &&
            EVP_EncryptFinal_ex(enc.get(), encrypted.data() + count, &final) == 1 &&
            EVP_CIPHER_CTX_ctrl(enc.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()), tag.data()) == 1, "GCM encryption");
    const int encrypted_size = count + final;
    require(EVP_DecryptInit_ex(dec.get(), EVP_aes_128_gcm(), nullptr, key.data(), iv.data()) == 1 &&
            EVP_DecryptUpdate(dec.get(), plain.data(), &count, encrypted.data(), encrypted_size) == 1 &&
            EVP_CIPHER_CTX_ctrl(dec.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()), tag.data()) == 1 &&
            EVP_DecryptFinal_ex(dec.get(), plain.data() + count, &final) == 1 &&
            count + final == static_cast<int>(random.size()) &&
            std::memcmp(plain.data(), random.data(), random.size()) == 0, "GCM authenticated roundtrip");
    stage("X25519 raw private key begin");
    owner<EVP_PKEY, EVP_PKEY_free> x25519(
        EVP_PKEY_new_raw_private_key_ex(nullptr, "X25519", nullptr, random.data(), random.size()), EVP_PKEY_free);
    require(bool(x25519), "X25519 private key");
    std::array<unsigned char, 32> public_key{}; std::size_t public_size = public_key.size();
    require(EVP_PKEY_get_raw_public_key(x25519.get(), public_key.data(), &public_size) == 1 && public_size == 32,
            "X25519 public key");
    stage("P-256 keygen begin");
    owner<EVP_PKEY, EVP_PKEY_free> ec(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"), EVP_PKEY_free);
    require(bool(ec), "P-256 key generation");
    stage("crypto passed");
}

struct contexts {
    context server{nullptr, SSL_CTX_free}, client{nullptr, SSL_CTX_free};
    explicit contexts(const std::filesystem::path& fixtures) {
        stage("SSL_CTX_new server begin"); server.reset(SSL_CTX_new(TLS_server_method()));
        require(bool(server), "server context");
        stage("SSL_CTX_new client begin"); client.reset(SSL_CTX_new(TLS_client_method()));
        require(bool(client), "client context");
        require(SSL_CTX_set_min_proto_version(server.get(), TLS1_2_VERSION) == 1 &&
                SSL_CTX_set_min_proto_version(client.get(), TLS1_2_VERSION) == 1, "minimum TLS version");
        stage("default trust paths begin");
        require(SSL_CTX_set_default_verify_paths(client.get()) == 1, "default trust paths");
        stage("certificate chain file begin");
        require(SSL_CTX_use_certificate_chain_file(server.get(), (fixtures / "localhost.pem").string().c_str()) == 1,
                "certificate chain");
        stage("private key file begin");
        require(SSL_CTX_use_PrivateKey_file(server.get(), (fixtures / "localhost-key.pem").string().c_str(), SSL_FILETYPE_PEM) == 1 &&
                SSL_CTX_check_private_key(server.get()) == 1, "private key");
        stage("application trust file begin");
        require(SSL_CTX_load_verify_locations(client.get(), (fixtures / "ca.pem").string().c_str(), nullptr) == 1, "application trust");
        SSL_CTX_set_verify(client.get(), SSL_VERIFY_PEER, nullptr);
        stage("contexts passed");
    }
};

// Match the legacy int/long BIO callback ABI used by PACE without sockets.
struct bio_state { std::array<char, 8> bytes{}; int size = 0; };
int bio_read(BIO* bio, char* output, int maximum) {
    auto& state = *static_cast<bio_state*>(BIO_get_data(bio));
    BIO_clear_retry_flags(bio);
    const int count = state.size < maximum ? state.size : maximum;
    if (count > 0) std::memcpy(output, state.bytes.data(), static_cast<std::size_t>(count));
    return count;
}
int bio_write(BIO* bio, const char* input, int size) {
    auto& state = *static_cast<bio_state*>(BIO_get_data(bio));
    BIO_clear_retry_flags(bio);
    if (size < 0 || size > static_cast<int>(state.bytes.size())) return -1;
    std::memcpy(state.bytes.data(), input, static_cast<std::size_t>(size)); state.size = size;
    return size;
}
void custom_bio() {
    stage("custom BIO method begin");
    owner<BIO_METHOD, BIO_meth_free> method(BIO_meth_new(BIO_TYPE_SOURCE_SINK | BIO_get_new_index(), "ABI probe"), BIO_meth_free);
    require(method && BIO_meth_set_read(method.get(), bio_read) == 1 && BIO_meth_set_write(method.get(), bio_write) == 1 &&
        BIO_meth_set_create(method.get(), [](BIO* bio) { BIO_set_init(bio, 1); return 1; }) == 1 &&
        BIO_meth_set_destroy(method.get(), [](BIO*) { return 1; }) == 1 &&
        BIO_meth_set_ctrl(method.get(), [](BIO*, int command, long, void*) -> long { return command == BIO_CTRL_FLUSH ? 1 : 0; }) == 1,
        "custom BIO callbacks");
    owner<BIO, BIO_free> bio(BIO_new(method.get()), BIO_free); require(bool(bio), "custom BIO allocation");
    bio_state state; BIO_set_data(bio.get(), &state);
    std::array<char, 8> actual{}; std::size_t count = 0;
    stage("custom BIO write_ex/read_ex begin");
    require(BIO_write_ex(bio.get(), "ABI-test", 8, &count) == 1 && count == 8 && BIO_flush(bio.get()) == 1 &&
            BIO_read_ex(bio.get(), actual.data(), actual.size(), &count) == 1 && count == 8 &&
            std::memcmp(actual.data(), "ABI-test", 8) == 0, "custom BIO roundtrip");
    stage("custom BIO passed");
}

void handshake(const std::filesystem::path& fixtures) {
    contexts config(fixtures);
    stage("SSL_new begin");
    session server(SSL_new(config.server.get()), SSL_free), client(SSL_new(config.client.get()), SSL_free);
    require(server && client, "TLS sessions");
    require(SSL_set1_host(client.get(), "localhost") == 1, "TLS hostname verification");
    BIO* client_bio = nullptr; BIO* server_bio = nullptr;
    require(BIO_new_bio_pair(&client_bio, 65536, &server_bio, 65536) == 1, "memory BIO pair");
    SSL_set_bio(client.get(), client_bio, client_bio); SSL_set_bio(server.get(), server_bio, server_bio);
    SSL_set_connect_state(client.get()); SSL_set_accept_state(server.get());
    stage("verified memory-BIO TLS handshake begin");
    bool client_done = false, server_done = false;
    auto advance = [](SSL* ssl, bool& done) {
        if (done) return;
        ERR_clear_error(); const int result = SSL_do_handshake(ssl);
        if (result == 1) { done = true; return; }
        const int error = SSL_get_error(ssl, result);
        require(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE, "memory TLS handshake");
    };
    for (unsigned step = 0; step < 4096 && !(client_done && server_done); ++step) {
        advance(client.get(), client_done); advance(server.get(), server_done);
    }
    require(client_done && server_done && SSL_get_verify_result(client.get()) == X509_V_OK, "verified handshake completion");
    std::fprintf(stderr, "OPENSSL_DIAGNOSTIC protocol=%s cipher=%s\n", SSL_get_version(client.get()), SSL_get_cipher(client.get()));
    stage("memory-BIO encrypted payload begin");
    std::size_t count = 0; std::array<char, 32> payload{};
    require(SSL_write_ex(client.get(), "verified payload", 16, &count) == 1 && count == 16 &&
            SSL_read_ex(server.get(), payload.data(), payload.size(), &count) == 1 && count == 16 &&
            std::memcmp(payload.data(), "verified payload", 16) == 0, "TLS payload");
    stage("memory-BIO close_notify begin");
    const int first = SSL_shutdown(client.get()); require(first >= 0, "client close_notify");
    require(SSL_shutdown(server.get()) >= 0 && SSL_shutdown(client.get()) == 1, "TLS close handshake");
    stage("memory-BIO TLS passed");
}
} // namespace
int main(int argc, char** argv) {
    try {
        const std::string selected = argc > 1 ? argv[1] : "all";
        const auto fixtures = argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path(__FILE__).parent_path() / "certs";
        require(argc <= 3 && (selected == "all" || selected == "crypto" || selected == "context" || selected == "bio" || selected == "handshake"),
                "usage: openssl_diagnostic [all|crypto|context|bio|handshake] [fixtures]");
        stage("process entered");
        std::fprintf(stderr, "OPENSSL_DIAGNOSTIC headers=%s runtime=%s platform=%s compiler=%s pointers=%zu long=%zu\n",
            OPENSSL_VERSION_TEXT, OpenSSL_version(OPENSSL_VERSION), OpenSSL_version(OPENSSL_PLATFORM),
            OpenSSL_version(OPENSSL_CFLAGS), sizeof(void*), sizeof(long));
        std::fflush(stderr);
        stage("OPENSSL_init_ssl begin"); require(OPENSSL_init_ssl(0, nullptr) == 1, "OpenSSL initialization");
        if (selected == "all" || selected == "crypto") crypto();
        if (selected == "all" || selected == "context") { contexts config(fixtures); }
        if (selected == "all" || selected == "bio") custom_bio();
        if (selected == "all" || selected == "handshake") handshake(fixtures);
        stage("selected checks passed");
    } catch (const std::exception& error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
