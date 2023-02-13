#pragma once
#include <cstdint>
#include <fmt/format.hpp>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
namespace remote_fmt {

inline std::map<std::uint16_t, std::string> parseStringConstantFromJsonsMapFromJsonFile(std::string const& file) {
    try {
        std::ifstream        f(file);
        nlohmann::json const data = nlohmann::json::parse(f);

        return data["StringConstants"].get<std::map<std::uint16_t, std::string>>();
    } catch(std::exception const& e) {
        fmt::print(stderr, "read format strings failed: {}", e.what());
        return {};
    }
}
}   // namespace remote_fmt
