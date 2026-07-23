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
      {0, std::string{std::string_view{fmtString}}},
      {1, std::string{std::string_view{argString}}}
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

    if(failures != 0) {
        std::printf("%d test(s) failed\n", failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
