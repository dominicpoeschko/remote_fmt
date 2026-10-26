// One TU per switch of src/remote_fmt/config.hpp and value (tests/CMakeLists.txt remote_fmt_config_switches): the
// formatter set that switch adds exists exactly when it is 1, so neither #if path can rot. EXPECT_* is set by CMake
// beside the switch: a switch define that does not reach config.hpp fails here.
//
// The decoder does not depend on the encoder's switches: an image built without a formatter set still talks to the
// same parser, which reads every kind. So each build also decodes hand-built inline frames of the kinds the switches
// add on the encoder side (`Y` styled, `E` enum, `Q` quantity).
#include "TestSupport.ipp"
#include "remote_fmt/parser.hpp"
#include "remote_fmt/remote_fmt.hpp"
#include "remote_fmt/wire.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

static_assert(REMOTE_FMT_USE_ENCHANTUM == REMOTE_FMT_TEST_EXPECT_ENCHANTUM);
static_assert(REMOTE_FMT_USE_STYLED == REMOTE_FMT_TEST_EXPECT_STYLED);
static_assert(REMOTE_FMT_USE_MP_UNITS == REMOTE_FMT_TEST_EXPECT_MP_UNITS);

#if REMOTE_FMT_USE_STYLED
using Styled = decltype(fmt::styled(1, fmt::emphasis::bold));
static_assert(
  requires { sizeof(remote_fmt::formatter<Styled>); },
  "the styled formatter exists with STYLED=1");
#endif

#if REMOTE_FMT_USE_ENCHANTUM
// an enum with enumerators: enchantum::count of one without any (std::byte) is not a constant expression on gcc
// (std::array<T, 0>::operator[] in enchantum's entries.hpp)
enum class SwitchProbe { first, second };
static_assert(enchantum::count<SwitchProbe> == 2);
#endif

#if REMOTE_FMT_USE_MP_UNITS
static_assert(requires { typename mp_units::quantity<mp_units::one>; });
#endif

namespace {
using remote_fmt::test::failures;

// A checked inline frame: start, inlineId, the described string (`fmt` 0x1e descriptor) with its LEB128 length,
// the value bytes, the CRC-8, the end marker.
std::vector<std::byte> inlineFrame(std::string_view                    described,
                                   std::initializer_list<std::uint8_t> values) {
    namespace protocol = remote_fmt::protocol;
    std::vector<std::byte> frame{protocol::checkedStartMarker, std::byte{0xFF}, std::byte{0xFF}};
    for(auto length = described.size();; length >>= 7U) {
        auto const low = static_cast<std::uint8_t>(length & 0x7FU);
        if(length < 0x80U) {
            frame.push_back(std::byte{low});
            break;
        }
        frame.push_back(std::byte{static_cast<std::uint8_t>(low | 0x80U)});
    }
    for(char const c : described) { frame.push_back(static_cast<std::byte>(c)); }
    for(auto const v : values) { frame.push_back(std::byte{v}); }
    frame.push_back(std::byte{protocol::crcFinal(protocol::crcOver(protocol::crcInit, frame))});
    frame.push_back(protocol::endMarker);
    return frame;
}

void decodes(std::string_view                    what,
             std::string_view                    described,
             std::initializer_list<std::uint8_t> values,
             std::string const&                  expected) {
    auto const                                           frame = inlineFrame(described, values);
    std::unordered_map<std::uint16_t, std::string> const noCatalog{};
    std::string                                          errors;
    auto const parsed = remote_fmt::parseMessage(std::span<std::byte const>{frame},
                                                 noCatalog,
                                                 [&](std::string_view error) {
                                                     errors += error;
                                                     errors += "; ";
                                                 });
    if(!parsed.message || *parsed.message != expected || !parsed.remaining.empty()) {
        ++failures;
        std::printf("FAIL %.*s: got %s, want %s (errors: %s)\n",
                    static_cast<int>(what.size()),
                    what.data(),
                    parsed.message ? parsed.message->c_str() : "no message",
                    expected.c_str(),
                    errors.c_str());
    }
}
}   // namespace

int main() {
    // `Y`: the flag byte (0: no style), then the value; with an RGB foreground (flag 1) its 4 bytes first
    decodes("styled without a style",
            "s {}\x1eYu1",
            {0x00, 42},
            fmt::format("s {}", fmt::styled("42", fmt::text_style{})));
    decodes("styled with a foreground",
            "s {}\x1eYu1",
            {0x01, 0x00, 0x00, 0xFF, 0x00, 42},
            fmt::format("s {}", fmt::styled("42", fmt::fg(fmt::color::red))));
    // `E`: the raw value and the names inline (an image without the catalog)
    decodes("enum by name",
            "e {}\x1e"
            "Eu12:0,3:off1,2:on",
            {0x01},
            "e on");
    decodes("enum without a name",
            "e {}\x1e"
            "Eu12:0,3:off1,2:on",
            {0x07},
            "e 7");
    // `Q`: the number, then the symbol, padded as one
    decodes("quantity", "q {}\x1eQ2: Vu2", {0xDC, 0x05}, "q 1500 V");
    decodes("quantity padded", "q {:>8}\x1eQ2: Vu2", {0xDC, 0x05}, "q   1500 V");

    return remote_fmt::test::report("config switches as expected, the decoder reads Y, E and Q");
}
