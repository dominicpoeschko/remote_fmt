#pragma once

// remote_fmt's encoder, for the target and the host: every layer, each on the ones before it (README "Layers"). The
// decoder is parser.hpp, which includes none of this.

#include "catalog.hpp"
#include "config.hpp"
#include "descriptor.hpp"
#include "format_grammar.hpp"
#include "wire.hpp"

#include <string_constant/string_constant.hpp>

// the kinds of argument
#include "traits.hpp"
// fmt's own checker on the types the host renders
#include "fmt_check.hpp"
// checkFormatString, useCatalog
#include "check.hpp"
// the formatter template, the descriptors of argument types, detail::Access
#include "describe.hpp"
// the formatters of the standard types
#include "formatters.hpp"
// fmt::styled (REMOTE_FMT_USE_STYLED)
#include "formatter_styled.hpp"
// Printer, format_to
#include "printer.hpp"
// mp-units (REMOTE_FMT_USE_MP_UNITS)
#include "formatter_units.hpp"
