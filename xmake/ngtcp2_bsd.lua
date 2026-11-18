-- The upstream xmake recipe does not provide an installer for BSD.
package("pace-ngtcp2-bsd")
    set_base("ngtcp2")
    on_install("bsd", function (package)
        local openssl = package:dep("openssl3")
        local shared = package:config("shared")
        local configs = {
            "-DENABLE_LIB_ONLY=ON",
            "-DBUILD_TESTING=OFF",
            "-DENABLE_OPENSSL=ON",
            "-DENABLE_SHARED_LIB=" .. (shared and "ON" or "OFF"),
            "-DENABLE_STATIC_LIB=" .. (shared and "OFF" or "ON"),
            "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"),
        }
        if not openssl:is_system() then
            table.insert(configs, "-DOPENSSL_ROOT_DIR=" .. openssl:installdir())
        end
        if not shared then package:add("defines", "NGTCP2_STATICLIB") end
        import("package.tools.cmake").install(package, configs)
    end)
package_end()
