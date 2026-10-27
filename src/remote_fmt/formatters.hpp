#pragma once

// The formatters of the standard types: integers, floats, pointers, durations and time points, bytes, strings and
// string constants, enums (by name with enchantum, enum_text.hpp), ranges, tuples, optional, void, expected, variant.
// Each declares the descriptor of its values (describe.hpp) next to the code that writes them.

#include "config.hpp"
#include "describe.hpp"
#include "descriptor.hpp"
#include "enum_text.hpp"
#include "traits.hpp"

#include <bit>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string_constant/string_constant.hpp>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace remote_fmt {

template<std::integral T>
struct formatter<T> {
    static_assert(8 >= sizeof(T),
                  "bad type: no [u]int128_t");
    static_assert(1 == sizeof(char),
                  "bad type: only 1 byte char");

    static consteval auto descriptor() {
        if constexpr(std::is_same_v<bool, T>) {
            return sc::StringConstant<'b'>{};
        } else if constexpr(std::is_same_v<char, T>) {
            return sc::StringConstant<'c'>{};
        } else if constexpr(std::is_signed_v<T>) {
            return desc::sized<'i', sizeof(T)>();
        } else {
            return desc::sized<'u', sizeof(T)>();
        }
    }

    // 4 bytes as LEB128 (zigzag for a signed one), every other width raw. Measured on Cortex-M33 and M0+: a wire
    // byte costs about what a LEB128 step does, so a small 32-bit value is cheapest as its 1-2 LEB128 bytes, while a
    // 64-bit one (the time stamp of every log line) is cheaper as one 8-byte store than as 5-7 LEB128 steps behind a
    // zigzag on 64 bits, and a 16-bit one as a 2-byte store.
    template<typename Printer>
    constexpr void format(T const& value,
                          Printer& printer) const {
        if constexpr(sizeof(T) == 4) {
            detail::Access::varint(printer, value);
        } else {
            detail::Access::printHelper(printer, value);
        }
    }
};

template<std::floating_point T>
struct formatter<T> {
    static_assert(std::numeric_limits<T>::is_iec559,
                  "strange floating_point");
    static_assert(4 == sizeof(T) || 8 == sizeof(T),
                  "bad type: float or double only");

    static consteval auto descriptor() { return desc::sized<'f', sizeof(T)>(); }

    template<typename Printer>
    constexpr void format(T const& value,
                          Printer& printer) const {
        detail::Access::printHelper(printer, value);
    }
};

template<typename T>
    requires std::is_same_v<T, void*> || std::is_same_v<T, void const*>
          || std::is_same_v<T, std::nullptr_t>
struct formatter<T> {
    static_assert(sizeof(std::uintptr_t) == sizeof(void*),
                  "bad type: strange pointer");

    static consteval auto descriptor() { return desc::sized<'p', sizeof(std::uintptr_t)>(); }

    template<typename Printer>
    constexpr void format(T        value,
                          Printer& printer) const {
        detail::Access::printHelper(printer,
                                    std::bit_cast<std::uintptr_t>(static_cast<void const*>(value)));
    }
};

// A duration and a time point: the ratio is static (in the descriptor), the count goes out as its rep's formatter
// writes it. A time point is its duration since the clock's epoch, as the host renders it.
template<typename Rep, typename Period>
struct formatter<std::chrono::duration<Rep, Period>> {
    static_assert(std::is_arithmetic_v<Rep>
                    && !std::is_same_v<Rep,
                                       bool>,
                  "unsupported Rep type for duration formatting: an integer or a floating point");

    static consteval auto descriptor() {
        return sc::StringConstant<'d'>{} + desc::number<static_cast<std::uint64_t>(Period::num)>()
             + sc::StringConstant<'/'>{} + desc::number<static_cast<std::uint64_t>(Period::den)>()
             + descriptorOf<Rep>();
    }

    template<typename Printer>
    constexpr void format(std::chrono::duration<Rep,
                                                Period> const& duration,
                          Printer&                             printer) const {
        formatter<Rep>{}.format(duration.count(), printer);
    }
};

template<typename Clock, typename Duration>
struct formatter<std::chrono::time_point<Clock, Duration>> {
    static consteval auto descriptor() { return descriptorOf<Duration>(); }

    template<typename Printer>
    constexpr void format(std::chrono::time_point<Clock,
                                                  Duration> const& timePoint,
                          Printer&                                 printer) const {
        formatter<Duration>{}.format(timePoint.time_since_epoch(), printer);
    }
};

template<>
struct formatter<std::byte> {
    static consteval auto descriptor() { return desc::sized<'u', 1>(); }

    template<typename Printer>
    constexpr void format(std::byte const& value,
                          Printer&         printer) const {
        detail::Access::printHelper(printer, value);
    }
};

template<detail::IsStringLike T>
struct formatter<T> {
    static consteval auto descriptor() { return sc::StringConstant<'s'>{}; }

    template<typename Printer>
    constexpr void format(T const& value,
                          Printer& printer) const {
        std::string_view const stringView{value};
        detail::Access::varint(printer, stringView.size());
        detail::Access::lowprint(printer, stringView);
    }
};

// A string constant argument (unit symbols, names): its text is in the descriptor, nothing goes out.
template<char... Chars>
struct formatter<sc::StringConstant<Chars...>> {
    static consteval auto descriptor() {
        return sc::StringConstant<'k'>{} + desc::counted(sc::StringConstant<Chars...>{});
    }

    template<typename Printer>
    constexpr void format(sc::StringConstant<Chars...> const&,
                          Printer&) const {}
};

// Formatters are looked up via remove_cvref_t, which also strips the element const from a
// string literal's array type - so char[N] is the type that arrives here.
template<std::size_t N>
struct formatter<char[N]> {
    static consteval auto descriptor() { return sc::StringConstant<'s'>{}; }

    template<typename Printer>
    constexpr void format(char const (&value)[N],
                          Printer& printer) const {
        formatter<std::string_view>{}.format(std::string_view{value, N - 1}, printer);
    }
};

template<typename T>
    requires std::is_enum_v<T> && (!std::is_same_v<std::byte, T>)
struct formatter<T> {
private:
    using underlying_t = std::underlying_type_t<T>;
    // a char-based enum goes out as a number
    using format_t = std::conditional_t<
      std::is_same_v<underlying_t, char>,
      std::conditional_t<std::is_unsigned_v<underlying_t>, std::uint8_t, std::int8_t>,
      underlying_t>;

#if REMOTE_FMT_USE_ENCHANTUM
    static constexpr bool named = enchantum::count<T> != 0;

    // only where there are names: enchantum's tables of an enum without enumerators are no constants on gcc
    template<typename E>
    using Text = decltype(desc::built<detail::EnumText<E, decltype(descriptorOf<format_t>())>>());
#else
    static constexpr bool named = false;
#endif

public:
    // the names live in the descriptor (a type entry with the catalog): only the raw value goes out
    static consteval auto descriptor() {
#if REMOTE_FMT_USE_ENCHANTUM
        if constexpr(named) {
            return typeDescriptor<Text<T>>();
        } else
#endif
        {
            return descriptorOf<format_t>();
        }
    }

    template<typename Printer>
    constexpr void format(T const& value,
                          Printer& printer) const {
        formatter<format_t>{}.format(static_cast<format_t>(value), printer);
    }
};

template<detail::IsRangeButNotStringLike T>
struct formatter<T> {
private:
    using value_t = std::ranges::range_value_t<T>;
    // A contiguous range of plain numbers goes out as one copy of its memory (`A`); everything else element by
    // element through the element's formatter.
    static constexpr bool raw = std::ranges::contiguous_range<T>
                             && (std::is_integral_v<value_t> || std::is_floating_point_v<value_t>
                                 || std::is_same_v<std::byte, value_t>);

    template<typename R,
             typename Printer>
    static constexpr void formatImpl(R&          range,
                                     std::size_t size,
                                     Printer&    printer) {
        detail::Access::varint(printer, size);
        if constexpr(raw) {
            detail::Access::lowprint(printer, std::span{range});
        } else {
            // by value where the range hands out proxies (std::vector<bool>): they have no formatter
            for(auto const& element : range) {
                formatter<value_t>{}.format(static_cast<value_t const&>(element), printer);
            }
        }
    }

public:
    static consteval auto descriptor() {
        if constexpr(raw) {
            return sc::StringConstant<'A'>{} + descriptorOf<value_t>();
        } else if constexpr(detail::IsMap<T>) {
            return sc::StringConstant<'M'>{} + descriptorOf<value_t>();
        } else if constexpr(detail::IsSet<T>) {
            return sc::StringConstant<'S'>{} + descriptorOf<value_t>();
        } else {
            return sc::StringConstant<'L'>{} + descriptorOf<value_t>();
        }
    }

    template<typename Printer>
    constexpr void format(T const& range,
                          Printer& printer) const
        requires std::ranges::sized_range<T>
    {
        formatImpl(range, std::ranges::size(range), printer);
    }

    template<typename Printer>
    constexpr void format(T&       range,
                          Printer& printer) const
        requires(!std::ranges::sized_range<T>) && std::ranges::forward_range<T>
    {
        auto       copy = range;
        auto const size = static_cast<std::size_t>(std::ranges::distance(copy));
        formatImpl(range, size, printer);
    }
};

template<detail::IsTupleLikeButNotRange T>
struct formatter<T> {
    static consteval auto descriptor() {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            return (sc::StringConstant<'('>{} + ... + descriptorOf<std::tuple_element_t<Is, T>>())
                 + sc::StringConstant<')'>{};
        }(std::make_index_sequence<std::tuple_size_v<T>>{});
    }

    template<typename Printer>
    constexpr void format(T const& tuple,
                          Printer& printer) const {
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            using std::get;
            (formatter<std::remove_cvref_t<std::tuple_element_t<Is, T>>>{}.format(get<Is>(tuple),
                                                                                  printer),
             ...);
        }(std::make_index_sequence<std::tuple_size_v<T>>{});
    }
};

template<typename T>
struct formatter<std::optional<T>> {
    static consteval auto descriptor() { return sc::StringConstant<'O'>{} + descriptorOf<T>(); }

    template<typename Printer>
    constexpr void format(std::optional<T> const& optional,
                          Printer&                printer) const {
        detail::Access::printHelper(printer,
                                    static_cast<std::uint8_t>(optional.has_value() ? 1 : 0));
        if(optional) { formatter<std::remove_cvref_t<T>>{}.format(*optional, printer); }
    }
};

template<>
struct formatter<void> {
    static consteval auto descriptor() { return sc::StringConstant<'v'>{}; }

    template<typename Printer>
    constexpr void format(Printer&) const {}
};

template<typename T, typename E>
struct formatter<std::expected<T, E>> {
    static consteval auto descriptor() {
        return sc::StringConstant<'X'>{} + descriptorOf<T>() + descriptorOf<E>();
    }

    template<typename Printer>
    constexpr void format(std::expected<T,
                                        E> const& expected,
                          Printer&                printer) const {
        detail::Access::printHelper(printer,
                                    static_cast<std::uint8_t>(expected.has_value() ? 1 : 0));
        if(expected.has_value()) {
            if constexpr(!std::is_void_v<T>) {
                formatter<std::remove_cvref_t<T>>{}.format(*expected, printer);
            }
        } else {
            formatter<std::remove_cvref_t<E>>{}.format(expected.error(), printer);
        }
    }
};

template<typename... Ts>
struct formatter<std::variant<Ts...>> {
    static_assert(sizeof...(Ts) <= 255,
                  "a variant's index is one byte on the wire");

    static consteval auto descriptor() {
        return (
          (sc::StringConstant<'V'>{} + desc::number<sizeof...(Ts)>() + sc::StringConstant<':'>{})
          + ... + descriptorOf<Ts>());
    }

    template<typename Printer>
    constexpr void format(std::variant<Ts...> const& variant,
                          Printer&                   printer) const {
        detail::Access::printHelper(printer, static_cast<std::uint8_t>(variant.index()));
        std::visit(
          [&]<typename T>(T const& value) {
              formatter<std::remove_cvref_t<T>>{}.format(value, printer);
          },
          variant);
    }
};

}   // namespace remote_fmt
