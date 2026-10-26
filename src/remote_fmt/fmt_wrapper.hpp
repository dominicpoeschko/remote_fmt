#pragma once

// fmt for the host side (the parser, the printers): every header it renders with, behind one pragma wall. fmt/color.h
// always: the parser decodes a styled value (`Y`) whatever the encoder's REMOTE_FMT_USE_STYLED.

#include "remote_fmt/detail/fmt_wall.hpp"

REMOTE_FMT_DETAIL_FMT_WALL_BEGIN
#include <fmt/chrono.h>
#include <fmt/color.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <fmt/std.h>
REMOTE_FMT_DETAIL_FMT_WALL_END
