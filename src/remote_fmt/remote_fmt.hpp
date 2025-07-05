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

    constexpr std::optional<std::size_t> checkReplacementFieldCount(std::string_view sv) {
        bool        wasLastOpenBracket  = false;
        bool        wasLastCloseBracket = false;
        int         openCount           = 0;
        std::size_t argCount            = 0;

        for(auto c : sv) {
            if(c == '{') {
                wasLastCloseBracket = false;
                if(wasLastOpenBracket) {
                    wasLastOpenBracket = false;
                    --openCount;
                } else {
                    wasLastOpenBracket = true;
                    ++openCount;
                }
            } else if(c == '}') {
                wasLastOpenBracket = false;
                if(wasLastCloseBracket) {
                    wasLastCloseBracket = false;
                    ++openCount;
                    --argCount;
                } else {
                    wasLastCloseBracket = true;
                    --openCount;
                    ++argCount;
                }
            } else {
                wasLastOpenBracket  = false;
                wasLastCloseBracket = false;
            }
        }

        if(openCount != 0) {
            return std::nullopt;
        }
        return argCount;
    }

    template<std::size_t N>
    consteval bool is_arg_count_valid(std::string_view sv) {
        auto const oRFCount = checkReplacementFieldCount(sv);
        return oRFCount && *oRFCount == N;
    }

    constexpr bool isValidChar(char c) {
        if(c > '~') {
            return false;
        }
        if(c < ' ') {
            return c == '\n';
        }
        return true;
    }

    constexpr bool allCharsValid(std::string_view sv) {
        return std::all_of(begin(sv), end(sv), isValidChar);
    }

    template<std::size_t N>
    consteval void compile_time_assert(char const (&str)[N],
                                       bool predicat) {
#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif
        [[maybe_unused]] auto const x = str[N - 1 + static_cast<std::size_t>(!predicat)];
#ifdef __clang__
    #pragma clang diagnostic pop
#endif
    }

    template<typename T>
    concept is_tuple_like = requires {
        {
            std::tuple_size<T>::value
        } -> std::convertible_to<std::size_t>;
    };

    template<typename T>
    concept is_tuple_like_but_not_range = is_tuple_like<T> && !std::ranges::range<T>;

    template<typename T>
    concept is_string_like = std::is_convertible_v<T, std::string_view> && requires(T v) {
        {
            v.size()
        } -> std::convertible_to<std::size_t>;
        {
            v.data()
        } -> std::convertible_to<char const*>;
        {
            v.starts_with(std::string_view{})
        } -> std::convertible_to<bool>;
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
    constexpr auto format(T const& v,
                          Printer& printer) const {
        static_assert(8 >= sizeof(v), "bad type: no [u]int128_t");
        static_assert(1 == sizeof(char), "bad type: only 1 byte char");

        constexpr auto ts = detail::typeToTypeSize<T>();
        constexpr auto ti
          = detail::trivialTypeIdentifier<std::is_same_v<bool, T>   ? detail::TrivialType::boolean
                                          : std::is_same_v<char, T> ? detail::TrivialType::character
                                          : std::is_signed_v<T>     ? detail::TrivialType::signed_
                                                                : detail::TrivialType::unsigned_,
                                          ts>();

        printer.printHelper(ti, v);
    }
};

template<std::floating_point T>
struct formatter<T> {
    template<typename Printer>
    constexpr auto format(T const& v,
                          Printer& printer) const {
        static_assert(std::numeric_limits<T>::is_iec559, "strange floating_point");
        static_assert(8 >= sizeof(v), "bad type: no long double");
        static_assert(4 == sizeof(float), "bad type: float");
        static_assert(8 == sizeof(double), "bad type: double");

        constexpr auto ts = detail::typeToTypeSize<T>();
        constexpr auto ti = detail::trivialTypeIdentifier<detail::TrivialType::floatingpoint, ts>();

        printer.printHelper(ti, v);
    }
};

template<typename T>
    requires std::is_same_v<T, void*> || std::is_same_v<T, void const*>
          || std::is_same_v<T, std::nullptr_t>
struct formatter<T> {
    template<typename Printer>
    constexpr auto format(T        v,
                          Printer& printer) const {
        static_assert(8 >= sizeof(v), "bad type: no 128 bit pointers...");
        static_assert(sizeof(std::uintptr_t) == sizeof(v), "bad type: strange pointer");

        constexpr auto ts = detail::typeToTypeSize<std::uintptr_t>();
        constexpr auto ti = detail::trivialTypeIdentifier<detail::TrivialType::pointer, ts>();

        printer.printHelper(ti, std::bit_cast<std::uintptr_t>(v));
    }
};

namespace detail {

    template<TimeType tt,
             typename Rep,
             std::intmax_t Num,
             std::intmax_t Denom,
             typename Append>
    constexpr auto format_time(std::chrono::duration<Rep,
                                                     std::ratio<Num,
                                                                Denom>> const& v,
                               Append                                          append) {
        static_assert(std::is_same<Rep, std::int64_t>::value
                        || (4 >= sizeof(Rep) && std::is_trivial_v<Rep>),
                      "only with std::int64_t or smaller rep");
        using SignedRep
          = std::conditional_t<std::is_same_v<std::uint32_t, Rep>,
                               std::int64_t,
                               std::conditional_t<std::is_same_v<std::uint16_t, Rep>
                                                    || std::is_same_v<std::uint8_t, Rep>,
                                                  std::int32_t,
                                                  Rep>>;
        constexpr auto num = std::ratio<Num, Denom>::num;
        constexpr auto den = std::ratio<Num, Denom>::den;

        static_assert(num > 0, "should not be possible std::chrono::duration checks that");
        static_assert(den > 0, "should not be possible std::chrono::duration checks that");

        constexpr auto num_size = sizeToTypeSize(static_cast<std::uint64_t>(num));
        constexpr auto den_size = sizeToTypeSize(static_cast<std::uint64_t>(den));

        auto const count = static_cast<SignedRep>(v.count());
        auto const ts    = detail::sizeToTimeSize(count);
        auto const ti    = detail::timeTypeIdentifier<tt, num_size, den_size>(ts);

        append(ti,
               static_cast<detail::typeSize_unsigned_t<num_size>>(num),
               static_cast<detail::typeSize_unsigned_t<den_size>>(den));

        appendSized(ts, count, append);
    }
}   // namespace detail

template<typename Rep, typename Period>
struct formatter<std::chrono::duration<Rep, Period>> {
    template<typename Printer>
    constexpr auto format(std::chrono::duration<Rep,
                                                Period> const& v,
                          Printer&                             printer) const {
        return detail::format_time<detail::TimeType::duration>(v, [&](auto const&... vs) {
            printer.printHelper(vs...);
        });
    }
};

template<typename Clock, typename Duration>
struct formatter<std::chrono::time_point<Clock, Duration>> {
    template<typename Printer>
    constexpr auto format(std::chrono::time_point<Clock,
                                                  Duration> const& v,
                          Printer&                                 printer) const {
        return detail::format_time<detail::TimeType::time_point>(
          v.time_since_epoch(),
          [&](auto const&... vs) { printer.printHelper(vs...); });
    }
};

template<>
struct formatter<std::byte> {
    template<typename Printer>
    constexpr auto format(std::byte const& v,
                          Printer&         printer) const {
        return formatter<std::uint8_t>{}.format(static_cast<std::uint8_t>(v), printer);
    }
};

template<detail::is_string_like T>
struct formatter<T> {
    template<typename Printer>
    constexpr auto format(T const& v,
                          Printer& printer) const {
        std::string_view const sv = [&]() -> std::string_view {
            if constexpr(std::is_same_v<char*, std::remove_cvref_t<T>>
                         || std::is_same_v<char const*, std::remove_cvref_t<T>>)
            {
                if(v == nullptr) {
                    return {};
                }
            }
            return v;
        }();

        auto const rs = detail::sizeToRangeSize(sv.size());
        auto const ti
          = detail::rangeTypeIdentifier<detail::RangeType::string, detail::RangeLayout::compact>(
            rs);

        printer.printHelper(ti);
        appendSized(rs, sv.size(), [&](auto const&... vs) { printer.printHelper(vs...); });
        printer.lowprint(sv);
    }
};

template<char... chars>
struct formatter<sc::StringConstant<chars...>> {
    template<typename Printer>
    constexpr auto format(sc::StringConstant<chars...> const& v,
                          Printer&                            printer) const {
        if constexpr(use_catalog) {
            auto constexpr rs = detail::sizeToRangeSize(std::numeric_limits<std::uint16_t>::max());
            auto constexpr ti = detail::rangeTypeIdentifier<detail::RangeType::cataloged_string,
                                                            detail::RangeLayout::compact>(rs);

            printer.printHelper(ti);
#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wundefined-func-template"
#endif
            appendSized(rs, catalog<decltype(v)>(), [&](auto const&... vs) {
                printer.printHelper(vs...);
            });
#ifdef __clang__
    #pragma clang diagnostic pop
#endif
        } else {
            formatter<std::string_view>{}.format(std::string_view{v}, printer);
        }
    }
};

template<std::size_t N>
struct formatter<char const (&)[N]> {
    template<typename Printer>
    constexpr auto format(char const (&v)[N],
                          Printer& printer) const {
        return formatter<std::string_view>{}.format(std::string_view{v, N - 1}, printer);
    }
};

template<typename T>
    requires std::is_enum_v<T>
          && (!std::is_same_v<std::byte,
                              T>)
struct formatter<T> {
    template<typename Printer>
    constexpr auto format(T const& v,
                          Printer& printer) const {
        auto as_int = [&]() {
            using underlying_t = std::underlying_type_t<T>;

            using format_t = std::conditional_t<
              std::is_same_v<underlying_t, char>,
              std::conditional_t<std::is_unsigned_v<underlying_t>, std::uint8_t, std::int8_t>,
              underlying_t>;

            return formatter<format_t>{}.format(static_cast<format_t>(v), printer);
        };
#if __has_include(<magic_enum.hpp>)
        if(magic_enum::enum_contains(v)) {
            return magic_enum::enum_switch(
              [&](auto vv) {
                  static constexpr T    vvv = vv;
                  static constexpr auto get
                    = sc::create([]() { return magic_enum::enum_name<vvv>(); });
                  return formatter<std::remove_cvref_t<decltype(get)>>{}.format(get, printer);
              },
              v);
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
    constexpr auto format(T const& v,
                          Printer& printer) const {
        auto constexpr rt = detail::is_map<T> ? detail::RangeType::map
                          : detail::is_set<T> ? detail::RangeType::set
                                              : detail::RangeType::list;

        constexpr bool is_contiguous         = std::ranges::contiguous_range<T>;
        using value_t                        = std::ranges::range_value_t<T>;
        constexpr bool is_trivial_formatable = std::is_integral_v<value_t>
                                            || std::is_floating_point_v<value_t>
                                            || std::is_same_v<std::byte, value_t>;

        auto const size = std::ranges::size(v);
        auto const rs   = detail::sizeToRangeSize(size);
        auto const ti
          = detail::rangeTypeIdentifier<rt,
                                        is_trivial_formatable ? detail::RangeLayout::compact
                                                              : detail::RangeLayout::on_ti_each>(
            rs);

        printer.printHelper(ti);

        appendSized(rs, size, [&](auto const&... vs) { printer.printHelper(vs...); });

        if constexpr(is_trivial_formatable) {
            if constexpr(is_contiguous) {
                if(std::ranges::size(v) != 0) {
                    formatter<value_t>{}.format(*std::ranges::begin(v), printer);
                    printer.lowprint(std::span{v}.subspan(1));
                }
            } else {
                for(bool first = true; auto const& vv : v) {
                    if(first) {
                        first = false;
                        formatter<value_t>{}.format(vv, printer);
                    } else {
                        printer.printHelper(vv);
                    }
                }
            }
        } else {
            for(auto const& vv : v) {
                formatter<value_t>{}.format(vv, printer);
            }
        }
    }
};

template<detail::is_tuple_like_but_not_range T>
struct formatter<T> {
    template<typename Printer>
    constexpr auto format(T const& v,
                          Printer& printer) const {
        constexpr auto rs = detail::sizeToRangeSize(std::tuple_size_v<T>);
        constexpr auto ti
          = detail::rangeTypeIdentifier<detail::RangeType::tuple, detail::RangeLayout::on_ti_each>(
            rs);

        printer.printHelper(ti,
                            static_cast<detail::rangeSize_unsigned_t<rs>>(std::tuple_size_v<T>));

        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            using std::get;
            (formatter<std::remove_cvref_t<std::tuple_element_t<Is, T>>>{}.format(get<Is>(v),
                                                                                  printer),
             ...);
        }(std::make_index_sequence<std::tuple_size_v<T>>{});
    }
};

template<typename T>
struct formatter<std::optional<T>> {
    template<typename Printer>
    constexpr auto format(std::optional<T> const& v,
                          Printer&                printer) const {
        detail::appendExtendedTypeIdentifier<detail::ExtendedTypeIdentifier::optional>(
          [&](auto const&... vs) { printer.printHelper(vs...); });

        printer.printHelper(static_cast<std::uint8_t>(v.has_value() ? 1 : 0));

        if(v) {
            return formatter<std::remove_cvref_t<T>>{}.format(*v, printer);
        }
    }
};

template<typename... Ts>
struct formatter<std::variant<Ts...>> {
    template<typename Printer>
    constexpr auto format(std::variant<Ts...> const& v,
                          Printer&                   printer) const {
        return std::visit(
          [&]<typename T>(T const& vv) {
              return formatter<std::remove_cvref_t<T>>{}.format(vv, printer);
          },
          v);
    }
};

#if __has_include(<fmt/color.h>)
template<typename T>
struct formatter<fmt::detail::styled_arg<T>> {
    template<typename Printer>
    constexpr auto format(fmt::detail::styled_arg<T> const& v,
                          Printer&                          printer) const {
        detail::appendExtendedTypeIdentifier<detail::ExtendedTypeIdentifier::styled>(
          [&](auto const&... vs) { printer.printHelper(vs...); });

        auto const& style = v.style;

        std::uint8_t set{};
        if(style.has_foreground()) {
            if(style.get_foreground().is_rgb) {
                set |= 1;
            } else {
                set |= 2;
            }
        }

        if(style.has_background()) {
            if(style.get_background().is_rgb) {
                set |= 4;
            } else {
                set |= 8;
            }
        }
        if(style.has_emphasis()) {
            set |= 16;
        }

        printer.printHelper(set);

        auto addColor = [&](auto color) {
            if(color.is_rgb) {
                printer.printHelper(static_cast<std::uint32_t>(color.value.rgb_color));
            } else {
                printer.printHelper(static_cast<std::uint8_t>(color.value.term_color));
            }
        };

        if(style.has_foreground()) {
            addColor(style.get_foreground());
        }
        if(style.has_background()) {
            addColor(style.get_background());
        }
        if(style.has_emphasis()) {
            printer.printHelper(static_cast<std::uint8_t>(style.get_emphasis()));
        }

        return formatter<T>{}.format(v.value, printer);
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
    constexpr void lowprint(R const& r) {
        lowprint(std::as_bytes(std::span{r}));
    }

    template<typename... Ts>
        requires(std::is_trivially_copyable_v<Ts> && ...)
    constexpr void printHelper(Ts const&... vs) {
        (lowprint(std::span{std::addressof(vs), 1}), ...);
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
            auto constexpr sv = std::string_view{fmt};
            auto constexpr rs = detail::sizeToRangeSize(sv.size());
            auto constexpr ti = detail::fmtStringTypeIdentifier<ft>(rs);

            printHelper(ti);
            appendSized(rs, sv.size(), [&](auto const&... vs) { printHelper(vs...); });
            lowprint(sv);
        } else {
            auto constexpr rs = detail::sizeToRangeSize(std::numeric_limits<std::uint16_t>::max());
            auto constexpr ti = detail::fmtStringTypeIdentifier<ft>(rs);

            printHelper(ti);
#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wundefined-func-template"
#endif
            appendSized(rs, catalog<decltype(fmt)>(), [&](auto const&... vs) {
                printHelper(vs...);
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
    constexpr explicit Printer(Cb&& cb) : comBackend{std::forward<Cb>(cb)} {}

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
