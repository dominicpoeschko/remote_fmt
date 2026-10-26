cmake_minimum_required(VERSION 3.28)
cmake_policy(SET CMP0175 NEW)

set(remote_fmt_dir
    ${CMAKE_CURRENT_LIST_DIR}
    CACHE INTERNAL "")

# The FMT_ASSERT of every cross TU (below) and of tests/CMakeLists.txt's fmt_patched_* checks: fmt's own reporter
# fmt::assert_fail, which base.h declares before its first FMT_ASSERT - so it needs no header - but with the condition's
# text and the file's name as assert() gives them. Only fmt's library object defines that function (format-inl.h), and
# no firmware links it: the image's runtime defines it with assert()'s end. Live whatever NDEBUG says - no firmware
# variant defines NDEBUG, so the assert() it replaces was live too. (A libc entry declared inside the macro does not
# work: g++ drops the asm label of a block-scope declaration in a template.) A compile option, not a definition: CMake's
# compile definitions take no function-style macros.
set(remote_fmt_fmt_assert_option
    "-DFMT_ASSERT(condition, message)=((condition) ? void() : ::fmt::assert_fail(__FILE_NAME__, __LINE__, #condition))"
    CACHE INTERNAL "")

add_library(remote_fmt INTERFACE)

include(FetchContent)
FetchContent_Declare(
    cmake_helpers
    GIT_REPOSITORY https://github.com/dominicpoeschko/cmake_helpers.git
    GIT_TAG master)
FetchContent_MakeAvailable(cmake_helpers)

include(${cmake_helpers_SOURCE_DIR}/FindOrFetch.cmake)

if(NOT CMAKE_CROSSCOMPILING)
    project(remote_fmt VERSION 0.1.0)

    option(FMT_INSTALL "" OFF)
    find_or_fetch_package(
        fmt
        GIT_REPOSITORY
        https://github.com/fmtlib/fmt.git
        GIT_TAG
        12.2.0
        GIT_BRANCH
        master)

    # the catalog reader (catalog_helpers.hpp). The same version and patch as aglio's (glaze.patch is a copy of
    # aglio's): in uc_log this fetch comes first and aglio then uses it, so both must agree.
    if(NOT TARGET glaze_glaze AND NOT TARGET glaze::glaze)
        find_or_fetch_package(
            glaze
            GIT_REPOSITORY
            https://github.com/stephenberry/glaze.git
            GIT_TAG
            v9.0.0
            GIT_BRANCH
            main
            PATCH_FILE
            ${CMAKE_CURRENT_LIST_DIR}/glaze.patch)
    endif()

    find_or_fetch_package(
        enchantum
        GIT_REPOSITORY
        https://github.com/ZXShady/enchantum
        GIT_TAG
        0.4.0
        GIT_BRANCH
        main)
else()
    option(FMT_INSTALL "" OFF)
    mark_as_advanced(FORCE FMT_INSTALL)

    populate_package(
        fmt_uc
        QUIET
        GIT_REPOSITORY
        https://github.com/fmtlib/fmt.git
        GIT_TAG
        12.2.0
        GIT_BRANCH
        master
        PATCH_FILE
        ${remote_fmt_dir}/fmt.patch)

    # SYSTEM: fmt's own warnings are not the firmware's (it was the last third-party include of remote_fmt without it)
    target_include_directories(remote_fmt SYSTEM INTERFACE ${fmt_uc_SOURCE_DIR}/include)
    target_compile_definitions(remote_fmt INTERFACE FMT_STATIC_THOUSANDS_SEPARATOR=1)

    # fmt defaults this to on, which is wrong for a standard library without localization - every target reached through
    # this branch. Together with fmt.patch it makes fmt/chrono.h and fmt/std.h includable; neither alone is enough.
    target_compile_definitions(remote_fmt INTERFACE FMT_USE_LOCALE=0)
    # And no Unicode width tables: a target's text is ASCII or passes through as bytes. Set here for every TU alike
    # (string_constant/format.hpp defaults it to 0 too, but a TU may reach fmt through remote_fmt's own headers first).
    target_compile_definitions(remote_fmt INTERFACE FMT_UNICODE=0)

    # Says fmt.patch is applied, so duration/optional/variant/expected get checked rather than skipped. Set here because
    # only this file knows about the patch.
    target_compile_definitions(remote_fmt INTERFACE REMOTE_FMT_FMT_CHECK_FULL=1)

    # FMT_ASSERT without fmt's library object or any header (remote_fmt_fmt_assert_option, top of this file). One
    # definition for the whole image, so all TUs agree; every header that reaches fmt compiles on its own with it
    # (tests/CMakeLists.txt, fmt_patched_*), also against an fmt fetched without fmt.patch.
    target_compile_options(remote_fmt INTERFACE "${remote_fmt_fmt_assert_option}")

    find_or_fetch_package(
        enchantum
        GIT_REPOSITORY
        https://github.com/ZXShady/enchantum
        GIT_TAG
        0.4.0
        GIT_BRANCH
        main)

    # SYSTEM: enchantum triggers -Wundef under gcc
    target_include_directories(remote_fmt SYSTEM INTERFACE ${enchantum_SOURCE_DIR}/enchantum/include)

    # The formatter sets of src/remote_fmt/config.hpp, for every TU of the image alike: this branch puts both headers on
    # every consumer's path, so the build says so instead of each TU finding them. (The host keeps the __has_include
    # defaults: its consumers are tests and tools, which all link remote_fmt::parser and so have both.)
    target_compile_definitions(remote_fmt INTERFACE REMOTE_FMT_USE_ENCHANTUM=1 REMOTE_FMT_USE_STYLED=1)

endif()

add_subdirectory(string_constant)

target_include_directories(remote_fmt INTERFACE src)
target_link_libraries(remote_fmt INTERFACE string_constant::string_constant)

add_library(remote_fmt::remote_fmt ALIAS remote_fmt)

if(NOT CMAKE_CROSSCOMPILING)
    add_library(remote_fmt_parser INTERFACE)

    target_include_directories(remote_fmt_parser INTERFACE src)
    target_link_libraries(remote_fmt_parser INTERFACE fmt::fmt enchantum::enchantum glaze::glaze)

    add_library(remote_fmt::parser ALIAS remote_fmt_parser)
endif()

# target_generate_string_constants(<target> [ID_KEY_STRIP <regex>]): the catalog of <target>'s string constants,
# `<target>_string_constants.json`, written once after the link: tools/generate_string_constants.py finds the strings
# before the link (the ids' .obj and `<target>_string_constants.prelink.json`), tools/extract_sites.py adds the call
# sites and writes the catalog. ID_KEY_STRIP: a regex matched at the start of a string whose group 1 is left out of the
# string's id hash (uc_log: a log line's line number); without it the whole string is the key.
function(target_generate_string_constants targetname)

    cmake_parse_arguments(PARSE_ARGV 1 PARSED_ARGS "" "ID_KEY_STRIP" "")

    if(PARSED_ARGS_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "unknown argument ${PARSED_ARGS_UNPARSED_ARGUMENTS}")
    endif()

    get_target_property(bin_dir ${targetname} BINARY_DIR)
    set(prelink_json "${bin_dir}/${targetname}_string_constants.prelink.json")
    set(catalog_json "${bin_dir}/${targetname}_string_constants.json")

    target_link_options(${targetname} PUBLIC ${bin_dir}/${targetname}_string_constants.obj)

    get_target_property(options_flags ${targetname} COMPILE_OPTIONS)

    # An unset property reads back as "options_flags-NOTFOUND" with a hyphen, so the old underscored comparison never
    # matched and the sentinel was passed to the compiler as a source file. Only showed up for a target with no
    # COMPILE_OPTIONS at all.
    if(NOT options_flags)
        string(JOIN " " flags ${CMAKE_CXX_FLAGS})
    else()
        string(JOIN " " flags ${CMAKE_CXX_FLAGS} ${options_flags})
    endif()
    # a host target's options carry $<COMPILE_LANGUAGE:...>, which a custom command rejects
    string(GENEX_STRIP "${flags}" flags)

    find_package(
        Python3
        COMPONENTS Interpreter
        REQUIRED)
    set(command
        ${Python3_EXECUTABLE}
        ${remote_fmt_dir}/tools/generate_string_constants.py
        --target_name
        ${targetname}
        --out_dir
        ${bin_dir}
        --source_dir
        ${remote_fmt_dir}
        --compiler
        ${CMAKE_CXX_COMPILER}
        --nm
        ${CMAKE_NM}
        --objects
        $<TARGET_OBJECTS:${targetname}>)

    if(PARSED_ARGS_ID_KEY_STRIP)
        list(APPEND command --id-key-strip=${PARSED_ARGS_ID_KEY_STRIP})
    endif()

    if(NOT "${flags}" STREQUAL "")
        list(APPEND command --flags=${flags})
    endif()

    add_custom_command(
        TARGET ${targetname}
        PRE_LINK COMMAND_EXPAND_LISTS
        COMMAND ${command}
        # not the .obj: as an input of this link it would be a cycle for ninja
        BYPRODUCTS ${bin_dir}/${targetname}_string_constants.cpp ${prelink_json}
        COMMENT "Extracting string constants for ${targetname}"
        # each argument as it is: an ID_KEY_STRIP regex holds quotes, brackets and parentheses
        VERBATIM)

    set_property(
        TARGET ${targetname}
        APPEND
        PROPERTY LINK_DEPENDS ${remote_fmt_dir}/tools/generate_string_constants.py
                 ${remote_fmt_dir}/tools/rf_symbols.py ${remote_fmt_dir}/tools/descriptor.py)
    set_property(
        TARGET ${targetname}
        APPEND
        PROPERTY ADDITIONAL_CLEAN_FILES ${bin_dir}/${targetname}_string_constants.cpp)
    set_property(
        TARGET ${targetname}
        APPEND
        PROPERTY ADDITIONAL_CLEAN_FILES ${bin_dir}/${targetname}_string_constants.obj)
    set_property(
        TARGET ${targetname}
        APPEND
        PROPERTY ADDITIONAL_CLEAN_FILES ${prelink_json} ${catalog_json})

    # after the link: the strings and the call sites (catalog.hpp REMOTE_FMT_SITE) into the catalog
    if(NOT CMAKE_CROSSCOMPILING)
        target_link_options(${targetname} PRIVATE "-Wl,-T,${remote_fmt_dir}/cmake/remote_fmt_sites_host.ld")
        set_property(
            TARGET ${targetname}
            APPEND
            PROPERTY LINK_DEPENDS ${remote_fmt_dir}/cmake/remote_fmt_sites_host.ld)
    endif()
    set(sites_command ${Python3_EXECUTABLE} ${remote_fmt_dir}/tools/extract_sites.py --elf $<TARGET_FILE:${targetname}>
                      --strings ${prelink_json} --json ${catalog_json})
    if(CMAKE_NM)
        list(APPEND sites_command --nm ${CMAKE_NM})
    endif()
    add_custom_command(
        TARGET ${targetname}
        POST_BUILD
        COMMAND ${sites_command}
        BYPRODUCTS ${catalog_json}
        COMMENT "Writing the string catalog of ${targetname}"
        VERBATIM)
    set_property(
        TARGET ${targetname}
        APPEND
        PROPERTY LINK_DEPENDS ${remote_fmt_dir}/tools/extract_sites.py ${remote_fmt_dir}/tools/rf_symbols.py
                 ${remote_fmt_dir}/tools/descriptor.py)
endfunction()
