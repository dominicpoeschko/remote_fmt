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

// Assembles a message the Printer cannot produce: start marker, a normal fmt-string header, the
// format string, one 4-byte signed argument, end marker. Needed for specs the serializer rejects at
// compile time but a hostile stream could still send.
std::vector<std::byte> rawFrame(std::string_view fmtString,
                                std::int32_t     value) {
    std::vector<std::byte> buffer;
    buffer.push_back(remote_fmt::protocol::Start_marker);
    buffer.push_back(
      remote_fmt::detail::fmtStringTypeIdentifier<remote_fmt::detail::FmtStringType::normal>(
        remote_fmt::detail::RangeSize::_1));
    buffer.push_back(static_cast<std::byte>(fmtString.size()));
    for(char const character : fmtString) { buffer.push_back(static_cast<std::byte>(character)); }
    buffer.push_back(
      remote_fmt::detail::trivialTypeIdentifier<remote_fmt::detail::TrivialType::signed_,
                                                remote_fmt::detail::TypeSize::_4>());
    auto const unsignedValue = static_cast<std::uint32_t>(value);
    for(unsigned shift = 0; shift < 32U; shift += 8U) {
        buffer.push_back(static_cast<std::byte>((unsignedValue >> shift) & 0xFFU));
    }
    buffer.push_back(remote_fmt::protocol::End_marker);
    return buffer;
}

// Same, with an inline string argument, for specs that are wrong for a string.
std::vector<std::byte> rawStringFrame(std::string_view fmtString,
                                      std::string_view value) {
    std::vector<std::byte> buffer;
    buffer.push_back(remote_fmt::protocol::Start_marker);
    buffer.push_back(
      remote_fmt::detail::fmtStringTypeIdentifier<remote_fmt::detail::FmtStringType::normal>(
        remote_fmt::detail::RangeSize::_1));
    buffer.push_back(static_cast<std::byte>(fmtString.size()));
    for(char const character : fmtString) { buffer.push_back(static_cast<std::byte>(character)); }
    buffer.push_back(
      remote_fmt::detail::rangeTypeIdentifier<remote_fmt::detail::RangeType::string,
                                              remote_fmt::detail::RangeLayout::compact>(
        remote_fmt::detail::RangeSize::_1));
    buffer.push_back(static_cast<std::byte>(value.size()));
    for(char const character : value) { buffer.push_back(static_cast<std::byte>(character)); }
    buffer.push_back(remote_fmt::protocol::End_marker);
    return buffer;
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

// Differential check: the round trip must produce byte-for-byte what fmt produces for the same
// format string and the same arguments. Unlike CHECK_RT there is no hand-written expectation, so
// these cannot quietly drift out of step with fmt when it changes its output - the oracle is fmt
// itself. This is the deterministic counterpart of what fuzz_roundtrip.cpp does with random data.
//
// Two rules for adding cases here:
//   - Only types fmt can format itself. The protocol deliberately renders std::optional,
//     std::expected, std::variant, std::byte, enums and nullptr its own way, so those belong in
//     the CHECK_RT sections with a written-out expectation, not here.
//   - Arguments are evaluated twice, so they must be side-effect free. Passing a temporary is
//     required for fmt::styled, which fmt refuses to accept as an lvalue.
#define CHECK_PARITY(fmtLiteral, ...)                                                      \
    do {                                                                                   \
        auto const parityExpected = fmt::format(fmtLiteral __VA_OPT__(, ) __VA_ARGS__);    \
        auto const parityActual   = roundTrip(fmtLiteral##_sc __VA_OPT__(, ) __VA_ARGS__); \
        if(!parityActual) {                                                                \
            std::printf("FAIL: parse failed for %s, fmt gives \"%s\" (line %d)\n",         \
                        #fmtLiteral,                                                       \
                        parityExpected.c_str(),                                            \
                        __LINE__);                                                         \
            ++failures;                                                                    \
        } else if(*parityActual != parityExpected) {                                       \
            std::printf("FAIL: %s -> got \"%s\" fmt gives \"%s\" (line %d)\n",             \
                        #fmtLiteral,                                                       \
                        parityActual->c_str(),                                             \
                        parityExpected.c_str(),                                            \
                        __LINE__);                                                         \
            ++failures;                                                                    \
        }                                                                                  \
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
    // Nested in a tuple, chars and strings get fmt's debug format - see debugFormatInRanges().
    CHECK_RT("(1, 'x', \"s\")", "{}"_sc, std::tuple{1, 'x', "s"sv});
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
    CHECK_RT("optional(42)", "{}"_sc, std::optional<int>{42});
    CHECK_RT("none", "{}"_sc, std::optional<int>{});
    CHECK_RT("expected(1)", "{}"_sc, std::expected<int, int>{1});
    CHECK_RT("unexpected(2)", "{}"_sc, std::expected<int, int>{std::unexpected{2}});
    CHECK_RT("expected()", "{}"_sc, std::expected<void, int>{});
    CHECK_RT("variant(7)", "{}"_sc, std::variant<int, std::string>{7});
    CHECK_RT("variant(\"v\")", "{}"_sc, std::variant<int, std::string>{"v"});

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

// Strings and chars nested in a range or tuple carry fmt's debug format: quoted, with control
// bytes and invalid UTF-8 escaped, and valid UTF-8 passed through. An explicit nested spec turns
// the debug format off, exactly as it does in fmt. Every expectation here is fmt's own output for
// the same container, so the two stay pinned together.
void debugFormatInRanges() {
    CHECK_RT("[\"a\\tb\"]", "{}"_sc, std::vector<std::string>{"a\tb"});
    CHECK_RT("[\"a\\x00b\"]",
             "{}"_sc,
             std::vector<std::string>{
               std::string{"a\0b", 3}
    });
    CHECK_RT("[\"a\\x1b[31m\"]", "{}"_sc, std::vector<std::string>{"a\x1b[31m"});
    CHECK_RT("[\"a\\\"b\"]", "{}"_sc, std::vector<std::string>{"a\"b"});
    CHECK_RT("[\"a\\\\b\"]", "{}"_sc, std::vector<std::string>{"a\\b"});
    CHECK_RT("[\"h\xc3\xa9llo\"]", "{}"_sc, std::vector<std::string>{"h\xc3\xa9llo"});
    CHECK_RT("[\"a\\xffb\"]",
             "{}"_sc,
             std::vector<std::string>{"a\xff"
                                      "b"});

    CHECK_RT("['a', 'b']", "{}"_sc, std::vector<char>{'a', 'b'});
    CHECK_RT("['\\t', '\\x00']", "{}"_sc, std::vector<char>{'\t', '\0'});
    CHECK_RT("['\\'']", "{}"_sc, std::vector<char>{'\''});
    CHECK_RT("{'k': 1}",
             "{}"_sc,
             std::map<char, int>{
               {'k', 1}
    });
    CHECK_RT("{\"k\\tv\": 1}",
             "{}"_sc,
             std::map<std::string, int>{
               {"k\tv", 1}
    });

    // An explicit nested spec replaces the debug format, so no quotes and no escaping.
    CHECK_RT("[   ab]", "{::>5}"_sc, std::vector<std::string>{"ab"});
    CHECK_RT("[  a]", "{::>3}"_sc, std::vector<char>{'a'});

    // Top level is not a range: no debug format there.
    CHECK_RT("a\tb", "{}"_sc, std::string{"a\tb"});
    CHECK_RT("\t", "{}"_sc, '\t');
}

// The parser hands width and precision straight to fmt, which allocates the padding eagerly, so
// a number arriving off the wire is bounded. Without the bound a 27-byte message carrying
// "{:2147483647}" made the host allocate 2 GiB in one malloc (found by fuzzing).
void replacementFieldNumberLimit() {
    auto rejects = [](auto fmtString, auto... args) {
        bool       errorReported = false;
        auto const buffer        = serialize(fmtString, args...);
        auto const [message, remaining, discarded]
          = remote_fmt::parse(std::span{buffer}, emptyCatalog(), [&](std::string_view) {
                errorReported = true;
            });
        return !message && errorReported;
    };

    CHECK(rejects("{:2147483647}"_sc, 1), "width 2147483647 rejected");
    CHECK(rejects("{:100000}"_sc, 1), "width 100000 rejected");
    CHECK(rejects("{:4097}"_sc, 1), "width 4097 rejected");

    // Dynamic specs take the number from an argument instead of the field text, so the digit scan
    // cannot see it and the bound would be bypassed. "{0:{0}}" is the one that matters: it reuses
    // the argument as its own width, so one integer off the wire asks fmt for gigabytes (found by
    // fuzzing).
    //
    // These cannot go through serialize() - the Printer's compile-time checkFormatString rejects a
    // dynamic spec outright, which is exactly why the protocol never emits one and only a malformed
    // or hostile stream can carry it. So the frames are assembled by hand.
    {
        auto const rejectsRaw = [](std::string_view fmtString) {
            auto const buffer        = rawFrame(fmtString, 0x7FFFFFFF);
            bool       errorReported = false;
            auto const [message, remaining, discarded]
              = remote_fmt::parse(std::span{buffer}, emptyCatalog(), [&](std::string_view) {
                    errorReported = true;
                });
            return !message && errorReported;
        };

        // Positive control first: the hand-built frame really is a frame the parser accepts.
        auto const control = rawFrame("v={}", 7);
        auto const [controlMessage, controlRemaining, controlDiscarded]
          = remote_fmt::parse(std::span{control}, emptyCatalog(), [](std::string_view) {});
        CHECK(controlMessage.has_value() && *controlMessage == "v=7", "hand-built frame parses");
        CHECK(controlRemaining.empty() && controlDiscarded == 0, "hand-built frame consumed whole");

        // The shape the fuzzer actually produced. Trailing text after the inner brace keeps the
        // outer braces balanced, so this gets past checkReplacementFieldCount and reaches the
        // nested-brace rule, which reports it.
        CHECK(rejectsRaw("{0:{0}Q%q0r}"), "self-referential dynamic width reported");

        // These are stopped earlier and more bluntly: checkReplacementFieldCount reads the trailing
        // "}}" as an escaped brace, so the count never balances and the format string is refused
        // before any field is looked at. Refused all the same, just without an error message.
        auto const refusedQuietly = [](std::string_view fmtString) {
            auto const buffer = rawFrame(fmtString, 0x7FFFFFFF);
            auto const [message, remaining, discarded]
              = remote_fmt::parse(std::span{buffer}, emptyCatalog(), [](std::string_view) {});
            return !message;
        };
        CHECK(refusedQuietly("{0:{0}}"), "self-referential dynamic width refused");
        CHECK(refusedQuietly("{:{}}"), "dynamic width refused");
        CHECK(refusedQuietly("{:.{}}"), "dynamic precision refused");
        CHECK(refusedQuietly("{:{0}}"), "indexed dynamic width refused");
    }

    // The limit itself still works, as does the largest precision anyone can justify: the exact
    // decimal expansion of a double needs ~767 significant digits.
    {
        auto const atLimit = roundTrip("{:4096}"_sc, 1);
        CHECK(atLimit && atLimit->size() == 4096, "width 4096 accepted");
        auto const precise = roundTrip("{:.1100f}"_sc, 1.5);
        CHECK(precise && precise->size() == 1101, "precision 1100 accepted");
    }

    // Range element specs use colons rather than braces, so the nested-brace rule leaves them alone.
    {
        auto const nested = roundTrip("{:::>4}"_sc, std::vector<std::vector<int>>{{1}});
        CHECK(nested && *nested == "[[   1]]", "nested element spec still accepted");
    }

    // Only the replacement field is bounded - literal text keeps its numbers.
    CHECK_RT("reboots 1000000 ok", "reboots 1000000 ok"_sc);
    CHECK_RT("         7", "{:>10}"_sc, 7);
}

void fmtParityScalars() {
    CHECK_PARITY("no arguments at all");
    CHECK_PARITY("braces {{}} and {}", 1);

    CHECK_PARITY("{}", std::uint8_t{0});
    CHECK_PARITY("{}", std::uint8_t{255});
    CHECK_PARITY("{}", std::uint16_t{65535});
    CHECK_PARITY("{}", std::uint32_t{4294967295U});
    CHECK_PARITY("{}", std::numeric_limits<std::uint64_t>::max());
    CHECK_PARITY("{}", std::int8_t{-128});
    CHECK_PARITY("{}", std::int16_t{-32768});
    CHECK_PARITY("{}", std::numeric_limits<std::int32_t>::min());
    CHECK_PARITY("{}", std::numeric_limits<std::int64_t>::min());
    CHECK_PARITY("{}", std::numeric_limits<std::int64_t>::max());

    CHECK_PARITY("{}", true);
    CHECK_PARITY("{}", false);
    CHECK_PARITY("{}", 'x');
    CHECK_PARITY("{}", '\t');
    CHECK_PARITY("{}", std::bit_cast<void const*>(std::uintptr_t{0x1234}));
    CHECK_PARITY("{}", std::bit_cast<void const*>(std::uintptr_t{0}));

    CHECK_PARITY("{} {} {}", -1, 2.5, true);
    CHECK_PARITY("a {} b {} c {}", 1, 'z', "s"sv);
}

// Floats are where a serializer is most likely to disagree with fmt: the bits go over the wire and
// fmt's shortest-round-trip formatting runs host side. Every one of these has to come back
// identical or the two implementations have diverged.
void fmtParityFloats() {
    for(double const value : {0.0,
                              -0.0,
                              1.0,
                              -1.0,
                              0.1,
                              1.0 / 3.0,
                              2.5,
                              1e300,
                              1e-300,
                              5e-324,
                              1.7976931348623157e308,
                              2.2250738585072014e-308,
                              3.141592653589793})
    {
        CHECK_PARITY("{}", value);
        CHECK_PARITY("{:e}", value);
        CHECK_PARITY("{:g}", value);
        CHECK_PARITY("{:a}", value);
        CHECK_PARITY("{:.17g}", value);
        CHECK_PARITY("{:.0f}", value);
    }

    for(float const value : {0.0F, -1.5F, 1.0F / 3.0F, 3.4028235e38F, 1.17549435e-38F}) {
        CHECK_PARITY("{}", value);
        CHECK_PARITY("{:e}", value);
    }

    auto const inf = std::numeric_limits<double>::infinity();
    auto const nan = std::numeric_limits<double>::quiet_NaN();
    CHECK_PARITY("{}", inf);
    CHECK_PARITY("{}", -inf);
    CHECK_PARITY("{}", nan);
    CHECK_PARITY("{:>10}", inf);
    CHECK_PARITY("{:+}", inf);
    CHECK_PARITY("{}", std::numeric_limits<float>::infinity());
    CHECK_PARITY("{}", std::numeric_limits<float>::quiet_NaN());
}

void fmtParityStrings() {
    CHECK_PARITY("{}", ""sv);
    CHECK_PARITY("{}", "plain"sv);
    CHECK_PARITY("{}", std::string{"std::string"});
    CHECK_PARITY("{}", "with \"quotes\" and \\ backslash"sv);
    CHECK_PARITY("{}", "tab\there"sv);
    CHECK_PARITY("{}", "h\xc3\xa9llo"sv);

    // An explicit debug spec at top level: the protocol passes it through to fmt unchanged.
    CHECK_PARITY("{:?}", "tab\there"sv);
    CHECK_PARITY("{:?}", "a\"b"sv);
}

void fmtParityRanges() {
    CHECK_PARITY("{}", std::vector<int>{});
    CHECK_PARITY("{}", std::vector<int>{1});
    CHECK_PARITY("{}", std::vector<int>{1, 2, 3});
    CHECK_PARITY("{}", std::vector<double>{1.5, -2.5, 0.1});
    CHECK_PARITY("{}", (std::array<int, 4>{1, 2, 3, 4}));
    CHECK_PARITY("{}", std::set<int>{3, 1, 2});
    CHECK_PARITY("{}",
                 std::vector<std::vector<int>>{
                   {1, 2},
                   {}
    });

    CHECK_PARITY("{}", std::vector<char>{'a', 'b'});
    CHECK_PARITY("{}", std::vector<char>{'\t', '\0', '\''});
    CHECK_PARITY("{}", std::vector<std::string>{"a", "b"});
    CHECK_PARITY("{}",
                 std::vector<std::string>{
                   "a\tb",
                   std::string{"n\0l", 3},
                   "a\xff"
                   "b"
    });
    CHECK_PARITY("{}", std::vector<std::string>{"q\"uote", "back\\slash"});

    CHECK_PARITY("{}",
                 (std::map<int, int>{
                   {1, 2},
                   {3, 4}
    }));
    CHECK_PARITY("{}",
                 (std::map<std::string, int>{
                   {   "k", 1},
                   {"k\tv", 2}
    }));
    CHECK_PARITY("{}",
                 (std::map<char, int>{
                   { 'k', 1},
                   {'\t', 2}
    }));

    // Nested specs: an explicit one replaces fmt's debug format for the elements, and the range
    // flags change the delimiters. Both have to track fmt exactly.
    CHECK_PARITY("{::>5}", std::vector<std::string>{"ab", "cd"});
    CHECK_PARITY("{::>3}", std::vector<char>{'a', 'b'});
    CHECK_PARITY("{:n}", std::vector<int>{1, 2, 3});
    CHECK_PARITY("{::04x}", std::vector<int>{1, 255});
    CHECK_PARITY("{::+}", std::vector<int>{1, -2});
}

// Nested element specs: everything after the second colon describes the element rather than the
// range, and a range of ranges nests a third time ("{:::>4}"). These are the specs most likely to
// drift, because the parser has to split the field the same way fmt's range_formatter does and
// then hand each level to the right formatter.
//
// Note what fmt does *not* accept, so nobody adds it here expecting it to work: for a map the
// element is a pair, whose formatter takes no spec of its own, so "{::n}" and "{::>12}" on a map
// are compile errors. "{:n}" on the map itself is fine.
void fmtParityNestedSpecs() {
    std::map<int, int> const intMap{
      {1, 2},
      {3, 4}
    };
    std::map<std::string, int> const stringMap{
      {   "k", 1},
      {"a\tb", 2}
    };
    std::map<char, int> const charMap{
      { 'k', 1},
      {'\t', 2}
    };
    std::set<int> const                 intSet{1, 2};
    std::vector<int> const              ints{1, 255};
    std::vector<std::string> const      strings{"ab", "c\td"};
    std::vector<std::vector<int>> const nested{
      {1, 2},
      {3}
    };

    CHECK_PARITY("{::}", intMap);
    CHECK_PARITY("{:n}", intMap);
    CHECK_PARITY("{::}", stringMap);
    CHECK_PARITY("{:n}", stringMap);
    CHECK_PARITY("{::}", charMap);
    CHECK_PARITY("{:n}", charMap);

    CHECK_PARITY("{::}", intSet);
    CHECK_PARITY("{:n}", intSet);

    CHECK_PARITY("{::}", ints);
    CHECK_PARITY("{::#x}", ints);
    CHECK_PARITY("{::04x}", ints);
    CHECK_PARITY("{::>8}", ints);
    CHECK_PARITY("{::+}", ints);

    // An empty explicit element spec still counts as explicit, so it turns fmt's debug format off
    // and the string comes out unquoted and unescaped. "{}" is the only field that keeps it on.
    CHECK_PARITY("{::}", strings);
    CHECK_PARITY("{::?}", strings);
    CHECK_PARITY("{::>6}", strings);

    // Range of ranges: outer, inner, then the element of the inner.
    CHECK_PARITY("{}", nested);
    CHECK_PARITY("{::}", nested);
    CHECK_PARITY("{:::>4}", nested);
    CHECK_PARITY("{:::#x}", nested);
    CHECK_PARITY("{::n}", nested);
    CHECK_PARITY("{:n}", nested);
}

void fmtParityTuples() {
    CHECK_PARITY("{}", (std::pair{1, 2}));
    CHECK_PARITY("{}", (std::tuple{1, 'x', "s"sv}));
    CHECK_PARITY("{}",
                 (std::tuple{
                   1,
                   '\t',
                   std::string{"a\0b", 3}
    }));
    CHECK_PARITY("{}",
                 (std::tuple{
                   std::pair{1, 2},
                   3
    }));
    CHECK_PARITY("{}", (std::tuple{1.5, -2.5}));
}

void fmtParityChrono() {
    CHECK_PARITY("{}", std::chrono::nanoseconds{42});
    CHECK_PARITY("{}", std::chrono::microseconds{-42});
    CHECK_PARITY("{}", std::chrono::milliseconds{123});
    CHECK_PARITY("{}", std::chrono::seconds{-5});
    CHECK_PARITY("{}", std::chrono::minutes{90});
    CHECK_PARITY("{}", std::chrono::hours{2});
    CHECK_PARITY("{}", (std::chrono::duration<double>{1.5}));
    CHECK_PARITY("{}", (std::chrono::duration<float>{-0.25F}));
    CHECK_PARITY("{}", (std::chrono::duration<std::int64_t, std::ratio<3, 7>>{5}));
    CHECK_PARITY("{}", (std::chrono::duration<std::int64_t, std::ratio<1, 1000000000000>>{7}));
    CHECK_PARITY("{}", std::chrono::milliseconds{std::numeric_limits<std::int32_t>::min()});

    // %Q prints the tick count only, so it is defined for every value. Calendar specs are left to
    // timeRoundTrips(): fmt implements them with a float-to-int cast that is UB for huge durations.
    CHECK_PARITY("{:%Q}", std::chrono::milliseconds{123});
    CHECK_PARITY("{:%Q}", (std::chrono::duration<double>{1.5}));
    CHECK_PARITY("{:%Q%q}", std::chrono::milliseconds{123});
    CHECK_PARITY("{:%q}", std::chrono::nanoseconds{1});
}

void fmtParitySpecs() {
    CHECK_PARITY("{:>10}", 7);
    CHECK_PARITY("{:<10}", 7);
    CHECK_PARITY("{:^10}", 7);
    CHECK_PARITY("{:*^7}", "ab"sv);
    CHECK_PARITY("{:*>7}", 42);
    CHECK_PARITY("{:07}", -42);
    CHECK_PARITY("{:+}", 42);
    CHECK_PARITY("{: }", 42);
    CHECK_PARITY("{:x}", 255);
    CHECK_PARITY("{:X}", 255);
    CHECK_PARITY("{:#x}", 255);
    CHECK_PARITY("{:#o}", 255);
    CHECK_PARITY("{:#b}", 255);
    CHECK_PARITY("{:04x}", 255);
    CHECK_PARITY("{:#010b}", 5);
    CHECK_PARITY("{:c}", 65);
    CHECK_PARITY("{:d}", 'A');
    CHECK_PARITY("{:.3f}", 3.14159);
    CHECK_PARITY("{:.0e}", 12345.0);
    CHECK_PARITY("{:12.4f}", -3.14159);
    CHECK_PARITY("{:.1100f}", 1.5);
    CHECK_PARITY("{:4096}", 1);
    CHECK_PARITY("{}", 'A');
    CHECK_PARITY("{:s}", true);
    CHECK_PARITY("{:d}", true);
}

void fmtParityStyled() {
    auto const rgb = fmt::fg(fmt::color::red) | fmt::emphasis::bold;
    CHECK_PARITY("{}", fmt::styled(42, rgb));
    CHECK_PARITY("{}", fmt::styled("text"sv, rgb));
    CHECK_PARITY("{}", fmt::styled(1.5, fmt::bg(fmt::color::blue)));
    CHECK_PARITY("{}", fmt::styled(7, fmt::fg(fmt::terminal_color::green)));
    CHECK_PARITY("{}", fmt::styled(7, fmt::emphasis::underline | fmt::emphasis::italic));
    CHECK_PARITY("{:>8}", fmt::styled(7, fmt::fg(fmt::terminal_color::bright_cyan)));
}

// Wrapper types: optional, variant and expected. These used to be the one family that still
// diverged from fmt - the payload was rendered as if it were top level, and the optional(...) and
// variant(...) wrappers were dropped entirely. They now track fmt, so fmt is a valid oracle and
// these are ordinary parity checks.
//
// std::variant is the one case that needed a wire change to make possible: it used to be serialized
// transparently, with std::visit handing the active alternative straight to its own formatter, so no
// marker distinguished a variant from a bare value and the parser could not have reproduced the
// wrapper from the bytes alone. It now carries an ExtendedTypeIdentifier::variant marker.
void fmtParityWrappers() {
    CHECK_PARITY("{}", std::optional<int>{42});
    CHECK_PARITY("{}", std::optional<int>{});
    CHECK_PARITY("{}", (std::optional<std::string>{"s"}));
    CHECK_PARITY("{}", (std::optional<std::string>{"a\tb"}));
    CHECK_PARITY("{}",
                 (std::optional<std::string>{
                   std::string{"a\0b", 3}
    }));
    CHECK_PARITY("{}", (std::optional<char>{'c'}));
    CHECK_PARITY("{}", (std::optional<char>{'\t'}));
    CHECK_PARITY("{}",
                 (std::optional<std::vector<std::string>>{
                   {"a", "b"}
    }));
    CHECK_PARITY("{}",
                 (std::optional<std::vector<int>>{
                   {1, 2}
    }));
    CHECK_PARITY("{}", (std::optional<std::optional<int>>{std::optional<int>{1}}));
    CHECK_PARITY("{}", (std::optional<std::optional<int>>{std::optional<int>{}}));

    CHECK_PARITY("{}", (std::variant<int, std::string>{7}));
    CHECK_PARITY("{}", (std::variant<int, std::string>{"vs"}));
    CHECK_PARITY("{}", (std::variant<int, std::string>{"a\tb"}));
    CHECK_PARITY("{}", (std::variant<char, int>{'c'}));
    CHECK_PARITY("{}",
                 (std::variant<std::vector<int>, int>{
                   std::vector<int>{1, 2}
    }));

    CHECK_PARITY("{}", (std::expected<int, int>{1}));
    CHECK_PARITY("{}", (std::expected<int, int>{std::unexpected{2}}));
    CHECK_PARITY("{}", (std::expected<std::string, int>{"ok"}));
    CHECK_PARITY("{}", (std::expected<int, std::string>{std::unexpected{"err"}}));
    CHECK_PARITY("{}", (std::expected<int, std::string>{std::unexpected{"e\tr"}}));
    CHECK_PARITY("{}", (std::expected<char, int>{'c'}));
    CHECK_PARITY("{}", (std::expected<void, int>{}));
    CHECK_PARITY("{}", (std::expected<void, std::string>{std::unexpected{"err"}}));

    // Wrappers around each other, where the nested position has to survive two levels.
    CHECK_PARITY(
      "{}",
      (std::optional<std::variant<int, std::string>>{std::variant<int, std::string>{"s"}}));
    CHECK_PARITY("{}",
                 (std::optional<std::optional<std::string>>{std::optional<std::string>{"s"}}));
    CHECK_PARITY(
      "{}",
      (std::expected<std::variant<int, std::string>, int>{std::variant<int, std::string>{"s"}}));
    CHECK_PARITY("{}", (std::expected<std::optional<int>, int>{std::optional<int>{1}}));
    CHECK_PARITY("{}", (std::variant<int, std::optional<int>>{std::optional<int>{1}}));
    CHECK_PARITY("{}",
                 (std::vector<std::optional<int>>{std::optional<int>{1}, std::optional<int>{}}));
    CHECK_PARITY("{}", (std::vector<std::optional<std::string>>{std::optional<std::string>{"s"}}));
}

// The one place the protocol still differs, and it differs because fmt contradicts itself.
//
// fmt's optional formatter forwards the debug format to its payload, and so do ranges:
//     optional<optional<string>>   -> optional(optional("s"))
//     vector<optional<string>>     -> [optional("s")]
//     expected<variant<string>>    -> expected(variant("s"))
// but expected's and variant's formatters do not forward it into a nested optional:
//     expected<optional<string>>   -> expected(optional(s))     <- quotes lost
//     variant<optional<string>>    -> variant(optional(s))      <- quotes lost
//
// The protocol propagates the nested position consistently, so it quotes in all five. Matching fmt
// exactly would mean reproducing a rule that disagrees with fmt one level up and one type over -
// an upstream bug rather than a design decision - and undoing it again once fmt fixes it.
//
// Both sides are pinned so this fires either way: if fmt starts quoting, the fmtExpected assertion
// fails and these cases can move into fmtParityWrappers(); if the protocol drifts, the other one does.
template<typename T>
void checkKnownFmtQuirk(char const*      what,
                        std::string_view protocolExpected,
                        std::string_view fmtExpected,
                        T const&         value) {
    auto const actual = roundTrip("{}"_sc, value);
    if(!actual) {
        std::printf("FAIL: %s did not parse\n", what);
        ++failures;
    } else if(*actual != protocolExpected) {
        std::printf("FAIL: %s protocol gives \"%s\" expected \"%s\"\n",
                    what,
                    actual->c_str(),
                    std::string{protocolExpected}.c_str());
        ++failures;
    }
    auto const viaFmt = fmt::format("{}", value);
    if(viaFmt != fmtExpected) {
        std::printf(
          "FAIL: %s fmt now gives \"%s\", this test expected \"%s\" - if fmt fixed its "
          "debug-format forwarding, move this case into fmtParityWrappers()\n",
          what,
          viaFmt.c_str(),
          std::string{fmtExpected}.c_str());
        ++failures;
    }
}

void optionalInsideWrapperQuirk() {
    checkKnownFmtQuirk(
      "expected<optional<string>>",
      "expected(optional(\"s\"))",
      "expected(optional(s))",
      std::expected<std::optional<std::string>, int>{std::optional<std::string>{"s"}});
    checkKnownFmtQuirk(
      "variant<optional<string>>",
      "variant(optional(\"s\"))",
      "variant(optional(s))",
      std::variant<int, std::optional<std::string>>{std::optional<std::string>{"s"}});
}

enum UnscopedStatus { active, idle };

// Enums are the one place the protocol does more than fmt rather than differently. fmt refuses both
// a scoped and an unscoped enum outright - it is a hard compile error, not an integer fallback - so
// there is no parity to check and no fmt output to compare against. The parser reflects the
// enumerator name with enchantum instead.
static_assert(
  !fmt::is_formattable<Color>::value,
  "fmt cannot format a scoped enum; if this starts failing, fmt gained enum support and "
  "these cases should be reconsidered against it");
static_assert(!fmt::is_formattable<UnscopedStatus>::value,
              "fmt cannot format an unscoped enum either");

void enumFormatting() {
    CHECK_RT("red", "{}"_sc, Color::red);
    CHECK_RT("blue", "{}"_sc, Color::blue);
    CHECK_RT("active", "{}"_sc, active);
    CHECK_RT("idle", "{}"_sc, idle);

    // A value with no enumerator has no name to reflect, so it falls back to the number.
    CHECK_RT("42", "{}"_sc, static_cast<Color>(42));
    CHECK_RT("7", "{}"_sc, static_cast<UnscopedStatus>(7));

    // The name travels as a string, so nested in a container it picks up the same debug quoting any
    // other string element gets.
    CHECK_RT("[\"red\", \"blue\"]", "{}"_sc, std::vector<Color>{Color::red, Color::blue});
    CHECK_RT("(\"green\", 1)", "{}"_sc, std::tuple{Color::green, 1});
    CHECK_RT("{\"red\": 1}",
             "{}"_sc,
             std::map<Color, int>{
               {Color::red, 1}
    });

    // An explicit element spec turns that quoting off, same as for any other string.
    CHECK_RT("[  red]", "{::>5}"_sc, std::vector<Color>{Color::red});
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
        // A spec that does not match its argument reports an error at parse time. Raw frame because the
        // serializer no longer compiles "{:d}" for a string - the host still sees untrusted bytes.
        bool       errorReported = false;
        auto const buffer        = rawStringFrame("{:d}", "not a number");
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
    debugFormatInRanges();
    replacementFieldNumberLimit();
    fmtParityScalars();
    fmtParityFloats();
    fmtParityStrings();
    fmtParityRanges();
    fmtParityNestedSpecs();
    fmtParityTuples();
    fmtParityChrono();
    fmtParitySpecs();
    fmtParityStyled();
    fmtParityWrappers();
    optionalInsideWrapperQuirk();
    enumFormatting();
    multipleMessages();
    malformedInput();

    if(failures != 0) {
        std::printf("%d test(s) failed\n", failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
