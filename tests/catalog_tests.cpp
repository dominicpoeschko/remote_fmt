// Tests for the catalog mode (use_catalog == true, the default): format strings and
// StringConstant arguments are transmitted as 16 bit ids and resolved through the
// string constants map on the parser side. The catalog<>() specializations that the
// toolchain normally generates are written by hand here, like in examples/catalog.cpp.
#include "remote_fmt/catalog.hpp"

#include "remote_fmt/parser.hpp"
#include "remote_fmt/remote_fmt.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

using namespace sc::literals;

static constexpr auto fmtString{"Test {}"_sc};
static constexpr auto argString{"hello"_sc};

template<>
std::uint16_t remote_fmt::catalog<std::remove_cvref_t<decltype(fmtString)>>() {
    return 0;
}

// Arguments are cataloged through their formatter, which instantiates catalog<>() with
// a const& qualified type - specialize for exactly that.
template<>
std::uint16_t remote_fmt::catalog<std::remove_cvref_t<decltype(argString)> const&>() {
    return 1;
}

// A bitflag enum. test_roundtrip covers the rendering, but it builds with the catalog OFF, so the
// combined-value path is only exercised here: each set flag's name is its own StringConstant and
// therefore its own catalog id, and the parser has to resolve every one of them before it can join
// them into "read|write".
enum struct Perm : std::uint8_t { read = 1 << 0, write = 1 << 1 };

constexpr Perm operator|(Perm a,
                         Perm b) {
    return Perm(std::uint8_t(std::uint8_t(a) | std::uint8_t(b)));
}

constexpr Perm operator&(Perm a,
                         Perm b) {
    return Perm(std::uint8_t(std::uint8_t(a) & std::uint8_t(b)));
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

static constexpr auto bitflagFmtString{"Perm {}"_sc};
static constexpr auto readName{sc::create([]() { return enchantum::to_string(Perm::read); })};
static constexpr auto writeName{sc::create([]() { return enchantum::to_string(Perm::write); })};

template<>
std::uint16_t remote_fmt::catalog<std::remove_cvref_t<decltype(bitflagFmtString)>>() {
    return 2;
}

template<>
std::uint16_t remote_fmt::catalog<std::remove_cvref_t<decltype(readName)> const&>() {
    return 3;
}

template<>
std::uint16_t remote_fmt::catalog<std::remove_cvref_t<decltype(writeName)> const&>() {
    return 4;
}

namespace {

int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

struct VectorBackend {
    std::vector<std::byte> memory;

    void write(std::span<std::byte const> data) {
        memory.insert(memory.end(), data.begin(), data.end());
    }
};

// Leaked on purpose: avoids the global-constructor and exit-time-destructor warnings.
std::unordered_map<std::uint16_t,
                   std::string> const&
stringConstantsMap() {
    static auto const& map = *new std::unordered_map<std::uint16_t, std::string>{
      {0,        std::string{std::string_view{fmtString}}},
      {1,        std::string{std::string_view{argString}}},
      {2, std::string{std::string_view{bitflagFmtString}}},
      {3,         std::string{std::string_view{readName}}},
      {4,        std::string{std::string_view{writeName}}}
    };
    return map;
}

}   // namespace

int main() {
    {
        remote_fmt::Printer<VectorBackend> printer{};
        printer.print(fmtString, 42);
        auto const& buffer = printer.get_com_backend().memory;

        auto const [message, remaining, discarded]
          = remote_fmt::parse(std::span{buffer}, stringConstantsMap(), [](std::string_view) {});
        CHECK(message.has_value() && *message == "Test 42", "cataloged format string resolves");
        CHECK(remaining.empty() && discarded == 0, "buffer fully consumed");
    }

    {
        remote_fmt::Printer<VectorBackend> printer{};
        printer.print(fmtString, argString);
        auto const& buffer = printer.get_com_backend().memory;

        auto const [message, remaining, discarded]
          = remote_fmt::parse(std::span{buffer}, stringConstantsMap(), [](std::string_view) {});
        CHECK(message.has_value() && *message == "Test hello",
              "cataloged string argument resolves");
        CHECK(remaining.empty() && discarded == 0, "buffer fully consumed");
    }

    {
        // Unknown catalog ids must fail with an error message instead of formatting garbage.
        remote_fmt::Printer<VectorBackend> printer{};
        printer.print(fmtString, 42);
        auto const& buffer = printer.get_com_backend().memory;

        bool errorReported = false;
        auto const [message, remaining, discarded]
          = remote_fmt::parse(std::span{buffer}, {}, [&](std::string_view error) {
                errorReported = error.find("not found") != std::string_view::npos;
            });
        CHECK(!message, "unknown catalog id produces no message");
        CHECK(errorReported, "unknown catalog id reports an error");
    }

    {
        remote_fmt::Printer<VectorBackend> printer{};
        printer.print(bitflagFmtString, Perm::read | Perm::write);
        auto const& buffer = printer.get_com_backend().memory;

        auto const [message, remaining, discarded]
          = remote_fmt::parse(std::span{buffer}, stringConstantsMap(), [](std::string_view) {});
        CHECK(message.has_value() && *message == "Perm read|write",
              "cataloged bitflag names resolve and join");
        CHECK(remaining.empty() && discarded == 0, "buffer fully consumed");
    }

    {
        // One name missing from the map must fail the whole bitflag, not render half of it.
        remote_fmt::Printer<VectorBackend> printer{};
        printer.print(bitflagFmtString, Perm::read | Perm::write);
        auto const& buffer = printer.get_com_backend().memory;

        auto partialMap = stringConstantsMap();
        partialMap.erase(4);

        bool errorReported = false;
        auto const [message, remaining, discarded]
          = remote_fmt::parse(std::span{buffer}, partialMap, [&](std::string_view error) {
                errorReported = error.find("not found") != std::string_view::npos;
            });
        CHECK(!message, "bitflag with an unknown name produces no message");
        CHECK(errorReported, "bitflag with an unknown name reports an error");
    }

    if(failures != 0) {
        std::printf("%d test(s) failed\n", failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
