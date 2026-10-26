#pragma once
// remote_fmt's own test scaffolding (tests/CMakeLists.txt puts tests/support on the include path of the tests, the
// fuzzers and the corpus generator): the backend that collects a Printer's bytes, an empty catalog for inline frames,
// what a build is meant to have of each encoder knob, and CHECK with its failure count. remote_fmt's tests build from a
// lone checkout, so this is not uc_log's tests/support.
#include "remote_fmt/config.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

// What this build is meant to have: tests/CMakeLists.txt sets REMOTE_FMT_TEST_EXPECT_<KNOB> next to each knob it
// changes, and the knob checks compare the wire with that, not with the knob macro - so a knob define that never
// reached remote_fmt (a typo) fails instead of testing the defaults twice. Unset, they follow the knobs.
#ifndef REMOTE_FMT_TEST_EXPECT_STAGING_BYTES
    #define REMOTE_FMT_TEST_EXPECT_STAGING_BYTES REMOTE_FMT_STAGING_BYTES
#endif
#ifndef REMOTE_FMT_TEST_EXPECT_FRAME_CHECK
    #define REMOTE_FMT_TEST_EXPECT_FRAME_CHECK REMOTE_FMT_FRAME_CHECK
#endif

namespace remote_fmt::test {
// Internal linkage on purpose: every test is one TU, and the fuzzers' coverage stays what it was with each harness's
// own copy (as inline functions of external linkage, fuzz_roundtrip_smoke counted cov 4261 ft 9616 instead of
// 4255 / 9470 on the same seed). That one TU is also why this is a .ipp and not a header.
namespace {

    // a Printer's backend: every write appended
    struct VectorBackend {
        std::vector<std::byte> memory;

        [[maybe_unused]] void write(std::span<std::byte const> data) {
            memory.insert(memory.end(), data.begin(), data.end());
        }
    };

    // the catalog of an image without one (inline frames). Leaked on purpose: no global constructor, no exit-time
    // destructor, and the static reference keeps the allocation reachable so LeakSanitizer stays quiet.
    [[maybe_unused]] std::unordered_map<std::uint16_t,
                                        std::string> const&
    emptyCatalog() {
        static auto const& catalog = *new std::unordered_map<std::uint16_t, std::string>{};
        return catalog;
    }

    [[maybe_unused]] int failures = 0;

    // a test program's end: the failure count, or `passed`; the exit code
    [[maybe_unused]] int report(char const* passed = "all tests passed") {
        if(failures != 0) {
            std::printf("%d test(s) failed\n", failures);
            return 1;
        }
        std::puts(passed);
        return 0;
    }

    // An enum value no enumerator names, for the tests of the number fallback: bit_cast, not a cast expression, so
    // clang-analyzer-optin.core.EnumCastOutOfRange has nothing to report on a value that is out of range on purpose.
    template<typename E>
        requires std::is_enum_v<E>
    constexpr E enumValue(std::underlying_type_t<E> value) {
        return std::bit_cast<E>(value);
    }
}   // namespace
}   // namespace remote_fmt::test

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++::remote_fmt::test::failures;                     \
        }                                                       \
    } while(0)
