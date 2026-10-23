-- Keep the upstream source, checksums and installer, with a separate package
-- identity so an already cached /Gs0 binary cannot bypass this correction.
package("pace-openssl3-arm64")
    set_base("openssl3")
    set_policy("package.precompiled", false)
    on_install("windows", function (package)
        assert(package:is_plat("windows") and package:is_arch("arm64"),
               "the OpenSSL stack-probe correction is only for Windows ARM64")
        -- MSVC /O2 + /Gs0 emitted a __chkstk call before saving LR in
        -- tls_parse_all_extensions. Returning then re-entered its prologue.
        -- Restore Microsoft's normal 4 KiB probe threshold; /O2, /GS and
        -- probes for large frames remain enabled. See
        -- docs/diagnostics/windows-arm64-openssl.md.
        local config = "Configurations/10-main.conf"
        local content = io.readfile(config)
        local corrected, count = content:gsub("/Gs0 /GF /Gy", "/Gs4096 /GF /Gy")
        assert(count == 1, "OpenSSL compiler flags changed; review the ARM64 stack-probe correction")
        io.writefile(config, corrected)
        package:base():script("install")(package)
    end)
package_end()
