cmake_minimum_required(VERSION 3.18)

project(remote_fmt VERSION 0.1.0)

include(FetchContent)

FetchContent_Declare(
  magic_enum
  GIT_REPOSITORY https://github.com/Neargye/magic_enum
  GIT_TAG master)
FetchContent_MakeAvailable(magic_enum)

add_library(remote_fmt INTERFACE)
add_subdirectory(string_constant)

target_include_directories(remote_fmt INTERFACE src)
target_link_libraries(
  remote_fmt INTERFACE string_constant::string_constant fmt::fmt-header-only
                       magic_enum::magic_enum)

add_library(remote_fmt::remote_fmt ALIAS remote_fmt)

if(NOT CMAKE_CROSSCOMPILING)
  include(cmake_helpers/FindOrFetch.cmake)
  find_or_fetch_package(fmt 9 GIT_REPOSITORY https://github.com/fmtlib/fmt.git
                        GIT_TAG master)
else()
  include(FetchContent)
  FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG master)
  FetchContent_GetProperties(fmt)
  FetchContent_Populate(fmt)
  add_subdirectory(${fmt_SOURCE_DIR} ${fmt_BINARY_DIR} EXCLUDE_FROM_ALL)

  target_compile_definitions(remote_fmt
      INTERFACE FMT_STATIC_THOUSANDS_SEPARATOR=1)

  target_compile_options(remote_fmt INTERFACE "-DFMT_ASSERT(condition, message)=assert(condition)")

endif()

if(NOT CMAKE_CROSSCOMPILING)
  add_library(remote_fmt_parser INTERFACE)

  target_include_directories(remote_fmt_parser INTERFACE src)
  target_link_libraries(remote_fmt_parser INTERFACE fmt::fmt magic_enum::magic_enum)

  add_library(remote_fmt::parser ALIAS remote_fmt_parser)
endif()
