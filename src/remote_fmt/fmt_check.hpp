#pragma once

// Checks a format string against its arguments with fmt's own compile-time checker. Emits no code.
//
// The spec is not applied to the type the device serialized: the host parser reconstructs a value
// and formats that. host_type below maps one to the other, mirroring parser.hpp.

#include "traits.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_constant/string_constant.hpp>
#include <string_view>
#include <type_traits>
#include <utility>

#ifndef REMOTE_FMT_USE_FMT_CHECK
    #if __has_include(<fmt/format.h>) && __has_include(<fmt/ranges.h>)
        #define REMOTE_FMT_USE_FMT_CHECK 1
    #else
        #define REMOTE_FMT_USE_FMT_CHECK 0
    #endif
#endif

#if REMOTE_FMT_USE_FMT_CHECK

    #include <version>

// Can fmt/chrono.h and fmt/std.h be included? Without localization they cannot unless fmt.patch is
// applied, which this header cannot detect - the cross branch of CMakeLists.txt sets it to 1.
// At 0, duration/optional/variant/expected degrade to UncheckedArg; other fields stay checked.
    #ifndef REMOTE_FMT_FMT_CHECK_FULL
        #if defined(_LIBCPP_HAS_LOCALIZATION) && !_LIBCPP_HAS_LOCALIZATION
            #define REMOTE_FMT_FMT_CHECK_FULL 0
        #elif __has_include(<fmt/std.h>) && __has_include(<fmt/chrono.h>)
            #define REMOTE_FMT_FMT_CHECK_FULL 1
        #else
            #define REMOTE_FMT_FMT_CHECK_FULL 0
        #endif
    #endif

    #include "detail/fmt_wall.hpp"

    #include <chrono>
    #include <expected>
    #include <map>
    #include <optional>
    #include <set>
    #include <tuple>
    #include <variant>
    #include <vector>

REMOTE_FMT_DETAIL_FMT_WALL_BEGIN
    #include <fmt/format.h>
    #include <fmt/ranges.h>

    #if REMOTE_FMT_FMT_CHECK_FULL
        #include <fmt/chrono.h>
        #include <fmt/std.h>
    #endif
REMOTE_FMT_DETAIL_FMT_WALL_END

namespace remote_fmt { namespace detail {

    // For an argument whose fmt formatter is unreachable here (see degradesToUnchecked). Swallows
    // any spec, so it holds its place in the field sequence and the rest stay checked.
    struct UncheckedArg {};

    template<typename T>
    struct HostType {
        using type = T;
    };

    template<typename T>
    using host_type_t = typename HostType<std::remove_cvref_t<T>>::type;

    // the one pointer with a formatter besides void const* (which maps to itself) and nullptr_t
    template<>
    struct HostType<void*> {
        using type = void const*;
    };

    // formatter<std::byte> puts a 1-byte unsigned on the wire.
    template<>
    struct HostType<std::byte> {
        using type = std::uint8_t;
    };

    template<IsStringLike T>
    struct HostType<T> {
        using type = std::string_view;
    };

    template<char... Chars>
    struct HostType<sc::StringConstant<Chars...>> {
        using type = std::string_view;
    };

    // A string literal is not IsStringLike (no .size()), so without this it would match the range
    // mapping below and fmt would check the spec against a range of chars.
    template<std::size_t N>
    struct HostType<char[N]> {
        using type = std::string_view;
    };

    // Value-dependent: the enumerator name when enchantum knows the value, the underlying integer
    // otherwise. The spec must suit both, hence the second check via host_type_alt.
    template<typename T>
        requires std::is_enum_v<T> && (!std::is_same_v<std::byte, T>)
    struct HostType<T> {
        using type = std::string_view;
    };

    template<IsTupleLikeButNotRange T>
    struct HostType<T> {
        using type = decltype([]<std::size_t... Is>(std::index_sequence<Is...>) {
            return std::tuple<host_type_t<std::tuple_element_t<Is, T>>...>{};
        }(std::make_index_sequence<std::tuple_size_v<T>>{}));
    };

    // Category matters: fmt renders a list as [...], a set as {...}, a map as {k: v}, and hands a
    // nested element spec ("{::>4}") to the element's own formatter.
    template<IsRangeButNotStringLike T>
    struct HostType<T> {
        using type = std::vector<host_type_t<std::ranges::range_value_t<T>>>;
    };

    // std::map/std::set rather than the container itself: the check only needs fmt to pick the same
    // rendering, and rebuilding an arbitrary container is impossible when it has a non-type template
    // parameter (Kvasir::StaticMap<K, V, Capacity>).
    //
    // Derived from the element type, never from T::key_type or T::mapped_type: the range formatter reads
    // neither - IsMap only picks the `M` descriptor, and elements go through formatter<range_value_t>.
    // A mapping that demanded more than the serializer would turn a supported type into a build error.
    // For the same reason the element requirement is in the constraint, so a map-like whose element is
    // not a pair falls back to the plain range mapping instead of failing.
    //
    // Repeating IsRangeButNotStringLike is required for subsumption; without it the partial
    // specializations are ambiguous.
    template<typename T>
        requires IsRangeButNotStringLike<T> && IsMap<T>
              && IsTupleLike<std::ranges::range_value_t<T>>
              && (std::tuple_size_v<std::ranges::range_value_t<T>> == 2)
    struct HostType<T> {
        using element = std::ranges::range_value_t<T>;
        using type    = std::map<host_type_t<std::tuple_element_t<0, element>>,
                                 host_type_t<std::tuple_element_t<1, element>>>;
    };

    template<typename T>
        requires IsRangeButNotStringLike<T> && IsSet<T>
    struct HostType<T> {
        using type = std::set<host_type_t<std::ranges::range_value_t<T>>>;
    };

    // In both modes: since C++26 an optional is a range too, and must not map to a vector.
    template<typename T>
    struct HostType<std::optional<T>> {
        using type = std::optional<host_type_t<T>>;
    };

    #if REMOTE_FMT_FMT_CHECK_FULL
    // Primary handles expected<void, E>: host_type_t<void> would be ill-formed.
    template<typename T, typename E>
    struct HostType<std::expected<T, E>> {
        using type = std::expected<T, host_type_t<E>>;
    };

    template<typename T, typename E>
        requires(!std::is_void_v<T>)
    struct HostType<std::expected<T, E>> {
        using type = std::expected<host_type_t<T>, host_type_t<E>>;
    };

    template<typename... Ts>
    struct HostType<std::variant<Ts...>> {
        using type = std::variant<host_type_t<Ts>...>;
    };

    // Serialized as time_since_epoch() and formatted as a duration by parser.hpp.
    template<typename Clock, typename Duration>
    struct HostType<std::chrono::time_point<Clock, Duration>> {
        using type = Duration;
    };
    #endif

    // As host_type, but an enum becomes its underlying integer.
    template<typename T>
    struct HostTypeAlt {
        using type = host_type_t<T>;
    };

    template<typename T>
    using host_type_alt_t = typename HostTypeAlt<std::remove_cvref_t<T>>::type;

    template<typename T>
        requires std::is_enum_v<T> && (!std::is_same_v<std::byte, T>)
    struct HostTypeAlt<T> {
        using underlying = std::underlying_type_t<T>;
        // Widened like the enum formatter does (formatters.hpp), so it prints as a number rather than a glyph.
        using type = std::conditional_t<
          std::is_same_v<underlying, char>,
          std::conditional_t<std::is_unsigned_v<underlying>, std::uint8_t, std::int8_t>,
          underlying>;
    };

}}   // namespace remote_fmt::detail

template<>
struct fmt::formatter<remote_fmt::detail::UncheckedArg> {
    // Accepts any spec. std::ranges::find because incrementing fmt's raw pointer by hand trips
    // -Wunsafe-buffer-usage.
    static constexpr auto parse(fmt::parse_context<char>& ctx) -> char const* {
        return std::ranges::find(ctx.begin(), ctx.end(), '}');
    }

    static auto format(remote_fmt::detail::UncheckedArg,
                       fmt::format_context& ctx) {
        return ctx.out();
    }
};

namespace remote_fmt { namespace detail {

    // Without REMOTE_FMT_FMT_CHECK_FULL fmt cannot format these, so their spec stays unchecked.
    template<typename T>
    inline constexpr bool degradesToUnchecked = false;

    #if !REMOTE_FMT_FMT_CHECK_FULL
    template<typename Rep, typename Period>
    inline constexpr bool degradesToUnchecked<std::chrono::duration<Rep, Period>> = true;
    template<typename Clock, typename Duration>
    inline constexpr bool degradesToUnchecked<std::chrono::time_point<Clock, Duration>> = true;
    template<typename T>
    inline constexpr bool degradesToUnchecked<std::optional<T>> = true;
    template<typename T, typename E>
    inline constexpr bool degradesToUnchecked<std::expected<T, E>> = true;
    template<typename... Ts>
    inline constexpr bool degradesToUnchecked<std::variant<Ts...>> = true;
    #endif

    // Anything else has its own remote_fmt::formatter, a sub format string the host renders to text
    // before applying the spec (applyFieldToSubFmtString): check the spec as a string's. A formatter
    // that forwards to another type (uc_log::Metric) specializes host_type instead.
    template<typename H>
    struct Checkable {
        using type = std::conditional_t<
          fmt::is_formattable<H>::value,
          H,
          std::conditional_t<degradesToUnchecked<H>, UncheckedArg, std::string_view>>;
    };

    template<typename H>
    using checkable_host_t = typename Checkable<H>::type;

    // Element by element, so a container of such types still has its own spec checked.
    template<typename T>
    struct Checkable<std::vector<T>> {
        using type = std::vector<checkable_host_t<T>>;
    };

    template<typename T>
    struct Checkable<std::set<T>> {
        using type = std::set<checkable_host_t<T>>;
    };

    template<typename K, typename V>
    struct Checkable<std::map<K, V>> {
        using type = std::map<checkable_host_t<K>, checkable_host_t<V>>;
    };

    template<typename... Ts>
    struct Checkable<std::tuple<Ts...>> {
        using type = std::tuple<checkable_host_t<Ts>...>;
    };

    #if REMOTE_FMT_FMT_CHECK_FULL
    template<typename T>
    struct Checkable<std::optional<T>> {
        using type = std::optional<checkable_host_t<T>>;
    };

    template<typename T, typename E>
    struct Checkable<std::expected<T, E>> {
        using type = std::expected<T, checkable_host_t<E>>;
    };

    template<typename T, typename E>
        requires(!std::is_void_v<T>)
    struct Checkable<std::expected<T, E>> {
        using type = std::expected<checkable_host_t<T>, checkable_host_t<E>>;
    };

    template<typename... Ts>
    struct Checkable<std::variant<Ts...>> {
        using type = std::variant<checkable_host_t<Ts>...>;
    };
    #endif

    template<typename T>
    using checkable_t = checkable_host_t<host_type_t<T>>;

    template<typename T>
    using checkable_alt_t = checkable_host_t<host_type_alt_t<T>>;

    template<typename... Args,
             char... Chars>
    consteval void checkFormatStringWithFmt(sc::StringConstant<Chars...>) {
        constexpr auto stringView = std::string_view{sc::StringConstant<Chars...>{}};

        // Constructing fmt::format_string runs fmt's checker; in a consteval function a rejection
        // becomes a compile error at the log line. It cannot be reduced to a bool for static_assert -
        // a requires-expression reports the call as valid even when the string is not.
        [[maybe_unused]] fmt::format_string<checkable_t<Args>...> const primary{stringView};

        if constexpr(!std::is_same_v<std::tuple<checkable_t<Args>...>,
                                     std::tuple<checkable_alt_t<Args>...>>)
        {
            [[maybe_unused]] fmt::format_string<checkable_alt_t<Args>...> const alternate{
              stringView};
        }
    }

}}   // namespace remote_fmt::detail

#else

// Defined either way, so dependent code can test it without testing REMOTE_FMT_USE_FMT_CHECK first.
    #if !defined(REMOTE_FMT_FMT_CHECK_FULL)
        #define REMOTE_FMT_FMT_CHECK_FULL 0
    #endif

namespace remote_fmt { namespace detail {
    template<typename... Args,
             char... Chars>
    consteval void checkFormatStringWithFmt(sc::StringConstant<Chars...>) {}
}}   // namespace remote_fmt::detail

#endif
