#pragma once
#include "remote_fmt/fmt_wrapper.hpp"

#include <cstdint>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>

namespace remote_fmt {

inline std::pair<std::map<std::uint16_t, std::string>, std::string>
parseStringConstantsFromJsonFile(std::string const& file) {
    try {
        std::ifstream        f(file);
        nlohmann::json const data = nlohmann::json::parse(f);

        return {data["StringConstants"].get<std::map<std::uint16_t, std::string>>(), ""};
    } catch(std::exception const& e) {
        return {{}, fmt::format("read format strings failed: {}", e.what())};
    }
}
}   // namespace remote_fmt
