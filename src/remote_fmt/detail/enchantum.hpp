#pragma once

// enchantum behind the one pragma wall it needs (its own code trips these under this project's warning flags): every
// remote_fmt header that reads enum names includes enchantum through here.

#ifdef __GNUC__
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wextra-semi"
    #pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    #pragma clang diagnostic ignored "-Wnewline-eof"
    #pragma clang diagnostic ignored "-Wsign-conversion"
#endif

#include <enchantum/algorithms.hpp>
#include <enchantum/bitflags.hpp>
#include <enchantum/enchantum.hpp>

#ifdef __GNUC__
    #pragma GCC diagnostic pop
#endif
#ifdef __clang__
    #pragma clang diagnostic pop
#endif
