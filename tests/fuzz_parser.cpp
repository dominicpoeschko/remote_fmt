// libFuzzer entry point for remote_fmt::parse. The parser consumes untrusted bytes from
// the wire, so it must never crash, hang or overflow no matter the input. The catalog
// map is populated so cataloged format string and cataloged string paths are reachable.
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
      {0,        "Test {}"},
      {1,             "{}"},
      {2,         "{:>10}"},
      {3, "a {} b {} c {}"},
      {4,          "plain"},
      {5,          "{:%Q}"},
      {6,         "{:*^7}"},
      {7,         "{:04x}"}
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
        auto const [message, remaining, discarded]
          = remote_fmt::parse(buffer, stringConstantsMap, [](std::string_view) {});
        static_cast<void>(message);
        static_cast<void>(discarded);
        if(remaining.size() == buffer.size()) { break; }
        buffer = remaining;
    }
    return 0;
}
