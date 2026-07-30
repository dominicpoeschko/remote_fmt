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

enum class Color : std::uint8_t { Red, Blue };
enum class Tiny : signed char { A, B };

#if REMOTE_FMT_USE_FMT_CHECK

namespace detail = remote_fmt::detail;

template<typename Device, typename Host>
constexpr bool maps_to = std::is_same_v<detail::host_type_t<Device>, Host>;

// Scalars: the wire preserves width and signedness.
static_assert(maps_to<std::int8_t,
                      std::int8_t>);
static_assert(maps_to<std::uint64_t,
                      std::uint64_t>);
static_assert(maps_to<float,
                      float>);
static_assert(maps_to<double,
                      double>);
static_assert(maps_to<bool,
                      bool>);
static_assert(maps_to<char,
                      char>);

// Types that change across the wire.
static_assert(maps_to<int*,
                      void const*>);
static_assert(maps_to<void const*,
                      void const*>);
// fmt has its own std::byte formatter, but the wire carries a 1-byte unsigned.
static_assert(maps_to<std::byte,
                      std::uint8_t>);
static_assert(maps_to<std::string,
                      std::string_view>);
static_assert(maps_to<std::string_view,
                      std::string_view>);
static_assert(maps_to<char[6],
                      std::string_view>);
static_assert(maps_to<decltype("abc"_sc),
                      std::string_view>);
static_assert(maps_to<Color,
                      std::string_view>);
static_assert(!fmt::is_formattable<Color>::value,
              "fmt still cannot format an enum directly");
// Underlying integer when enchantum does not know the value; char-like types are widened.
static_assert(std::is_same_v<detail::host_type_alt_t<Color>,
                             std::uint8_t>);
static_assert(std::is_same_v<detail::host_type_alt_t<Tiny>,
                             std::int8_t>);

// Containers keep their category; elements are mapped too.
static_assert(maps_to<std::vector<int>,
                      std::vector<int>>);
static_assert(maps_to<std::array<std::byte,
                                 4>,
                      std::vector<std::uint8_t>>);
static_assert(maps_to<std::vector<Color>,
                      std::vector<std::string_view>>);
static_assert(maps_to<std::set<Color>,
                      std::set<std::string_view>>);
static_assert(maps_to<std::map<Color,
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

    constexpr auto begin() const { return data_.begin(); }

    constexpr auto end() const { return data_.end(); }
};

template<typename K, std::size_t Capacity>
struct FixedSet {
    using key_type   = K;
    using value_type = K;
    std::array<K, Capacity> data_{};

    constexpr auto begin() const { return data_.begin(); }

    constexpr auto end() const { return data_.end(); }
};

static_assert(maps_to<FixedMap<Color,
                               std::byte,
                               4>,
                      std::map<std::string_view,
                               std::uint8_t>>);
static_assert(maps_to<FixedSet<Color,
                               4>,
                      std::set<std::string_view>>);

// The mapping must never demand more of a type than remote_fmt's own formatter does, or a type the
// device can serialize becomes a build error. remote_fmt.hpp's is_map asks only for mapped_type and
// then serializes elements through formatter<range_value_t>, so neither of these may hard-error.
template<typename V>
struct MappedTypeOnly {   // is_map, but no key_type
    using mapped_type = V;
    using value_type  = std::pair<int, V>;
    std::array<value_type, 2> data_{};

    constexpr auto begin() const { return data_.begin(); }

    constexpr auto end() const { return data_.end(); }
};

struct MapWithoutPairElement {   // is_map, but the element is not a pair
    using mapped_type = int;
    std::array<int, 2> data_{};

    constexpr auto begin() const { return data_.begin(); }

    constexpr auto end() const { return data_.end(); }
};

static_assert(maps_to<MappedTypeOnly<Color>,
                      std::map<int,
                               std::string_view>>);
static_assert(maps_to<MapWithoutPairElement,
                      std::vector<int>>,
              "falls back to the plain range mapping rather than failing");

// Any map-like lands on std::map; fmt renders both as {k: v}, and only spec validity matters here.
static_assert(maps_to<std::unordered_map<int,
                                         std::string>,
                      std::map<int,
                               std::string_view>>);
static_assert(maps_to<std::tuple<int,
                                 char,
                                 std::string>,
                      std::tuple<int,
                                 char,
                                 std::string_view>>);
static_assert(maps_to<std::pair<Color,
                                int>,
                      std::tuple<std::string_view,
                                 int>>);

    #if REMOTE_FMT_FMT_CHECK_FULL
static_assert(maps_to<std::optional<std::byte>,
                      std::optional<std::uint8_t>>);
static_assert(maps_to<std::variant<Color,
                                   int>,
                      std::variant<std::string_view,
                                   int>>);
static_assert(maps_to<std::expected<std::byte,
                                    Color>,
                      std::expected<std::uint8_t,
                                    std::string_view>>);
static_assert(maps_to<std::expected<void,
                                    Color>,
                      std::expected<void,
                                    std::string_view>>);
static_assert(maps_to<std::chrono::milliseconds,
                      std::chrono::milliseconds>);
static_assert(maps_to<std::chrono::sys_time<std::chrono::milliseconds>,
                      std::chrono::milliseconds>);
    #endif

// A type fmt knows nothing about degrades instead of breaking the build - this is what keeps user
// wrappers such as uc_log::Metric working.
struct OnlyRemoteFmt {};

static_assert(std::is_same_v<detail::checkable_t<OnlyRemoteFmt>,
                             detail::unchecked_arg>);
static_assert(std::is_same_v<detail::checkable_t<int>,
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

    remote_fmt::checkFormatString<std::vector<int>>("{}"_sc);
    remote_fmt::checkFormatString<std::vector<int>>("{::>5}"_sc);
    remote_fmt::checkFormatString<std::vector<int>>("{:n}"_sc);
    remote_fmt::checkFormatString<std::vector<std::vector<int>>>("{:::>4}"_sc);
    remote_fmt::checkFormatString<std::map<int, int>>("{}"_sc);
    remote_fmt::checkFormatString<std::map<int, int>>("{::}"_sc);
    remote_fmt::checkFormatString<std::set<int>>("{}"_sc);
    remote_fmt::checkFormatString<std::tuple<int, char, std::string_view>>("{}"_sc);

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

// fmt considers every string here valid; these are remote_fmt's own rules.
void argumentIdRules() {
    using remote_fmt::detail::fieldsCarryNoArgumentId;
    static_assert(fieldsCarryNoArgumentId("{}"));
    static_assert(fieldsCarryNoArgumentId("{:>5}"));
    static_assert(fieldsCarryNoArgumentId("{} {}"));
    static_assert(fieldsCarryNoArgumentId("{{0}} literal"));
    static_assert(fieldsCarryNoArgumentId("{::>4}"), "a range element spec is not an argument id");
    static_assert(fieldsCarryNoArgumentId("{:%Q%q}"));

    static_assert(!fieldsCarryNoArgumentId("{0}"));
    static_assert(!fieldsCarryNoArgumentId("{1} {0}"));
    static_assert(!fieldsCarryNoArgumentId("{name}"));
    static_assert(!fieldsCarryNoArgumentId("{0:>5}"));
    static_assert(!fieldsCarryNoArgumentId("{:{}}"), "dynamic width");
    static_assert(!fieldsCarryNoArgumentId("{:.{}}"), "dynamic precision");
    static_assert(!fieldsCarryNoArgumentId("{"));
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
