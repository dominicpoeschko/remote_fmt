#pragma once

// What a formatter declares and what it writes through: the formatter template, the descriptor of an argument type
// (descriptorOf, described, typeDescriptor/typeEntry, subFormatDescriptor; descriptor.hpp's grammar) and
// detail::Access, the way of remote_fmt's own formatters into the Printer's raw fields.

#include "check.hpp"
#include "descriptor.hpp"

#include <concepts>
#include <cstdint>
#include <string_constant/string_constant.hpp>
#include <type_traits>
#include <utility>

namespace remote_fmt {

template<typename T>
struct formatter;

// The descriptor of T's values on the wire (descriptor.hpp): what T's formatter declares, `*` (self-described: the
// formatter writes through format_to, which sends a sub format string with its own descriptor) when it declares none.
template<typename T>
consteval auto descriptorOf() {
    using F = formatter<std::remove_cvref_t<T>>;
    if constexpr(requires { F::descriptor(); }) {
        return F::descriptor();
    } else {
        return sc::StringConstant<'*'>{};
    }
}

// A format string with the descriptors of its arguments: what a call site's tag names and what the catalog holds
// for a format string. REMOTE_FMT_SITE()(described<Args...>(fmt)) is a call site's id.
template<typename... Args,
         char... Chars>
consteval auto described(sc::StringConstant<Chars...> fmt) {
    return ((fmt + sc::StringConstant<desc::separator>{}) + ... + descriptorOf<Args>());
}

// A type entry (enums, registers, reflected structs): with a catalog the descriptor is the entry's key and the text
// travels once per image (detail::typeTag); without one the text is inline. The formatter calls typeEntry<Text>() in
// its format (which every image that sends the type instantiates) and returns typeDescriptor<Text>().
template<typename Text>
consteval auto typeDescriptor() {
    if constexpr(useCatalog) {
        return desc::typeKey<Text>();
    } else {
        return Text{};
    }
}

template<typename Text>
[[gnu::always_inline]] inline void typeEntry() {
    if constexpr(useCatalog) { detail::typeTag<Text>(); }
}

// `F<n>:<fmt><k>:<D>...`: a sub format string whose arguments' descriptors are static - the format string never
// travels, only the arguments (formatArguments). For a formatter that always writes the same sub format (aglio's
// reflected structs).
template<typename... Args,
         char... Chars>
consteval auto subFormatDescriptor(sc::StringConstant<Chars...> fmt) {
    checkFormatString<Args...>(fmt);
    return ((sc::StringConstant<'F'>{} + desc::counted(fmt) + desc::number<sizeof...(Args)>()
             + sc::StringConstant<':'>{})
            + ... + descriptorOf<Args>());
}

namespace detail {
    // The wire-level members of Printer (raw fields, varints, string ids) are private; remote_fmt's own formatters
    // reach them through here, the one friend. A formatter outside remote_fmt declares a descriptor and writes
    // through value(x) / formatArguments(xs...), or declares none and writes through format_to(printer, fmt, args...).
    struct Access {
        // one fixed-width field: a raw value of 1, 2, 4 or 8 bytes
        template<typename P,
                 typename Value>
        [[gnu::always_inline]] static constexpr void printHelper(P&      printer,
                                                                 Value&& value) {
            printer.printHelper(std::forward<Value>(value));
        }

        template<typename P,
                 typename Bytes>
        [[gnu::always_inline]] static constexpr void lowprint(P&           printer,
                                                              Bytes const& bytes) {
            printer.lowprint(bytes);
        }

        // an integer as LEB128, zigzag for a signed one
        template<typename P,
                 std::integral T>
        [[gnu::always_inline]] static constexpr void varint(P& printer,
                                                            T  value) {
            if constexpr(std::is_signed_v<T>) {
                using U = std::make_unsigned_t<T>;
                // zigzag: the top bit is shifted out anyway - masked first, so no bit leaves the type
                auto const u = static_cast<U>(static_cast<U>(value)
                                              & static_cast<U>(static_cast<U>(~U{}) >> 1U));
                varint(printer,
                       static_cast<U>(static_cast<U>(u << 1U)
                                      ^ static_cast<U>(value < 0 ? static_cast<U>(~U{}) : U{})));
            } else if constexpr(sizeof(T) <= 4) {
                printer.varint32(static_cast<std::uint32_t>(value));
            } else {
                printer.varint64(static_cast<std::uint64_t>(value));
            }
        }
    };
}   // namespace detail

}   // namespace remote_fmt
