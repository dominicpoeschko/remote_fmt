// The one format-string scanner (format_grammar.hpp) against every string of length 0..9 over the
// alphabet { '{', '}', ':', '0', 'x' } (2 441 406 strings): what the target's compile-time check and
// the host's parser make of each one.
//
// The scanner replaced four functions that each held a piece of the grammar (the target's brace
// count and argument-id walk, the parser's own scan and its nested-brace check). Verbatim copies of
// them were compared with it string by string: every outcome agreed (field text of a refused nested
// field aside, which the model below does not record), and a mutation of the scanner (nested fields
// not marked) differed in 8650 strings. The copies are gone; the digest of the outcomes taken then
// pins them. A grammar change that is meant changes the digest: say in the commit what it changes.

#include "remote_fmt/format_grammar.hpp"
#include "remote_fmt/remote_fmt.hpp"
#include "remote_fmt/wire.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::array<char, 5> Alphabet{'{', '}', ':', '0', 'x'};
constexpr std::size_t         MaxLength = 9;

// taken while the old functions and the scanner agreed on every string
constexpr std::uint64_t ExpectedDigest  = 0xfbffd2f0c2193dffULL;
constexpr std::size_t   ExpectedStrings = 2441406;

// target: '-' (refused: "invalid replacement field count") or the field count, then 'p' (plain) or
// 'i' ("argument id or dynamic spec in replacement field"). parser: 'S' (refused silently by the
// pre-check), 'M' (malformed), 'D' (dynamic spec / number above the limit), or 'O', the text and the
// fields. The parser is modelled with an argument for every field.
std::string outcomeNew(std::string_view s) {
    using namespace remote_fmt::detail;
    std::string out;
    auto const  shape = formatShape(s);
    if(!shape.wellFormed) {
        out += "-S";
        return out;
    }
    out += static_cast<char>('0' + shape.fields);
    out += shape.plainFields ? 'p' : 'i';

    std::string              text;
    std::vector<std::string> fields;
    while(true) {
        auto const scan = nextField(s);
        if(scan.malformed()) { return out + 'M'; }
        appendLiteral(text, scan.literal);
        if(scan.field.empty()) { break; }
        if(scan.error == ScanError::nested || !replacementFieldWithinLimits(scan.field)) {
            return out + 'D';
        }
        fields.emplace_back(scan.field);
        s = scan.rest;
    }
    out += 'O';
    out += text;
    for(auto const& f : fields) {
        out += '|';
        out += f;
    }
    return out;
}

}   // namespace

int main() {
    std::uint64_t digest  = 0xcbf29ce484222325ULL;   // FNV-1a 64 over every outcome
    std::size_t   strings = 0;
    std::string   s;

    for(std::size_t length = 0; length <= MaxLength; ++length) {
        std::vector<std::size_t> digits(length, 0);
        while(true) {
            s.clear();
            for(auto const d : digits) { s += Alphabet[d]; }
            ++strings;

            auto const now = outcomeNew(s);
            for(char const c : now + '\n') {
                digest = (digest ^ static_cast<unsigned char>(c)) * 0x100000001b3ULL;
            }
            std::size_t i = 0;
            for(; i < length; ++i) {
                if(++digits[i] < Alphabet.size()) { break; }
                digits[i] = 0;
            }
            if(i == length) { break; }
        }
    }

    std::printf("%zu strings, digest 0x%016llx\n",
                strings,
                static_cast<unsigned long long>(digest));
    bool const ok = strings == ExpectedStrings && digest == ExpectedDigest;
    if(!ok) { std::puts("FAIL"); }
    return ok ? 0 : 1;
}
