// Tests for the catalog mode (useCatalog == true, the default): a format string with its arguments' descriptors is
// transmitted as a 16 bit id and resolved through the string constants map on the parser side, which is checked
// against the json the build wrote (its strings, sites and type entries).
#include "remote_fmt/catalog.hpp"

#include "TestSupport.ipp"
#include "remote_fmt/catalog_helpers.hpp"
#include "remote_fmt/parser.hpp"
#include "remote_fmt/remote_fmt.hpp"
#include "remote_fmt/wire.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ios>
#include <iterator>
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

// A bitflag enum. test_roundtrip covers the rendering, but it builds with the catalog OFF, where the names are inline;
// here they are a type entry of the catalog (detail::typeTag, extract_sites.py "Types"), the site's descriptor has its
// key, and the reader has to resolve it before the names can be joined into "read|write".
enum struct [[clang::flag_enum]] Perm : std::uint8_t { read = 1 << 0, write = 1 << 1 };

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

// The same sub format string sent two ways: format_to (`*`: the sub format string's id, then the arguments) and a
// declared descriptor (`F`: the arguments only). Both decode to the same text; the declared one is the id shorter.
static constexpr auto pairFmtString{"{}-{}"_sc};
static constexpr auto twoFmtString{"Two {} {}"_sc};

struct PairByFormatTo {
    int a;
    int b;
};

struct PairDeclared {
    int a;
    int b;
};

template<>
struct remote_fmt::formatter<PairByFormatTo> {
    template<typename Printer>
    constexpr auto format(PairByFormatTo const& p,
                          Printer&              printer) const {
        return format_to(printer, pairFmtString, p.a, p.b);
    }
};

template<>
struct remote_fmt::formatter<PairDeclared> {
    static consteval auto descriptor() {
        return remote_fmt::subFormatDescriptor<int, int>(pairFmtString);
    }

    template<typename Printer>
    constexpr void format(PairDeclared const& p,
                          Printer&            printer) const {
        printer.formatArguments(p.a, p.b);
    }
};

// the id of a format string with its arguments' descriptors, as print(fmt, args...) sends it
template<typename... Args,
         typename T>
remote_fmt::catalog_id idOf(T const&) {
    return remote_fmt::catalog<decltype(remote_fmt::described<Args...>(T{}))>();
}

template<typename... Args,
         typename T>
std::string textOf(T const&) {
    return std::string{std::string_view{remote_fmt::described<Args...>(T{})}};
}

namespace {

using remote_fmt::test::VectorBackend;

// Leaked on purpose: avoids the global-constructor and exit-time-destructor warnings.
std::unordered_map<std::uint16_t,
                   std::string> const&
stringConstantsMap() {
    static auto const& map = *new std::unordered_map<std::uint16_t, std::string>{
      {idOf<int>(fmtString), textOf<int>(fmtString)},
      {idOf<decltype(argString)>(fmtString), textOf<decltype(argString)>(fmtString)},
      {idOf<int, int>(pairFmtString), textOf<int, int>(pairFmtString)},
      {idOf<PairByFormatTo>(fmtString), textOf<PairByFormatTo>(fmtString)},
      {idOf<PairDeclared>(fmtString), textOf<PairDeclared>(fmtString)},
      {idOf<int, decltype(argString)>(twoFmtString),
       textOf<int, decltype(argString)>(twoFmtString)}
    };
    return map;
}

remote_fmt::catalog_id printAtSite(remote_fmt::Printer<VectorBackend>& printer) {
    auto const site = REMOTE_FMT_SITE();
    printer.print(remote_fmt::SiteId{site(remote_fmt::described<int>(fmtString))}, fmtString, 7);
    return site(remote_fmt::described<int>(fmtString));
}

struct CountingBackend {
    std::vector<std::byte> memory;
    std::size_t            writes{};

    void write(std::span<std::byte const> data) {
        ++writes;
        memory.insert(memory.end(), data.begin(), data.end());
    }
};

// print(SiteId, fmt, args...) and uc_log's three pieces (beginCataloged, formatArguments, endCataloged) are one record
// on the wire; this file is built once per encoder knob (tests/CMakeLists.txt), so the identities hold at every knob
// value.
void catalogedPathsAgree() {
    auto const site = remote_fmt::SiteId{idOf<int, decltype(argString)>(twoFmtString)};

    remote_fmt::Printer<VectorBackend> whole{};
    whole.print(site, twoFmtString, 7, argString);
    remote_fmt::Printer<VectorBackend> pieces{};
    pieces.beginCataloged(site);
    pieces.formatArguments(7, argString);
    pieces.endCataloged();
    CHECK(whole.getComBackend().memory == pieces.getComBackend().memory,
          "print(SiteId) == beginCataloged + formatArguments + endCataloged");

    remote_fmt::Printer<VectorBackend> twice{};
    twice.beginCataloged(site);
    twice.formatArguments(7);
    twice.formatArguments(argString);
    twice.endCataloged();
    CHECK(whole.getComBackend().memory == twice.getComBackend().memory,
          "formatArguments in two calls == in one");

    auto const parsed = remote_fmt::parseMessage(std::span{whole.getComBackend().memory},
                                                 stringConstantsMap(),
                                                 [](std::string_view) {});
    CHECK(parsed.message == std::optional<std::string>{"Two 7 hello"}, "the record decodes");

    remote_fmt::Printer<VectorBackend> byFormatTo{};
    byFormatTo.print(fmtString, PairByFormatTo{3, -400});
    remote_fmt::Printer<VectorBackend> declared{};
    declared.print(fmtString, PairDeclared{3, -400});
    CHECK(byFormatTo.getComBackend().memory.size() == declared.getComBackend().memory.size() + 2,
          "format_to sends the sub format string's id, a declared descriptor does not");
    for(auto const* p : {&byFormatTo, &declared}) {
        auto const sub = remote_fmt::parseMessage(
          std::span{p->getComBackend().memory},
          stringConstantsMap(),
          [](std::string_view e) {
              std::printf("parser: %.*s\n", static_cast<int>(e.size()), e.data());
          });
        CHECK(sub.message == std::optional<std::string>{"Test 3--400"},
              "the sub format string decodes");
    }
}

// The knob this build claims took effect (roundtrip_tests.cpp knobEffects() for the inline-string frames).
void knobEffects() {
    remote_fmt::Printer<CountingBackend> printer{};
    printer.print(fmtString, std::uint32_t{5});
    auto const& backend = printer.getComBackend();
    // start marker, the 2-byte id, then the value: 5 as one LEB128 byte, [CRC], end marker
    CHECK(backend.memory.size()
            == std::size_t{4} + (REMOTE_FMT_TEST_EXPECT_FRAME_CHECK ? 1 : 0) + 1,
          "a whole frame of id and one byte");
    if(backend.memory.size() < 5) { return; }
#if REMOTE_FMT_TEST_EXPECT_FRAME_CHECK
    CHECK(backend.memory.front() == remote_fmt::protocol::checkedStartMarker,
          "FRAME_CHECK 1: a checked frame");
#else
    CHECK(backend.memory.front() == remote_fmt::protocol::startMarker,
          "FRAME_CHECK 0: an unchecked frame");
#endif
    CHECK(backend.memory[3] == std::byte{5}, "5 goes out in one byte");
#if REMOTE_FMT_TEST_EXPECT_STAGING_BYTES == 0
    CHECK(backend.writes > 1, "STAGING_BYTES 0: every field is a write of its own");
#else
    CHECK(backend.writes == 1, "staged: a short record is one write");
#endif
}

}   // namespace

namespace {
// The catalog reader on hand-written files: every key and every entry's "args" are required (the build writes the
// catalog once, whole, after the link: tools/extract_sites.py), "Types" and their resolution, and broken ones (an error
// that names what is wrong, never a half-read catalog).
void catalogFileShapes() {
    using remote_fmt::FrameCheck;
    using remote_fmt::parseCatalogFile;

    {
        auto const f = parseCatalogFile(
          R"j({"StringConstants": [{"id": 3, "fmt": "a {} {}", "args": ["u4", "F7:<{} {}>2:i4i4"]},)j"
          R"j( {"id": 7, "fmt": "b", "args": []}], "Sites": {"32769": "void f()"}, "FrameCheck": true,)j"
          R"j( "Types": {}, "Other": 1})j");
        CHECK(f && f->strings.size() == 2 && f->strings.at(3) == "a {} {}\x1eu4F7:<{} {}>2:i4i4"
                && f->strings.at(7) == "b\x1e",
              "entries joined: fmt 0x1e descriptors, also without arguments");
        CHECK(f && f->sites.at("32769") == "void f()", "Sites read");
        CHECK(f && f->frameCheck == FrameCheck::required, "FrameCheck true: required");
    }
    {
        auto const f = parseCatalogFile(
          R"j({"StringConstants": [], "Sites": {}, "FrameCheck": false, "Types": {}})j");
        auto const last = parseCatalogFile(
          "{\"StringConstants\": [],\n  \"Sites\": {},\n  \"Types\": {},\n  \"FrameCheck\": "
          "true\n}\n");
        CHECK(last && last->frameCheck == FrameCheck::required, "FrameCheck true as the last key");
        CHECK(f && f->frameCheck == FrameCheck::none, "FrameCheck false: none");
    }

    // a key missing: malformed, whichever it is, and the error names it
    struct Missing {
        std::string_view key;
        std::string_view text;
    };

    for(auto const& [key, text] :
        std::array{
          Missing{"StringConstants",           R"j({"Sites": {}, "FrameCheck": true, "Types": {}})j"                  },
          Missing{          "Sites", R"j({"StringConstants": [], "FrameCheck": true, "Types": {}})j"},
          Missing{     "FrameCheck",        R"j({"StringConstants": [], "Sites": {}, "Types": {}})j"},
          Missing{          "Types", R"j({"StringConstants": [], "Sites": {}, "FrameCheck": true})j"},
          Missing{
                  "args",                                 R"j({"StringConstants": [{"id": 9, "fmt": "plain"}], "Sites": {}, "FrameCheck": true,)j"
                                 R"j( "Types": {}})j"                      },
          Missing{
                  "fmt",                                 R"j({"StringConstants": [{"id": 1, "args": []}], "Sites": {}, "FrameCheck": true,)j"
                                 R"j( "Types": {}})j"                       },
    })
    {
        auto const f = parseCatalogFile(std::string{text});
        CHECK(!f && f.error().contains(key), "a key missing: an error that names it");
    }
    CHECK(!parseCatalogFile(
            R"j({"StringConstants": [], "Sites": {}, "FrameCheck": "yes", "Types": {}})j"),
          "FrameCheck not a bool: an error");
    {
        auto const f = parseCatalogFile(
          R"j({"StringConstants": [{"id": 1, "fmt": "x", "args": []}, {"id": 1, "fmt": "y", "args": []}],)j"
          R"j( "Sites": {}, "FrameCheck": true, "Types": {}})j");
        CHECK(f && f->strings.at(1) == "x\x1e", "a repeated id keeps the first");
    }
    {
        // a type reference resolved into the string, also inside another descriptor and inside another entry;
        // a '#' in the format string or in a counted text is no reference
        auto const f = parseCatalogFile(
          "{\"StringConstants\": [{\"id\": 5, \"fmt\": \"{:#x} {}\", \"args\": "
          "[\"O#00000000000000aa\", \"(#00000000000000bbk2:#a)\"]}], \"Sites\": {}, "
          "\"FrameCheck\": true,"
          " \"Types\": {\"00000000000000aa\": \"Eu11:1,1:a\", \"00000000000000bb\": "
          "\"F2:{}1:#00000000000000aa\"}}");
        CHECK(f && f->strings.at(5) == "{:#x} {}\x1eOEu11:1,1:a(F2:{}1:Eu11:1,1:ak2:#a)",
              "type references resolved");
        auto const missing = parseCatalogFile(
          R"j({"StringConstants": [{"id": 5, "fmt": "{}", "args": ["#00000000000000cc"]}], "Sites": {},)j"
          R"j( "FrameCheck": true, "Types": {}})j");
        CHECK(!missing && missing.error().contains("string 5"),
              "a reference without its entry: an error that names the string");
        auto const inEntry = parseCatalogFile(
          R"j({"StringConstants": [{"id": 6, "fmt": "{}", "args": ["#00000000000000aa"]}], "Sites": {},)j"
          R"j( "FrameCheck": true, "Types": {"00000000000000aa": "O#00000000000000cc"}})j");
        CHECK(!inEntry && inEntry.error().contains("string 6"),
              "an entry's own reference without its entry: an error too");
    }
    CHECK(!parseCatalogFile(R"j({"StringConstants": [{"id": 1, "fmt": "x", "args": []})j"),
          "a truncated file is an error");
    CHECK(!parseCatalogFile(
            R"j({"StringConstants": [{"id": 70000, "fmt": "x", "args": []}], "Sites": {},)j"
            R"j( "FrameCheck": true, "Types": {}})j"),
          "an id above 16 bits is an error");
    CHECK(!remote_fmt::readCatalogFile("/nonexistent/catalog.json"), "a missing file is an error");
}
}   // namespace

int main() {
    catalogedPathsAgree();
    knobEffects();

    {
        remote_fmt::Printer<VectorBackend> printer{};
        printer.print(fmtString, 42);
        auto const& buffer = printer.getComBackend().memory;

        auto const [message, remaining, discarded, catalogId]
          = remote_fmt::parseMessage(std::span{buffer}, stringConstantsMap(), [](std::string_view) {
            });
        CHECK(message.has_value() && *message == "Test 42", "cataloged format string resolves");
        CHECK(remaining.empty() && discarded == 0, "buffer fully consumed");
    }

    {
        remote_fmt::Printer<VectorBackend> printer{};
        printer.print(fmtString, argString);
        auto const& buffer = printer.getComBackend().memory;
        CHECK(buffer.size() == (REMOTE_FMT_FRAME_CHECK ? 5U : 4U),
              "a string constant argument sends nothing");

        auto const [message, remaining, discarded, catalogId]
          = remote_fmt::parseMessage(std::span{buffer}, stringConstantsMap(), [](std::string_view) {
            });
        CHECK(message.has_value() && *message == "Test hello",
              "a string constant argument comes from the descriptor");
        CHECK(remaining.empty() && discarded == 0, "buffer fully consumed");
    }

    {
        // Unknown catalog ids must fail with an error message instead of formatting garbage.
        remote_fmt::Printer<VectorBackend> printer{};
        printer.print(fmtString, 42);
        auto const& buffer = printer.getComBackend().memory;

        bool errorReported = false;
        auto const [message, remaining, discarded, catalogId]
          = remote_fmt::parseMessage(std::span{buffer}, {}, [&](std::string_view error) {
                errorReported = error.contains("not found");
            });
        CHECK(!message, "unknown catalog id produces no message");
        CHECK(errorReported, "unknown catalog id reports an error");
    }

    auto const file = remote_fmt::readCatalogFile(REMOTE_FMT_TEST_CATALOG_JSON);
    CHECK(file.has_value(),
          "the catalog json reads (every type reference of the image has its entry)");

    if(file) {
        // the names are a type entry: the line decodes with the catalog as the reader loads it
        remote_fmt::Printer<VectorBackend> printer{};
        printer.print(bitflagFmtString, Perm::read | Perm::write);
        auto const& buffer = printer.getComBackend().memory;
        CHECK(buffer.size() == (REMOTE_FMT_FRAME_CHECK ? 6U : 5U),
              "a bitflag enum sends its raw value");

        auto const [message, remaining, discarded, catalogId]
          = remote_fmt::parseMessage(std::span{buffer}, file->strings, [](std::string_view e) {
                std::printf("parser: %.*s\n", static_cast<int>(e.size()), e.data());
            });
        CHECK(message.has_value() && *message == "Perm read|write",
              "the type entry's names resolve and join");
        CHECK(remaining.empty() && discarded == 0, "buffer fully consumed");
        std::ifstream     in{REMOTE_FMT_TEST_CATALOG_JSON, std::ios::binary};
        std::string const text{std::istreambuf_iterator<char>{in}, {}};
        auto const        raw = text.contains("\"Types\"") && !text.contains("8:Perm");
        CHECK(raw && file->types.size() == 1
                && file->types.begin()->second == "Gu12:1,4:read2,5:write",
              "one entry per type, keyed by its text's hash");

        // without the entry the line does not decode, and says why
        auto       map  = file->strings;
        auto const id   = idOf<Perm>(bitflagFmtString);
        map[id]         = textOf<Perm>(bitflagFmtString);   // the key, unresolved
        bool       said = false;
        auto const r    = remote_fmt::parseMessage(std::span{buffer}, map, [&](std::string_view e) {
            said = e.contains("type reference");
        });
        CHECK(!r.message && said, "a type reference without its entry: no text, an error");
    }

    if(file) {
        for(auto const& [id, text] : stringConstantsMap()) {
            auto const it = file->strings.find(id);
            CHECK(it != file->strings.end() && it->second == text,
                  "the json has every string at its id");
        }
    }

    {
        remote_fmt::Printer<VectorBackend> printer{};
        auto const                         site   = printAtSite(printer);
        auto const&                        buffer = printer.getComBackend().memory;
        CHECK(site != idOf<int>(fmtString), "a site has an id of its own");

        auto map  = stringConstantsMap();
        map[site] = textOf<int>(fmtString);
        auto const parsed
          = remote_fmt::parseMessage(std::span{buffer}, map, [](std::string_view) {});
        CHECK(parsed.message == std::optional<std::string>{"Test 7"}, "a site's line resolves");
        CHECK(parsed.catalogId == std::optional<remote_fmt::catalog_id>{site},
              "parseMessage hands back the site's id");

        auto const key = std::to_string(static_cast<unsigned>(site));
        CHECK(file && file->sites.contains(key) && file->sites.at(key).contains("printAtSite"),
              "the json names the site's function");
        CHECK(file
                && file->frameCheck
                     == (REMOTE_FMT_FRAME_CHECK ? remote_fmt::FrameCheck::required
                                                : remote_fmt::FrameCheck::none),
              "the json says whether the image checks its frames");
        if(file) {
            auto const it = file->strings.find(site);
            CHECK(it != file->strings.end() && it->second == "Test {}\x1ei4",
                  "the json has the site's string");
        }
    }

    catalogFileShapes();

    return remote_fmt::test::report();
}
