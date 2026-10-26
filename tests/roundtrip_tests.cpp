// Round-trip tests: serialize with remote_fmt::Printer, parse with remote_fmt::parseMessage and
// compare against the fmt formatted result. Also covers parser robustness on malformed
// buffers (garbage, truncation, resync).
#include <cstddef>

// The bytes the encoder puts into frame CRCs (printer.hpp REMOTE_FMT_CRC_HOOK): each byte of a frame exactly once.
namespace crc_count { inline std::size_t bytes = 0; }

#define REMOTE_FMT_CRC_HOOK(n) static_cast<void>(::crc_count::bytes += (n))

#include "TestSupport.ipp"
#include "remote_fmt/descriptor_grammar.hpp"
#include "remote_fmt/parser.hpp"
#include "remote_fmt/remote_fmt.hpp"
#include "remote_fmt/wire.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>

#if __has_include(<mp-units/systems/si.h>)
    #include <mp-units/systems/si.h>
    #define REMOTE_FMT_TEST_MP_UNITS 1
#endif
#include <vector>

using namespace sc::literals;
using namespace std::literals;

namespace {

using remote_fmt::test::emptyCatalog;
using remote_fmt::test::enumValue;
using remote_fmt::test::failures;
using remote_fmt::test::VectorBackend;

template<typename... Args>
std::vector<std::byte> serialize(auto fmtString,
                                 Args&&... args) {
    remote_fmt::Printer<VectorBackend> printer{};
    printer.print(fmtString, std::forward<Args>(args)...);
    return printer.getComBackend().memory;
}

void appendVarint(std::vector<std::byte>& out,
                  std::uint64_t           value) {
    while(value >= 0x80U) {
        out.push_back(static_cast<std::byte>((value & 0x7FU) | 0x80U));
        value >>= 7U;
    }
    out.push_back(static_cast<std::byte>(value));
}

// An inline string reference as the Printer writes one without a catalog: the inline id, the length, the bytes.
void appendInlineString(std::vector<std::byte>& out,
                        std::string_view        text) {
    out.push_back(std::byte{0xFF});
    out.push_back(std::byte{0xFF});
    appendVarint(out, text.size());
    for(char const character : text) { out.push_back(static_cast<std::byte>(character)); }
}

// A whole inline frame around `described` (format string, 0x1e, descriptors) and the values' bytes, with the CRC when
// this build checks its frames.
std::vector<std::byte> inlineFrame(std::string_view              described,
                                   std::vector<std::byte> const& values) {
    std::vector<std::byte> buffer;
    buffer.push_back(REMOTE_FMT_FRAME_CHECK ? remote_fmt::protocol::checkedStartMarker
                                            : remote_fmt::protocol::startMarker);
    appendInlineString(buffer, described);
    buffer.insert(buffer.end(), values.begin(), values.end());
    if constexpr(REMOTE_FMT_FRAME_CHECK != 0) {
        buffer.push_back(static_cast<std::byte>(remote_fmt::protocol::crcFinal(
          remote_fmt::protocol::crcOver(remote_fmt::protocol::crcInit, std::span{buffer}))));
    }
    buffer.push_back(remote_fmt::protocol::endMarker);
    return buffer;
}

std::string describedText(std::string_view fmtString,
                          std::string_view descriptors) {
    std::string out{fmtString};
    out += '\x1e';
    out += descriptors;
    return out;
}

// Assembles a message the Printer cannot produce: an inline format string with one 4-byte signed argument. Needed for
// specs the serializer rejects at compile time but a hostile stream could still send.
std::vector<std::byte> rawFrame(std::string_view fmtString,
                                std::int32_t     value) {
    std::vector<std::byte> values;
    auto const             zigzag
      = (static_cast<std::uint32_t>(value) << 1U) ^ static_cast<std::uint32_t>(value >> 31);
    appendVarint(values, zigzag);
    return inlineFrame(describedText(fmtString, "i4"), values);
}

// A frame whose single argument is a sub format string of pure literal text (a `*` argument), for specs that a
// rendered sub format string cannot take.
std::vector<std::byte> rawSubFmtStringFrame(std::string_view fmtString,
                                            std::string_view subFmtString) {
    std::vector<std::byte> values;
    appendInlineString(values, describedText(subFmtString, ""));
    return inlineFrame(describedText(fmtString, "*"), values);
}

// Same, with an inline string argument, for specs that are wrong for a string.
std::vector<std::byte> rawStringFrame(std::string_view fmtString,
                                      std::string_view value) {
    std::vector<std::byte> values;
    appendVarint(values, value.size());
    for(char const character : value) { values.push_back(static_cast<std::byte>(character)); }
    return inlineFrame(describedText(fmtString, "s"), values);
}

template<typename... Args>
std::optional<std::string> roundTrip(auto fmtString,
                                     Args&&... args) {
    auto const buffer = serialize(fmtString, std::forward<Args>(args)...);
    auto const [message, remaining, discarded, catalogId]
      = remote_fmt::parseMessage(std::span{buffer}, emptyCatalog(), [](std::string_view error) {
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
    CHECK_RT("42", "{}"_sc, enumValue<Color>(42));
}

void rangeRoundTrips() {
    CHECK_RT("[1, 2, 3]", "{}"_sc, std::vector<int>{1, 2, 3});
    CHECK_RT("[]", "{}"_sc, std::vector<int>{});
    CHECK_RT("[1.5, 2.5]", "{}"_sc, std::array<double, 2>{1.5, 2.5});
    CHECK_RT("{1, 2}", "{}"_sc, std::set<int>{1, 2});
    CHECK_RT("[true, false, true]", "{}"_sc, std::vector<bool>{true, false, true});
    CHECK_RT("[]", "{}"_sc, std::vector<bool>{});
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
        auto const [message, remaining, discarded, catalogId]
          = remote_fmt::parseMessage(std::span{buffer}, emptyCatalog(), [&](std::string_view) {
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
        auto const [message, remaining, discarded, catalogId]
          = remote_fmt::parseMessage(std::span{buffer}, emptyCatalog(), [&](std::string_view) {
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
            auto const [message, remaining, discarded, catalogId]
              = remote_fmt::parseMessage(std::span{buffer}, emptyCatalog(), [&](std::string_view) {
                    errorReported = true;
                });
            return !message && errorReported;
        };

        // Positive control first: the hand-built frame really is a frame the parser accepts.
        auto const control = rawFrame("v={}", 7);
        auto const [controlMessage, controlRemaining, controlDiscarded, controlId]
          = remote_fmt::parseMessage(std::span{control}, emptyCatalog(), [](std::string_view) {});
        CHECK(controlMessage.has_value() && *controlMessage == "v=7", "hand-built frame parses");
        CHECK(controlRemaining.empty() && controlDiscarded == 0, "hand-built frame consumed whole");

        // The shape the fuzzer actually produced. Trailing text after the inner brace keeps the
        // outer braces balanced, so this gets past the grammar's pre-check and reaches the
        // nested-brace rule, which reports it.
        CHECK(rejectsRaw("{0:{0}Q%q0r}"), "self-referential dynamic width reported");

        // The plain dynamic specs reach the same rule and are reported the same way. They used to be
        // refused a stage earlier and silently, by the escape rule that also mis-read "{:}}x}" as
        // balanced; now that braces escape only outside a field, they arrive here with an error.
        CHECK(rejectsRaw("{0:{0}}"), "self-referential dynamic width reported");
        CHECK(rejectsRaw("{:{}}"), "dynamic width reported");
        CHECK(rejectsRaw("{:.{}}"), "dynamic precision reported");
        CHECK(rejectsRaw("{:{0}}"), "indexed dynamic width reported");
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
                              std::numbers::pi})
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
    CHECK_PARITY("{}", std::vector<bool>{true, false, true});
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
// wrapper from the bytes alone. It now has a descriptor of its own (`V`: the index byte, then the alternative).
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

// a fixed underlying type: 7 below is then a value of the type (without one the range ends at 1, and the cast is UB)
enum UnscopedStatus : int { active, idle };

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
    CHECK_RT("42", "{}"_sc, enumValue<Color>(42));
    CHECK_RT("7", "{}"_sc, enumValue<UnscopedStatus>(7));

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

// Bitflags. The opt-in is these five operators - enchantum/common.hpp detects a bitflag enum by
// looking for them, there is no trait to specialize.
enum struct [[clang::flag_enum]] Perm : std::uint8_t {
    read  = 1 << 0,
    write = 1 << 1,
    exec  = 1 << 2
};

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

// Same, with a named zero so has_zero_flag is true.
enum struct [[clang::flag_enum]] Mode : std::uint8_t { none = 0, sync = 1 << 0, async = 1 << 1 };

constexpr Mode operator|(Mode a,
                         Mode b) {
    return Mode(std::uint8_t(a) | std::uint8_t(b));
}

constexpr Mode operator&(Mode a,
                         Mode b) {
    return Mode(std::uint8_t(a) & std::uint8_t(b));
}

[[maybe_unused]] constexpr Mode operator~(Mode a) { return Mode(std::uint8_t(~std::uint8_t(a))); }

[[maybe_unused]] constexpr Mode& operator|=(Mode& a,
                                            Mode  b) {
    return a = a | b;
}

[[maybe_unused]] constexpr Mode& operator&=(Mode& a,
                                            Mode  b) {
    return a = a & b;
}

// A 64-bit bitflag enum with its top bit: the descriptor spells that enumerator 9223372036854775808, past INT64_MAX.
enum struct [[clang::flag_enum]] Wide : std::uint64_t { lo = 1, hi = std::uint64_t{1} << 63U };

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

// A signed enum with a negative enumerator: '-' in the descriptor.
enum struct Level : std::int8_t { low = -100, zero = 0, high = 100 };

static_assert(enchantum::is_bitflag<Wide>,
              "Wide must be detected as a bitflag enum");
static_assert(enchantum::is_bitflag<Perm>,
              "Perm must be detected as a bitflag enum");
static_assert(enchantum::is_bitflag<Mode>,
              "Mode must be detected as a bitflag enum");
static_assert(!enchantum::has_zero_flag<Perm>,
              "Perm has no zero enumerator");
static_assert(enchantum::has_zero_flag<Mode>,
              "Mode::none is the zero enumerator");

void bitflagFormatting() {
    // A single flag is still a single enumerator, and reads exactly like a plain enum.
    CHECK_RT("read", "{}"_sc, Perm::read);
    CHECK_RT("exec", "{}"_sc, Perm::exec);

    // The point of the exercise: a combined value names both bits instead of printing 3.
    CHECK_RT("read|write", "{}"_sc, Perm::read | Perm::write);
    CHECK_RT("read|write|exec", "{}"_sc, Perm::read | Perm::write | Perm::exec);

    // Order follows the enumerator declaration, not the order the caller ORed them in.
    CHECK_RT("read|exec", "{}"_sc, Perm::exec | Perm::read);

    // No zero enumerator, so 0 has no name and falls back to the number.
    CHECK_RT("0", "{}"_sc, enumValue<Perm>(0));

    // A bit no enumerator covers means the names are not the whole value - printing "read" would
    // silently drop the 0x8, so the whole thing goes out as an integer.
    CHECK_RT("9", "{}"_sc, enumValue<Perm>(0x9));

    // With a named zero, 0 gets its name and travels as a plain enumerator, no sequence.
    CHECK_RT("none", "{}"_sc, Mode::none);
    CHECK_RT("sync|async", "{}"_sc, Mode::sync | Mode::async);

    // The spec applies to the joined string, not to each name: 20 wide in total.
    CHECK_RT("          read|write", "{:>20}"_sc, Perm::read | Perm::write);
    CHECK_RT("read|write..........", "{:.<20}"_sc, Perm::read | Perm::write);

    // Nested in a container the joined name picks up the same debug quoting any other string
    // element gets, and an explicit element spec turns it off.
    CHECK_RT("[\"read|write\", \"exec\"]",
             "{}"_sc,
             std::vector<Perm>{Perm::read | Perm::write, Perm::exec});
    CHECK_RT("[read|write]", "{::}"_sc, std::vector<Perm>{Perm::read | Perm::write});
    CHECK_RT("(\"read|exec\", 1)", "{}"_sc, std::tuple{Perm::read | Perm::exec, 1});
}

// Enumerator values as the descriptor spells them: an unsigned one in full, a signed one with '-' - read as the 64
// bits of the value either way, so both name the value on the host.
void enumeratorValues() {
    CHECK_RT("hi", "{}"_sc, Wide::hi);
    CHECK_RT("lo|hi", "{}"_sc, Wide::lo | Wide::hi);
    CHECK_RT("2", "{}"_sc, enumValue<Wide>(2));
    CHECK_RT("low", "{}"_sc, Level::low);
    CHECK_RT("zero high", "{} {}"_sc, Level::zero, Level::high);
    CHECK_RT("-5", "{}"_sc, enumValue<Level>(-5));

    // the grammar alone, at the edges of both ranges (the same strings as test_descriptor_split.py's)
    auto const skips
      = [](std::string_view d) { return remote_fmt::detail::grammar::skip(d) && d.empty(); };
    CHECK(skips("Gu82:1,2:lo9223372036854775808,2:hi"), "a u64 bitflag's top bit");
    CHECK(skips("Eu81:18446744073709551615,3:max"), "the largest unsigned value");
    CHECK(!skips("Eu81:18446744073709551616,3:max"), "past 64 bits: malformed");
    CHECK(skips("Ei81:-9223372036854775808,3:min"), "the smallest signed value");
    CHECK(!skips("Ei81:-9223372036854775809,3:min"), "below INT64_MIN: malformed");
}

void multipleMessages() {
    auto       first  = serialize("first {}"_sc, 1);
    auto const second = serialize("second {}"_sc, 2);
    first.insert(first.end(), second.begin(), second.end());

    auto const [message1, remaining1, discarded1, catalogId1]
      = remote_fmt::parseMessage(std::span{first}, emptyCatalog(), [](std::string_view) {});
    CHECK(message1.has_value() && *message1 == "first 1", "first message parses");
    CHECK(discarded1 == 0, "no bytes discarded before first message");

    auto const [message2, remaining2, discarded2, catalogId2]
      = remote_fmt::parseMessage(remaining1, emptyCatalog(), [](std::string_view) {});
    CHECK(message2.has_value() && *message2 == "second 2", "second message parses");
    CHECK(remaining2.empty(), "buffer fully consumed");
    CHECK(discarded2 == 0, "no bytes discarded before second message");
}

void malformedInput() {
    {
        auto const [message, remaining, discarded, catalogId]
          = remote_fmt::parseMessage(std::span<std::byte const>{},
                                     emptyCatalog(),
                                     [](std::string_view) {});
        CHECK(!message && remaining.empty() && discarded == 0, "empty buffer");
    }

    {
        std::vector<std::byte> const garbage{std::byte{0x00}, std::byte{0x01}, std::byte{0x02}};
        auto const [message, remaining, discarded, catalogId]
          = remote_fmt::parseMessage(std::span{garbage}, emptyCatalog(), [](std::string_view) {});
        CHECK(!message, "garbage produces no message");
        CHECK(discarded == 3, "garbage fully discarded");
        CHECK(remaining.empty(), "garbage buffer consumed");
    }

    {
        auto buffer = serialize("Test {}"_sc, 123);
        buffer.insert(buffer.begin(), {std::byte{0x00}, std::byte{0x13}});
        auto const [message, remaining, discarded, catalogId]
          = remote_fmt::parseMessage(std::span{buffer}, emptyCatalog(), [](std::string_view) {});
        CHECK(message.has_value() && *message == "Test 123", "message after garbage parses");
        CHECK(discarded == 2, "garbage prefix discarded");
        CHECK(remaining.empty(), "buffer fully consumed");
    }

    {
        auto truncated = serialize("Test {}"_sc, 123);
        truncated.pop_back();
        auto const [message, remaining, discarded, catalogId]
          = remote_fmt::parseMessage(std::span{truncated}, emptyCatalog(), [](std::string_view) {});
        CHECK(!message, "missing end marker fails");
        CHECK(remaining.size() == truncated.size(), "incomplete message kept for retry");
        CHECK(discarded == 0, "incomplete message not discarded");
    }

    {
        // Drop one payload byte so the end marker is consumed as argument data.
        auto corrupted = serialize("{}"_sc, 0x11223344);
        corrupted.erase(std::prev(corrupted.end(), 2));
        auto const [message, remaining, discarded, catalogId]
          = remote_fmt::parseMessage(std::span{corrupted}, emptyCatalog(), [](std::string_view) {});
        CHECK(!message, "corrupted payload fails");
    }

    {
        // Streaming: an incomplete prefix parses to nothing, the full buffer succeeds.
        auto const full   = serialize("Test {}"_sc, 123);
        auto const prefix = std::span{full}.subspan(0, full.size() / 2);
        auto const [message, remaining, discarded, catalogId]
          = remote_fmt::parseMessage(prefix, emptyCatalog(), [](std::string_view) {});
        CHECK(!message, "incomplete prefix produces no message");
        CHECK(remaining.size() == prefix.size(), "incomplete prefix kept for retry");

        auto const [fullMessage, fullRemaining, fullDiscarded, fullId]
          = remote_fmt::parseMessage(std::span{full}, emptyCatalog(), [](std::string_view) {});
        CHECK(fullMessage.has_value() && *fullMessage == "Test 123", "full buffer parses");
    }

    {
        // A spec that does not match its argument reports an error at parse time. Raw frame because the
        // serializer no longer compiles "{:d}" for a string - the host still sees untrusted bytes.
        bool       errorReported = false;
        auto const buffer        = rawStringFrame("{:d}", "not a number");
        auto const [message, remaining, discarded, catalogId]
          = remote_fmt::parseMessage(std::span{buffer}, emptyCatalog(), [&](std::string_view) {
                errorReported = true;
            });
        CHECK(!message, "bad format spec fails");
        CHECK(errorReported, "bad format spec reports an error");
    }
}

// A frame whose argument is "depth" one-element ranges (or tuples) wrapped around a u8. A level costs a byte of
// descriptor and a whole parse frame on the stack, and an inline descriptor is the stream's choice.
std::vector<std::byte> nestedFrame(char        kind,
                                   std::size_t depth) {
    std::string            descriptor;
    std::vector<std::byte> values;
    for(std::size_t level = 0; level < depth; ++level) {
        descriptor += kind;
        if(kind == 'L') { values.push_back(std::byte{1}); }
    }
    descriptor += "u1";
    if(kind == '(') { descriptor.append(depth, ')'); }
    values.push_back(std::byte{42});
    return inlineFrame(describedText("{}", descriptor), values);
}

// Asserts the rejection, not the crash: a test that overflowed the stack would take the binary
// down with it and report nothing.
void nestingDepthLimit() {
    constexpr auto Limit = remote_fmt::detail::Parser::maxNestingDepth;

    auto const parseNested = [](auto const& buffer, std::string& error) {
        auto const [message, remaining, discarded, catalogId]
          = remote_fmt::parseMessage(std::span{buffer}, emptyCatalog(), [&](std::string_view e) {
                error = e;
            });
        return message;
    };

    // Shallow nesting is ordinary data and has to keep working.
    {
        std::string error;
        auto const  message = parseNested(nestedFrame('L', 4), error);
        CHECK(message.has_value() && *message == "[[[[42]]]]", "4-deep list renders");
    }

    // The boundary itself, from both sides.
    {
        std::string error;
        auto const  message = parseNested(nestedFrame('L', Limit - 1), error);
        CHECK(message.has_value(), "nesting one below the limit still parses");
    }
    {
        std::string error;
        auto const  message = parseNested(nestedFrame('L', Limit), error);
        CHECK(!message, "nesting at the limit is refused");
        CHECK(!error.empty(), "nesting at the limit reports an error");
    }

    // Deep enough to overflow in every build if the guard were gone: the least sensitive of them,
    // the -O2 sanitizer build, dies between 6 000 and 8 000 levels.
    for(std::size_t depth : {std::size_t{8000}, std::size_t{20000}}) {
        {
            std::string error;
            auto const  message = parseNested(nestedFrame('L', depth), error);
            CHECK(!message, "deeply nested list refused");
        }
        {
            // a tuple recurses through value() just as a list does
            std::string error;
            auto const  message = parseNested(nestedFrame('(', depth), error);
            CHECK(!message, "deeply nested tuple refused");
        }
    }

    // The decoder keeps working afterwards - a refused frame must not poison the stream.
    CHECK_RT("Test 123", "Test {}"_sc, 123);
}

// Format strings the old target check accepted but the parser's own scan could not consume (two
// scanners then, one now: format_grammar.hpp). The scan asserted on exactly what the validator was supposed to guarantee, so these aborted an
// assert-enabled build and sailed through a release one. Rejection is the contract, in every build.
void malformedFormatString() {
    auto const refuses = [](std::string_view fmtString) {
        auto const buffer = rawFrame(fmtString, 42);
        auto const [message, remaining, discarded, catalogId]
          = remote_fmt::parseMessage(std::span{buffer}, emptyCatalog(), [](std::string_view) {});
        return !message;
    };

    // The two that actually aborted: the validator read "}}" as an escape pair even inside an open
    // field, called the string balanced, and left the scan staring at a lone '}'.
    CHECK(refuses("{:}}x}"), "brace escape inside an open field refused");

    // The same disagreement, in the shape that had been sitting in the committed fuzz corpus all
    // along - it only ever failed where asserts were live.
    CHECK(refuses("{}} {{}"), "corpus shape refused");

    // Neighbouring shapes, already refused before the fix. Here so the stricter escape rule cannot
    // regress them, not because they ever aborted.
    CHECK(refuses("{"), "trailing lone open brace refused");
    CHECK(refuses("}"), "trailing lone close brace refused");
    CHECK(refuses("x{"), "lone open brace after text refused");
    CHECK(refuses("{}{"), "lone open brace after a field refused");
    CHECK(refuses("{:>10"), "unclosed replacement field refused");

    // Escapes that really are escapes still round-trip, so the stricter rule costs nothing.
    CHECK_RT("{}", "{{}}"_sc);
    CHECK_RT("{7}", "{{{}}}"_sc, 7);
    CHECK_RT("7 }", "{} }}"_sc, 7);
    CHECK_RT("{ 7", "{{ {}"_sc, 7);

    CHECK_RT("Test 123", "Test {}"_sc, 123);
}

// The scanner itself (format_grammar.hpp), tested directly rather than through parseMessage(): nothing on
// the wire reaches its error branches past the pre-check, but the parser's walk does not trust that
// check, and this is the layer that decides whether a bad string is a rejected frame or an abort.
void replacementFieldScanRejections() {
    using remote_fmt::detail::nextField;
    using remote_fmt::detail::ScanError;

    // A trailing brace of either kind: neither an escape pair nor a field that can close.
    CHECK(nextField("{").malformed(), "trailing '{' reported malformed");
    CHECK(nextField("}").malformed(), "trailing '}' reported malformed");
    CHECK(nextField("abc{").malformed(), "trailing '{' after text reported malformed");

    // An unescaped '}' with no field open closes nothing.
    CHECK(nextField("}x").error == ScanError::loneClose, "unopened '}' reported malformed");

    // A field that never closes: the search for '}' runs off the end.
    CHECK(nextField("{:>10").error == ScanError::unclosed, "unclosed field reported malformed");

    // A nested brace is one field, reported as such.
    {
        auto const field = nextField("{:{}}x");
        CHECK(field.error == ScanError::nested && field.field == "{:{}}" && field.rest == "x",
              "dynamic width is one nested field");
    }

    // The well-formed cases still behave, and running out of fields is not "malformed".
    {
        auto const field = nextField("{}");
        CHECK(field.error == ScanError::none && field.field == "{}", "'{}' yields one field");
    }
    {
        auto const field = nextField("a {:>4} b");
        CHECK(field.error == ScanError::none && field.literal == "a " && field.field == "{:>4}"
                && field.rest == " b",
              "spec field returned");
    }
    {
        auto const none = nextField("plain text");
        CHECK(!none.malformed() && none.field.empty() && none.literal == "plain text",
              "no field is not malformed");
    }
    {
        auto const  none = nextField("{{}}");
        std::string text;
        remote_fmt::detail::appendLiteral(text, none.literal);
        CHECK(!none.malformed() && none.field.empty() && text == "{}",
              "escapes alone yield no field");
    }
}

}   // namespace

// A formatter built on format_to, the way a small wrapper is usually written (see
// Kvasir::Suppressed). It renders to a string on the host, so a string spec applies to it and a
// numeric one cannot -- the host_type below is what turns the latter into a compile error rather
// than a message that quietly never arrives.
namespace {
struct Suppressed {
    unsigned n;
};
}   // namespace

template<>
struct remote_fmt::formatter<Suppressed> {
    template<typename Printer>
    constexpr auto format(Suppressed const& s,
                          Printer&          printer) const {
        if(s.n == 0) { return format_to(printer, SC_LIFT("")); }
        return format_to(printer, SC_LIFT("(+{} not logged)"), s.n);
    }
};

#if REMOTE_FMT_USE_FMT_CHECK
template<>
struct remote_fmt::detail::HostType<Suppressed> {
    using type = std::string_view;
};
#endif

// The writer vocabulary (Printer: out, value, formatArguments, format_to): a formatter declares its descriptor and
// writes the values it names, or declares none and writes through format_to (`*`). No wire byte written by hand.
namespace {
struct LoopPair {
    int a;
    int b;
};

struct Label {
    std::string_view text;
};

struct Either {
    bool     big;
    unsigned n;
};
}   // namespace

// a declared sub format: only the two numbers travel
template<>
struct remote_fmt::formatter<LoopPair> {
    static consteval auto descriptor() {
        return remote_fmt::subFormatDescriptor<int, int>(SC_LIFT("<{} {}>"));
    }

    template<typename Printer>
    constexpr void format(LoopPair const& p,
                          Printer&        printer) const {
        printer.formatArguments(p.a, p.b);
    }
};

// a value of another type's shape: declared as that type's descriptor, written through value()
template<>
struct remote_fmt::formatter<Label> {
    static consteval auto descriptor() { return remote_fmt::descriptorOf<std::string_view>(); }

    template<typename Printer>
    constexpr void format(Label const& l,
                          Printer&     printer) const {
        printer.value(l.text);
    }
};

// a shape chosen by the value: `*`, each branch a format_to of its own
template<>
struct remote_fmt::formatter<Either> {
    template<typename Printer>
    constexpr auto format(Either const& e,
                          Printer&      printer) const {
        if(e.big) { return format_to(printer, SC_LIFT("big {} ({})"), e.n, std::string_view{"x"}); }
        return format_to(printer, SC_LIFT("small"));
    }
};

#if REMOTE_FMT_USE_FMT_CHECK
template<>
struct remote_fmt::detail::HostType<LoopPair> {
    using type = std::string_view;
};

template<>
struct remote_fmt::detail::HostType<Label> {
    using type = std::string_view;
};

template<>
struct remote_fmt::detail::HostType<Either> {
    using type = std::string_view;
};
#endif

namespace {
void writerVocabulary() {
    static_assert(remote_fmt::descriptorOf<LoopPair>() == "F7:<{} {}>2:i4i4"_sc);
    static_assert(remote_fmt::descriptorOf<Label>() == "s"_sc);
    static_assert(remote_fmt::descriptorOf<Either>() == "*"_sc);
    CHECK_RT("x <1 2>", "x {}"_sc, LoopPair{1, 2});
    CHECK_RT("[<1 -2>, <3 4>]",
             "{}"_sc,
             std::vector<LoopPair>{
               {1, -2},
               {3,  4}
    });
    CHECK_RT("  <1 2>", "{:>7}"_sc, LoopPair{1, 2});
    CHECK_RT("label: hi", "label: {}"_sc, Label{"hi"});
    CHECK_RT("[\"hi\"]", "{}"_sc, std::array<Label, 1>{Label{"hi"}});
    CHECK_RT("big 7 (x) / small", "{} / {}"_sc, Either{true, 7}, Either{false, 0});
    CHECK_RT("optional(small)",
             "{}"_sc,
             std::optional<Either>{
               Either{false, 0}
    });
}
}   // namespace

namespace {

// A sub format string is text by the time the outer field is seen, so a spec that needs a value
// cannot apply -- but everything fmt accepts on a string can, and used to be thrown away along
// with the whole message.
void subFmtStringSpecs() {
    CHECK_RT("x (+3 not logged)", "x {}"_sc, Suppressed{3});
    CHECK_RT("x ", "x {}"_sc, Suppressed{0});

    // String specs reach the rendered text.
    CHECK_RT("|(+3 not logged)     |", "|{:<20}|"_sc, Suppressed{3});
    CHECK_RT("|     (+3 not logged)|", "|{:>20}|"_sc, Suppressed{3});
    CHECK_RT("|...(+3 not logged)...|", "|{:.^21}|"_sc, Suppressed{3});
    CHECK_RT("(+3 n", "{:.5}"_sc, Suppressed{3});

    {
        // A numeric spec still cannot apply -- but it now says so instead of dropping the message
        // in silence. Raw frame, because host_type makes "{:.2f}" a compile error at the print
        // call; the host still has to cope with untrusted bytes.
        bool       errorReported = false;
        auto const buffer        = rawSubFmtStringFrame("{:.2f}", "(+3 not logged)");
        auto const [message, remaining, discarded, catalogId]
          = remote_fmt::parseMessage(std::span{buffer}, emptyCatalog(), [&](std::string_view) {
                errorReported = true;
            });
        CHECK(!message, "numeric spec on a sub format string fails");
        CHECK(errorReported, "numeric spec on a sub format string reports an error");
    }
}

// mp-units quantities. The point of the protocol's `quantity` marker rather than a sub format
// string: the caller's replacement field reaches the *number*, so a spec means what it always
// means and the unit symbol follows it. Fill, align and width are the exception - those position
// the number and the unit together.
void quantityRoundTrips() {
#ifdef REMOTE_FMT_TEST_MP_UNITS
    namespace mpu = mp_units;
    namespace si  = mp_units::si;

    // An integral quantity stays integral on the wire: nothing converts to float to be logged.
    CHECK_RT(
      "23500 mK",
      "{}"_sc,
      mpu::quantity<si::milli<si::kelvin>, std::int32_t>{mpu::delta<si::milli<si::kelvin>>(23500)});
    CHECK_RT("-40 K",
             "{}"_sc,
             mpu::quantity<si::kelvin, std::int32_t>{mpu::delta<si::kelvin>(-40)});

    // The spec applies to the number, which a sub format string could not do.
    CHECK_RT("1.50 V", "{:.2f}"_sc, mpu::quantity<si::volt, float>{1.5F * si::volt});
    CHECK_RT("0x2a lx", "{:#x}"_sc, mpu::quantity<si::lux, std::int32_t>{42 * si::lux});

    // ...but fill, align and width position the whole "1.5 V", the way they would position any
    // other argument. A bare width right-aligns, because that is what the representation does.
    CHECK_RT("     1.5 V", "{:10}"_sc, mpu::quantity<si::volt, float>{1.5F * si::volt});
    CHECK_RT("     1.5 V", "{:>10}"_sc, mpu::quantity<si::volt, float>{1.5F * si::volt});
    CHECK_RT("1.5 V     ", "{:<10}"_sc, mpu::quantity<si::volt, float>{1.5F * si::volt});
    CHECK_RT("***1.50 V***", "{:*^12.2f}"_sc, mpu::quantity<si::volt, float>{1.5F * si::volt});
    // Width below the rendered length changes nothing, so the unit is never truncated away.
    CHECK_RT("42 lx", "{:4}"_sc, mpu::quantity<si::lux, std::int32_t>{42 * si::lux});

    // The zero flag is left with the number: sign-aware zero padding is a numeric property, and
    // zero-padding a string that ends in a unit symbol would be nonsense.
    CHECK_RT("00001.50 V", "{:08.2f}"_sc, mpu::quantity<si::volt, float>{1.5F * si::volt});
    // '0' followed by an align is a fill, not the zero flag, so this one does pad the whole.
    CHECK_RT("000001.5 V", "{:0>10}"_sc, mpu::quantity<si::volt, float>{1.5F * si::volt});
    // A sign stays with the number, where it belongs.
    CHECK_RT("    +1.5 V", "{:>+10}"_sc, mpu::quantity<si::volt, float>{1.5F * si::volt});

    // A derived unit's symbol comes through whole.
    CHECK_RT("9 m/s²",
             "{}"_sc,
             mpu::quantity<si::metre / (si::second * si::second), std::int32_t>{
               9 * si::metre / (si::second * si::second)});

    // mp-units' space_before_unit_symbol decides the separator, and it is baked into the stored
    // symbol. A dimensionless quantity has an empty symbol, so this also covers sc::create and
    // the catalog on a zero-length string.
    CHECK_RT("5", "{}"_sc, mpu::quantity<mpu::one, std::int32_t>{5 * mpu::one});
    CHECK_RT("45°", "{}"_sc, mpu::quantity<si::degree, std::int32_t>{mpu::delta<si::degree>(45)});
    CHECK_RT("   45°",
             "{:6}"_sc,
             mpu::quantity<si::degree, std::int32_t>{mpu::delta<si::degree>(45)});

    // Celsius is an offset unit, so a value is a delta -- which is what a sensor reports.
    CHECK_RT("2350 c℃",
             "{}"_sc,
             mpu::quantity<si::centi<si::degree_Celsius>, std::int32_t>{
               mpu::delta<si::centi<si::degree_Celsius>>(2350)});

    // A quantity_point logs what quantity_from_zero() yields -- and the unit comes from that
    // quantity, not from the point's Reference, because quantity_from_zero restores the point's
    // own unit only when doing so is non-truncating.
    CHECK_RT("20 ℃", "{}"_sc, mpu::quantity_point{mpu::delta<si::degree_Celsius>(20)});
    CHECK_RT("293.15 K",
             "{:.2f}"_sc,
             mpu::quantity_point{mpu::delta<si::degree_Celsius>(20.0)}.in(si::kelvin));
    // Padding counts display columns, not bytes: "20 \u2103" is four columns wide even though
    // the symbol is three bytes.
    CHECK_RT("     20 ℃", "{:9}"_sc, mpu::quantity_point{mpu::delta<si::degree_Celsius>(20)});

    // Inside the wrappers and inside ranges, like any other argument -- so in_list and inMap
    // forwarding is covered too.
    CHECK_RT("optional(5 A)",
             "{}"_sc,
             std::optional{mpu::quantity<si::ampere, std::int32_t>{5 * si::ampere}});
    CHECK_RT("[1 A, 2 A]",
             "{}"_sc,
             std::vector{mpu::quantity<si::ampere, std::int32_t>{1 * si::ampere},
                         mpu::quantity<si::ampere, std::int32_t>{2 * si::ampere}});
    CHECK_RT("{\"a\": 1 A}",
             "{}"_sc,
             std::map<std::string_view, mpu::quantity<si::ampere, std::int32_t>>{
               {"a", mpu::quantity<si::ampere, std::int32_t>{1 * si::ampere}}
    });
#endif
}

// The frame check (REMOTE_FMT_FRAME_CHECK, on by default): a checked frame starts with 0x57 and carries a CRC-8 before
// its end marker; one flipped bit drops the frame with a message instead of decoding other text; a reader told the
// image's kind of frame (the catalog's "FrameCheck") refuses the other kind loudly.
void frameCheck() {
    static constexpr auto checkInput = [] {
        std::array<std::byte, 9> bytes{};
        for(std::size_t i = 0; i != bytes.size(); ++i) {
            bytes[i] = static_cast<std::byte>('1' + i);
        }
        return bytes;
    }();
    static_assert(
      remote_fmt::protocol::crcFinal(
        remote_fmt::protocol::crcOver(remote_fmt::protocol::crcInit, std::span{checkInput}))
        == 0xDF,
      "CRC-8/AUTOSAR check value");
    // the table is the polynomial's, bit by bit
    static_assert([] {
        for(unsigned n = 0; n != 256; ++n) {
            unsigned c = n;
            for(int k = 0; k != 8; ++k) {
                c = (c & 0x80U) != 0 ? ((c << 1U) ^ 0x2FU) & 0xFFU : (c << 1U) & 0xFFU;
            }
            if(remote_fmt::protocol::crcTable[n] != c) { return false; }
        }
        return true;
    }());
    crc_count::bytes = 0;
    auto const frame = serialize("frame {} {}"_sc, 42, "ok");
    // the encoder's CRC covers each byte before the CRC exactly once (once there were two passes over the staged bytes)
    CHECK(crc_count::bytes == (REMOTE_FMT_TEST_EXPECT_FRAME_CHECK ? frame.size() - 2 : 0),
          "every byte of a frame goes into its CRC once");
    crc_count::bytes     = 0;
    auto const longFrame = serialize("{} {} {}"_sc,
                                     std::string_view{"a string longer than the staging bytes"},
                                     7,
                                     std::string(100, 'x'));
    CHECK(crc_count::bytes == (REMOTE_FMT_TEST_EXPECT_FRAME_CHECK ? longFrame.size() - 2 : 0),
          "also when the record is written in pieces");
    std::size_t errors = 0;
    auto const  count  = [&errors](std::string_view) { ++errors; };
    using remote_fmt::FrameCheck;
#if REMOTE_FMT_TEST_EXPECT_FRAME_CHECK
    CHECK(frame.front() == remote_fmt::protocol::checkedStartMarker, "a checked frame by default");
    for(auto const expect : {FrameCheck::either, FrameCheck::required}) {
        errors       = 0;
        auto const r = remote_fmt::parseMessage(std::span{frame}, emptyCatalog(), count, expect);
        CHECK(r.message && *r.message == "frame 42 ok" && errors == 0, "a checked frame decodes");
    }
    errors = 0;
    auto const n
      = remote_fmt::parseMessage(std::span{frame}, emptyCatalog(), count, FrameCheck::none);
    CHECK(!n.message && errors != 0, "a checked frame where the catalog says none: refused, said");
    // every single bit flipped inside the frame (not the markers): dropped, said, never other text
    std::size_t decodedWrong = 0;
    std::size_t silent       = 0;
    for(std::size_t i = 1; i + 1 < frame.size(); ++i) {
        for(int bit = 0; bit != 8; ++bit) {
            auto damaged = frame;
            damaged[i] ^= static_cast<std::byte>(1U << bit);
            errors       = 0;
            auto const d = remote_fmt::parseMessage(std::span{damaged},
                                                    emptyCatalog(),
                                                    count,
                                                    FrameCheck::required);
            if(d.message) { ++decodedWrong; }
            // said: an error, or bytes counted as discarded (the reader reports those), or kept for more input
            if(!d.message && errors == 0 && d.discarded == 0 && d.remaining.empty()) { ++silent; }
        }
    }
    CHECK(decodedWrong == 0, "no damaged frame decodes");
    CHECK(silent == 0, "every damaged frame is said");
#else
    CHECK(frame.front() == remote_fmt::protocol::startMarker, "an unchecked frame");
    errors = 0;
    auto const r
      = remote_fmt::parseMessage(std::span{frame}, emptyCatalog(), count, FrameCheck::required);
    CHECK(!r.message && errors != 0,
          "an unchecked frame where the catalog says checked: refused, said");
#endif
}

// Each encoder knob changes the bytes or the writes, never the text (README "The wire"). The suite
// is built once per knob at its other value (tests/CMakeLists.txt); this checks that the build really has the knob it
// claims, so a misspelled define fails instead of testing the defaults twice. FRAME_CHECK: frameCheck() above.
struct CountingBackend {
    std::vector<std::byte> memory;
    std::size_t            writes{};

    void write(std::span<std::byte const> data) {
        ++writes;
        memory.insert(memory.end(), data.begin(), data.end());
    }
};

void knobEffects() {
    remote_fmt::Printer<CountingBackend> printer{};
    printer.print("{}"_sc, std::uint32_t{5});
    auto const& backend = printer.getComBackend();
    // start marker, the inline id (2), the length (1), "{}" 0x1e "u4", then the value: 5 as one LEB128 byte
    auto constexpr valueAt = std::size_t{4} + "{}\x1eu4"sv.size();
    CHECK(backend.memory.size()
            == valueAt + 1 + (REMOTE_FMT_TEST_EXPECT_FRAME_CHECK != 0 ? 1 : 0) + 1,
          "a whole frame");
    CHECK(backend.memory.size() > valueAt && backend.memory[valueAt] == std::byte{5},
          "5 goes out as one byte");
#if REMOTE_FMT_TEST_EXPECT_STAGING_BYTES == 0
    CHECK(backend.writes > 1, "STAGING_BYTES 0: every field is a write of its own");
#else
    CHECK(backend.writes == 1, "staged: a short record is one write");
    remote_fmt::Printer<CountingBackend> longPrinter{};
    longPrinter.print("{}"_sc,
                      std::string_view{"a string longer than the staging bytes of the Printer"});
    static_assert(REMOTE_FMT_STAGING_BYTES < 53, "the string above must not fit the staging bytes");
    CHECK(longPrinter.getComBackend().writes > 1, "staged: a longer record goes out in pieces");
#endif
}

// The bytes an integer takes (README "Descriptors"): 4 bytes as LEB128 (zigzag for signed), every other width raw,
// little endian, whatever the value.
void integerWidths() {
    auto const valueBytes = [](auto value) {
        remote_fmt::Printer<CountingBackend> printer{};
        printer.print("{}"_sc, value);
        auto const& m = printer.getComBackend().memory;
        auto const  valueAt
          = std::size_t{4}
          + std::string_view{decltype(remote_fmt::described<decltype(value)>("{}"_sc)){}}.size();
        auto const tail = std::size_t{1} + (REMOTE_FMT_TEST_EXPECT_FRAME_CHECK != 0 ? 1U : 0U);
        return std::vector<std::byte>(std::next(m.begin(), static_cast<std::ptrdiff_t>(valueAt)),
                                      std::prev(m.end(), static_cast<std::ptrdiff_t>(tail)));
    };
    auto const bytes = [](std::initializer_list<unsigned> list) {
        std::vector<std::byte> out;
        for(auto const b : list) { out.push_back(static_cast<std::byte>(b)); }
        return out;
    };
    CHECK(valueBytes(std::uint32_t{5}) == bytes({5}), "u4 5: one LEB128 byte");
    CHECK(valueBytes(std::uint32_t{300}) == bytes({0xAC, 0x02}), "u4 300: two LEB128 bytes");
    CHECK(valueBytes(std::int32_t{-3}) == bytes({5}), "i4 -3: zigzag 5");
    CHECK(valueBytes(std::uint16_t{5}) == bytes({5, 0}), "u2 5: two raw bytes");
    CHECK(valueBytes(std::int16_t{-2}) == bytes({0xFE, 0xFF}), "i2 -2: two raw bytes, no zigzag");
    CHECK(valueBytes(std::uint64_t{5}) == bytes({5, 0, 0, 0, 0, 0, 0, 0}), "u8 5: eight raw bytes");
    CHECK(valueBytes(std::int64_t{-2}) == bytes({0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}),
          "i8 -2: eight raw bytes, no zigzag");
    CHECK(valueBytes(std::int8_t{-2}) == bytes({0xFE}), "i1 -2: one raw byte");
}
}   // namespace

int main() {
    frameCheck();
    knobEffects();
    integerWidths();
    trivialRoundTrips();
    stringRoundTrips();
    rangeRoundTrips();
    tupleRoundTrips();
    extendedTypeRoundTrips();
    timeRoundTrips();
    subFmtStringSpecs();
    writerVocabulary();
    quantityRoundTrips();
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
    bitflagFormatting();
    enumeratorValues();
    multipleMessages();
    malformedInput();
    nestingDepthLimit();
    malformedFormatString();
    replacementFieldScanRejections();

    return remote_fmt::test::report();
}
