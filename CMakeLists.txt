cmake_minimum_required(VERSION 3.18)

project(remote_fmt VERSION 0.1.0)

set(remote_fmt_dir
    ${CMAKE_CURRENT_LIST_DIR}
    CACHE INTERNAL "")

add_library(remote_fmt INTERFACE)

if(NOT CMAKE_CROSSCOMPILING)
  include(cmake_helpers/FindOrFetch.cmake)
  find_or_fetch_package(fmt 9 GIT_REPOSITORY https://github.com/fmtlib/fmt.git
                        GIT_TAG master)
  find_or_fetch_package(nlohmann_json 3.9 GIT_REPOSITORY
                        https://github.com/nlohmann/json GIT_TAG master)
  find_or_fetch_package(magic_enum 0.9.6 GIT_REPOSITORY
                        https://github.com/Neargye/magic_enum GIT_TAG v0.9.6)
else()
  include(FetchContent)
  FetchContent_Populate(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG master
    )
  add_subdirectory(${fmt_SOURCE_DIR} ${fmt_BINARY_DIR} EXCLUDE_FROM_ALL)

  target_compile_definitions(remote_fmt
                             INTERFACE FMT_STATIC_THOUSANDS_SEPARATOR=1)

  target_compile_options(
    remote_fmt INTERFACE "-DFMT_ASSERT(condition, message)=assert(condition)")

  FetchContent_Declare(
    magic_enum
    GIT_REPOSITORY https://github.com/Neargye/magic_enum
    GIT_TAG master
    GIT_SHALLOW TRUE)

  FetchContent_MakeAvailable(magic_enum)
endif()

add_subdirectory(string_constant)

target_include_directories(remote_fmt INTERFACE src)
target_link_libraries(
  remote_fmt INTERFACE string_constant::string_constant fmt::fmt-header-only
                       magic_enum::magic_enum)

add_library(remote_fmt::remote_fmt ALIAS remote_fmt)

if(NOT CMAKE_CROSSCOMPILING)
  add_library(remote_fmt_parser INTERFACE)

  target_include_directories(remote_fmt_parser INTERFACE src)
  target_link_libraries(
    remote_fmt_parser INTERFACE fmt::fmt magic_enum::magic_enum
                                nlohmann_json::nlohmann_json)

  add_library(remote_fmt::parser ALIAS remote_fmt_parser)
endif()

function(target_generate_string_constants targetname)

  cmake_parse_arguments(PARSE_ARGV 1 PARSED_ARGS "" "STRING_CONSTANTS_MAP_FILE"
                        "")

  if(PARSED_ARGS_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "unknown argument ${PARSED_ARGS_UNPARSED_ARGUMENTS}")
  endif()

  if(NOT PARSED_ARGS_STRING_CONSTANTS_MAP_FILE)
    set(PARSED_ARGS_STRING_CONSTANTS_MAP_FILE
        "${targetname}_string_constants.json")
  endif()

  get_target_property(bin_dir ${targetname} BINARY_DIR)

  target_link_options(${targetname} PUBLIC
                      ${bin_dir}/${targetname}_string_constants.obj)

  get_target_property(options_flags ${targetname} COMPILE_OPTIONS)

  if("${options_flags}" STREQUAL "options_flags_NOTFOUND")
    string(JOIN " " flags ${CMAKE_CXX_FLAGS})
  else()
    string(JOIN " " flags ${CMAKE_CXX_FLAGS} ${options_flags})
  endif()

  find_package(Python3 COMPONENTS Interpreter REQUIRED)
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
      --objects
      $<TARGET_OBJECTS:${targetname}>)

  if(NOT "${flags}" STREQUAL "")
    list(APPEND command --flags=${flags})
  endif()

  add_custom_command(
    TARGET ${targetname}
    PRE_LINK COMMAND_EXPAND_LISTS
    COMMAND
      ${command} DEPENDS ${remote_fmt_dir}/tools/generate_string_constants.py
      BYPRODUCS ${bin_dir}/${targetname}_string_constants.cpp
      ${bin_dir}/${targetname}_string_constants.obj
      ${bin_dir}/${targetname}_string_constants.json)

  set_property(
    TARGET ${targetname}
    APPEND
    PROPERTY LINK_DEPENDS ${remote_fmt_dir}/tools/generate_string_constants.py)
  set_property(
    TARGET ${targetname}
    APPEND
    PROPERTY ADDITIONAL_CLEAN_FILES
             ${bin_dir}/${targetname}_string_constants.cpp)
  set_property(
    TARGET ${targetname}
    APPEND
    PROPERTY ADDITIONAL_CLEAN_FILES
             ${bin_dir}/${targetname}_string_constants.obj)
  set_property(
    TARGET ${targetname}
    APPEND
    PROPERTY ADDITIONAL_CLEAN_FILES
             ${bin_dir}/${targetname}_string_constants.json)
endfunction()
