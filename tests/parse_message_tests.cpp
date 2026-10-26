// parseMessage's error callback: `(text)` or `(text, ParseError)`. A frame that is whole and checks out but whose text
// the host cannot render is ParseError::format and is consumed; bytes that make no frame are ParseError::stream. The
// kind goes by where the error came from, not by its words: the time value fmt cannot take says "time value out of
// range", which no "bad format" prefix matches.

#include "TestSupport.ipp"
#include "remote_fmt/parser.hpp"
#include "remote_fmt/wire.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using remote_fmt::ParseError;

constexpr std::byte Start = remote_fmt::protocol::startMarker;   // an unchecked frame
constexpr std::byte End   = remote_fmt::protocol::endMarker;

std::unordered_map<std::uint16_t,
                   std::string> const&
catalog() {
    static auto const& c = *new std::unordered_map<std::uint16_t, std::string>{
      {3,
       "t {:%H}\x1e"
       "d1/1f8"},
      {4,
       "v {:Z}\x1e"
       "u4"    },
      {5,
       "v {}\x1e"
       "u4"    },
    };
    return c;
}

// garbage, a time value out of range, a spec fmt refuses, a good frame
std::vector<std::byte> stream() {
    std::vector<std::byte> out{std::byte{0x01}, std::byte{0x02}};
    auto const             huge = std::bit_cast<std::array<std::byte, 8>>(1e300);
    out.insert(out.end(), {Start, std::byte{3}, std::byte{0}});
    out.insert(out.end(), huge.begin(), huge.end());
    out.push_back(End);
    out.insert(out.end(), {Start, std::byte{4}, std::byte{0}, std::byte{5}, End});
    out.insert(out.end(), {Start, std::byte{5}, std::byte{0}, std::byte{7}, End});
    return out;
}

void withKind() {
    auto const                                      bytes = stream();
    std::vector<std::pair<ParseError, std::string>> errors;
    auto const                                      parsed = remote_fmt::parseMessage(
      std::span<std::byte const>{bytes},
      catalog(),
      [&](std::string_view m, ParseError kind) { errors.emplace_back(kind, m); });
    CHECK(parsed.message == "v 7" && parsed.remaining.empty() && parsed.discarded == 2,
          "both refused frames consumed, the good one decoded, the garbage counted");
    CHECK(errors.size() == 2, "one error per refused frame (the garbage is the caller's to say)");
    CHECK(errors.size() == 2 && errors[0].first == ParseError::format
            && errors[0].second.starts_with("time value out of range")
            && errors[1].first == ParseError::format
            && errors[1].second.starts_with("bad format for replacement field"),
          "both are format errors, whatever their words");
}

void unknownIdIsStream() {
    std::vector<std::byte> const                    bytes{Start, std::byte{9}, std::byte{0}, End};
    std::vector<std::pair<ParseError, std::string>> errors;
    auto const                                      parsed = remote_fmt::parseMessage(
      std::span<std::byte const>{bytes},
      catalog(),
      [&](std::string_view m, ParseError kind) { errors.emplace_back(kind, m); });
    CHECK(!parsed.message && errors.size() == 1 && errors[0].first == ParseError::stream
            && errors[0].second.starts_with("cataloged format string not found 9"),
          "an id the catalog does not have is a stream error");
}

void textOnly() {
    auto const               bytes = stream();
    std::vector<std::string> errors;
    auto const               parsed
      = remote_fmt::parseMessage(std::span<std::byte const>{bytes},
                                 catalog(),
                                 [&](std::string_view m) { errors.emplace_back(m); });
    CHECK(parsed.message == "v 7" && errors.size() == 2,
          "a callback of the text alone gets the same errors");
}
}   // namespace

int main() {
    withKind();
    unknownIdIsStream();
    textOnly();
    return remote_fmt::test::report();
}
