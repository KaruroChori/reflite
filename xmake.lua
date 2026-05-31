add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate")

set_arch("native")
set_optimize("fastest")
set_languages("cxx26")

add_requires("sqlite3")

target("reflite")
    set_kind("headeronly")
    add_includedirs("include", {public=true})    
    add_headerfiles("include/(**)", {prefixdir = ""})
    add_packages("sqlite3")
    set_languages("cxx26")

-- It will only really work on gcc-16 with this configuration.
target("demo")
    set_kind("binary")
    add_files("./examples/sample.cpp")
    add_packages("sqlite3")
    add_deps("reflite")
    set_languages("cxx26")
    add_cxflags( "-freflection", {force=true})
    set_default(false)