#pragma once
#include "remote_fmt/fmt_wrapper.hpp"

#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wsign-conversion"
    #pragma clang diagnostic ignored "-Wcovered-switch-default"
    #pragma clang diagnostic ignored "-Wswitch-default"
    #pragma clang diagnostic ignored "-Wimplicit-int-conversion"
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-libc-call"
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

#include <nlohmann/json.hpp>

#ifdef __clang__
    #pragma clang diagnostic pop
#endif

#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>

namespace remote_fmt {

inline std::pair<std::unordered_map<std::uint16_t,
                                    std::string>,
                 std::string>
parseStringConstantsFromJsonFile(std::string const& file) {
    try {
        std::ifstream        fileStream(file);
        nlohmann::json const data = nlohmann::json::parse(fileStream);
        return {data["StringConstants"].get<std::unordered_map<std::uint16_t, std::string>>(), ""};
    } catch(std::exception const& e) {
        return {{}, fmt::format("read format strings failed: {}", e.what())};
    }
}
}   // namespace remote_fmt
