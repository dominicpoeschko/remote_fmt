#pragma once
// The one reader of an image's string catalog, `<target>_string_constants.json` as
// target_generate_string_constants writes it, once and whole, after the link (tools/extract_sites.py): its format
// strings ("StringConstants"), its call sites ("Sites"), whether the image checks its frames ("FrameCheck") and its
// type entries ("Types"). Read once, with glaze. A string is an entry `{"id": <id>, "fmt": <format string>, "args":
// [<descriptor>, ...]}` (one descriptor per argument, tools/descriptor.py); the reader joins it back into `fmt 0x1e
// descriptors` and resolves the type references (`#<hex>`), so the parser sees one whole described string. Every key
// is required, and a type reference without its entry is an error: no reader sees half a catalog.
#include "remote_fmt/catalog.hpp"
#include "remote_fmt/descriptor.hpp"
#include "remote_fmt/descriptor_grammar.hpp"
#include "remote_fmt/detail/Lifetimebound.hpp"
#include "remote_fmt/wire.hpp"

#include <expected>
#include <filesystem>
#include <fstream>
#include <glaze/glaze.hpp>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace remote_fmt {

struct CatalogFile {
    // "StringConstants": id -> described string (`fmt 0x1e descriptors`, type references resolved)
    std::unordered_map<catalog_id, std::string> strings;
    std::map<std::string, std::string> sites;   // "Sites": call-site id (decimal) -> signature
    // "FrameCheck": true -> required (the image checks its frames), false -> none (an image without the check)
    FrameCheck frameCheck{FrameCheck::none};
    // "Types": key (16 hex digits) -> descriptor text
    std::unordered_map<std::string, std::string> types;
};

namespace detail {
    // one "StringConstants" entry
    struct CatalogEntry {
        catalog_id               id{};
        std::string              fmt;
        std::vector<std::string> args;   // one descriptor per argument
    };

    struct CatalogJson {
        std::vector<CatalogEntry>          stringConstants;
        std::map<std::string, std::string> sites;
        bool                               frameCheck{};
        std::map<std::string, std::string> types;
    };

    // a described string with its descriptors' type references replaced by the entries' texts; nullopt when one
    // has no entry
    inline std::optional<std::string> resolved(std::string const&                     text,
                                               std::unordered_map<std::string,
                                                                  std::string> const& types) {
        auto const at = text.find(desc::separator);
        if(at == std::string::npos || text.find('#', at) == std::string::npos) { return text; }
        std::string out = text.substr(0, at + 1);
        if(!grammar::resolve(std::string_view{text}.substr(at + 1), types, out)) {
            return std::nullopt;
        }
        return out;
    }

    // what the parser reads: `fmt 0x1e descriptors`
    inline std::string joined(CatalogEntry const& entry) {
        std::string text = entry.fmt;
        text += desc::separator;
        for(auto const& arg : entry.args) { text += arg; }
        return text;
    }
}   // namespace detail
}   // namespace remote_fmt

// The catalog file's keys keep their spelling (generate_string_constants.py, extract_sites.py and every catalog read
// them): the members follow the code's names, glaze gets the keys through rename_key.
template<>
struct glz::meta<remote_fmt::detail::CatalogJson> {
    static constexpr std::string_view rename_key(std::string_view member REMOTE_FMT_LIFETIMEBOUND) {
        if(member == "stringConstants") { return "StringConstants"; }
        if(member == "sites") { return "Sites"; }
        if(member == "frameCheck") { return "FrameCheck"; }
        if(member == "types") { return "Types"; }
        return member;
    }
};

namespace remote_fmt {

inline std::expected<CatalogFile,
                     std::string>
parseCatalogFile(std::string text) {
    detail::CatalogJson data{};
    if(auto const ec
       = glz::read<glz::opts{.error_on_unknown_keys = false, .error_on_missing_keys = true}>(data,
                                                                                             text);
       ec)
    {
        return std::unexpected(glz::format_error(ec, text));
    }
    CatalogFile file{};
    for(auto& [key, entry] : data.types) { file.types.emplace(key, std::move(entry)); }
    for(auto const& entry : data.stringConstants) {
        auto whole = detail::resolved(detail::joined(entry), file.types);
        if(!whole) {
            return std::unexpected("string " + std::to_string(unsigned{entry.id})
                                   + " refers to a type entry that \"Types\" does not hold");
        }
        file.strings.emplace(entry.id, std::move(*whole));   // a repeated id: the first one
    }
    file.sites      = std::move(data.sites);
    file.frameCheck = data.frameCheck ? FrameCheck::required : FrameCheck::none;
    return file;
}

// The error names neither the file nor what was being read: the caller says both.
inline std::expected<CatalogFile,
                     std::string>
readCatalogFile(std::filesystem::path const& path) {
    std::ifstream in{path, std::ios::binary};
    if(!in) { return std::unexpected(std::string{"cannot open it"}); }
    return parseCatalogFile(std::string{std::istreambuf_iterator<char>{in}, {}});
}
}   // namespace remote_fmt
