#pragma once

// The one pragma wall around fmt's headers: what fmt's own code trips under this project's warning flags (clang's
// -Weverything list, gcc's -Wsign-conversion), silenced for fmt and nothing else. Every remote_fmt header that
// includes fmt puts its includes between the two:
//
//     REMOTE_FMT_DETAIL_FMT_WALL_BEGIN
//     #include <fmt/format.h>
//     REMOTE_FMT_DETAIL_FMT_WALL_END
//
// A consumer whose fmt is no SYSTEM include (the host builds) stays silent too.

#ifdef __clang__
    #define REMOTE_FMT_DETAIL_FMT_WALL_BEGIN                                                       \
        _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wsign-conversion\"") \
          _Pragma("clang diagnostic ignored \"-Wunsafe-buffer-usage\"")                            \
            _Pragma("clang diagnostic ignored \"-Wunsafe-buffer-usage-in-libc-call\"")             \
              _Pragma("clang diagnostic ignored \"-Wswitch-default\"")                             \
                _Pragma("clang diagnostic ignored \"-Wswitch-enum\"")                              \
                  _Pragma("clang diagnostic ignored \"-Wfloat-equal\"")                            \
                    _Pragma("clang diagnostic ignored \"-Wreserved-macro-identifier\"")            \
                      _Pragma("clang diagnostic ignored \"-Wduplicate-enum\"") _Pragma(            \
                        "clang diagnostic ignored \"-Wglobal-constructors\"")                      \
                        _Pragma("clang diagnostic ignored \"-Wundefined-func-template\"") _Pragma( \
                          "clang diagnostic ignored \"-Wextra-semi\"")                             \
                          _Pragma("clang diagnostic ignored \"-Wextra-semi-stmt\"") _Pragma(       \
                            "clang diagnostic ignored \"-Wmissing-noreturn\"")                     \
                            _Pragma("clang diagnostic ignored \"-Wweak-vtables\"") _Pragma(        \
                              "clang diagnostic ignored \"-Wunused-parameter\"")                   \
                              _Pragma("clang diagnostic ignored \"-Wshorten-64-to-32\"") _Pragma(  \
                                "clang diagnostic ignored \"-Wdeprecated-declarations\"")          \
                                _Pragma("clang diagnostic ignored \"-Wdocumentation\"") _Pragma(   \
                                  "clang diagnostic ignored \"-Wdocumentation-unknown-command\"")
    #define REMOTE_FMT_DETAIL_FMT_WALL_END _Pragma("clang diagnostic pop")
#elifdef __GNUC__
    #define REMOTE_FMT_DETAIL_FMT_WALL_BEGIN                                                   \
        _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wsign-conversion\"")
    #define REMOTE_FMT_DETAIL_FMT_WALL_END _Pragma("GCC diagnostic pop")
#else
    #define REMOTE_FMT_DETAIL_FMT_WALL_BEGIN
    #define REMOTE_FMT_DETAIL_FMT_WALL_END
#endif
