-- To add to your packet registry.
package("reflite")
    set_kind("library", {headeronly = true})
    set_homepage("https://codeberg.org/karurochori/reflite")
    set_description("C++26 reflection wrapper for SQLITE")
    set_license("AGPL3.0")

    add_urls("https://codeberg.org/karurochori/reflite")

    on_install(function (package)
        import("package.tools.xmake").install(package)
    end)

    --Self check disabled for now, it requires to set flags to enable reflections.
    --on_test(function (package)
    --    assert(package:check_cxxsnippets({test = [[
    --        #include <reflite/reflite.hpp>
    --        void test() { /* use library API */ }
    --    ]]}, {configs = {languages = "c++26"}}))
    --end)
