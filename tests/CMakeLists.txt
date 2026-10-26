cmake_minimum_required(VERSION 3.28)
project(remote_fmt_tests)

include(FetchContent)

FetchContent_Declare(
    cmake_helpers
    GIT_REPOSITORY https://github.com/dominicpoeschko/cmake_helpers.git
    GIT_TAG master)

FetchContent_MakeAvailable(cmake_helpers)

include(${cmake_helpers_SOURCE_DIR}/BuildOptions.cmake)

add_subdirectory(../ ${CMAKE_CURRENT_BINARY_DIR}/remote_fmt)

# fmt uses well-defined unsigned wraparound on purpose; keep clang's non-UB "integer" group checks out of fmt sources
# (see fmt_sanitizer_ignorelist.txt). Harmless when the checks are not enabled.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(fmt_sanitizer_ignorelist -fsanitize-ignorelist=${CMAKE_CURRENT_SOURCE_DIR}/fmt_sanitizer_ignorelist.txt)
else()
    set(fmt_sanitizer_ignorelist)
endif()

# Propagate stdlib/sanitizer flags into fetched dependencies so libc++/debug-ABI builds link.
foreach(dep_target fmt)
    if(TARGET ${dep_target})
        get_target_property(_imported ${dep_target} IMPORTED)
        if(NOT _imported)
            target_add_stdlib_flags(${dep_target})
            target_add_stdlib_debug_flags(${dep_target})
            target_add_sanitizer_flags(${dep_target} PUBLIC)
            target_add_optimizer_flags(${dep_target} PUBLIC)
            target_compile_options(${dep_target} PRIVATE ${fmt_sanitizer_ignorelist})
        endif()
    endif()
endforeach()

# mp-units, so the round-trip tests cover the quantity formatter. Optional: without it the quantity cases compile out
# (__has_include) and everything else still runs. The parser half needs no mp-units at all -- a quantity reaches the
# host as a string and a number.
option(REMOTE_FMT_TESTS_WITH_MP_UNITS "fetch mp-units for the quantity round-trip tests" ON)
if(REMOTE_FMT_TESTS_WITH_MP_UNITS)
    set(MP_UNITS_API_CONTRACTS
        "NONE"
        CACHE STRING "" FORCE) # otherwise mp-units wants gsl-lite
    set(MP_UNITS_API_NATURAL_UNITS
        OFF
        CACHE BOOL "" FORCE)
    set(MP_UNITS_BUILD_CXX_MODULES
        OFF
        CACHE BOOL "" FORCE)
    set(MP_UNITS_BUILD_INSTALL
        OFF
        CACHE BOOL "" FORCE)
    FetchContent_Declare(
        mp-units
        GIT_REPOSITORY https://github.com/mpusz/mp-units.git
        GIT_TAG v2.5.0
        GIT_SHALLOW ON
        SOURCE_SUBDIR src
        UPDATE_DISCONNECTED ON)
    FetchContent_MakeAvailable(mp-units)
    foreach(_t mp-units-core mp-units-systems)
        if(TARGET ${_t})
            get_target_property(_dirs ${_t} INTERFACE_INCLUDE_DIRECTORIES)
            if(_dirs)
                target_include_directories(${_t} SYSTEM INTERFACE ${_dirs})
            endif()
        endif()
    endforeach()
endif()

enable_testing()

function(remote_fmt_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE remote_fmt::remote_fmt remote_fmt::parser)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    target_add_default_build_options(${name} PRIVATE)
    target_compile_options(${name} PRIVATE ${fmt_sanitizer_ignorelist})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# The round-trip test, built once with the defaults (test_roundtrip) and once per encoder knob at its other value below.
# Extra arguments are compile definitions.
#
# The fmt-parity tests instantiate fixed-point specs ("{:.0f}", "{:.1100f}"), and gcc then reports "writing 2 bytes into
# a region of size 0" inside fmt's own write_fixed/write_padded. It is a false positive - the buffer is sized by a
# precision gcc cannot see through - and there is nothing to fix on this side, so it is silenced for these targets
# rather than dropped from the tests: fixed-point formatting is where fmt's bigint and dragonbox paths live, where three
# of the committed corpus_regressions/ were found.
#
# The flag has to be on the link options as well. The default build options enable -flto, so the codegen that produces
# this warning runs at link time, where compile options no longer apply - and for the same reason a #pragma diagnostic
# region inside the source file does not reach it either.
function(remote_fmt_add_roundtrip_test name)
    remote_fmt_add_test(${name} roundtrip_tests.cpp)
    target_compile_definitions(${name} PRIVATE REMOTE_FMT_USE_CATALOG=false ${ARGN})
    if(TARGET mp-units::mp-units)
        target_link_libraries(${name} PRIVATE mp-units::mp-units)
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${name} PRIVATE -Wno-stringop-overflow)
        target_link_options(${name} PRIVATE -Wno-stringop-overflow)
    endif()
endfunction()

# The catalog test, same scheme. Each build has a catalog json of its own (target_generate_string_constants writes
# <target>_string_constants.json), read back by the test.
function(remote_fmt_add_catalog_test name)
    remote_fmt_add_test(${name} catalog_tests.cpp)
    target_generate_string_constants(${name})
    target_compile_definitions(
        ${name} PRIVATE REMOTE_FMT_TEST_CATALOG_JSON="${CMAKE_CURRENT_BINARY_DIR}/${name}_string_constants.json"
                        ${ARGN})
endfunction()

remote_fmt_add_roundtrip_test(test_roundtrip)
remote_fmt_add_catalog_test(test_catalog)

# Every encoder knob of config.hpp (README "The wire") built at its other value, one knob per build so a failure names
# it: a change to the Printer is checked at each. The text must not change, and the tests check the wire against
# REMOTE_FMT_TEST_EXPECT_<KNOB> (knobEffects(), frameCheck(); the defaults in support/TestSupport.ipp), so a knob define
# that does not reach config.hpp fails.
foreach(knob STAGING_BYTES FRAME_CHECK)
    remote_fmt_add_roundtrip_test(test_roundtrip_${knob}_0 REMOTE_FMT_${knob}=0 REMOTE_FMT_TEST_EXPECT_${knob}=0)
    remote_fmt_add_catalog_test(test_catalog_${knob}_0 REMOTE_FMT_${knob}=0 REMOTE_FMT_TEST_EXPECT_${knob}=0)
endforeach()

# The formatter-set switches of src/remote_fmt/config.hpp (a firmware build sets them for the whole image), each forced
# to 0 and to 1 with the other two at their host default, so both #if paths of each keep compiling.
set(remote_fmt_config_default_ENCHANTUM 1)
set(remote_fmt_config_default_STYLED 1)
if(TARGET mp-units::mp-units)
    set(remote_fmt_config_default_MP_UNITS 1)
else()
    set(remote_fmt_config_default_MP_UNITS 0)
endif()
foreach(switch ENCHANTUM STYLED MP_UNITS)
    foreach(value 0 1)
        if(switch STREQUAL "MP_UNITS"
           AND value EQUAL 1
           AND NOT TARGET mp-units::mp-units)
            continue()
        endif()
        set(name remote_fmt_config_switches_${switch}_${value})
        remote_fmt_add_test(${name} config_switch_tests.cpp)
        if(TARGET mp-units::mp-units)
            target_link_libraries(${name} PRIVATE mp-units::mp-units)
        endif()
        set(defs REMOTE_FMT_USE_CATALOG=false REMOTE_FMT_USE_${switch}=${value})
        foreach(other ENCHANTUM STYLED MP_UNITS)
            if(other STREQUAL switch)
                list(APPEND defs REMOTE_FMT_TEST_EXPECT_${other}=${value})
            else()
                list(APPEND defs REMOTE_FMT_TEST_EXPECT_${other}=${remote_fmt_config_default_${other}})
            endif()
        endforeach()
        target_compile_definitions(${name} PRIVATE ${defs})
    endforeach()
endforeach()

# tools/generate_string_constants.py, end to end with this build's compiler and nm
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_generate_string_constants COMMAND ${Python3_EXECUTABLE}
                                                     ${CMAKE_CURRENT_SOURCE_DIR}/test_generate_string_constants.py -v)
set_tests_properties(
    test_generate_string_constants
    PROPERTIES
        ENVIRONMENT
        "PYTHONDONTWRITEBYTECODE=1;RF_TEST_CXX=${CMAKE_CXX_COMPILER};RF_TEST_NM=${CMAKE_NM};RF_TEST_INCLUDES=${CMAKE_CURRENT_SOURCE_DIR}/../src:${CMAKE_CURRENT_SOURCE_DIR}/../string_constant/src"
)

add_test(NAME test_extract_sites COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_extract_sites.py -v)
set_tests_properties(
    test_extract_sites
    PROPERTIES
        ENVIRONMENT
        "PYTHONDONTWRITEBYTECODE=1;RF_TEST_INCLUDES=${CMAKE_CURRENT_SOURCE_DIR}/../src:${CMAKE_CURRENT_SOURCE_DIR}/../string_constant/src"
)

# tools/descriptor.py (the catalog json's per-argument "args") against the C++ descriptors: descriptor_cases prints
# described<Args...> for a set of argument types with the catalog on (enums as type keys) and off (inline), the python
# test splits each into exactly its number of arguments
foreach(catalog true false)
    set(name remote_fmt_descriptor_cases_catalog_${catalog})
    remote_fmt_add_test(${name} descriptor_cases.cpp)
    target_compile_definitions(${name} PRIVATE REMOTE_FMT_USE_CATALOG=${catalog})
    if(TARGET mp-units::mp-units)
        target_link_libraries(${name} PRIVATE mp-units::mp-units)
    endif()
endforeach()
add_test(
    NAME test_descriptor_split
    COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_descriptor_split.py
        $<TARGET_FILE:remote_fmt_descriptor_cases_catalog_true>
        $<TARGET_FILE:remote_fmt_descriptor_cases_catalog_false>)
set_tests_properties(test_descriptor_split PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# the one format-string scanner over every short string (format_grammar.hpp)
remote_fmt_add_test(test_scanner_equivalence scanner_equivalence_tests.cpp)
target_compile_definitions(test_scanner_equivalence PRIVATE REMOTE_FMT_USE_CATALOG=false)

# parseMessage's error kinds: a line lost to its format (ParseError::format) against bytes that make no frame
remote_fmt_add_test(test_parse_message parse_message_tests.cpp)

# a record in pieces without the catalog (beginInline / formatArguments / endInline): what print() writes
remote_fmt_add_test(test_inline_record inline_record_tests.cpp)
target_compile_definitions(test_inline_record PRIVATE REMOTE_FMT_USE_CATALOG=false)

remote_fmt_add_test(test_fmt_check fmt_check_tests.cpp)
target_compile_definitions(test_fmt_check PRIVATE REMOTE_FMT_USE_CATALOG=false)

# The same without fmt/chrono.h and fmt/std.h (a standard library without localization and no fmt.patch): duration,
# optional, variant and expected degrade to UncheckedArg, the other fields stay checked.
remote_fmt_add_test(test_fmt_check_FULL_0 fmt_check_tests.cpp)
target_compile_definitions(test_fmt_check_FULL_0 PRIVATE REMOTE_FMT_USE_CATALOG=false REMOTE_FMT_FMT_CHECK_FULL=0)

# Every public header compiles alone - one TU with nothing in front of its include - under the host flags, warnings as
# errors: a header that leans on another one's includes fails here, not in the next user. The ones that reach fmt are
# also built against the patched fmt with the cross definitions below (remote_fmt_fmt_patch_self_contained_*).
set(remote_fmt_public_headers
    wire
    catalog
    config
    descriptor
    format_grammar
    traits
    fmt_check
    check
    describe
    enum_text
    formatters
    formatter_styled
    printer
    formatter_units
    remote_fmt
    descriptor_grammar
    parser
    catalog_helpers
    fmt_wrapper
    detail/Lifetimebound)
foreach(header IN LISTS remote_fmt_public_headers)
    string(REPLACE "/" "_" _name ${header})
    set(_source ${CMAKE_CURRENT_BINARY_DIR}/header_alone/${_name}.cpp)
    file(CONFIGURE OUTPUT ${_source} CONTENT
         "// generated by tests/CMakeLists.txt: remote_fmt/${header}.hpp alone\n#include <remote_fmt/${header}.hpp>\n")
    set(_target remote_fmt_header_alone_${_name})
    add_library(${_target} OBJECT EXCLUDE_FROM_ALL ${_source})
    target_link_libraries(${_target} PRIVATE remote_fmt::remote_fmt remote_fmt::parser)
    if(TARGET mp-units::mp-units)
        target_link_libraries(${_target} PRIVATE mp-units::mp-units)
    endif()
    target_add_default_build_options(${_target} PRIVATE)
    target_compile_options(${_target} PRIVATE -Werror ${fmt_sanitizer_ignorelist})
    add_test(NAME ${_target} COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ${_target} --config
                                     $<CONFIG>)
    set_tests_properties(${_target} PROPERTIES RESOURCE_LOCK fmt_check_build)
endforeach()

# Format strings that must NOT compile. A rejection is a hard compile error that cannot be observed as a bool, so each
# case is its own excluded target and the test is "building it fails". Keep in step with fmt_check_fail.cpp.
set(fmt_check_fail_cases
    1
    2
    3
    4
    5
    6
    7
    8
    9
    10
    11
    12
    13
    14
    15
    16
    17
    18)

foreach(fail_case IN LISTS fmt_check_fail_cases)
    add_executable(fmt_check_fail_${fail_case} EXCLUDE_FROM_ALL fmt_check_fail.cpp)
    target_link_libraries(fmt_check_fail_${fail_case} PRIVATE remote_fmt::remote_fmt remote_fmt::parser)
    target_add_default_build_options(fmt_check_fail_${fail_case} PRIVATE)
    target_compile_options(fmt_check_fail_${fail_case} PRIVATE ${fmt_sanitizer_ignorelist})
    target_compile_definitions(fmt_check_fail_${fail_case} PRIVATE REMOTE_FMT_USE_CATALOG=false
                                                                   REMOTE_FMT_FAIL_CASE=${fail_case})

    add_test(NAME fmt_check_rejects_${fail_case} COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target
                                                         fmt_check_fail_${fail_case} --config $<CONFIG>)
    # RESOURCE_LOCK: these drive the build system, so they must not run concurrently under "ctest -j".
    set_tests_properties(fmt_check_rejects_${fail_case} PROPERTIES WILL_FAIL TRUE RESOURCE_LOCK fmt_check_build)
endforeach()

# The cross-compiling branch of ../CMakeLists.txt uses fmt with fmt.patch applied and defines FMT_ASSERT on the command
# line (remote_fmt_fmt_assert_option: a call of fmt's own fmt::assert_fail, no header needed). Each header that reaches
# that fmt must compile alone with those definitions. The host branch above uses a plain fmt, so the patched one is
# fetched here on its own (_deps/fmt_patched-src; the other tests keep theirs) and each header is one TU, built by its
# test. Keep the definitions in step with that branch.
populate_package(
    fmt_patched
    QUIET
    GIT_REPOSITORY
    https://github.com/fmtlib/fmt.git
    GIT_TAG
    12.2.0
    GIT_BRANCH
    master
    PATCH_FILE
    ${remote_fmt_dir}/fmt.patch)

set(remote_fmt_fmt_headers
    fmt_check
    check
    describe
    formatters
    formatter_styled
    printer
    formatter_units
    remote_fmt)
foreach(header IN LISTS remote_fmt_fmt_headers)
    file(
        CONFIGURE
        OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/fmt_patched/${header}.cpp
        CONTENT
        "// generated by tests/CMakeLists.txt: remote_fmt/${header}.hpp alone against the patched fmt, with the definitions a cross build puts on every TU\n#include <remote_fmt/${header}.hpp>\n"
    )
endforeach()
foreach(header string_constant ${remote_fmt_fmt_headers})
    set(_target fmt_patched_${header})
    if(header STREQUAL "string_constant")
        set(_source fmt_patched_string_constant.cpp)
    else()
        set(_source ${CMAKE_CURRENT_BINARY_DIR}/fmt_patched/${header}.cpp)
    endif()
    add_library(${_target} OBJECT EXCLUDE_FROM_ALL ${_source})
    target_include_directories(${_target} SYSTEM PRIVATE ${fmt_patched_SOURCE_DIR}/include)
    target_include_directories(${_target} PRIVATE ${remote_fmt_dir}/src ${remote_fmt_dir}/string_constant/src)
    if(TARGET enchantum::enchantum)
        target_link_libraries(${_target} PRIVATE enchantum::enchantum)
    endif()
    target_compile_definitions(${_target} PRIVATE FMT_USE_LOCALE=0 FMT_UNICODE=0 FMT_STATIC_THOUSANDS_SEPARATOR=1
                                                  REMOTE_FMT_FMT_CHECK_FULL=1)
    target_compile_options(${_target} PRIVATE "${remote_fmt_fmt_assert_option}")
    target_add_default_build_options(${_target} PRIVATE)

    add_test(NAME remote_fmt_fmt_patch_self_contained_${header} COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                                                                        --target ${_target} --config $<CONFIG>)
    set_tests_properties(remote_fmt_fmt_patch_self_contained_${header} PROPERTIES RESOURCE_LOCK fmt_check_build)
endforeach()

# A failed FMT_ASSERT reaches fmt::assert_fail with the condition's text: the program defines it itself (as a firmware's
# runtime does, with assert()'s end) and makes fmt fail one check at run time.
add_executable(fmt_assert_path fmt_assert_path.cpp)
target_include_directories(fmt_assert_path SYSTEM PRIVATE ${fmt_patched_SOURCE_DIR}/include)
target_compile_definitions(fmt_assert_path PRIVATE FMT_USE_LOCALE=0 FMT_UNICODE=0 FMT_STATIC_THOUSANDS_SEPARATOR=1)
target_compile_options(fmt_assert_path PRIVATE "${remote_fmt_fmt_assert_option}")
target_add_default_build_options(fmt_assert_path PRIVATE)
add_test(NAME remote_fmt_fmt_assert_path COMMAND fmt_assert_path)
set_tests_properties(remote_fmt_fmt_assert_path PROPERTIES PASS_REGULAR_EXPRESSION
                                                           "assert path: .*value >= 0 base\\.h:[0-9]+ end")

# Fuzzing: enabled whenever the compiler ships libFuzzer (clang today, gcc once it supports -fsanitize=fuzzer).
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS -fsanitize=fuzzer-no-link)
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=fuzzer-no-link)
check_cxx_source_compiles("int main() { return 0; }" REMOTE_FMT_HAS_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)

# libFuzzer's crash handling is incompatible with TSan (signal-unsafe malloc aborts at startup), and the parser is
# single-threaded anyway - skip fuzzing in TSan builds.
if(REMOTE_FMT_HAS_LIBFUZZER AND NOT "${USE_SANITIZER}" STREQUAL "thread")
    # -fno-sanitize-recover: UBSan reports are print-and-continue by default, which lets the fuzzer (and CI) sail past
    # findings - make them fatal so an artifact is written. -fno-sanitize-coverage=stack-depth: libFuzzer's stack-depth
    # feature measures from the process's stack start, which ASLR moves, so the same input gave another feature set in
    # every process and a seeded run took another path each time (as in uc_log and aglio).
    function(remote_fmt_add_fuzzer name)
        add_executable(${name} ${ARGN})
        target_link_libraries(${name} PRIVATE remote_fmt::remote_fmt remote_fmt::parser)
        target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
        target_add_default_build_options(${name} PRIVATE)
        target_compile_options(${name} PRIVATE ${fmt_sanitizer_ignorelist})
        target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-coverage=stack-depth
                                               -fno-sanitize-recover=all)
        target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
    endfunction()

    # Decoder: arbitrary bytes in, must never crash or hang.
    remote_fmt_add_fuzzer(fuzz_parser fuzz_parser.cpp)

    # Encoder: fuzzer-chosen argument values through Printer, back through parse, compared against fmt. Catalog off so
    # the binary needs no linker-generated catalog object (same as test_roundtrip).
    remote_fmt_add_fuzzer(fuzz_roundtrip fuzz_roundtrip.cpp)
    target_compile_definitions(fuzz_roundtrip PRIVATE REMOTE_FMT_USE_CATALOG=false)

    # Descriptors: a descriptor and values of the fuzzer's choice through the parser's walker (an inline descriptor
    # comes off the wire), checked and unchecked, inline and cataloged with type references.
    remote_fmt_add_fuzzer(fuzz_descriptor fuzz_descriptor.cpp)
    target_compile_definitions(fuzz_descriptor PRIVATE REMOTE_FMT_USE_CATALOG=false)
    target_link_libraries(fuzz_descriptor PRIVATE glaze::glaze)

    add_executable(fuzz_corpus_generator corpus_generator.cpp)
    target_compile_definitions(fuzz_corpus_generator PRIVATE REMOTE_FMT_USE_CATALOG=false)
    target_link_libraries(fuzz_corpus_generator PRIVATE remote_fmt::remote_fmt remote_fmt::parser)
    target_include_directories(fuzz_corpus_generator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    target_add_default_build_options(fuzz_corpus_generator PRIVATE)
    target_compile_options(fuzz_corpus_generator PRIVATE ${fmt_sanitizer_ignorelist})

    add_test(NAME fuzz_corpus_generate COMMAND fuzz_corpus_generator ${CMAKE_CURRENT_BINARY_DIR}/corpus
                                               ${CMAKE_CURRENT_BINARY_DIR}/corpus_descriptor_seeds)
    set_tests_properties(fuzz_corpus_generate PROPERTIES FIXTURES_SETUP fuzz_corpus)

    # libFuzzer errors out if a corpus directory it is told to write to does not already exist, so create them at
    # configure time rather than relying on a test to do it.
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/corpus ${CMAKE_CURRENT_BINARY_DIR}/corpus_roundtrip
         ${CMAKE_CURRENT_BINARY_DIR}/corpus_descriptor ${CMAKE_CURRENT_BINARY_DIR}/corpus_descriptor_seeds
         ${CMAKE_CURRENT_BINARY_DIR}/corpus_parser_new)

    # corpus.tar.xz is the committed minimized corpus, ~4300 inputs. Without it every run restarts from the generated
    # seeds and spends its whole budget rediscovering ~3400 edges it already knew about (540 edges from seeds alone vs
    # 3902 from this corpus).
    #
    # It is committed as one archive rather than as loose files purely to keep a single tracked entry instead of
    # thousands. The trade-off is that git cannot delta a compressed blob, so each refresh costs its full ~210kB rather
    # than only the inputs that changed.
    #
    # Unpacked at configure time into the build tree, which also keeps it read-only: libFuzzer writes new inputs only
    # into the first corpus directory it is given, so the committed set and corpus_regressions/ are never modified.
    set(fuzz_corpus_committed ${CMAKE_CURRENT_BINARY_DIR}/corpus_committed)
    file(REMOVE_RECURSE ${fuzz_corpus_committed})
    file(MAKE_DIRECTORY ${fuzz_corpus_committed})
    file(ARCHIVE_EXTRACT INPUT ${CMAKE_CURRENT_SOURCE_DIR}/corpus.tar.xz DESTINATION ${fuzz_corpus_committed})

    # Re-pack after a fuzzing run that found new coverage. Minimize first, then pack the contents deterministically -
    # sorted, fixed timestamps, no ownership - so an unchanged corpus produces a byte-identical archive and no diff:
    #
    # ./build/fuzz_parser -merge=1 fresh build/corpus tests/corpus_regressions tar --sort=name --mtime='UTC 2020-01-01'
    # --owner=0 --group=0 --numeric-owner --format=gnu \ -C fresh -c . | xz -9e > tests/corpus.tar.xz The ctest smokes
    # are the same on every run (a fixed seed and number of runs, no clock; -use_cmp=0: compare tracing records pointer
    # operands, which ASLR changes, and feeds them into the mutations): each fuzzer's writable corpus - the first
    # directory, libFuzzer writes new inputs only there - is emptied first, so a run starts from the same inputs as the
    # last one. -timeout only guards a single hanging input. The long exploring runs are the CI job `fuzz-explore`.
    foreach(fuzz_new corpus_parser_new corpus_descriptor corpus_roundtrip)
        add_test(NAME fuzz_${fuzz_new}_clean COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_CURRENT_BINARY_DIR}/${fuzz_new})
        add_test(NAME fuzz_${fuzz_new}_mkdir COMMAND ${CMAKE_COMMAND} -E make_directory
                                                     ${CMAKE_CURRENT_BINARY_DIR}/${fuzz_new})
        set_tests_properties(fuzz_${fuzz_new}_clean fuzz_${fuzz_new}_mkdir PROPERTIES FIXTURES_SETUP fuzz_corpus)
        set_tests_properties(fuzz_${fuzz_new}_mkdir PROPERTIES DEPENDS fuzz_${fuzz_new}_clean)
    endforeach()

    # The parser: the generated seeds, the committed corpus and the regressions are read, not written. The six inputs of
    # corpus_regressions/ are frames without a 0x57/0x58 start: the parser skips them as noise, they stay as bytes it
    # must survive.
    add_test(
        NAME fuzz_parser_smoke
        COMMAND
            fuzz_parser -runs=20000 -seed=1 -use_cmp=0 -timeout=10 -dict=${CMAKE_CURRENT_SOURCE_DIR}/fuzz_parser.dict
            ${CMAKE_CURRENT_BINARY_DIR}/corpus_parser_new ${CMAKE_CURRENT_BINARY_DIR}/corpus ${fuzz_corpus_committed}
            ${CMAKE_CURRENT_SOURCE_DIR}/corpus_regressions)
    set_tests_properties(fuzz_parser_smoke PROPERTIES FIXTURES_REQUIRED fuzz_corpus)

    # The descriptor fuzzer from its generated seeds.
    add_test(NAME fuzz_descriptor_smoke
             COMMAND fuzz_descriptor -runs=20000 -seed=1 -use_cmp=0 -timeout=10
                     ${CMAKE_CURRENT_BINARY_DIR}/corpus_descriptor ${CMAKE_CURRENT_BINARY_DIR}/corpus_descriptor_seeds)
    set_tests_properties(fuzz_descriptor_smoke PROPERTIES FIXTURES_REQUIRED fuzz_corpus)

    # The round-trip fuzzer builds its own inputs from scratch, so it needs no seed corpus.
    add_test(NAME fuzz_roundtrip_smoke COMMAND fuzz_roundtrip -runs=20000 -seed=1 -use_cmp=0 -timeout=10
                                               ${CMAKE_CURRENT_BINARY_DIR}/corpus_roundtrip)
    set_tests_properties(fuzz_roundtrip_smoke PROPERTIES FIXTURES_REQUIRED fuzz_corpus)
else()
    message(STATUS "libFuzzer (-fsanitize=fuzzer) not supported by ${CMAKE_CXX_COMPILER_ID}; fuzz target disabled")
endif()
