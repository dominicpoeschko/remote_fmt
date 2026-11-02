// The host_type mapping, plus format strings that must compile.
// Rejections cannot be tested here (a hard compile error, not observable as a bool) - see
// fmt_check_fail.cpp.

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
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>

using namespace sc::literals;
using namespace std::literals;

namespace {

enum class Color : std::uint8_t { red, blue };
enum class Tiny : signed char { a, b };

#if REMOTE_FMT_USE_FMT_CHECK

namespace Detail = remote_fmt::Detail;

template<typename Device, typename Host>
constexpr bool mapsTo = std::is_same_v<Detail::host_type_t<Device>, Host>;

// Scalars: the wire preserves width and signedness.
static_assert(mapsTo<std::int8_t,
                     std::int8_t>);
static_assert(mapsTo<std::uint64_t,
                     std::uint64_t>);
static_assert(mapsTo<float,
                     float>);
static_assert(mapsTo<double,
                     double>);
static_assert(mapsTo<bool,
                     bool>);
static_assert(mapsTo<char,
                     char>);

// Types that change across the wire.
static_assert(mapsTo<void*,
                     void const*>);
static_assert(mapsTo<void const*,
                     void const*>);
// fmt has its own std::byte formatter, but the wire carries a 1-byte unsigned.
static_assert(mapsTo<std::byte,
                     std::uint8_t>);
static_assert(mapsTo<std::string,
                     std::string_view>);
static_assert(mapsTo<std::string_view,
                     std::string_view>);
static_assert(mapsTo<char[6],
                     std::string_view>);
static_assert(mapsTo<decltype("abc"_sc),
                     std::string_view>);
static_assert(mapsTo<Color,
                     std::string_view>);
static_assert(!fmt::is_formattable<Color>::value,
              "fmt still cannot format an enum directly");
// Underlying integer when enchantum does not know the value; char-like types are widened.
static_assert(std::is_same_v<Detail::host_type_alt_t<Color>,
                             std::uint8_t>);
static_assert(std::is_same_v<Detail::host_type_alt_t<Tiny>,
                             std::int8_t>);

// Containers keep their category; elements are mapped too.
static_assert(mapsTo<std::vector<int>,
                     std::vector<int>>);
static_assert(mapsTo<std::array<std::byte,
                                4>,
                     std::vector<std::uint8_t>>);
static_assert(mapsTo<std::vector<Color>,
                     std::vector<std::string_view>>);
static_assert(mapsTo<std::set<Color>,
                     std::set<std::string_view>>);
static_assert(mapsTo<std::map<Color,
                              std::byte>,
                     std::map<std::string_view,
                              std::uint8_t>>);

// A capacity-parameterised container - Kvasir::StaticMap and StaticSet are these - has to map by
// category: its own template cannot be rebuilt around mapped types because of the non-type
// parameter.
template<typename K, typename V, std::size_t Capacity>
struct FixedMap {
    using key_type    = K;
    using mapped_type = V;
    using value_type  = std::pair<K, V>;
    std::array<value_type, Capacity> data_{};

    [[nodiscard]] constexpr auto begin() const REMOTE_FMT_LIFETIMEBOUND { return data_.begin(); }

    [[nodiscard]] constexpr auto end() const REMOTE_FMT_LIFETIMEBOUND { return data_.end(); }
};

template<typename K, std::size_t Capacity>
struct FixedSet {
    using key_type   = K;
    using value_type = K;
    std::array<K, Capacity> data_{};

    [[nodiscard]] constexpr auto begin() const REMOTE_FMT_LIFETIMEBOUND { return data_.begin(); }

    [[nodiscard]] constexpr auto end() const REMOTE_FMT_LIFETIMEBOUND { return data_.end(); }
};

static_assert(mapsTo<FixedMap<Color,
                              std::byte,
                              4>,
                     std::map<std::string_view,
                              std::uint8_t>>);
static_assert(mapsTo<FixedSet<Color,
                              4>,
                     std::set<std::string_view>>);

// The mapping must never demand more of a type than remote_fmt's own formatter does, or a type the
// device can serialize becomes a build error. traits.hpp's is_map asks only for mapped_type and
// then serializes elements through formatter<range_value_t>, so neither of these may hard-error.
template<typename V>
struct MappedTypeOnly {   // is_map, but no key_type
    using mapped_type = V;
    using value_type  = std::pair<int, V>;
    std::array<value_type, 2> data_{};

    [[nodiscard]] constexpr auto begin() const REMOTE_FMT_LIFETIMEBOUND { return data_.begin(); }

    [[nodiscard]] constexpr auto end() const REMOTE_FMT_LIFETIMEBOUND { return data_.end(); }
};

struct MapWithoutPairElement {   // is_map, but the element is not a pair
    using mapped_type = int;
    std::array<int, 2> data_{};

    [[nodiscard]] constexpr auto begin() const REMOTE_FMT_LIFETIMEBOUND { return data_.begin(); }

    [[nodiscard]] constexpr auto end() const REMOTE_FMT_LIFETIMEBOUND { return data_.end(); }
};

static_assert(mapsTo<MappedTypeOnly<Color>,
                     std::map<int,
                              std::string_view>>);
static_assert(mapsTo<MapWithoutPairElement,
                     std::vector<int>>,
              "falls back to the plain range mapping rather than failing");

// Any map-like lands on std::map; fmt renders both as {k: v}, and only spec validity matters here.
static_assert(mapsTo<std::unordered_map<int,
                                        std::string>,
                     std::map<int,
                              std::string_view>>);
static_assert(mapsTo<std::tuple<int,
                                char,
                                std::string>,
                     std::tuple<int,
                                char,
                                std::string_view>>);
static_assert(mapsTo<std::pair<Color,
                               int>,
                     std::tuple<std::string_view,
                                int>>);

    #if REMOTE_FMT_FMT_CHECK_FULL
static_assert(mapsTo<std::optional<std::byte>,
                     std::optional<std::uint8_t>>);
static_assert(mapsTo<std::variant<Color,
                                  int>,
                     std::variant<std::string_view,
                                  int>>);
static_assert(mapsTo<std::expected<std::byte,
                                   Color>,
                     std::expected<std::uint8_t,
                                   std::string_view>>);
static_assert(mapsTo<std::expected<void,
                                   Color>,
                     std::expected<void,
                                   std::string_view>>);
static_assert(mapsTo<std::chrono::milliseconds,
                     std::chrono::milliseconds>);
static_assert(mapsTo<std::chrono::sys_time<std::chrono::milliseconds>,
                     std::chrono::milliseconds>);
    #endif

// In both modes: a C++26 optional is also a range, which must not win.
static_assert(mapsTo<std::optional<int>,
                     std::optional<int>>);

// A type with only a remote_fmt::formatter is text on the host, so its spec is a string's.
struct OnlyRemoteFmt {};

static_assert(std::is_same_v<Detail::checkable_t<OnlyRemoteFmt>,
                             std::string_view>);
static_assert(std::is_same_v<Detail::checkable_t<OnlyRemoteFmt const&>,
                             std::string_view>);
    #if !REMOTE_FMT_FMT_CHECK_FULL
// remote_fmt's own types whose fmt formatter is out of reach stay unchecked.
static_assert(std::is_same_v<Detail::checkable_t<std::optional<int>>,
                             Detail::UncheckedArg>);
static_assert(std::is_same_v<Detail::checkable_t<std::chrono::milliseconds>,
                             Detail::UncheckedArg>);
// Inside a container only the element is, the container's own spec is still checked.
static_assert(std::is_same_v<Detail::checkable_t<std::vector<std::optional<int>>>,
                             std::vector<Detail::UncheckedArg>>);
static_assert(std::is_same_v<Detail::checkable_t<std::tuple<std::chrono::milliseconds,
                                                            int>>,
                             std::tuple<Detail::UncheckedArg,
                                        int>>);
static_assert(std::is_same_v<Detail::checkable_t<std::map<int,
                                                          std::chrono::seconds>>,
                             std::map<int,
                                      Detail::UncheckedArg>>);
    #endif
static_assert(std::is_same_v<Detail::checkable_t<std::array<OnlyRemoteFmt,
                                                            2>>,
                             std::vector<std::string_view>>);
static_assert(std::is_same_v<Detail::checkable_t<std::set<OnlyRemoteFmt>>,
                             std::set<std::string_view>>);
static_assert(std::is_same_v<Detail::checkable_t<int>,
                             int>);

#endif   // REMOTE_FMT_USE_FMT_CHECK

// Each of these must compile.
void acceptedFormatStrings() {
    remote_fmt::checkFormatString<>("no fields"_sc);
    remote_fmt::checkFormatString<>("escaped {{}} braces"_sc);
    remote_fmt::checkFormatString<int>("{}"_sc);
    remote_fmt::checkFormatString<int>("{:>8}"_sc);
    remote_fmt::checkFormatString<int>("{:#06x}"_sc);
    remote_fmt::checkFormatString<int>("{:+}"_sc);
    remote_fmt::checkFormatString<unsigned>("{:b}"_sc);
    remote_fmt::checkFormatString<double>("{:.3f}"_sc);
    remote_fmt::checkFormatString<double>("{:.1100f}"_sc);
    remote_fmt::checkFormatString<double>("{:e}"_sc);
    remote_fmt::checkFormatString<bool>("{:s}"_sc);
    remote_fmt::checkFormatString<char>("{:?}"_sc);
    remote_fmt::checkFormatString<void const*>("{}"_sc);
    remote_fmt::checkFormatString<std::string_view>("{:>5}"_sc);
    remote_fmt::checkFormatString<std::string_view>("{:?}"_sc);
    remote_fmt::checkFormatString<char const(&)[3]>("{:*^7}"_sc);
    remote_fmt::checkFormatString<Color>("{:>8}"_sc);
    remote_fmt::checkFormatString<std::byte>("{:#x}"_sc);
    remote_fmt::checkFormatString<OnlyRemoteFmt>("{}"_sc);
    remote_fmt::checkFormatString<OnlyRemoteFmt>("{:>20}"_sc);

    remote_fmt::checkFormatString<std::vector<int>>("{}"_sc);
    remote_fmt::checkFormatString<std::vector<int>>("{::>5}"_sc);
    remote_fmt::checkFormatString<std::vector<int>>("{:n}"_sc);
    remote_fmt::checkFormatString<std::vector<std::vector<int>>>("{:::>4}"_sc);
    remote_fmt::checkFormatString<std::map<int, int>>("{}"_sc);
    remote_fmt::checkFormatString<std::map<int, int>>("{::}"_sc);
    remote_fmt::checkFormatString<std::set<int>>("{}"_sc);
    remote_fmt::checkFormatString<std::tuple<int, char, std::string_view>>("{}"_sc);
    remote_fmt::checkFormatString<std::vector<OnlyRemoteFmt>>("{::>8}"_sc);
    remote_fmt::checkFormatString<std::tuple<OnlyRemoteFmt, int>>("{:n}"_sc);
    remote_fmt::checkFormatString<std::vector<std::chrono::milliseconds>>("{::%Q}"_sc);
    remote_fmt::checkFormatString<std::vector<std::chrono::milliseconds>>("{:n:%Q}"_sc);
    remote_fmt::checkFormatString<std::map<int, std::chrono::seconds>>("{}"_sc);

    // literal text containing digits must not trip the number limit
    remote_fmt::checkFormatString<bool, float, unsigned>(
      "vbus: {:<5}, ucTemp: {:.1f} C, loops: {}"_sc);
    remote_fmt::checkFormatString<int>("reboot count 1000000: {}"_sc);

#if REMOTE_FMT_FMT_CHECK_FULL
    remote_fmt::checkFormatString<std::chrono::milliseconds>("{}"_sc);
    remote_fmt::checkFormatString<std::chrono::milliseconds>("{:%Q}"_sc);
    remote_fmt::checkFormatString<std::chrono::milliseconds>("{:%Q%q}"_sc);
    remote_fmt::checkFormatString<std::optional<int>>("{}"_sc);
    remote_fmt::checkFormatString<std::optional<int>>("{:>6}"_sc);
    remote_fmt::checkFormatString<std::variant<int, std::string_view>>("{}"_sc);
    remote_fmt::checkFormatString<std::expected<int, int>>("{}"_sc);
    remote_fmt::checkFormatString<std::expected<void, int>>("{}"_sc);
#endif
}

// fmt considers every string here valid; these are remote_fmt's own rules (format_grammar.hpp).
constexpr bool plainFields(std::string_view s) {
    auto const shape = remote_fmt::Detail::formatShape(s);
    return shape.wellFormed && shape.plainFields;
}

void argumentIdRules() {
    static_assert(plainFields("{}"));
    static_assert(plainFields("{:>5}"));
    static_assert(plainFields("{} {}"));
    static_assert(plainFields("{{0}} literal"));
    static_assert(plainFields("{::>4}"), "a range element spec is not an argument id");
    static_assert(plainFields("{:%Q%q}"));

    static_assert(!plainFields("{0}"));
    static_assert(!plainFields("{1} {0}"));
    static_assert(!plainFields("{name}"));
    static_assert(!plainFields("{0:>5}"));
    static_assert(!plainFields("{:{}}"), "dynamic width");
    static_assert(!plainFields("{:.{}}"), "dynamic precision");
    static_assert(!plainFields("{"));

    // the field count, nested fields counted as one: the target's "invalid replacement field count"
    static_assert(remote_fmt::Detail::formatShape("{} {{}} {:>3}").fields == 2);
    static_assert(remote_fmt::Detail::formatShape("{:{}}").fields == 1);
    static_assert(!remote_fmt::Detail::formatShape("{:}}x}").wellFormed,
                  "no escape inside a field");
}

}   // namespace

int main() {
    acceptedFormatStrings();
    argumentIdRules();
    std::printf("fmt_check: mapping and positive table ok (fmt check %s, full %s)\n",
                REMOTE_FMT_USE_FMT_CHECK ? "on" : "off",
                REMOTE_FMT_FMT_CHECK_FULL ? "yes" : "no");
    return 0;
}
