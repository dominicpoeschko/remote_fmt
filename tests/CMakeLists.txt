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

# Propagate stdlib/sanitizer flags into fetched dependencies so libc++/debug-ABI builds link.
foreach(dep_target fmt)
    if(TARGET ${dep_target})
        get_target_property(_imported ${dep_target} IMPORTED)
        if(NOT _imported)
            target_add_stdlib_flags(${dep_target})
            target_add_stdlib_debug_flags(${dep_target})
            target_add_sanitizer_flags(${dep_target} PUBLIC)
            target_add_optimizer_flags(${dep_target} PUBLIC)
        endif()
    endif()
endforeach()

enable_testing()

function(remote_fmt_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE remote_fmt::remote_fmt remote_fmt::parser)
    target_add_default_build_options(${name} PRIVATE)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

remote_fmt_add_test(test_roundtrip roundtrip_tests.cpp)
target_compile_definitions(test_roundtrip PRIVATE REMOTE_FMT_USE_CATALOG=false)

remote_fmt_add_test(test_catalog catalog_tests.cpp)

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
    add_executable(fuzz_parser fuzz_parser.cpp)
    target_link_libraries(fuzz_parser PRIVATE remote_fmt::remote_fmt remote_fmt::parser)
    target_add_default_build_options(fuzz_parser PRIVATE)
    # -fno-sanitize-recover: UBSan reports are print-and-continue by default, which lets the fuzzer (and CI) sail past
    # findings - make them fatal so an artifact is written.
    target_compile_options(fuzz_parser PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
    target_link_options(fuzz_parser PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)

    add_executable(fuzz_corpus_generator corpus_generator.cpp)
    target_compile_definitions(fuzz_corpus_generator PRIVATE REMOTE_FMT_USE_CATALOG=false)
    target_link_libraries(fuzz_corpus_generator PRIVATE remote_fmt::remote_fmt remote_fmt::parser)
    target_add_default_build_options(fuzz_corpus_generator PRIVATE)

    add_test(NAME fuzz_corpus_generate COMMAND fuzz_corpus_generator ${CMAKE_CURRENT_BINARY_DIR}/corpus)
    set_tests_properties(fuzz_corpus_generate PROPERTIES FIXTURES_SETUP fuzz_corpus)

    # corpus_regressions holds committed inputs from past findings; libFuzzer reads extra corpus directories after the
    # first one without writing to them.
    add_test(NAME fuzz_parser_smoke COMMAND fuzz_parser -max_total_time=10 ${CMAKE_CURRENT_BINARY_DIR}/corpus
                                            ${CMAKE_CURRENT_SOURCE_DIR}/corpus_regressions)
    set_tests_properties(fuzz_parser_smoke PROPERTIES FIXTURES_REQUIRED fuzz_corpus)
else()
    message(STATUS "libFuzzer (-fsanitize=fuzzer) not supported by ${CMAKE_CXX_COMPILER_ID}; fuzz target disabled")
endif()
