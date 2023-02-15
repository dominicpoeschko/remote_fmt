cmake_minimum_required(VERSION 3.18)


project(remote_fmt_examples VERSION 0.1.0)

find_package(fmt)

include(FetchContent)
FetchContent_Declare(
    remote_fmt
    GIT_REPOSITORY git@github.com:dominicpoeschko/remote_fmt.git
    GIT_TAG master
)
FetchContent_MakeAvailable(remote_fmt)

FetchContent_Declare(
  magic_enum
  GIT_REPOSITORY https://github.com/Neargye/magic_enum
  GIT_TAG master)
FetchContent_MakeAvailable(magic_enum)

set(CMAKE_CXX_FLAGS -std=c++20)

add_executable(basic basic_usage.cpp)
target_compile_options(basic PUBLIC -std=c++20)
target_link_libraries(basic
                    remote_fmt::remote_fmt
                    remote_fmt::parser
                    fmt::fmt-header-only 
                    string_constant::string_constant
                    magic_enum::magic_enum)

add_executable(catalog catalog.cpp)
target_compile_options(catalog PUBLIC -std=c++20)
target_link_libraries(catalog 
                    remote_fmt::remote_fmt
                    remote_fmt::parser
                    fmt::fmt-header-only 
                    string_constant::string_constant
                    magic_enum::magic_enum)

add_executable(catalog_generator catalog_generator.cpp)
target_compile_options(catalog_generator PUBLIC -std=c++20)
target_generate_string_constants(catalog_generator)
target_link_libraries(catalog_generator 
                    remote_fmt::remote_fmt
                    remote_fmt::parser
                    fmt::fmt-header-only 
                    string_constant::string_constant
                    magic_enum::magic_enum)


