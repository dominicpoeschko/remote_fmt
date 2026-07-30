// One rejected format string per REMOTE_FMT_FAIL_CASE; each must fail to compile. CMakeLists.txt
// builds every case as its own target with WILL_FAIL - a rejection is a hard compile error that
// cannot be observed as a bool, so static_assert is not an option. Keep both lists in step.

#include "remote_fmt/remote_fmt.hpp"

#include <chrono>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

using namespace sc::literals;

namespace { enum class Color : std::uint8_t { Red, Blue }; }

void failCase();

void failCase() {
#if REMOTE_FMT_FAIL_CASE == 1
    remote_fmt::checkFormatString<int>("{:s}"_sc);

#elif REMOTE_FMT_FAIL_CASE == 2
    remote_fmt::checkFormatString<std::string_view>("{:d}"_sc);

#elif REMOTE_FMT_FAIL_CASE == 3
    remote_fmt::checkFormatString<int>("{:.3f}"_sc);

#elif REMOTE_FMT_FAIL_CASE == 4
    remote_fmt::checkFormatString<int>("{} {}"_sc);

#elif REMOTE_FMT_FAIL_CASE == 5
    // an argument id cannot be honoured: one argument per field, in order
    remote_fmt::checkFormatString<int, int>("{1} {0}"_sc);

#elif REMOTE_FMT_FAIL_CASE == 6
    // a dynamic width needs a second argument the protocol cannot send
    remote_fmt::checkFormatString<int, int>("{:{}}"_sc);

#elif REMOTE_FMT_FAIL_CASE == 7
    remote_fmt::checkFormatString<std::vector<int>>("{:s}"_sc);

#elif REMOTE_FMT_FAIL_CASE == 8
    // wrong for the enumerator-name half of the enum mapping
    remote_fmt::checkFormatString<Color>("{:d}"_sc);

#elif REMOTE_FMT_FAIL_CASE == 9
    remote_fmt::checkFormatString<int>("{"_sc);

#elif REMOTE_FMT_FAIL_CASE == 10
    remote_fmt::checkFormatString<int>("{:%Q}"_sc);

// Cases 11 to 13 need the full check; without it these arguments degrade to unchecked_arg and the
// strings are legitimately accepted. #error rather than silently testing nothing.
#elif REMOTE_FMT_FAIL_CASE == 11
    #if REMOTE_FMT_FMT_CHECK_FULL
    // a duration has no date
    remote_fmt::checkFormatString<std::chrono::milliseconds>("{:%Y}"_sc);
    #else
        #error "case 11 needs the full check (fmt/chrono.h); not applicable here"
    #endif

#elif REMOTE_FMT_FAIL_CASE == 12
    #if REMOTE_FMT_FMT_CHECK_FULL
    // the spec is forwarded to the payload, and 's' is wrong for an int
    remote_fmt::checkFormatString<std::optional<int>>("{:s}"_sc);
    #else
        #error "case 12 needs the full check (fmt/std.h); not applicable here"
    #endif

#elif REMOTE_FMT_FAIL_CASE == 13
    #if REMOTE_FMT_FMT_CHECK_FULL
    // fmt's variant formatter accepts no spec
    remote_fmt::checkFormatString<std::variant<int, std::string_view>>("{:>5}"_sc);
    #else
        #error "case 13 needs the full check (fmt/std.h); not applicable here"
    #endif

#else
    #error "REMOTE_FMT_FAIL_CASE must name a case defined in this file"
#endif
}
