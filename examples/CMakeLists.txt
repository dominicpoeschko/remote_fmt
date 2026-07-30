cmake_minimum_required(VERSION 3.28)

project(remote_fmt_examples VERSION 0.1.0)

# Built against the checked-out tree rather than fetched from the remote: fetching master meant the examples never
# compiled the code they ship with, which is the point of building them in CI.
add_subdirectory(../ ${CMAKE_CURRENT_BINARY_DIR}/remote_fmt)

function(remote_fmt_add_example name source)
    add_executable(${name} ${source})
    target_compile_features(${name} PRIVATE cxx_std_23)
    # fmt arrives through remote_fmt::parser, which the examples use directly as well.
    target_link_libraries(${name} PRIVATE remote_fmt::remote_fmt remote_fmt::parser)
endfunction()

remote_fmt_add_example(basic basic_usage.cpp)
remote_fmt_add_example(catalog catalog.cpp)

remote_fmt_add_example(catalog_generator catalog_generator.cpp)
target_generate_string_constants(catalog_generator)
