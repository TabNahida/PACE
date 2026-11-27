-- Build-only checks. `--header_check=y` compiles every public header on its own, so a header that
-- forgets one of its includes fails here instead of in a consumer's build. Headers of disabled modules
-- compile to nothing, because each module header is guarded by its PACE_HAS_<module> macro.
option("header_check", {default = false, showmenu = true, description = "Compile every public header in its own translation unit"})

-- Paths stay anchored to PACE when included by another project.
local pace_root = path.directory(os.scriptdir())
if has_config("header_check") then
    target("pace_header_check")
        set_kind("object")
        add_deps("pace")
        on_load(function (target)
            local include_root = path.join(pace_root, "include")
            for _, header in ipairs(os.files(path.join(include_root, "pace", "**.hpp"))) do
                local name = path.relative(header, include_root):gsub("\\", "/")
                local source = path.join(target:autogendir(), "header_check", name:gsub("[/%.]", "_") .. ".cpp")
                io.writefile(source, "#include <" .. name .. ">\n")
                target:add("files", source)
            end
        end)
    target_end()
end
