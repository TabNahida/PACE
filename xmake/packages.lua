-- Third-party packages, pinned. The runtime and net modules need none of them.
local enable_quic = has_config("quic") or has_config("http3")
if has_config("uring") and is_plat("linux") then add_requires("liburing 2.12") end
if has_config("tls") or enable_quic then
    if is_plat("windows") and is_arch("arm64") then
        includes("openssl_arm64.lua")
        add_requires("pace-openssl3-arm64 3.5.7", {alias = "openssl3", system = false})
    else
        add_requires("openssl3 3.5.7")
    end
end
if enable_quic and get_config("quic_backend") == "ngtcp2" then
    if is_plat("bsd") then
        includes("ngtcp2_bsd.lua")
        add_requires("pace-ngtcp2-bsd 1.26.0", {alias = "ngtcp2", system = false, configs = {openssl = "openssl3"}})
        add_requireconfs("pace-ngtcp2-bsd.openssl3", {version = "3.5.7", override = true})
    else
        add_requires("ngtcp2 1.26.0", {configs = {openssl = "openssl3"}})
        add_requireconfs("ngtcp2.openssl3", {version = "3.5.7", override = true})
    end
end
if has_config("http2") then add_requires("nghttp2 1.70.0") end
if has_config("dns") then add_requires("c-ares 1.34.8") end
if has_config("http3") then add_requires("nghttp3 v1.18.0") end
