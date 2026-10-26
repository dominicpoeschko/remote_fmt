// libFuzzer entry point for the descriptor half of the protocol: a descriptor (any bytes) and values (any bytes)
// through the parser's walker. Without type bytes the descriptor decides how every byte is read, and an inline one
// (REMOTE_FMT_USE_CATALOG=false) comes off the wire just like the values - so neither may crash, hang or allocate
// without bound, whatever the two say. Also drives the catalog reader's type resolution on the same text.
//
// Input: <described string> 0x00 <values>. The described string is a format string, 0x1e and descriptors, as the
// Printer writes one (corpus_generator.cpp writes seeds of this shape from real records); without a separator the
// whole text is taken as descriptors under "{}" fields.
//
// Oracle: resolving descriptors without a type reference copies them byte for byte, and succeeds exactly when the
// walker can read them (grammar::resolve and grammar::skip are one walk, descriptor_grammar.hpp).
#include "remote_fmt/catalog_helpers.hpp"
#include "remote_fmt/descriptor.hpp"
#include "remote_fmt/descriptor_grammar.hpp"
#include "remote_fmt/parser.hpp"
#include "remote_fmt/wire.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data,
                                      std::size_t         size);

namespace {
void appendVarint(std::vector<std::byte>& out,
                  std::uint64_t           value) {
    while(value >= 0x80U) {
        out.push_back(static_cast<std::byte>((value & 0x7FU) | 0x80U));
        value >>= 7U;
    }
    out.push_back(static_cast<std::byte>(value));
}

std::vector<std::byte> frame(bool                       checked,
                             std::uint16_t              id,
                             std::string_view           inlineText,
                             std::span<std::byte const> values) {
    std::vector<std::byte> out;
    out.push_back(checked ? remote_fmt::protocol::checkedStartMarker
                          : remote_fmt::protocol::startMarker);
    out.push_back(static_cast<std::byte>(id & 0xFFU));
    out.push_back(static_cast<std::byte>(id >> 8U));
    if(id == remote_fmt::protocol::inlineId) {
        appendVarint(out, inlineText.size());
        for(char const c : inlineText) { out.push_back(static_cast<std::byte>(c)); }
    }
    out.insert(out.end(), values.begin(), values.end());
    if(checked) {
        out.push_back(static_cast<std::byte>(remote_fmt::protocol::crcFinal(
          remote_fmt::protocol::crcOver(remote_fmt::protocol::crcInit, out))));
    }
    out.push_back(remote_fmt::protocol::endMarker);
    return out;
}

// resolve without type entries is the identity on any descriptor run without a '#', and fails where skip fails
void checkResolveCopies(std::string_view descriptors) {
    if(descriptors.contains('#')) { return; }
    std::string      out;
    bool const       resolved = remote_fmt::detail::grammar::resolve(descriptors, {}, out);
    std::string_view rest     = descriptors;
    bool             readable = true;
    while(readable && !rest.empty()) { readable = remote_fmt::detail::grammar::skip(rest); }
    if(resolved != readable || (resolved && out != descriptors)) { std::abort(); }
}

void drain(std::span<std::byte const>             buffer,
           std::unordered_map<std::uint16_t,
                              std::string> const& strings) {
    while(!buffer.empty()) {
        auto const parsed = remote_fmt::parseMessage(buffer, strings, [](std::string_view) {});
        if(parsed.remaining.size() == buffer.size()) { break; }
        buffer = parsed.remaining;
    }
}
}   // namespace

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data,
                                      std::size_t         size) {
#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-container"
#endif
    std::span<std::byte const> input{reinterpret_cast<std::byte const*>(data), size};
#ifdef __clang__
    #pragma clang diagnostic pop
#endif
    std::size_t split = 0;
    while(split != input.size() && input[split] != std::byte{0}) { ++split; }
    std::string text{reinterpret_cast<char const*>(input.data()), split};
    auto const  values
      = split == input.size() ? std::span<std::byte const>{} : input.subspan(split + 1);
    if(!text.contains(remote_fmt::desc::separator)) {
        // descriptors only: one plain field per descriptor the grammar can read (at most 16)
        std::string      fields;
        std::string_view d = text;
        for(int n = 0; n != 16 && !d.empty() && remote_fmt::detail::grammar::skip(d); ++n) {
            fields += "{}";
        }
        text = fields + remote_fmt::desc::separator + text;
    }
    checkResolveCopies(std::string_view{text}.substr(text.find(remote_fmt::desc::separator) + 1));

    // inline (the descriptor off the wire), checked and unchecked
    for(bool const checked : {true, false}) {
        auto const f = frame(checked, remote_fmt::protocol::inlineId, text, values);
        drain(f, {});
    }

    // cataloged: the same string behind a site id, with its type references resolved by the catalog reader against a
    // small table (and left alone where it has no entry)
    static auto const& types = *new std::unordered_map<std::string, std::string>{
      {"00000000000000aa",              "Eu12:1,3:one2,3:two"},
      {"00000000000000bb",                  "Gu12:1,1:a2,1:b"},
      {"00000000000000cc",  "F6:<{}|{}>2:#00000000000000aai2"},
      {"00000000000000dd", "Ru49:a={} b={}0:2:3,1:1,1:x12,0:"}
    };
    auto const resolved = remote_fmt::detail::resolved(text, types);
    std::unordered_map<std::uint16_t, std::string> strings{
      {0x8001,        resolved ? *resolved : text},
      // a `*` value's sub format string
      {0x0001, std::string{"<{} {}>\x1e"} + "u4s"},
      {0x0002,         std::string{"no args\x1e"}}
    };
    auto const f = frame(true, 0x8001, {}, values);
    drain(f, strings);
    return 0;
}
