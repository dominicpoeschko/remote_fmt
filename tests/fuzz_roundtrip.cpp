// libFuzzer entry point for the *encode* half of the protocol. Drives
// remote_fmt::Printer with fuzzer-chosen argument values, parses the produced bytes back
// with remote_fmt::parse, and requires the result to match fmt's own rendering of the
// same value.
//
// fuzz_parser feeds arbitrary bytes to the decoder, which is the right shape for a
// wire-facing parser but cannot reach the serializer at all: every byte the serializer
// would emit is a byte fuzz_parser has to guess. This harness starts from the other end,
// so Printer, the formatter specializations and the size/type-identifier helpers in
// type_identifier.hpp all get driven directly.
//
// It is also a stronger oracle than "did not crash": for every type fmt can format
// itself, a disagreement between fmt and the round trip is a real encode-or-decode bug,
// not just a robustness issue.
//
// Built with REMOTE_FMT_USE_CATALOG=false so string constants are emitted inline and the
// binary needs no linker-generated catalog object. The cataloged paths are covered by
// fuzz_parser (whose catalog map is populated) and by test_catalog.
#include "remote_fmt/parser.hpp"
#include "remote_fmt/remote_fmt.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <map>
#include <optional>
#include <ratio>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

using namespace sc::literals;
using namespace std::literals;

namespace {

struct VectorBackend {
    std::vector<std::byte> memory;

    void write(std::span<std::byte const> data) {
        memory.insert(memory.end(), data.begin(), data.end());
    }
};

// Leaked on purpose: no exit-time destructor, and the static reference keeps the
// allocation reachable so LeakSanitizer stays quiet.
std::unordered_map<std::uint16_t,
                   std::string> const&
emptyCatalog() {
    static auto const& catalog = *new std::unordered_map<std::uint16_t, std::string>{};
    return catalog;
}

// Pulls typed values out of the fuzzer's byte string. Reading past the end yields zeroes
// instead of stopping, so even a one-byte input still builds and checks a full message -
// that keeps the mapping from input to behaviour total, which the mutator relies on.
class Reader {
public:
    explicit Reader(std::span<std::byte const> data) : data_{data} {}

    bool exhausted() const { return data_.empty(); }

    std::uint8_t byte() {
        if(data_.empty()) { return 0; }
        auto const value = static_cast<std::uint8_t>(data_.front());
        data_            = data_.subspan(1);
        return value;
    }

    template<typename T>
    T take() {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
        std::array<std::byte, sizeof(T)> bytes{};
        auto const                       available = std::min(sizeof(T), data_.size());
        std::memcpy(bytes.data(), data_.data(), available);
        data_ = data_.subspan(available);
        T value;
        std::memcpy(&value, bytes.data(), sizeof(T));
        return value;
    }

    // Element counts straddle 255, the point where the protocol widens its length field
    // from one byte to two (sizeToRangeSize), so both encodings get exercised. The cap
    // keeps a single input cheap enough to stay in the thousands-of-execs-per-second
    // range.
    std::size_t count() { return take<std::uint16_t>() % 600U; }

    std::string string() {
        auto const  size = count();
        std::string value;
        value.reserve(size);
        for(std::size_t i{}; i < size; ++i) { value.push_back(static_cast<char>(byte())); }
        return value;
    }

    std::vector<char> chars() {
        auto const        size = count();
        std::vector<char> value;
        value.reserve(size);
        for(std::size_t i{}; i < size; ++i) { value.push_back(static_cast<char>(byte())); }
        return value;
    }

private:
    std::span<std::byte const> data_;
};

// Both sides routinely contain NUL and escape bytes, which a plain %s report would
// truncate at the first NUL - escape them so the artifact is actually diagnosable.
std::string escaped(std::string_view text) {
    std::string out;
    for(char const character : text) {
        auto const value = static_cast<unsigned char>(character);
        if(value >= 0x20U && value < 0x7FU && character != '"' && character != '\\') {
            out.push_back(character);
        } else {
            out += fmt::format("\\x{:02x}", value);
        }
    }
    return out;
}

[[noreturn]] void fail(char const*      what,
                       std::string_view expected,
                       std::string_view actual) {
    std::fprintf(stderr,
                 "ROUNDTRIP FAILURE: %s\n  expected: \"%s\"\n  actual:   \"%s\"\n",
                 what,
                 escaped(expected).c_str(),
                 escaped(actual).c_str());
    // abort() rather than returning: libFuzzer only writes a reproducer artifact when the
    // process dies.
    std::abort();
}

// Args are forwarded rather than taken by const reference: fmt::styled produces a view
// type that fmt refuses to accept as an lvalue, so it has to stay an rvalue all the way
// into print().
template<typename... Args>
std::optional<std::string> roundTrip(auto fmtString,
                                     Args&&... args) {
    remote_fmt::Printer<VectorBackend> printer{};
    printer.print(fmtString, std::forward<Args>(args)...);
    auto const& memory = printer.get_com_backend().memory;
    auto const [message, remaining, discarded]
      = remote_fmt::parse(std::span<std::byte const>{memory}, emptyCatalog(), [](std::string_view) {
        });
    // A message the Printer just produced must be consumed whole: anything left over or
    // skipped means the framing disagrees with itself.
    if(!remaining.empty() || discarded != 0) { return std::nullopt; }
    return message;
}

// No independent oracle: the protocol renders these types its own way, deliberately
// unlike fmt (fmt/std.h prints std::optional as "optional(42)"/"none" where the protocol
// prints "42"/"()"), so comparing against fmt would report a difference on every input.
// Driving them is still worth it - it covers the extended-type encoders and catches
// crashes, truncation and framing desync.
template<typename... Args>
void checkParses(auto fmtString,
                 Args&&... args) {
    auto const actual = roundTrip(fmtString, std::forward<Args>(args)...);
    if(!actual) { fail("parse failed", "<any message>", "<no message>"); }
}

void compare(std::string_view                  expected,
             std::optional<std::string> const& actual) {
    if(!actual) { fail("parse failed", expected, "<no message>"); }
    if(*actual != expected) { fail("mismatch", expected, *actual); }
}

// fmt formats these types itself, so it is an oracle independent of the code under test:
// any difference is a genuine bug in the serializer or the parser, not just a robustness
// issue.
//
// The arguments are forwarded into a lambda so they are evaluated exactly once - they are
// Reader calls, and evaluating them twice would hand fmt and the Printer different values
// and report a mismatch on every input. The format string stays a literal in both
// positions, which keeps fmt's consteval format-string checking.
#define CHECK_FMT(fmtLiteral, ...)                                                                \
    [&](auto const&... checkArgs) {                                                               \
        compare(fmt::format(fmtLiteral, checkArgs...), roundTrip(fmtLiteral##_sc, checkArgs...)); \
    }(__VA_ARGS__)

enum struct Color : std::uint8_t { red, green, blue };

template<typename T>
std::vector<T> takeVector(Reader& reader) {
    auto const     size = reader.count();
    std::vector<T> value;
    value.reserve(size);
    for(std::size_t i{}; i < size; ++i) { value.push_back(reader.take<T>()); }
    return value;
}

// One message per call: a selector byte picks the argument shape, the remaining bytes
// fill in the values. Keeping the selector first means the mutator's byte flips move
// between shapes cheaply while splices keep a shape and rewrite its payload.
void oneMessage(Reader& reader) {
    switch(reader.byte() % 55U) {
        // --- trivial types, every width the protocol encodes -----------------------
    case 0:  CHECK_FMT("{}", reader.take<std::uint8_t>()); break;
    case 1:  CHECK_FMT("{}", reader.take<std::uint16_t>()); break;
    case 2:  CHECK_FMT("{}", reader.take<std::uint32_t>()); break;
    case 3:  CHECK_FMT("{}", reader.take<std::uint64_t>()); break;
    case 4:  CHECK_FMT("{}", reader.take<std::int8_t>()); break;
    case 5:  CHECK_FMT("{}", reader.take<std::int16_t>()); break;
    case 6:  CHECK_FMT("{}", reader.take<std::int32_t>()); break;
    case 7:  CHECK_FMT("{}", reader.take<std::int64_t>()); break;
    case 8:  CHECK_FMT("{}", static_cast<bool>(reader.byte() & 1U)); break;
    case 9:  CHECK_FMT("{}", static_cast<char>(reader.byte())); break;
    case 10: CHECK_FMT("{}", reader.take<float>()); break;
    case 11: CHECK_FMT("{}", reader.take<double>()); break;
    case 12: CHECK_FMT("{}", std::bit_cast<void const*>(reader.take<std::uintptr_t>())); break;
    // fmt has no formatter for std::nullptr_t, so this one only asserts it round-trips.
    case 13:
        checkParses("{}"_sc, nullptr);
        break;

        // --- strings --------------------------------------------------------------
    case 14:
        {
            auto const value = reader.string();
            CHECK_FMT("{}", value);
        }
        break;
    case 15:
        {
            auto const value = reader.string();
            CHECK_FMT("{}", std::string_view{value});
        }
        break;

        // --- ranges ---------------------------------------------------------------
    case 16: CHECK_FMT("{}", takeVector<int>(reader)); break;
    case 17: CHECK_FMT("{}", takeVector<double>(reader)); break;
    case 18:
        {
            auto const               size = reader.count();
            std::vector<std::string> value;
            value.reserve(size);
            for(std::size_t i{}; i < size; ++i) { value.push_back(reader.string()); }
            CHECK_FMT("{}", value);
        }
        break;
    case 19:
        {
            std::array<int, 4> const value{reader.take<int>(),
                                           reader.take<int>(),
                                           reader.take<int>(),
                                           reader.take<int>()};
            CHECK_FMT("{}", value);
        }
        break;
    case 20:
        {
            auto const    size = reader.count();
            std::set<int> value;
            for(std::size_t i{}; i < size; ++i) { value.insert(reader.take<int>()); }
            CHECK_FMT("{}", value);
        }
        break;
    case 21:
        {
            auto const         size = reader.count();
            std::map<int, int> value;
            for(std::size_t i{}; i < size; ++i) {
                value.emplace(reader.take<int>(), reader.take<int>());
            }
            CHECK_FMT("{}", value);
        }
        break;
    case 22:
        {
            auto const                 size = reader.count();
            std::map<std::string, int> value;
            for(std::size_t i{}; i < size; ++i) {
                value.emplace(reader.string(), reader.take<int>());
            }
            CHECK_FMT("{}", value);
        }
        break;
    case 23:
        {
            auto const                    size = reader.count() % 20U;
            std::vector<std::vector<int>> value;
            value.reserve(size);
            for(std::size_t i{}; i < size; ++i) { value.push_back(takeVector<int>(reader)); }
            CHECK_FMT("{}", value);
        }
        break;

        // --- tuples ---------------------------------------------------------------
    case 24: CHECK_FMT("{}", std::pair{reader.take<int>(), reader.take<int>()}); break;
    case 25:
        {
            auto const first  = reader.take<int>();
            auto const second = static_cast<char>(reader.byte());
            auto const third  = reader.string();
            CHECK_FMT("{}", std::tuple{first, second, std::string_view{third}});
        }
        break;

        // --- durations, every representation and both numerator/denominator widths --
    case 26: CHECK_FMT("{}", std::chrono::nanoseconds{reader.take<std::int64_t>()}); break;
    case 27: CHECK_FMT("{}", std::chrono::milliseconds{reader.take<std::int32_t>()}); break;
    case 28: CHECK_FMT("{}", std::chrono::seconds{reader.take<std::int64_t>()}); break;
    case 29: CHECK_FMT("{}", std::chrono::hours{reader.take<std::int32_t>()}); break;
    case 30: CHECK_FMT("{}", std::chrono::duration<double>{reader.take<double>()}); break;
    case 31: CHECK_FMT("{}", std::chrono::duration<float>{reader.take<float>()}); break;
    case 32:
        CHECK_FMT(
          "{}",
          std::chrono::duration<std::int64_t, std::ratio<3, 7>>{reader.take<std::int64_t>()});
        break;
    case 33:
        // A denominator that needs the wide numerator/denominator encoding.
        CHECK_FMT("{}",
                  std::chrono::duration<std::int64_t, std::ratio<1, 1000000000000>>{
                    reader.take<std::int64_t>()});
        break;

        // --- styled: fmt formats fmt::styled itself, so this stays an exact check.
        //     Not routed through CHECK_FMT: fmt::styled yields a view type that fmt
        //     refuses to accept as an lvalue, so it has to be built fresh as a temporary
        //     at each of the two call sites rather than bound to a name.
    case 34:
        {
            auto const style
              = fmt::fg(static_cast<fmt::color>(reader.take<std::uint32_t>() & 0xFFFFFFU))
              | static_cast<fmt::emphasis>(reader.byte());
            auto const value = reader.take<int>();
            compare(fmt::format("{}", fmt::styled(value, style)),
                    roundTrip("{}"_sc, fmt::styled(value, style)));
        }
        break;
    case 35:
        {
            // fmt::terminal_color is not a bitmask - only 30-37 and 90-97 are enumerators.
            // Casting an arbitrary byte would make the Printer emit an escape sequence no
            // caller could ever ask for, and the mismatch would say nothing about the
            // protocol.
            static constexpr std::array<fmt::terminal_color, 16> terminalColors{
              fmt::terminal_color::black,
              fmt::terminal_color::red,
              fmt::terminal_color::green,
              fmt::terminal_color::yellow,
              fmt::terminal_color::blue,
              fmt::terminal_color::magenta,
              fmt::terminal_color::cyan,
              fmt::terminal_color::white,
              fmt::terminal_color::bright_black,
              fmt::terminal_color::bright_red,
              fmt::terminal_color::bright_green,
              fmt::terminal_color::bright_yellow,
              fmt::terminal_color::bright_blue,
              fmt::terminal_color::bright_magenta,
              fmt::terminal_color::bright_cyan,
              fmt::terminal_color::bright_white};
            auto const style = fmt::fg(terminalColors.at(reader.byte() % terminalColors.size()));
            auto const value = reader.string();
            compare(fmt::format("{}", fmt::styled(std::string_view{value}, style)),
                    roundTrip("{}"_sc, fmt::styled(std::string_view{value}, style)));
        }
        break;

        // --- format specs: exercise fmt's own formatting machinery through the parser,
        //     which re-parses the spec at parse time from bytes on the wire ----------
    case 36: CHECK_FMT("{:>10}", reader.take<int>()); break;
    case 37: CHECK_FMT("{:04x}", reader.take<std::uint32_t>()); break;
    case 38:
        {
            auto const value = reader.string();
            CHECK_FMT("{:*^7}", value);
        }
        break;
    case 39: CHECK_FMT("{:.3f}", reader.take<double>()); break;
    case 40: CHECK_FMT("{:#x}", reader.take<std::uint64_t>()); break;
    case 41: CHECK_FMT("{:+d}", reader.take<std::int64_t>()); break;
    case 42: CHECK_FMT("{:e}", reader.take<double>()); break;
    case 43:
        // %Q prints the tick count only - safe for every value, unlike the calendar
        // specs, which fmt implements with a float-to-int cast that is UB for huge
        // durations (see corpus_regressions/fmt-chrono-float-to-int-ub).
        CHECK_FMT("{:%Q}", std::chrono::milliseconds{reader.take<std::int64_t>()});
        break;
    case 44:
        CHECK_FMT("a {} b {} c {}",
                  reader.take<std::int32_t>(),
                  reader.take<double>(),
                  static_cast<bool>(reader.byte() & 1U));
        break;

        // --- wrapper types: these now track fmt, so they get the exact oracle. The two nested
        //     combinations fmt renders inconsistently (expected/variant holding an optional that
        //     holds a string) are excluded - see optionalInsideWrapperQuirk() in roundtrip_tests.cpp.
    case 45:
        {
            auto const value = reader.take<std::int32_t>();
            switch(reader.byte() % 8U) {
            case 0:
                if((reader.byte() & 1U) != 0) {
                    CHECK_FMT("{}", std::optional<std::int32_t>{value});
                } else {
                    CHECK_FMT("{}", std::optional<std::int32_t>{});
                }
                break;
            case 1:
                {
                    auto const text = reader.string();
                    CHECK_FMT("{}", (std::optional<std::string>{text}));
                }
                break;
            case 2: CHECK_FMT("{}", (std::optional<char>{static_cast<char>(reader.byte())})); break;
            case 3:
                CHECK_FMT("{}", (std::optional<std::vector<int>>{takeVector<int>(reader)}));
                break;
            case 4:
                if((reader.byte() & 1U) != 0) {
                    CHECK_FMT("{}", (std::expected<std::int32_t, std::int32_t>{value}));
                } else {
                    CHECK_FMT("{}",
                              (std::expected<std::int32_t, std::int32_t>{std::unexpected{value}}));
                }
                break;
            case 5:
                {
                    auto const text = reader.string();
                    if((reader.byte() & 1U) != 0) {
                        CHECK_FMT("{}", (std::expected<std::string, int>{text}));
                    } else {
                        CHECK_FMT("{}", (std::expected<int, std::string>{std::unexpected{text}}));
                    }
                }
                break;
            case 6: CHECK_FMT("{}", (std::expected<void, std::int32_t>{})); break;
            default:
                if((reader.byte() & 1U) != 0) {
                    CHECK_FMT("{}", (std::variant<std::int32_t, std::string>{value}));
                } else {
                    auto const text = reader.string();
                    CHECK_FMT("{}", (std::variant<std::int32_t, std::string>{text}));
                }
                break;
            }
        }
        break;

        // --- types the protocol renders its own way, with no fmt formatter to compare against:
        //     std::byte, enums (fmt rejects those outright) and nullptr -----------------
    case 53:
        {
            switch(reader.byte() % 3U) {
            case 0: checkParses("{}"_sc, std::byte{reader.byte()}); break;
            case 1: checkParses("{}"_sc, static_cast<Color>(reader.byte())); break;
            default:
                checkParses("{}"_sc, std::vector<Color>{static_cast<Color>(reader.byte())});
                break;
            }
        }
        break;

        // Chars nested in a range, and chars as map keys. Both go through fmt's debug format,
        // so both stay under the exact oracle - which is what polices the escaping.
    case 46: CHECK_FMT("{}", reader.chars()); break;
    case 47:
        {
            auto const          size = reader.count();
            std::map<char, int> value;
            for(std::size_t i{}; i < size; ++i) {
                value.emplace(static_cast<char>(reader.byte()), reader.take<int>());
            }
            CHECK_FMT("{}", value);
        }
        break;

        // --- nested element specs: everything after the second colon describes the element,
        //     and a range of ranges nests a third time. The parser has to split the field the
        //     same way fmt's range_formatter does, which is the part most likely to drift.
        //     A map element is a pair whose formatter takes no spec, so "{::n}" and "{::>12}"
        //     on a map do not compile - only the map itself takes "{:n}".
    case 48: CHECK_FMT("{::>8}", takeVector<int>(reader)); break;
    case 49: CHECK_FMT("{::#x}", takeVector<int>(reader)); break;
    case 50:
        {
            auto const               size = reader.count();
            std::vector<std::string> value;
            value.reserve(size);
            for(std::size_t i{}; i < size; ++i) { value.push_back(reader.string()); }
            // "{::?}" asks for the debug format explicitly; "{::}" turns it off. Both must track
            // fmt, and they are the two sides of the rule the escaping fix implements.
            if((reader.byte() & 1U) != 0) {
                CHECK_FMT("{::?}", value);
            } else {
                CHECK_FMT("{::}", value);
            }
        }
        break;
    case 51:
        {
            auto const                    size = reader.count() % 20U;
            std::vector<std::vector<int>> value;
            value.reserve(size);
            for(std::size_t i{}; i < size; ++i) { value.push_back(takeVector<int>(reader)); }
            CHECK_FMT("{:::>4}", value);
        }
        break;
    case 52:
        {
            auto const                 size = reader.count();
            std::map<std::string, int> value;
            for(std::size_t i{}; i < size; ++i) {
                value.emplace(reader.string(), reader.take<int>());
            }
            if((reader.byte() & 1U) != 0) {
                CHECK_FMT("{::}", value);
            } else {
                CHECK_FMT("{:n}", value);
            }
        }
        break;

        // --- no arguments at all: spelled out rather than via CHECK_FMT, which needs at
        //     least one argument for its pack ---------------------------------------
    default:
        compare(fmt::format("plain, no arguments"), roundTrip("plain, no arguments"_sc));
        break;
    }
}

}   // namespace

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data,
                                      std::size_t         size);

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data,
                                      std::size_t         size) {
#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-container"
#endif
    // libFuzzer hands over a raw pointer and size - the two-parameter span construction
    // is the only way to adopt it.
    std::span<std::byte const> const buffer{reinterpret_cast<std::byte const*>(data), size};
#ifdef __clang__
    #pragma clang diagnostic pop
#endif

    Reader reader{buffer};
    // Several messages per input: the shapes are cheap, and letting one input chain a few
    // of them lets the mutator build inputs that hit combinations of encoders.
    for(int i{}; i < 8; ++i) {
        oneMessage(reader);
        if(reader.exhausted()) { break; }
    }
    return 0;
}
