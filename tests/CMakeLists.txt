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

enable_testing()

function(remote_fmt_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE remote_fmt::remote_fmt remote_fmt::parser)
    target_add_default_build_options(${name} PRIVATE)
    target_compile_options(${name} PRIVATE ${fmt_sanitizer_ignorelist})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

remote_fmt_add_test(test_roundtrip roundtrip_tests.cpp)
target_compile_definitions(test_roundtrip PRIVATE REMOTE_FMT_USE_CATALOG=false)

# The fmt-parity tests instantiate fixed-point specs ("{:.0f}", "{:.1100f}"), and gcc then reports "writing 2 bytes into
# a region of size 0" inside fmt's own write_fixed/write_padded. It is a false positive - the buffer is sized by a
# precision gcc cannot see through - and there is nothing to fix on this side, so it is silenced for this one target
# rather than dropped from the tests: fixed-point formatting is where fmt's bigint and dragonbox paths live, which is
# where all three committed corpus_regressions came from.
#
# The flag has to be on the link options as well. The default build options enable -flto, so the codegen that produces
# this warning runs at link time, where compile options no longer apply - and for the same reason a #pragma diagnostic
# region inside the source file does not reach it either.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(test_roundtrip PRIVATE -Wno-stringop-overflow)
    target_link_options(test_roundtrip PRIVATE -Wno-stringop-overflow)
endif()

remote_fmt_add_test(test_catalog catalog_tests.cpp)

remote_fmt_add_test(test_fmt_check fmt_check_tests.cpp)
target_compile_definitions(test_fmt_check PRIVATE REMOTE_FMT_USE_CATALOG=false)

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
    13)

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
    # findings - make them fatal so an artifact is written.
    function(remote_fmt_add_fuzzer name)
        add_executable(${name} ${ARGN})
        target_link_libraries(${name} PRIVATE remote_fmt::remote_fmt remote_fmt::parser)
        target_add_default_build_options(${name} PRIVATE)
        target_compile_options(${name} PRIVATE ${fmt_sanitizer_ignorelist})
        target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
        target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
    endfunction()

    # Decoder: arbitrary bytes in, must never crash or hang.
    remote_fmt_add_fuzzer(fuzz_parser fuzz_parser.cpp)

    # Encoder: fuzzer-chosen argument values through Printer, back through parse, compared against fmt. Catalog off so
    # the binary needs no linker-generated catalog object (same as test_roundtrip).
    remote_fmt_add_fuzzer(fuzz_roundtrip fuzz_roundtrip.cpp)
    target_compile_definitions(fuzz_roundtrip PRIVATE REMOTE_FMT_USE_CATALOG=false)

    add_executable(fuzz_corpus_generator corpus_generator.cpp)
    target_compile_definitions(fuzz_corpus_generator PRIVATE REMOTE_FMT_USE_CATALOG=false)
    target_link_libraries(fuzz_corpus_generator PRIVATE remote_fmt::remote_fmt remote_fmt::parser)
    target_add_default_build_options(fuzz_corpus_generator PRIVATE)
    target_compile_options(fuzz_corpus_generator PRIVATE ${fmt_sanitizer_ignorelist})

    add_test(NAME fuzz_corpus_generate COMMAND fuzz_corpus_generator ${CMAKE_CURRENT_BINARY_DIR}/corpus)
    set_tests_properties(fuzz_corpus_generate PROPERTIES FIXTURES_SETUP fuzz_corpus)

    # libFuzzer errors out if a corpus directory it is told to write to does not already exist, so create them at
    # configure time rather than relying on a test to do it.
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/corpus ${CMAKE_CURRENT_BINARY_DIR}/corpus_roundtrip)

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
    # --owner=0 --group=0 --numeric-owner --format=gnu \ -C fresh -c . | xz -9e > tests/corpus.tar.xz
    add_test(
        NAME fuzz_parser_smoke
        COMMAND
            fuzz_parser -max_total_time=10 -dict=${CMAKE_CURRENT_SOURCE_DIR}/fuzz_parser.dict
            ${CMAKE_CURRENT_BINARY_DIR}/corpus ${fuzz_corpus_committed} ${CMAKE_CURRENT_SOURCE_DIR}/corpus_regressions)
    set_tests_properties(fuzz_parser_smoke PROPERTIES FIXTURES_REQUIRED fuzz_corpus)

    # The round-trip fuzzer builds its own inputs from scratch, so it needs no seed corpus.
    add_test(NAME fuzz_roundtrip_smoke COMMAND fuzz_roundtrip -max_total_time=10
                                               ${CMAKE_CURRENT_BINARY_DIR}/corpus_roundtrip)
else()
    message(STATUS "libFuzzer (-fsanitize=fuzzer) not supported by ${CMAKE_CXX_COMPILER_ID}; fuzz target disabled")
endif()
