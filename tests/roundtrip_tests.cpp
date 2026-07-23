// Round-trip tests: serialize with remote_fmt::Printer, parse with remote_fmt::parse and
// compare against the fmt formatted result. Also covers parser robustness on malformed
// buffers (garbage, truncation, resync).
#include "remote_fmt/parser.hpp"
#include "remote_fmt/remote_fmt.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

using namespace sc::literals;
using namespace std::literals;

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
emptyCatalog() {
    static auto const& catalog = *new std::unordered_map<std::uint16_t, std::string>{};
    return catalog;
}

template<typename... Args>
std::vector<std::byte> serialize(auto fmtString,
                                 Args&&... args) {
    remote_fmt::Printer<VectorBackend> printer{};
    printer.print(fmtString, std::forward<Args>(args)...);
    return printer.get_com_backend().memory;
}

template<typename... Args>
std::optional<std::string> roundTrip(auto fmtString,
                                     Args&&... args) {
    auto const buffer = serialize(fmtString, std::forward<Args>(args)...);
    auto const [message, remaining, discarded]
      = remote_fmt::parse(std::span{buffer}, emptyCatalog(), [](std::string_view error) {
            std::printf("parser error: %.*s\n", static_cast<int>(error.size()), error.data());
        });
    if(!remaining.empty() || discarded != 0) { return std::nullopt; }
    return message;
}

#define CHECK_RT(expected, ...)                                         \
    do {                                                                \
        auto const rt_result = roundTrip(__VA_ARGS__);                  \
        if(!rt_result) {                                                \
            std::printf("FAIL: parse failed (line %d)\n", __LINE__);    \
            ++failures;                                                 \
        } else if(*rt_result != (expected)) {                           \
            std::printf("FAIL: got \"%s\" expected \"%s\" (line %d)\n", \
                        rt_result->c_str(),                             \
                        std::string{expected}.c_str(),                  \
                        __LINE__);                                      \
            ++failures;                                                 \
        }                                                               \
    } while(0)

enum struct Color : std::uint8_t { red, green, blue };

void trivialRoundTrips() {
    CHECK_RT("no arguments", "no arguments"_sc);
    CHECK_RT("Test 123", "Test {}"_sc, 123);
    CHECK_RT("{} 1", "{{}} {}"_sc, 1);

    CHECK_RT("255", "{}"_sc, std::uint8_t{255});
    CHECK_RT("65535", "{}"_sc, std::uint16_t{65535});
    CHECK_RT("4294967295", "{}"_sc, std::uint32_t{4294967295U});
    CHECK_RT("18446744073709551615", "{}"_sc, std::numeric_limits<std::uint64_t>::max());

    CHECK_RT("-128", "{}"_sc, std::int8_t{-128});
    CHECK_RT("-32768", "{}"_sc, std::int16_t{-32768});
    CHECK_RT("-2147483648", "{}"_sc, std::numeric_limits<std::int32_t>::min());
    CHECK_RT("-9223372036854775808", "{}"_sc, std::numeric_limits<std::int64_t>::min());

    CHECK_RT("true false", "{} {}"_sc, true, false);
    CHECK_RT("x", "{}"_sc, 'x');
    CHECK_RT("1.5", "{}"_sc, 1.5F);
    CHECK_RT("3.25", "{}"_sc, 3.25);
    CHECK_RT("171", "{}"_sc, std::byte{0xAB});

    CHECK_RT("0x1234", "{}"_sc, std::bit_cast<void const*>(std::uintptr_t{0x1234}));
    CHECK_RT("0x0", "{}"_sc, nullptr);

    CHECK_RT("00ff", "{:04x}"_sc, 255);
    CHECK_RT("**ab***", "{:*^7}"_sc, "ab");
}

void stringRoundTrips() {
    CHECK_RT("literal", "{}"_sc, "literal");
    CHECK_RT("view", "{}"_sc, "view"sv);
    CHECK_RT("string", "{}"_sc, std::string{"string"});
    CHECK_RT("red", "{}"_sc, Color::red);
    CHECK_RT("42", "{}"_sc, static_cast<Color>(42));
}

void rangeRoundTrips() {
    CHECK_RT("[1, 2, 3]", "{}"_sc, std::vector<int>{1, 2, 3});
    CHECK_RT("[]", "{}"_sc, std::vector<int>{});
    CHECK_RT("[1.5, 2.5]", "{}"_sc, std::array<double, 2>{1.5, 2.5});
    CHECK_RT("{1, 2}", "{}"_sc, std::set<int>{1, 2});
    CHECK_RT("{1: 2, 3: 4}",
             "{}"_sc,
             std::map<int, int>{
               {1, 2},
               {3, 4}
    });
    CHECK_RT("[\"a\", \"b\"]", "{}"_sc, std::vector<std::string>{"a", "b"});
    CHECK_RT("[[1, 2], []]",
             "{}"_sc,
             std::vector<std::vector<int>>{
               {1, 2},
               {}
    });
}

void tupleRoundTrips() {
    CHECK_RT("(1, 2)", "{}"_sc, std::pair{1, 2});
    CHECK_RT("(1, x, \"s\")", "{}"_sc, std::tuple{1, 'x', "s"sv});
    CHECK_RT("((1, 2), 3)",
             "{}"_sc,
             std::tuple{
               std::pair{1, 2},
               3
    });
    CHECK_RT("{\"k\": 1}",
             "{}"_sc,
             std::map<std::string, int>{
               {"k", 1}
    });
}

void extendedTypeRoundTrips() {
    CHECK_RT("42", "{}"_sc, std::optional<int>{42});
    CHECK_RT("()", "{}"_sc, std::optional<int>{});
    CHECK_RT("expected(1)", "{}"_sc, std::expected<int, int>{1});
    CHECK_RT("unexpected(2)", "{}"_sc, std::expected<int, int>{std::unexpected{2}});
    CHECK_RT("expected(void)", "{}"_sc, std::expected<void, int>{});
    CHECK_RT("7", "{}"_sc, std::variant<int, std::string>{7});
    CHECK_RT("v", "{}"_sc, std::variant<int, std::string>{"v"});

    auto const style    = fmt::fg(fmt::color::red) | fmt::emphasis::bold;
    auto const expected = fmt::format("{}", fmt::styled(42, style));
    CHECK_RT(expected, "{}"_sc, fmt::styled(42, style));

    auto const termStyle    = fmt::fg(fmt::terminal_color::green);
    auto const termExpected = fmt::format("{}", fmt::styled("x"sv, termStyle));
    CHECK_RT(termExpected, "{}"_sc, fmt::styled("x"sv, termStyle));
}

void timeRoundTrips() {
    CHECK_RT("123ms", "{}"_sc, std::chrono::milliseconds{123});
    CHECK_RT("-5s", "{}"_sc, std::chrono::seconds{-5});
    CHECK_RT("42ns", "{}"_sc, std::chrono::nanoseconds{42});
    CHECK_RT("2h", "{}"_sc, std::chrono::hours{2});
    CHECK_RT("1.5s", "{}"_sc, std::chrono::duration<double>{1.5});
    CHECK_RT("123", "{:%Q}"_sc, std::chrono::milliseconds{123});
    CHECK_RT("5[3/7]s", "{}"_sc, std::chrono::duration<std::int64_t, std::ratio<3, 7>>{5});

    // Calendar specs work for sane values and huge %Q values still print...
    CHECK_RT(fmt::format("{:%j}", std::chrono::duration<double>{172800.0}),
             "{:%j}"_sc,
             std::chrono::duration<double>{172800.0});
    CHECK_RT(fmt::format("{:%Q}", std::chrono::duration<double>{5.5e43}),
             "{:%Q}"_sc,
             std::chrono::duration<double>{5.5e43});

    // ...but huge values with calendar specs are rejected: fmt's float->int day count
    // cast is UB for them (found by fuzzing).
    {
        bool       errorReported = false;
        auto const buffer        = serialize("{:%j}"_sc, std::chrono::duration<double>{5.5e43});
        auto const [message, remaining, discarded]
          = remote_fmt::parse(std::span{buffer}, emptyCatalog(), [&](std::string_view) {
                errorReported = true;
            });
        CHECK(!message, "huge duration with calendar spec fails");
        CHECK(errorReported, "huge duration with calendar spec reports an error");
    }
}

void multipleMessages() {
    auto       first  = serialize("first {}"_sc, 1);
    auto const second = serialize("second {}"_sc, 2);
    first.insert(first.end(), second.begin(), second.end());

    auto const [message1, remaining1, discarded1]
      = remote_fmt::parse(std::span{first}, emptyCatalog(), [](std::string_view) {});
    CHECK(message1.has_value() && *message1 == "first 1", "first message parses");
    CHECK(discarded1 == 0, "no bytes discarded before first message");

    auto const [message2, remaining2, discarded2]
      = remote_fmt::parse(remaining1, emptyCatalog(), [](std::string_view) {});
    CHECK(message2.has_value() && *message2 == "second 2", "second message parses");
    CHECK(remaining2.empty(), "buffer fully consumed");
    CHECK(discarded2 == 0, "no bytes discarded before second message");
}

void malformedInput() {
    {
        auto const [message, remaining, discarded] = remote_fmt::parse(std::span<std::byte const>{},
                                                                       emptyCatalog(),
                                                                       [](std::string_view) {});
        CHECK(!message && remaining.empty() && discarded == 0, "empty buffer");
    }

    {
        std::vector<std::byte> const garbage{std::byte{0x00}, std::byte{0x01}, std::byte{0x02}};
        auto const [message, remaining, discarded]
          = remote_fmt::parse(std::span{garbage}, emptyCatalog(), [](std::string_view) {});
        CHECK(!message, "garbage produces no message");
        CHECK(discarded == 3, "garbage fully discarded");
        CHECK(remaining.empty(), "garbage buffer consumed");
    }

    {
        auto buffer = serialize("Test {}"_sc, 123);
        buffer.insert(buffer.begin(), {std::byte{0x00}, std::byte{0x13}});
        auto const [message, remaining, discarded]
          = remote_fmt::parse(std::span{buffer}, emptyCatalog(), [](std::string_view) {});
        CHECK(message.has_value() && *message == "Test 123", "message after garbage parses");
        CHECK(discarded == 2, "garbage prefix discarded");
        CHECK(remaining.empty(), "buffer fully consumed");
    }

    {
        auto truncated = serialize("Test {}"_sc, 123);
        truncated.pop_back();
        auto const [message, remaining, discarded]
          = remote_fmt::parse(std::span{truncated}, emptyCatalog(), [](std::string_view) {});
        CHECK(!message, "missing end marker fails");
        CHECK(remaining.size() == truncated.size(), "incomplete message kept for retry");
        CHECK(discarded == 0, "incomplete message not discarded");
    }

    {
        // Drop one payload byte so the end marker is consumed as argument data.
        auto corrupted = serialize("{}"_sc, 0x11223344);
        corrupted.erase(std::prev(corrupted.end(), 2));
        auto const [message, remaining, discarded]
          = remote_fmt::parse(std::span{corrupted}, emptyCatalog(), [](std::string_view) {});
        CHECK(!message, "corrupted payload fails");
    }

    {
        // Streaming: an incomplete prefix parses to nothing, the full buffer succeeds.
        auto const full   = serialize("Test {}"_sc, 123);
        auto const prefix = std::span{full}.subspan(0, full.size() / 2);
        auto const [message, remaining, discarded]
          = remote_fmt::parse(prefix, emptyCatalog(), [](std::string_view) {});
        CHECK(!message, "incomplete prefix produces no message");
        CHECK(remaining.size() == prefix.size(), "incomplete prefix kept for retry");

        auto const [fullMessage, fullRemaining, fullDiscarded]
          = remote_fmt::parse(std::span{full}, emptyCatalog(), [](std::string_view) {});
        CHECK(fullMessage.has_value() && *fullMessage == "Test 123", "full buffer parses");
    }

    {
        // A format spec that does not match the argument type reports an error at parse time.
        bool       errorReported = false;
        auto const buffer        = serialize("{:d}"_sc, "not a number");
        auto const [message, remaining, discarded]
          = remote_fmt::parse(std::span{buffer}, emptyCatalog(), [&](std::string_view) {
                errorReported = true;
            });
        CHECK(!message, "bad format spec fails");
        CHECK(errorReported, "bad format spec reports an error");
    }
}

}   // namespace

int main() {
    trivialRoundTrips();
    stringRoundTrips();
    rangeRoundTrips();
    tupleRoundTrips();
    extendedTypeRoundTrips();
    timeRoundTrips();
    multipleMessages();
    malformedInput();

    if(failures != 0) {
        std::printf("%d test(s) failed\n", failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
