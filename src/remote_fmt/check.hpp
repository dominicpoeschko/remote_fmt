#pragma once

// The compile-time check of a format string against its arguments - the grammar (format_grammar.hpp), the characters
// (wire.hpp) and fmt's own checker (fmt_check.hpp) - and whether this image sends its strings as catalog ids. Emits no
// code.

#include "fmt_check.hpp"
#include "format_grammar.hpp"
#include "wire.hpp"

#include <cstddef>
#include <string_constant/string_constant.hpp>
#include <string_view>

namespace remote_fmt {

#ifndef REMOTE_FMT_USE_CATALOG
inline constexpr bool useCatalog = true;
#else
inline constexpr bool useCatalog{REMOTE_FMT_USE_CATALOG};
#endif

namespace detail {

    // never defined: the call fails constant evaluation, turning a failed assert into a compile error
    void compileTimeAssertFailed(char const* message);

    template<std::size_t N>
    consteval void compileTimeAssert(char const (&str)[N],
                                     bool predicate) {
        if(!predicate) { compileTimeAssertFailed(str); }
    }
}   // namespace detail

template<typename... Args,
         char... Chars>
consteval void checkFormatString(sc::StringConstant<Chars...>) {
    auto constexpr stringView = std::string_view{sc::StringConstant<Chars...>{}};
    detail::compileTimeAssert("invalid chars in format",
                              useCatalog ? detail::allCharsValidCataloged(stringView)
                                         : detail::allCharsValid(stringView));
    // one walk of the grammar the parser uses (format_grammar.hpp)
    auto constexpr shape = detail::formatShape(stringView);
    detail::compileTimeAssert("invalid replacement field count",
                              shape.wellFormed && shape.fields == sizeof...(Args));
    // One argument per field, in order, and no argument index on the wire - so an index above 0
    // makes the host report "argument not found" and drop the whole line. fmt accepts "{1} {0}",
    // so its checker does not catch this. A nested brace is refused too: a dynamic width needs a
    // second argument the protocol cannot send.
    detail::compileTimeAssert("argument id or dynamic spec in replacement field",
                              shape.plainFields);
    detail::checkFormatStringWithFmt<Args...>(sc::StringConstant<Chars...>{});
}

}   // namespace remote_fmt
