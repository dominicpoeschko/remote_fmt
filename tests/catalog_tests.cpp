// Tests for the catalog mode (use_catalog == true, the default): format strings and
// StringConstant arguments are transmitted as 16 bit ids and resolved through the string
// constants map on the parser side, which is checked against the json the build wrote.
#include "remote_fmt/catalog.hpp"

#include "remote_fmt/catalog_helpers.hpp"
#include "remote_fmt/parser.hpp"
#include "remote_fmt/remote_fmt.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

using namespace sc::literals;

// catalog<>() is defined by the generated catalog
#ifdef __clang__
    #pragma clang diagnostic ignored "-Wundefined-func-template"
#endif

static constexpr auto fmtString{"Test {}"_sc};
static constexpr auto argString{"hello"_sc};

// A bitflag enum. test_roundtrip covers the rendering, but it builds with the catalog OFF, so the
// combined-value path is only exercised here: each set flag's name is its own StringConstant and
// therefore its own catalog id, and the parser has to resolve every one of them before it can join
// them into "read|write".
enum struct Perm : std::uint8_t { read = 1 << 0, write = 1 << 1 };

constexpr Perm operator|(Perm a,
                         Perm b) {
    return Perm(std::uint8_t(std::uint8_t(a) | std::uint8_t(b)));
}

constexpr Perm operator&(Perm a,
                         Perm b) {
    return Perm(std::uint8_t(std::uint8_t(a) & std::uint8_t(b)));
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

static constexpr auto bitflagFmtString{"Perm {}"_sc};
static constexpr auto readName{sc::create([]() { return enchantum::to_string(Perm::read); })};
static constexpr auto writeName{sc::create([]() { return enchantum::to_string(Perm::write); })};

template<typename T>
remote_fmt::catalog_id idOf(T const&) {
    return remote_fmt::catalog<T>();
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
      {       idOf(fmtString),        std::string{std::string_view{fmtString}}},
      {       idOf(argString),        std::string{std::string_view{argString}}},
      {idOf(bitflagFmtString), std::string{std::string_view{bitflagFmtString}}},
      {        idOf(readName),         std::string{std::string_view{readName}}},
      {       idOf(writeName),        std::string{std::string_view{writeName}}}
    };
    return map;
}

remote_fmt::catalog_id printAtSite(remote_fmt::Printer<VectorBackend>& printer) {
    auto const site = REMOTE_FMT_SITE();
    printer.print(remote_fmt::SiteId{site(fmtString)}, fmtString, 7);
    return site(fmtString);
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

    {
        remote_fmt::Printer<VectorBackend> printer{};
        printer.print(bitflagFmtString, Perm::read | Perm::write);
        auto const& buffer = printer.get_com_backend().memory;

        auto const [message, remaining, discarded]
          = remote_fmt::parse(std::span{buffer}, stringConstantsMap(), [](std::string_view) {});
        CHECK(message.has_value() && *message == "Perm read|write",
              "cataloged bitflag names resolve and join");
        CHECK(remaining.empty() && discarded == 0, "buffer fully consumed");
    }

    {
        // One name missing from the map must fail the whole bitflag, not render half of it.
        remote_fmt::Printer<VectorBackend> printer{};
        printer.print(bitflagFmtString, Perm::read | Perm::write);
        auto const& buffer = printer.get_com_backend().memory;

        auto partialMap = stringConstantsMap();
        partialMap.erase(idOf(writeName));

        bool errorReported = false;
        auto const [message, remaining, discarded]
          = remote_fmt::parse(std::span{buffer}, partialMap, [&](std::string_view error) {
                errorReported = error.find("not found") != std::string_view::npos;
            });
        CHECK(!message, "bitflag with an unknown name produces no message");
        CHECK(errorReported, "bitflag with an unknown name reports an error");
    }

    {
        CHECK(idOf(fmtString) != idOf(argString) && idOf(readName) != idOf(writeName),
              "one tag per string");
        CHECK(remote_fmt::catalog<decltype(fmtString) const&>() == idOf(fmtString),
              "const& asks for the same tag");
    }

    auto const written = remote_fmt::parseStringConstantsFromJsonFile(REMOTE_FMT_TEST_CATALOG_JSON);
    CHECK(written.has_value(), "the catalog json reads");
    if(written) {
        for(auto const& [id, text] : stringConstantsMap()) {
            auto const it = written->find(id);
            CHECK(it != written->end() && it->second == text,
                  "the json has every string at its id");
        }
    }

    {
        remote_fmt::Printer<VectorBackend> printer{};
        auto const                         site   = printAtSite(printer);
        auto const&                        buffer = printer.get_com_backend().memory;
        CHECK(site != idOf(fmtString), "a site has an id of its own");

        auto map  = stringConstantsMap();
        map[site] = std::string{std::string_view{fmtString}};
        auto const parsed
          = remote_fmt::parseMessage(std::span{buffer}, map, [](std::string_view) {});
        CHECK(parsed.message == std::optional<std::string>{"Test 7"}, "a site's line resolves");
        CHECK(parsed.catalogId == std::optional<remote_fmt::catalog_id>{site},
              "parseMessage hands back the site's id");

        std::ifstream stream{REMOTE_FMT_TEST_CATALOG_JSON};
        auto const    json  = nlohmann::json::parse(stream);
        auto const    sites = json.at("Sites");
        auto const    key   = std::to_string(static_cast<unsigned>(site));
        CHECK(sites.contains(key)
                && sites.at(key).get<std::string>().find("printAtSite") != std::string::npos,
              "the json names the site's function");
        if(written) {
            auto const it = written->find(site);
            CHECK(it != written->end() && it->second == "Test {}",
                  "the json has the site's string");
        }
    }

    if(failures != 0) {
        std::printf("%d test(s) failed\n", failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
