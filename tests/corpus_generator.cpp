// Writes seed inputs for fuzz_parser: one serialized message per file, produced by the
// real Printer so the fuzzer starts from structurally valid protocol data.
#include "remote_fmt/remote_fmt.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
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
}   // namespace

int main(int    argc,
         char** argv) {
    if(argc != 2) { return 1; }
#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif
    std::filesystem::path const outDir{argv[1]};
#ifdef __clang__
    #pragma clang diagnostic pop
#endif
    std::filesystem::create_directories(outDir);

    std::size_t index{};
    auto        dump = [&](auto fmtString, auto&&... args) {
        remote_fmt::Printer<VectorBackend> printer{};
        printer.print(fmtString, args...);
        auto const&   memory = printer.get_com_backend().memory;
        std::ofstream file{outDir / ("seed_" + std::to_string(index++)), std::ios::binary};
        file.write(reinterpret_cast<char const*>(memory.data()),
                   static_cast<std::streamsize>(memory.size()));
    };

    dump("plain"_sc);
    dump("Test {}"_sc, 123);
    dump("{} {} {}"_sc, -1, 2.5, true);
    dump("{}"_sc, "a string"sv);
    dump("{}"_sc, std::vector<int>{1, 2, 3});
    dump("{}"_sc,
         std::map<int, int>{
           {1, 2},
           {3, 4}
    });
    dump("{}"_sc, std::tuple{1, 'c', "str"sv});
    dump("{}"_sc, std::optional<int>{42});
    dump("{}"_sc, std::optional<int>{});
    dump("{}"_sc, std::expected<int, int>{std::unexpected{2}});
    dump("{}"_sc, std::chrono::milliseconds{123});
    dump("{}"_sc, std::chrono::duration<double>{1.5});
    dump("{:>10}"_sc, 7);
    dump("{}"_sc, fmt::styled(1, fmt::fg(fmt::color::red) | fmt::emphasis::bold));
    return 0;
}
