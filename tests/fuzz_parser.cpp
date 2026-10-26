// libFuzzer entry point for remote_fmt::parseMessage. The parser consumes untrusted bytes from
// the wire, so it must never crash, hang or overflow no matter the input. The catalog
// map holds described strings of every kind (format string, 0x1e, descriptors), so the
// cataloged paths, the `*` sub format strings and the inline ones (id 0xFFFF) are reachable.
#include "remote_fmt/parser.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data,
                                      std::size_t         size);

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data,
                                      std::size_t         size) {
    // Leaked on purpose: no exit-time destructor, and the static reference keeps the
    // allocation reachable so LeakSanitizer stays quiet.
    static auto const& stringConstantsMap = *new std::unordered_map<std::uint16_t, std::string>{
      {     0,                                   std::string{"Test {}\x1e"} + "i4"              },
      {     1,                                                       std::string{"{}\x1e"} + "s"},
      {     2,                                           std::string{"{:>10}\x1e"} + "d1/1000i8"},
      {     3,                                    std::string{"a {} b {} c {}\x1e"} + "u2Lu1Ou4"},
      {     4,                                                          std::string{"plain\x1e"}},
      {     5,                                               std::string{"{:%Q}\x1e"} + "d1/1i4"},
      {     6,                                             std::string{"{:*^7}\x1e"} + "V2:u4f4"},
      {     7,                                          std::string{"{:04x}\x1e"} + "Ei41:0,1:a"},
      {     8,                                             std::string{"{} {}\x1e"} + "*Q2: Vf4"},
      {     9,                                                std::string{"<{}>\x1e"} + "(u1i2)"},
      {0x8001,
       std::string{"(\"f\", 1, 2, {}[1/1000000]s, \"\"\"g\"\"\")x {}\x1e"} + "i8Gu12:1,1:a2,1:b"},
      {0x8002,                      std::string{"r {}\x1e"} + "Ru49:a={} b={}0:2:3,1:1,1:x12,0:"}
    };

#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-container"
#endif
    // libFuzzer hands over a raw pointer and size - the two-parameter span construction
    // is the only way to adopt it.
    std::span<std::byte const> buffer{reinterpret_cast<std::byte const*>(data), size};
#ifdef __clang__
    #pragma clang diagnostic pop
#endif

    while(!buffer.empty()) {
        auto const [message, remaining, discarded, catalogId]
          = remote_fmt::parseMessage(buffer, stringConstantsMap, [](std::string_view) {});
        static_cast<void>(message);
        static_cast<void>(discarded);
        if(remaining.size() == buffer.size()) { break; }
        buffer = remaining;
    }
    return 0;
}
