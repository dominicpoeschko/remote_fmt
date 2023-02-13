cmake_minimum_required(VERSION 3.18)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(remote_fmt_examples VERSION 0.1.0)

find_package(fmt)

include_directories(../src)

include(FetchContent)
FetchContent_Declare(
    string_constant
    GIT_REPOSITORY git@github.com:dominicpoeschko/string_constant.git
    GIT_TAG master
)
FetchContent_MakeAvailable(string_constant)

FetchContent_Declare(
  magic_enum
  GIT_REPOSITORY https://github.com/Neargye/magic_enum
  GIT_TAG master)
FetchContent_MakeAvailable(magic_enum)


add_executable(basic basic_usage.cpp)
target_link_libraries(basic 
                    fmt::fmt-header-only 
                    string_constant::string_constant
                    magic_enum::magic_enum)

add_executable(catalog catalog.cpp)
target_link_libraries(catalog 
                    fmt::fmt-header-only 
                    string_constant::string_constant
                    magic_enum::magic_enum)