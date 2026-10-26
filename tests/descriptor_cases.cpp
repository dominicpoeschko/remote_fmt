// Prints described<Args...>(fmt) for a set of argument lists, one line each: the number of arguments, a tab, the
// described string as hex, a tab, the host's split of its descriptors (grammar::take, descriptor_grammar.hpp) as hex
// pieces joined by ','. test_descriptor_split.py splits each with tools/descriptor.py (what the catalog json's "args"
// are made of) and checks that it finds exactly that many complete descriptors, the same ones the C++ walker finds -
// the C++ grammar (descriptor.hpp, the formatters' descriptor(), the parser's walker) and the Python tokenizer cannot
// drift apart unnoticed. Built with the catalog on (enums as type keys `#<hex>`) and off (enums inline).
// With any argument (`descriptor_cases split`) it prints the C++ split of each run of descriptors given as a hex line
// on stdin instead ("error" if it does not parse): edge strings the formatters do not produce.
#include "remote_fmt/descriptor.hpp"

#include "remote_fmt/descriptor_grammar.hpp"
#include "remote_fmt/remote_fmt.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#if __has_include(<mp-units/systems/si.h>)
    #include <mp-units/systems/si.h>
    #define REMOTE_FMT_TEST_MP_UNITS 1
#endif

using namespace sc::literals;

enum struct Color : std::uint8_t { red, green, blue };

enum struct Signed : std::int16_t { low = -3, mid = 0, high = 3 };

// a bitflag enum: enchantum detects it by these operators
enum struct Perm : std::uint8_t { read = 1 << 0, write = 1 << 1 };

constexpr Perm operator|(Perm a,
                         Perm b) {
    return Perm(std::uint8_t(a) | std::uint8_t(b));
}

constexpr Perm operator&(Perm a,
                         Perm b) {
    return Perm(std::uint8_t(a) & std::uint8_t(b));
}

[[maybe_unused]] constexpr Perm operator~(Perm a) { return Perm(std::uint8_t(~std::uint8_t(a))); }

[[maybe_unused]] constexpr Perm& operator|=(Perm& a,
                                            Perm  b) {
    return a = a | b;
}

[[maybe_unused]] constexpr Perm& operator&=(Perm& a,
                                            Perm  b) {
    return a = a & b;
}

// a 64-bit bitflag enum with its top bit (an enumerator past INT64_MAX in the descriptor)
enum struct Wide : std::uint64_t { lo = 1, hi = std::uint64_t{1} << 63U };

constexpr Wide operator|(Wide a,
                         Wide b) {
    return Wide(std::uint64_t(a) | std::uint64_t(b));
}

constexpr Wide operator&(Wide a,
                         Wide b) {
    return Wide(std::uint64_t(a) & std::uint64_t(b));
}

[[maybe_unused]] constexpr Wide operator~(Wide a) { return Wide(~std::uint64_t(a)); }

[[maybe_unused]] constexpr Wide& operator|=(Wide& a,
                                            Wide  b) {
    return a = a | b;
}

[[maybe_unused]] constexpr Wide& operator&=(Wide& a,
                                            Wide  b) {
    return a = a & b;
}

struct Declared {
    int   a;
    Color c;
};

struct SelfDescribed {
    int a;
};

template<>
struct remote_fmt::formatter<Declared> {
    static consteval auto descriptor() {
        return remote_fmt::subFormatDescriptor<int, Color>("<{} {}>"_sc);
    }

    template<typename Printer>
    constexpr void format(Declared const& d,
                          Printer&        printer) const {
        printer.formatArguments(d.a, d.c);
    }
};

template<>
struct remote_fmt::formatter<SelfDescribed> {
    template<typename Printer>
    constexpr auto format(SelfDescribed const& s,
                          Printer&             printer) const {
        return format_to(printer, "({})"_sc, s.a);
    }
};

namespace {
void printHex(std::string_view text) {
    for(char const c : text) {
        std::printf("%02x", static_cast<unsigned>(static_cast<unsigned char>(c)));
    }
}

// the descriptors one after the other as grammar::take cuts them; only "error" if any piece does not parse (no
// pieces before it: the reader takes the whole column as the split)
void printSplit(std::string_view d) {
    std::vector<std::string_view> pieces;
    for(std::string_view rest = d; !rest.empty();) {
        auto const one = remote_fmt::detail::grammar::take(rest);
        if(!one) {
            std::printf("error");
            return;
        }
        pieces.push_back(*one);
    }
    for(std::size_t i = 0; i != pieces.size(); ++i) {
        if(i != 0) { std::printf(","); }
        printHex(pieces[i]);
    }
}

template<typename... Args>
void show() {
    constexpr auto         text = remote_fmt::described<Args...>("f"_sc);
    std::string_view const view{text};
    std::printf("%zu\t", sizeof...(Args));
    printHex(view);
    std::printf("\t");
    printSplit(view.substr(view.find(remote_fmt::desc::separator) + 1));
    std::printf("\n");
}

std::string fromHex(std::string_view hex) {
    std::string out;
    for(std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto const digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        out.push_back(static_cast<char>((digit(hex[i]) * 16) + digit(hex[i + 1])));
    }
    return out;
}
}   // namespace

int main(int argc,
         char**) {
    if(argc > 1) {
        for(std::string hex; std::getline(std::cin, hex);) {
            printSplit(fromHex(hex));
            std::printf("\n");
        }
        return 0;
    }
    using namespace std::chrono;
    show<>();
    show<bool,
         char,
         std::uint8_t,
         std::int8_t,
         std::uint16_t,
         std::int32_t,
         std::uint64_t,
         std::int64_t>();
    show<float, double, void*, std::string_view, char[6]>();
    show<decltype("constant"_sc), int>();
    show<milliseconds,
         duration<std::int64_t, std::ratio<1, 150'000'000>>,
         time_point<steady_clock>>();
    show<std::optional<int>,
         std::optional<std::optional<std::string_view>>,
         std::expected<int, Color>>();
    show<std::variant<int, std::string_view, std::optional<float>>,
         std::variant<std::monostate, Color>>();
    show<std::vector<int>,
         std::vector<std::string_view>,
         std::set<std::uint16_t>,
         std::map<std::string_view, int>>();
    show<std::array<std::uint8_t, 4>, std::span<double const>, std::vector<std::vector<Color>>>();
    show<std::tuple<>,
         std::tuple<int, std::string_view>,
         std::pair<Color, std::tuple<std::optional<int>, Perm>>>();
    show<Color, Signed, Perm, Wide, std::optional<Color>, std::vector<Perm>>();
    show<Declared, SelfDescribed, std::optional<Declared>, std::tuple<SelfDescribed, Declared>>();
#if REMOTE_FMT_USE_STYLED
    show<decltype(fmt::styled(1, fmt::text_style{})),
         decltype(fmt::styled(Color::red, fmt::text_style{}))>();
#endif
#ifdef REMOTE_FMT_TEST_MP_UNITS
    show<mp_units::quantity<mp_units::si::milli<mp_units::si::kelvin>, std::int32_t>,
         mp_units::quantity<mp_units::si::volt, float>,
         std::optional<mp_units::quantity<mp_units::si::metre, double>>>();
#endif
    return 0;
}
