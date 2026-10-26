#pragma once

// The switches of remote_fmt that every TU of one image must see with the same value: the optional formatter sets
// (each changes which formatters exist and which strings the catalog holds) and the encoder's knobs (each shapes every
// frame, and is a member of Printer<B>). The build sets them for the whole image (CMakeLists.txt's cross branch sets
// ENCHANTUM and STYLED, the library that brings mp-units sets MP_UNITS). A user without that build gets the defaults
// below, the formatter sets found by __has_include. Never set one per TU: tools/extract_sites.py fails the link of an
// image whose TUs disagree (the markers at the end of this file).

// mp-units: a quantity and a quantity_point are formattable like any other argument (formatter_units.hpp's
// formatters). 0 keeps the dependency out even where the headers are reachable.
#ifndef REMOTE_FMT_USE_MP_UNITS
    #if __has_include(<mp-units/framework/quantity.h>)                                             \
      && __has_include(<mp-units/framework/quantity_point.h>)                                      \
      && __has_include(<mp-units/framework/unit.h>)
        #define REMOTE_FMT_USE_MP_UNITS 1
    #else
        #define REMOTE_FMT_USE_MP_UNITS 0
    #endif
#endif

// enchantum: enums print by name, bit-flag enums as a name set (else as their number).
#ifndef REMOTE_FMT_USE_ENCHANTUM
    #if __has_include(<enchantum/enchantum.hpp>)
        #define REMOTE_FMT_USE_ENCHANTUM 1
    #else
        #define REMOTE_FMT_USE_ENCHANTUM 0
    #endif
#endif

// fmt/color.h: fmt::styled(...) arguments keep their style on the host.
#ifndef REMOTE_FMT_USE_STYLED
    #if __has_include(<fmt/color.h>)
        #define REMOTE_FMT_USE_STYLED 1
    #else
        #define REMOTE_FMT_USE_STYLED 0
    #endif
#endif

// Every frame carries a CRC-8 (wire.hpp protocol::crcOver). On by default: a damaged frame is reported as damaged,
// never shown as text. Without the type bytes the CRC is the frame's only check besides its id and its length, so 0 is
// for measurements.
#ifndef REMOTE_FMT_FRAME_CHECK
    #define REMOTE_FMT_FRAME_CHECK 1
#endif

// A message is gathered in this many bytes on the stack and handed to the backend in one write; more is written in
// pieces. 0 writes every field on its own.
#ifndef REMOTE_FMT_STAGING_BYTES
    #define REMOTE_FMT_STAGING_BYTES 32
#endif

// REMOTE_FMT_USE_CATALOG (check.hpp useCatalog, default true): false sends every string inline.

// One byte per switch and value in the call-site section (not loaded on a target): tools/extract_sites.py fails the
// post-link step when an image holds two values of one switch, i.e. two of its TUs were compiled with different
// definitions - a silent ODR violation otherwise - and reads FRAME_CHECK's into the catalog's "FrameCheck". The name
// is REMOTE_FMT_<name>, the value a decimal literal (a value written otherwise is not compared).
// clang-format off: the formatter reads a marker as a declaration and puts each argument on a line of its own
#define REMOTE_FMT_DETAIL_CFG_CAT2(a, b) a##b
#define REMOTE_FMT_DETAIL_CFG_CAT(a, b)  REMOTE_FMT_DETAIL_CFG_CAT2(a, b)
#define REMOTE_FMT_DETAIL_CFG_MARKER(name, value)                                                  \
    [[gnu::used, gnu::section("remote_fmt_sites.cfg_" #name)]] inline char REMOTE_FMT_DETAIL_CFG_CAT( \
      REMOTE_FMT_DETAIL_CFG_CAT(remote_fmt_cfg_##name##_, value),                                  \
      _) = 1;

extern "C" {
REMOTE_FMT_DETAIL_CFG_MARKER(USE_MP_UNITS, REMOTE_FMT_USE_MP_UNITS)
REMOTE_FMT_DETAIL_CFG_MARKER(USE_ENCHANTUM, REMOTE_FMT_USE_ENCHANTUM)
REMOTE_FMT_DETAIL_CFG_MARKER(USE_STYLED, REMOTE_FMT_USE_STYLED)
REMOTE_FMT_DETAIL_CFG_MARKER(FRAME_CHECK, REMOTE_FMT_FRAME_CHECK)
REMOTE_FMT_DETAIL_CFG_MARKER(STAGING_BYTES, REMOTE_FMT_STAGING_BYTES)
#if defined(REMOTE_FMT_USE_CATALOG) && !(REMOTE_FMT_USE_CATALOG)
REMOTE_FMT_DETAIL_CFG_MARKER(USE_CATALOG, 0)
#else
REMOTE_FMT_DETAIL_CFG_MARKER(USE_CATALOG, 1)
#endif
}
// clang-format on

#undef REMOTE_FMT_DETAIL_CFG_MARKER
#undef REMOTE_FMT_DETAIL_CFG_CAT
#undef REMOTE_FMT_DETAIL_CFG_CAT2
