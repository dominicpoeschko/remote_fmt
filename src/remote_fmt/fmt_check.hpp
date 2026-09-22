#pragma once

// Checks a format string against its arguments with fmt's own compile-time checker. Emits no code.
//
// The spec is not applied to the type the device serialized: the host parser reconstructs a value
// and formats that. host_type below maps one to the other, mirroring parser.hpp.

#include "catalog.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

#if !defined(REMOTE_FMT_USE_FMT_CHECK)
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
// At 0, duration/optional/variant/expected degrade to unchecked_arg; other fields stay checked.
    #if !defined(REMOTE_FMT_FMT_CHECK_FULL)
        #if defined(_LIBCPP_HAS_LOCALIZATION) && !_LIBCPP_HAS_LOCALIZATION
            #define REMOTE_FMT_FMT_CHECK_FULL 0
        #elif __has_include(<fmt/std.h>) && __has_include(<fmt/chrono.h>)
            #define REMOTE_FMT_FMT_CHECK_FULL 1
        #else
            #define REMOTE_FMT_FMT_CHECK_FULL 0
        #endif
    #endif

    #ifdef __clang__
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wduplicate-enum"
        #pragma clang diagnostic ignored "-Wsign-conversion"
        #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
        #pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-libc-call"
        #pragma clang diagnostic ignored "-Wswitch-default"
        #pragma clang diagnostic ignored "-Wswitch-enum"
        #pragma clang diagnostic ignored "-Wfloat-equal"
        #pragma clang diagnostic ignored "-Wshorten-64-to-32"
        #pragma clang diagnostic ignored "-Wreserved-macro-identifier"
        #pragma clang diagnostic ignored "-Wextra-semi"
        #pragma clang diagnostic ignored "-Wextra-semi-stmt"
        #pragma clang diagnostic ignored "-Wundefined-func-template"
        #pragma clang diagnostic ignored "-Wweak-vtables"
        #pragma clang diagnostic ignored "-Wglobal-constructors"
        #pragma clang diagnostic ignored "-Wmissing-noreturn"
        #pragma clang diagnostic ignored "-Wdeprecated-declarations"
        #pragma clang diagnostic ignored "-Wdocumentation"
        #pragma clang diagnostic ignored "-Wdocumentation-unknown-command"
    #endif
    #ifdef __GNUC__
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wsign-conversion"
    #endif

    #include <fmt/format.h>
    #include <fmt/ranges.h>

    #if REMOTE_FMT_FMT_CHECK_FULL
        #include <fmt/chrono.h>
        #include <fmt/std.h>
    #endif

    #include <chrono>
    #include <expected>
    #include <map>
    #include <optional>
    #include <set>
    #include <tuple>
    #include <variant>
    #include <vector>

    #ifdef __GNUC__
        #pragma GCC diagnostic pop
    #endif
    #ifdef __clang__
        #pragma clang diagnostic pop
    #endif

namespace remote_fmt { namespace detail {

    // For an argument whose fmt formatter is unreachable here (see degrades_to_unchecked). Swallows
    // any spec, so it holds its place in the field sequence and the rest stay checked.
    struct unchecked_arg {};

    template<typename T>
    struct host_type {
        using type = T;
    };

    template<typename T>
    using host_type_t = typename host_type<std::remove_cvref_t<T>>::type;

    template<typename T>
    struct host_type<T*> {
        using type = void const*;
    };

    // formatter<std::byte> puts a 1-byte unsigned on the wire.
    template<>
    struct host_type<std::byte> {
        using type = std::uint8_t;
    };

    template<is_string_like T>
    struct host_type<T> {
        using type = std::string_view;
    };

    template<char... chars>
    struct host_type<sc::StringConstant<chars...>> {
        using type = std::string_view;
    };

    // A string literal is not is_string_like (no .size()), so without this it would match the range
    // mapping below and fmt would check the spec against a range of chars.
    template<std::size_t N>
    struct host_type<char[N]> {
        using type = std::string_view;
    };

    // Value-dependent: the enumerator name when enchantum knows the value, the underlying integer
    // otherwise. The spec must suit both, hence the second check via host_type_alt.
    template<typename T>
        requires std::is_enum_v<T> && (!std::is_same_v<std::byte, T>)
    struct host_type<T> {
        using type = std::string_view;
    };

    template<is_tuple_like_but_not_range T>
    struct host_type<T> {
        using type = decltype([]<std::size_t... Is>(std::index_sequence<Is...>) {
            return std::tuple<host_type_t<std::tuple_element_t<Is, T>>...>{};
        }(std::make_index_sequence<std::tuple_size_v<T>>{}));
    };

    // Category matters: fmt renders a list as [...], a set as {...}, a map as {k: v}, and hands a
    // nested element spec ("{::>4}") to the element's own formatter.
    template<is_range_but_not_string_like T>
    struct host_type<T> {
        using type = std::vector<host_type_t<std::ranges::range_value_t<T>>>;
    };

    // std::map/std::set rather than the container itself: the check only needs fmt to pick the same
    // rendering, and rebuilding an arbitrary container is impossible when it has a non-type template
    // parameter (Kvasir::StaticMap<K, V, Capacity>).
    //
    // Derived from the element type, never from T::key_type or T::mapped_type: remote_fmt.hpp reads
    // neither - is_map only picks the RangeType tag, and elements go through formatter<range_value_t>.
    // A mapping that demanded more than the serializer would turn a supported type into a build error.
    // For the same reason the element requirement is in the constraint, so a map-like whose element is
    // not a pair falls back to the plain range mapping instead of failing.
    //
    // Repeating is_range_but_not_string_like is required for subsumption; without it the partial
    // specializations are ambiguous.
    template<typename T>
        requires is_range_but_not_string_like<T> && is_map<T>
              && is_tuple_like<std::ranges::range_value_t<T>>
              && (std::tuple_size_v<std::ranges::range_value_t<T>> == 2)
    struct host_type<T> {
        using element = std::ranges::range_value_t<T>;
        using type    = std::map<host_type_t<std::tuple_element_t<0, element>>,
                                 host_type_t<std::tuple_element_t<1, element>>>;
    };

    template<typename T>
        requires is_range_but_not_string_like<T> && is_set<T>
    struct host_type<T> {
        using type = std::set<host_type_t<std::ranges::range_value_t<T>>>;
    };

    // In both modes: since C++26 an optional is a range too, and must not map to a vector.
    template<typename T>
    struct host_type<std::optional<T>> {
        using type = std::optional<host_type_t<T>>;
    };

    #if REMOTE_FMT_FMT_CHECK_FULL
    // Primary handles expected<void, E>: host_type_t<void> would be ill-formed.
    template<typename T, typename E>
    struct host_type<std::expected<T, E>> {
        using type = std::expected<T, host_type_t<E>>;
    };

    template<typename T, typename E>
        requires(!std::is_void_v<T>)
    struct host_type<std::expected<T, E>> {
        using type = std::expected<host_type_t<T>, host_type_t<E>>;
    };

    template<typename... Ts>
    struct host_type<std::variant<Ts...>> {
        using type = std::variant<host_type_t<Ts>...>;
    };

    // Serialized as time_since_epoch() and formatted as a duration by parser.hpp.
    template<typename Clock, typename Duration>
    struct host_type<std::chrono::time_point<Clock, Duration>> {
        using type = Duration;
    };
    #endif

    #if __has_include(<fmt/color.h>)
    // Both fmt and parser.hpp forward the spec to the payload's formatter.
    template<typename T>
    struct host_type<fmt::detail::styled_arg<T>> {
        using type = host_type_t<T>;
    };
    #endif

    // As host_type, but an enum becomes its underlying integer.
    template<typename T>
    struct host_type_alt {
        using type = host_type_t<T>;
    };

    template<typename T>
    using host_type_alt_t = typename host_type_alt<std::remove_cvref_t<T>>::type;

    template<typename T>
        requires std::is_enum_v<T> && (!std::is_same_v<std::byte, T>)
    struct host_type_alt<T> {
        using underlying = std::underlying_type_t<T>;
        // Widened like remote_fmt.hpp does, so it prints as a number rather than a glyph.
        using type = std::conditional_t<
          std::is_same_v<underlying, char>,
          std::conditional_t<std::is_unsigned_v<underlying>, std::uint8_t, std::int8_t>,
          underlying>;
    };

}}   // namespace remote_fmt::detail

template<>
struct fmt::formatter<remote_fmt::detail::unchecked_arg> {
    // Accepts any spec. std::ranges::find because incrementing fmt's raw pointer by hand trips
    // -Wunsafe-buffer-usage.
    static constexpr auto parse(fmt::parse_context<char>& ctx) -> char const* {
        return std::ranges::find(ctx.begin(), ctx.end(), '}');
    }

    static auto format(remote_fmt::detail::unchecked_arg,
                       fmt::format_context& ctx) {
        return ctx.out();
    }
};

namespace remote_fmt { namespace detail {

    // Without REMOTE_FMT_FMT_CHECK_FULL fmt cannot format these, so their spec stays unchecked.
    template<typename T>
    inline constexpr bool degrades_to_unchecked = false;

    #if !REMOTE_FMT_FMT_CHECK_FULL
    template<typename Rep, typename Period>
    inline constexpr bool degrades_to_unchecked<std::chrono::duration<Rep, Period>> = true;
    template<typename Clock, typename Duration>
    inline constexpr bool degrades_to_unchecked<std::chrono::time_point<Clock, Duration>> = true;
    template<typename T>
    inline constexpr bool degrades_to_unchecked<std::optional<T>> = true;
    template<typename T, typename E>
    inline constexpr bool degrades_to_unchecked<std::expected<T, E>> = true;
    template<typename... Ts>
    inline constexpr bool degrades_to_unchecked<std::variant<Ts...>> = true;
    #endif

    // Anything else has its own remote_fmt::formatter, a sub format string the host renders to text
    // before applying the spec (applyFieldToSubFmtString): check the spec as a string's. A formatter
    // that forwards to another type (uc_log::Metric) specializes host_type instead.
    template<typename H>
    struct checkable {
        using type = std::conditional_t<
          fmt::is_formattable<H>::value,
          H,
          std::conditional_t<degrades_to_unchecked<H>, unchecked_arg, std::string_view>>;
    };

    template<typename H>
    using checkable_host_t = typename checkable<H>::type;

    // Element by element, so a container of such types still has its own spec checked.
    template<typename T>
    struct checkable<std::vector<T>> {
        using type = std::vector<checkable_host_t<T>>;
    };

    template<typename T>
    struct checkable<std::set<T>> {
        using type = std::set<checkable_host_t<T>>;
    };

    template<typename K, typename V>
    struct checkable<std::map<K, V>> {
        using type = std::map<checkable_host_t<K>, checkable_host_t<V>>;
    };

    template<typename... Ts>
    struct checkable<std::tuple<Ts...>> {
        using type = std::tuple<checkable_host_t<Ts>...>;
    };

    #if REMOTE_FMT_FMT_CHECK_FULL
    template<typename T>
    struct checkable<std::optional<T>> {
        using type = std::optional<checkable_host_t<T>>;
    };

    template<typename T, typename E>
    struct checkable<std::expected<T, E>> {
        using type = std::expected<T, checkable_host_t<E>>;
    };

    template<typename T, typename E>
        requires(!std::is_void_v<T>)
    struct checkable<std::expected<T, E>> {
        using type = std::expected<checkable_host_t<T>, checkable_host_t<E>>;
    };

    template<typename... Ts>
    struct checkable<std::variant<Ts...>> {
        using type = std::variant<checkable_host_t<Ts>...>;
    };
    #endif

    template<typename T>
    using checkable_t = checkable_host_t<host_type_t<T>>;

    template<typename T>
    using checkable_alt_t = checkable_host_t<host_type_alt_t<T>>;

    template<typename... Args,
             char... chars>
    consteval void checkFormatStringWithFmt(sc::StringConstant<chars...>) {
        constexpr auto stringView = std::string_view{sc::StringConstant<chars...>{}};

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
             char... chars>
    consteval void checkFormatStringWithFmt(sc::StringConstant<chars...>) {}
}}   // namespace remote_fmt::detail

#endif
