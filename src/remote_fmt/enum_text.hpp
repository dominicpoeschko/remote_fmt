#pragma once

// An enum's names, for the enum formatter (formatters.hpp): read with enchantum (REMOTE_FMT_USE_ENCHANTUM) into the
// text of a type entry. Without enchantum an enum goes out as its number and this header holds nothing.

#include "config.hpp"
#include "descriptor.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#if REMOTE_FMT_USE_ENCHANTUM
    #include "detail/enchantum.hpp"
#endif

#if REMOTE_FMT_USE_ENCHANTUM
namespace remote_fmt { namespace detail {
    // An enum's type entry: `E<D><k>:<value>,<n>:<name>...` (G for a bitflag enum), the enumerators in enchantum's
    // order. The host names the raw value: an unknown value - or an unknown bit of a bitflag enum - prints as the
    // number, as before.
    template<typename E, typename D>
    struct EnumText {
        static constexpr void build(desc::TextBuilder& b) {
            using U = std::underlying_type_t<E>;
            b.put(enchantum::is_bitflag<E> ? 'G' : 'E');
            b.put(std::string_view{D{}});
            b.num(enchantum::count<E>);
            b.put(':');
            for(std::size_t i = 0; i != enchantum::count<E>; ++i) {
                auto const v = static_cast<U>(enchantum::values<E>[i]);
                if constexpr(std::is_signed_v<U>) {
                    b.signedNum(static_cast<std::int64_t>(v));
                } else {
                    b.num(static_cast<std::uint64_t>(v));
                }
                b.put(',');
                b.counted(enchantum::names<E>[i]);
            }
        }
    };
}}   // namespace remote_fmt::detail
#endif
