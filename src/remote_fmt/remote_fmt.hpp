#pragma once

#include "catalog.hpp"
#include "type_identifier.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <ratio>
#include <span>
#include <string>
#include <string_constant/string_constant.hpp>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#if __has_include(<magic_enum/magic_enum.hpp>)

    #ifdef __GNUC__
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wextra-semi"
    #endif

    #ifdef __clang__
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wextra-semi-stmt"
        #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    #endif
    #include <magic_enum/magic_enum.hpp>
    #include <magic_enum/magic_enum_switch.hpp>

    #ifdef __GNUC__
        #pragma GCC diagnostic pop
    #endif
    #ifdef __clang__
        #pragma clang diagnostic pop
    #endif

#endif

#if __has_include(<fmt/color.h>)
    #ifdef __clang__
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
        #pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-libc-call"
    #endif
    #include <fmt/color.h>
    #ifdef __clang__
        #pragma clang diagnostic pop
    #endif
#endif

namespace remote_fmt {

// Protocol constants
namespace protocol {
    constexpr std::byte Start_marker{0x55};
    constexpr std::byte End_marker{0xAA};
}   // namespace protocol

#ifndef REMOTE_FMT_USE_CATALOG
static constexpr bool use_catalog = true;
#else
static constexpr bool use_catalog{REMOTE_FMT_USE_CATALOG};
#endif

namespace detail {

    template<FmtStringType T>
    consteval FmtStringType maybeCataloged() {
        if constexpr(T == FmtStringType::normal && use_catalog) {
            return FmtStringType::cataloged_normal;
        }
        if constexpr(T == FmtStringType::sub && use_catalog) {
            return FmtStringType::cataloged_sub;
        }
        if constexpr(T == FmtStringType::cataloged_normal && !use_catalog) {
            return FmtStringType::normal;
        }
        if constexpr(T == FmtStringType::cataloged_sub && !use_catalog) {
            return FmtStringType::sub;
        }
        return T;
    }

    constexpr std::optional<std::size_t> checkReplacementFieldCount(std::string_view stringView) {
        int         openCount = 0;
        std::size_t argCount  = 0;

        for(auto iterator = stringView.begin(); iterator != stringView.end();
            std::advance(iterator, 1))
        {
            char const character = *iterator;

            if(character == '{') {
                if(std::next(iterator) != stringView.end() && *std::next(iterator) == '{') {
                    std::advance(iterator, 1);
                } else {
                    ++openCount;
                }
            } else if(character == '}') {
                if(std::next(iterator) != stringView.end() && *std::next(iterator) == '}') {
                    std::advance(iterator, 1);
                } else {
                    if(--openCount == 0) {
                        ++argCount;
                    } else if(openCount < 0) {
                        return std::nullopt;
                    }
                }
            }
        }

        return openCount == 0 ? std::optional{argCount} : std::nullopt;
    }

    template<std::size_t N>
    consteval bool is_arg_count_valid(std::string_view stringView) {
        auto const optionalReplacementFieldCount = checkReplacementFieldCount(stringView);
        return optionalReplacementFieldCount && *optionalReplacementFieldCount == N;
    }

    constexpr bool isValidChar(char character) {
        if(character > '~') { return false; }
        if(character < ' ') { return character == '\n'; }
        return true;
    }

    constexpr bool allCharsValid(std::string_view stringView) {
        return std::ranges::all_of(stringView, isValidChar);
    }

    template<std::size_t N>
    consteval void compile_time_assert(char const (&str)[N],
                                       bool predicate) {
#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif
        [[maybe_unused]] auto const character = str[N - 1 + static_cast<std::size_t>(!predicate)];
#ifdef __clang__
    #pragma clang diagnostic pop
#endif
    }

    template<typename T>
    concept is_tuple_like = requires {
        { std::tuple_size<T>::value } -> std::convertible_to<std::size_t>;
    };

    template<typename T>
    concept is_tuple_like_but_not_range = is_tuple_like<T> && !std::ranges::range<T>;

    template<typename T>
    concept is_string_like = std::is_convertible_v<T, std::string_view> && requires(T value) {
        { value.size() } -> std::convertible_to<std::size_t>;
        { value.data() } -> std::convertible_to<char const*>;
        { value.starts_with(std::string_view{}) } -> std::convertible_to<bool>;
    };

    template<typename T>
    concept is_range_but_not_string_like = std::ranges::range<T> && !is_string_like<T>;

    template<typename T>
    concept is_map = requires { typename T::mapped_type; };

    template<typename T>
    concept is_set = requires { typename T::key_type; } && !is_map<T>;

}   // namespace detail

template<typename... Args,
         char... chars>
consteval void checkFormatString(sc::StringConstant<chars...>) {
    detail::compile_time_assert(
      "invalid chars in format",
      detail::allCharsValid(std::string_view{sc::StringConstant<chars...>{}}));
    detail::compile_time_assert("invalid replacement field count",
                                detail::is_arg_count_valid<sizeof...(Args)>(
                                  std::string_view{sc::StringConstant<chars...>{}}));
}

template<typename T>
struct formatter;

template<std::integral T>
struct formatter<T> {
    template<typename Printer>
    constexpr auto format(T const& value,
                          Printer& printer) const {
        static_assert(8 >= sizeof(value), "bad type: no [u]int128_t");
        static_assert(1 == sizeof(char), "bad type: only 1 byte char");

        constexpr auto typeSize    = detail::typeToTypeSize<T>();
        constexpr auto trivialType = []() constexpr {
            if constexpr(std::is_same_v<bool, T>) {
                return detail::TrivialType::boolean;
            } else if constexpr(std::is_same_v<char, T>) {
                return detail::TrivialType::character;
            } else if constexpr(std::is_signed_v<T>) {
                return detail::TrivialType::signed_;
            } else {
                return detail::TrivialType::unsigned_;
            }
        }();
        constexpr auto typeIdentifier = detail::trivialTypeIdentifier<trivialType, typeSize>();

        printer.printHelper(typeIdentifier, value);
    }
};

template<std::floating_point T>
struct formatter<T> {
    template<typename Printer>
    constexpr auto format(T const& value,
                          Printer& printer) const {
        static_assert(std::numeric_limits<T>::is_iec559, "strange floating_point");
        static_assert(8 >= sizeof(value), "bad type: no long double");
        static_assert(4 == sizeof(float), "bad type: float");
        static_assert(8 == sizeof(double), "bad type: double");

        constexpr auto typeSize = detail::typeToTypeSize<T>();
        constexpr auto typeIdentifier
          = detail::trivialTypeIdentifier<detail::TrivialType::floatingpoint, typeSize>();

        printer.printHelper(typeIdentifier, value);
    }
};

template<typename T>
    requires std::is_same_v<T, void*> || std::is_same_v<T, void const*>
          || std::is_same_v<T, std::nullptr_t>
struct formatter<T> {
    template<typename Printer>
    constexpr auto format(T        value,
                          Printer& printer) const {
        static_assert(8 >= sizeof(value), "bad type: no 128 bit pointers...");
        static_assert(sizeof(std::uintptr_t) == sizeof(value), "bad type: strange pointer");

        constexpr auto typeSize = detail::typeToTypeSize<std::uintptr_t>();
        constexpr auto typeIdentifier
          = detail::trivialTypeIdentifier<detail::TrivialType::pointer, typeSize>();

        printer.printHelper(typeIdentifier, std::bit_cast<std::uintptr_t>(value));
    }
};

namespace detail {

    template<TimeType timeType,
             typename Rep,
             std::intmax_t Num,
             std::intmax_t Denom,
             typename Append>
    constexpr auto format_time(std::chrono::duration<Rep,
                                                     std::ratio<Num,
                                                                Denom>> const& duration,
                               Append                                          append) {
        static_assert(std::is_same_v<Rep, std::int64_t>
                        || (4 >= sizeof(Rep) && std::is_trivial_v<Rep>),
                      "only with std::int64_t or smaller rep");
        using SignedRep
          = std::conditional_t<std::is_same_v<std::uint32_t, Rep>,
                               std::int64_t,
                               std::conditional_t<std::is_same_v<std::uint16_t, Rep>
                                                    || std::is_same_v<std::uint8_t, Rep>,
                                                  std::int32_t,
                                                  Rep>>;
        constexpr auto numerator   = std::ratio<Num, Denom>::num;
        constexpr auto denominator = std::ratio<Num, Denom>::den;

        static_assert(numerator > 0, "should not be possible std::chrono::duration checks that");
        static_assert(denominator > 0, "should not be possible std::chrono::duration checks that");

        constexpr auto numerator_size   = sizeToTypeSize(static_cast<std::uint64_t>(numerator));
        constexpr auto denominator_size = sizeToTypeSize(static_cast<std::uint64_t>(denominator));

        auto const count    = static_cast<SignedRep>(duration.count());
        auto const timeSize = detail::sizeToTimeSize(count);
        auto const typeIdentifier
          = detail::timeTypeIdentifier<timeType, numerator_size, denominator_size>(timeSize);

        append(typeIdentifier,
               static_cast<detail::typeSize_unsigned_t<numerator_size>>(numerator),
               static_cast<detail::typeSize_unsigned_t<denominator_size>>(denominator));

        appendSized(timeSize, count, append);
    }
}   // namespace detail

template<typename Rep, typename Period>
struct formatter<std::chrono::duration<Rep, Period>> {
    template<typename Printer>
    constexpr auto format(std::chrono::duration<Rep,
                                                Period> const& duration,
                          Printer&                             printer) const {
        return detail::format_time<detail::TimeType::duration>(
          duration,
          [&](auto const&... values) { printer.printHelper(values...); });
    }
};

template<typename Clock, typename Duration>
struct formatter<std::chrono::time_point<Clock, Duration>> {
    template<typename Printer>
    constexpr auto format(std::chrono::time_point<Clock,
                                                  Duration> const& timePoint,
                          Printer&                                 printer) const {
        return detail::format_time<detail::TimeType::time_point>(
          timePoint.time_since_epoch(),
          [&](auto const&... values) { printer.printHelper(values...); });
    }
};

template<>
struct formatter<std::byte> {
    template<typename Printer>
    constexpr auto format(std::byte const& value,
                          Printer&         printer) const {
        return formatter<std::uint8_t>{}.format(static_cast<std::uint8_t>(value), printer);
    }
};

template<detail::is_string_like T>
struct formatter<T> {
    template<typename Printer>
    constexpr auto format(T const& value,
                          Printer& printer) const {
        std::string_view const stringView = [&]() -> std::string_view {
            if constexpr(std::is_same_v<char*, std::remove_cvref_t<T>>
                         || std::is_same_v<char const*, std::remove_cvref_t<T>>)
            {
                if(value == nullptr) { return {}; }
            }
            return value;
        }();

        auto const rangeSize = detail::sizeToRangeSize(stringView.size());
        auto const typeIdentifier
          = detail::rangeTypeIdentifier<detail::RangeType::string, detail::RangeLayout::compact>(
            rangeSize);

        printer.printHelper(typeIdentifier);
        appendSized(rangeSize, stringView.size(), [&](auto const&... valueArgs) {
            printer.printHelper(valueArgs...);
        });
        printer.lowprint(stringView);
    }
};

template<char... chars>
struct formatter<sc::StringConstant<chars...>> {
    template<typename Printer>
    constexpr auto format(sc::StringConstant<chars...> const& value,
                          Printer&                            printer) const {
        if constexpr(use_catalog) {
            auto constexpr rangeSize
              = detail::sizeToRangeSize(std::numeric_limits<std::uint16_t>::max());
            auto constexpr typeIdentifier
              = detail::rangeTypeIdentifier<detail::RangeType::cataloged_string,
                                            detail::RangeLayout::compact>(rangeSize);

            printer.printHelper(typeIdentifier);
#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wundefined-func-template"
#endif
            appendSized(rangeSize, catalog<decltype(value)>(), [&](auto const&... valueArgs) {
                printer.printHelper(valueArgs...);
            });
#ifdef __clang__
    #pragma clang diagnostic pop
#endif
        } else {
            formatter<std::string_view>{}.format(std::string_view{value}, printer);
        }
    }
};

template<std::size_t N>
struct formatter<char const (&)[N]> {
    template<typename Printer>
    constexpr auto format(char const (&value)[N],
                          Printer& printer) const {
        return formatter<std::string_view>{}.format(std::string_view{value, N - 1}, printer);
    }
};

template<typename T>
    requires std::is_enum_v<T> && (!std::is_same_v<std::byte, T>)
struct formatter<T> {
    template<typename Printer>
    constexpr auto format(T const& value,
                          Printer& printer) const {
        auto as_int = [&]() {
            using underlying_t = std::underlying_type_t<T>;

            using format_t = std::conditional_t<
              std::is_same_v<underlying_t, char>,
              std::conditional_t<std::is_unsigned_v<underlying_t>, std::uint8_t, std::int8_t>,
              underlying_t>;

            return formatter<format_t>{}.format(static_cast<format_t>(value), printer);
        };
#if __has_include(<magic_enum/magic_enum.hpp>)
        if(magic_enum::enum_contains(value)) {
            return magic_enum::enum_switch(
              [&](auto enumValue) {
                  static constexpr T    enumConstant = enumValue;
                  static constexpr auto get
                    = sc::create([]() { return magic_enum::enum_name<enumConstant>(); });
                  return formatter<std::remove_cvref_t<decltype(get)>>{}.format(get, printer);
              },
              value);
        } else {
            return as_int();
        }
#else
        return as_int();
#endif
    }
};

template<detail::is_range_but_not_string_like T>
struct formatter<T> {
    template<typename Printer>
    constexpr auto format(T const& range,
                          Printer& printer) const {
        auto constexpr rangeType = []() constexpr {
            if constexpr(detail::is_map<T>) {
                return detail::RangeType::map;
            } else if constexpr(detail::is_set<T>) {
                return detail::RangeType::set;
            } else {
                return detail::RangeType::list;
            }
        }();

        constexpr bool is_contiguous         = std::ranges::contiguous_range<T>;
        using value_t                        = std::ranges::range_value_t<T>;
        constexpr bool is_trivial_formatable = std::is_integral_v<value_t>
                                            || std::is_floating_point_v<value_t>
                                            || std::is_same_v<std::byte, value_t>;

        auto const size      = std::ranges::size(range);
        auto const rangeSize = detail::sizeToRangeSize(size);
        auto const typeIdentifier
          = detail::rangeTypeIdentifier<rangeType,
                                        is_trivial_formatable ? detail::RangeLayout::compact
                                                              : detail::RangeLayout::on_ti_each>(
            rangeSize);

        printer.printHelper(typeIdentifier);

        appendSized(rangeSize, size, [&](auto const&... valueArgs) {
            printer.printHelper(valueArgs...);
        });

        if constexpr(is_trivial_formatable) {
            if constexpr(is_contiguous) {
                if(std::ranges::size(range) != 0) {
                    formatter<value_t>{}.format(*std::ranges::begin(range), printer);
                    printer.lowprint(std::span{range}.subspan(1));
                }
            } else {
                for(bool first = true; auto const& element : range) {
                    if(first) {
                        first = false;
                        formatter<value_t>{}.format(element, printer);
                    } else {
                        printer.printHelper(element);
                    }
                }
            }
        } else {
            for(auto const& element : range) { formatter<value_t>{}.format(element, printer); }
        }
    }
};

template<detail::is_tuple_like_but_not_range T>
struct formatter<T> {
    template<typename Printer>
    constexpr auto format(T const& tuple,
                          Printer& printer) const {
        constexpr auto rangeSize = detail::sizeToRangeSize(std::tuple_size_v<T>);
        constexpr auto typeIdentifier
          = detail::rangeTypeIdentifier<detail::RangeType::tuple, detail::RangeLayout::on_ti_each>(
            rangeSize);

        printer.printHelper(
          typeIdentifier,
          static_cast<detail::rangeSize_unsigned_t<rangeSize>>(std::tuple_size_v<T>));

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
    template<typename Printer>
    constexpr auto format(std::optional<T> const& optional,
                          Printer&                printer) const {
        detail::appendExtendedTypeIdentifier<detail::ExtendedTypeIdentifier::optional>(
          [&](auto const&... valueArgs) { printer.printHelper(valueArgs...); });

        printer.printHelper(static_cast<std::uint8_t>(optional.has_value() ? 1 : 0));

        if(optional) { return formatter<std::remove_cvref_t<T>>{}.format(*optional, printer); }
    }
};

template<typename... Ts>
struct formatter<std::variant<Ts...>> {
    template<typename Printer>
    constexpr auto format(std::variant<Ts...> const& variant,
                          Printer&                   printer) const {
        return std::visit(
          [&]<typename T>(T const& value) {
              return formatter<std::remove_cvref_t<T>>{}.format(value, printer);
          },
          variant);
    }
};

#if __has_include(<fmt/color.h>)
template<typename T>
struct formatter<fmt::detail::styled_arg<T>> {
    template<typename Printer>
    constexpr auto format(fmt::detail::styled_arg<T> const& styledArg,
                          Printer&                          printer) const {
        detail::appendExtendedTypeIdentifier<detail::ExtendedTypeIdentifier::styled>(
          [&](auto const&... valueArgs) { printer.printHelper(valueArgs...); });

        auto const& style = styledArg.style;

        std::uint8_t flags{};
        if(style.has_foreground()) {
            if(style.get_foreground().is_rgb) {
                flags |= 1U;
            } else {
                flags |= 2U;
            }
        }

        if(style.has_background()) {
            if(style.get_background().is_rgb) {
                flags |= 4U;
            } else {
                flags |= 8U;
            }
        }
        if(style.has_emphasis()) { flags |= 16U; }

        printer.printHelper(flags);

        auto addColor = [&](auto color) {
            if(color.is_rgb) {
                printer.printHelper(static_cast<std::uint32_t>(color.value.rgb_color));
            } else {
                printer.printHelper(static_cast<std::uint8_t>(color.value.term_color));
            }
        };

        if(style.has_foreground()) { addColor(style.get_foreground()); }
        if(style.has_background()) { addColor(style.get_background()); }
        if(style.has_emphasis()) {
            printer.printHelper(static_cast<std::uint8_t>(style.get_emphasis()));
        }

        return formatter<T>{}.format(styledArg.value, printer);
    }
};
#endif

template<typename Printer,
         char... chars,
         typename... Args>
static constexpr auto format_to(Printer&                     printer,
                                sc::StringConstant<chars...> fmt,
                                Args&&... args) {
    checkFormatString<decltype(args)...>(fmt);
    return printer.format(fmt, std::forward<Args>(args)...);
}

template<typename ComBackend>
struct Printer {
private:
    void constexpr lowprint(std::span<std::byte const> span) {
        if constexpr(requires { ComBackend::write(span); }) {
            ComBackend::write(span);
        } else {
            comBackend.write(span);
        }
    }

    template<std::ranges::contiguous_range R>
        requires std::is_trivially_copyable_v<std::ranges::range_value_t<R>>
    constexpr void lowprint(R const& range) {
        lowprint(std::as_bytes(std::span{range}));
    }

    template<typename... Ts>
        requires(std::is_trivially_copyable_v<Ts> && ...)
    constexpr void printHelper(Ts const&... values) {
        (lowprint(std::span{std::addressof(values), 1}), ...);
    }

    template<typename T>
    friend struct formatter;

    template<typename Printer,
             char... chars,
             typename... Args>
    friend constexpr auto format_to(Printer&                     printer,
                                    sc::StringConstant<chars...> fmt,
                                    Args&&... args);

    constexpr Printer& out() { return *this; }

    template<detail::FmtStringType ft
             = detail::maybeCataloged<detail::FmtStringType::cataloged_sub>(),
             char... chars,
             typename... Args>
    constexpr void format(sc::StringConstant<chars...> fmt,
                          Args&&... args) {
        checkFormatString<decltype(args)...>(fmt);

        if constexpr(ft == detail::FmtStringType::sub || ft == detail::FmtStringType::normal) {
            auto constexpr stringView = std::string_view{fmt};
            auto constexpr rangeSize  = detail::sizeToRangeSize(stringView.size());
            auto constexpr typeId     = detail::fmtStringTypeIdentifier<ft>(rangeSize);

            printHelper(typeId);
            appendSized(rangeSize, stringView.size(), [&](auto const&... valueArgs) {
                printHelper(valueArgs...);
            });
            lowprint(stringView);
        } else {
            auto constexpr rangeSize
              = detail::sizeToRangeSize(std::numeric_limits<std::uint16_t>::max());
            auto constexpr typeId = detail::fmtStringTypeIdentifier<ft>(rangeSize);

            printHelper(typeId);
#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wundefined-func-template"
#endif
            appendSized(rangeSize, catalog<decltype(fmt)>(), [&](auto const&... valueArgs) {
                printHelper(valueArgs...);
            });
#ifdef __clang__
    #pragma clang diagnostic pop
#endif
        }

        (formatter<std::remove_cvref_t<Args>>{}.format(std::forward<Args>(args), *this), ...);
    }

    [[no_unique_address]] ComBackend comBackend{};

public:
    constexpr Printer() = default;

    template<typename Cb>
        requires std::is_same_v<std::remove_cvref_t<Cb>,
                                std::remove_cvref_t<ComBackend>>
    constexpr explicit Printer(Cb&& callback) : comBackend{std::forward<Cb>(callback)} {}

    ComBackend const& get_com_backend() const { return comBackend; }

    ComBackend& get_com_backend() { return comBackend; }

    template<char... chars,
             typename... Args>
    constexpr void print(sc::StringConstant<chars...> fmt,
                         Args&&... args) {
        checkFormatString<decltype(args)...>(fmt);

        if constexpr(requires { ComBackend::initTransfer(); }) {
            ComBackend::initTransfer();
        } else if constexpr(requires { comBackend.initTransfer(); }) {
            comBackend.initTransfer();
        }

        printHelper(protocol::Start_marker);
        format<detail::maybeCataloged<detail::FmtStringType::cataloged_normal>()>(
          fmt,
          std::forward<Args>(args)...);
        printHelper(protocol::End_marker);

        if constexpr(requires { ComBackend::finalizeTransfer(); }) {
            ComBackend::finalizeTransfer();
        } else if constexpr(requires { comBackend.finalizeTransfer(); }) {
            comBackend.finalizeTransfer();
        }
    }

    template<char... chars,
             typename... Args>
    static constexpr void staticPrint(sc::StringConstant<chars...> fmt,
                                      Args&&... args) {
        checkFormatString<decltype(args)...>(fmt);
        static_assert(
          requires { ComBackend::write(std::span<std::byte const>{}); },
          "staticPrint needs static ComBackend");

        Printer{}.print(fmt, std::forward<Args>(args)...);
    }
};

}   // namespace remote_fmt
