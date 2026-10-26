// A record in pieces without the catalog (beginInline, formatArguments, endInline - uc_log's RecordFrame on a host):
// byte for byte what print() writes for the same string and values, and the parser reads it back. Pieces formatted
// one after the other make the same frame as one formatArguments call.
#include "TestSupport.ipp"
#include "remote_fmt/parser.hpp"
#include "remote_fmt/remote_fmt.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

using namespace sc::literals;

namespace {

using remote_fmt::test::emptyCatalog;
using remote_fmt::test::VectorBackend;

std::string parsed(std::vector<std::byte> const& bytes) {
    auto const [message, remaining, discarded, catalogId]
      = remote_fmt::parseMessage(std::span{bytes}, emptyCatalog(), [](std::string_view error) {
            std::printf("parser error: %.*s\n", static_cast<int>(error.size()), error.data());
        });
    CHECK(remaining.empty(), "the frame is consumed whole");
    CHECK(discarded == 0, "nothing discarded");
    return message.value_or("<none>");
}

void piecesAreAPrint() {
    constexpr auto      fmt   = "t {} v {} s {}"_sc;
    std::uint32_t const time  = 7;
    int const           value = -3;
    std::string const   text  = "x";

    remote_fmt::Printer<VectorBackend> whole{};
    whole.print(fmt, time, value, text);

    remote_fmt::Printer<VectorBackend> pieces{};
    pieces.beginInline(
      remote_fmt::described<std::uint32_t const&, int const&, std::string const&>(fmt));
    pieces.formatArguments(time);
    pieces.formatArguments(value, text);
    pieces.endInline();

    CHECK(pieces.getComBackend().memory == whole.getComBackend().memory,
          "pieces write the bytes print writes");
    CHECK(parsed(pieces.getComBackend().memory) == "t 7 v -3 s x", "pieces parse back");
}

void noArguments() {
    constexpr auto                     fmt = "plain"_sc;
    remote_fmt::Printer<VectorBackend> pieces{};
    pieces.beginInline(remote_fmt::described<>(fmt));
    pieces.endInline();
    CHECK(parsed(pieces.getComBackend().memory) == "plain", "no arguments");
}

}   // namespace

int main() {
    static_assert(!remote_fmt::useCatalog, "this test is the record without the catalog");
    piecesAreAPrint();
    noArguments();
    return remote_fmt::test::report();
}
