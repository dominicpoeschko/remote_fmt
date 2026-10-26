// Writes seed inputs for fuzz_parser (one serialized message per file, produced by the real Printer so the fuzzer
// starts from structurally valid protocol data) and, with a second directory, for fuzz_descriptor (the same records as
// <described string> 0x00 <values>).
#include "TestSupport.ipp"
#include "remote_fmt/remote_fmt.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

using namespace sc::literals;
using namespace std::literals;

using remote_fmt::test::VectorBackend;

int main(int    argc,
         char** argv) {
    if(argc != 2 && argc != 3) { return 1; }
#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif
    std::filesystem::path const outDir{argv[1]};
    std::filesystem::path const descriptorDir{argc == 3 ? argv[2] : ""};
#ifdef __clang__
    #pragma clang diagnostic pop
#endif
    std::filesystem::create_directories(outDir);
    if(!descriptorDir.empty()) { std::filesystem::create_directories(descriptorDir); }

    std::size_t index{};
    auto        dump = [&](auto fmtString, auto&&... args) {
        remote_fmt::Printer<VectorBackend> printer{};
        printer.print(fmtString, args...);
        auto const&   memory = printer.getComBackend().memory;
        std::ofstream file{outDir / ("seed_" + std::to_string(index)), std::ios::binary};
        file.write(reinterpret_cast<char const*>(memory.data()),
                   static_cast<std::streamsize>(memory.size()));
        if(!descriptorDir.empty()) {
            // an inline frame: start, 0xFFFF, the string's LEB128 length, the string, the values, [CRC], end
            std::size_t  at     = 3;
            std::size_t  length = 0;
            unsigned     shift  = 0;
            std::uint8_t b      = 0x80;
            while((b & 0x80U) != 0 && at < memory.size()) {
                b = std::to_integer<std::uint8_t>(memory[at++]);
                length |= static_cast<std::size_t>(b & 0x7FU) << shift;
                shift += 7;
            }
            std::size_t const tail = REMOTE_FMT_FRAME_CHECK ? 2 : 1;
            if(at + length + tail <= memory.size()) {
                std::vector<std::byte> seed{memory.begin() + static_cast<std::ptrdiff_t>(at),
                                            memory.begin()
                                              + static_cast<std::ptrdiff_t>(at + length)};
                seed.push_back(std::byte{0});
                seed.insert(seed.end(),
                            memory.begin() + static_cast<std::ptrdiff_t>(at + length),
                            memory.end() - static_cast<std::ptrdiff_t>(tail));
                std::ofstream d{descriptorDir / ("seed_" + std::to_string(index)),
                                std::ios::binary};
                d.write(reinterpret_cast<char const*>(seed.data()),
                        static_cast<std::streamsize>(seed.size()));
            }
        }
        ++index;
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
    dump("{}"_sc, std::expected<void, int>{});
    // the variant's index byte, which no other seed has
    dump("{}"_sc, std::variant<int, std::string_view>{7});
    dump("{}"_sc, std::variant<int, std::string_view>{"v"sv});
    dump("{}"_sc, std::chrono::milliseconds{123});
    dump("{}"_sc, std::chrono::duration<double>{1.5});
    dump("{:>10}"_sc, 7);
    dump("{}"_sc, fmt::styled(1, fmt::fg(fmt::color::red) | fmt::emphasis::bold));
    dump("{} {}"_sc, std::set<std::uint16_t>{1, 300}, std::array<std::uint8_t, 3>{1, 2, 3});
    dump("{}"_sc, std::tuple<std::uint8_t, std::int16_t, std::int64_t>{7, -7, -1'000'000'000'000});
    dump("{}"_sc, "const"_sc);
    return 0;
}
