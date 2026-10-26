#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

// The one grammar of a format string, shared by the target's compile-time check (check.hpp
// checkFormatString) and the host's parser (parser.hpp formatString). Two implementations of it used
// to disagree, and the parser aborted on what the check let through; tests/scanner_equivalence_tests
// pins this one against both of the old ones.
//
// - Outside a field "{{" and "}}" are escapes; a lone '}' is an error.
// - A '{' opens a field, which ends where its braces balance again: "{:{}}" is ONE field, marked
//   `nested` (a dynamic width or precision - the protocol pairs one argument with each field and
//   cannot send a second one, so both sides refuse it). A field that never balances is `unclosed`.
// - No escapes inside a field.
// constexpr and without allocation: the constant evaluator walks every log string with it, one
// element at a time, so it walks each string once.
namespace remote_fmt::detail {

enum class ScanError : std::uint8_t { none, loneClose, unclosed, nested };

struct Scan {
    std::string_view literal;   // the text before the field, escapes NOT collapsed (appendLiteral)
    std::string_view
      field;   // "{...}" with its braces; empty at the end of the string or on a lone '}' / unclosed
    std::string_view rest;   // what follows the field
    ScanError        error{ScanError::none};

    [[nodiscard]] constexpr bool malformed() const {
        return error == ScanError::loneClose || error == ScanError::unclosed;
    }
};

constexpr Scan nextField(std::string_view s) {
    std::size_t i = 0;
    while(i < s.size()) {
        char const c = s[i];
        if(c != '{' && c != '}') {
            ++i;
            continue;
        }
        if(i + 1 < s.size() && s[i + 1] == c) {
            i += 2;   // an escape pair
            continue;
        }
        if(c == '}') { return {s.substr(0, i), {}, {}, ScanError::loneClose}; }

        std::size_t depth  = 1;
        bool        nested = false;
        for(std::size_t j = i + 1; j < s.size(); ++j) {
            if(s[j] == '{') {
                ++depth;
                nested = true;
            } else if(s[j] == '}' && --depth == 0) {
                return {s.substr(0, i),
                        s.substr(i, j + 1 - i),
                        s.substr(j + 1),
                        nested ? ScanError::nested : ScanError::none};
            }
        }
        return {s.substr(0, i), {}, {}, ScanError::unclosed};
    }
    return {s, {}, {}, ScanError::none};
}

// Appends a literal from nextField with its escape pairs collapsed ("{{" -> "{"). Only well-formed
// literals arrive here, so every brace in it is the first of a pair.
template<typename Out>
constexpr void appendLiteral(Out&             out,
                             std::string_view literal) {
    for(std::size_t i = 0; i < literal.size(); ++i) {
        out.push_back(literal[i]);
        if(literal[i] == '{' || literal[i] == '}') { ++i; }
    }
}

struct FormatShape {
    bool        wellFormed{true};    // no lone '}', no unclosed field
    std::size_t fields{};            // replacement fields, nested ones included
    bool        plainFields{true};   // no argument id ("{0}", "{name}") and no nested field
};

// One walk over the whole string.
constexpr FormatShape formatShape(std::string_view s) {
    FormatShape shape{};
    while(true) {
        auto const scan = nextField(s);
        if(scan.malformed()) {
            shape.wellFormed = false;
            return shape;
        }
        if(scan.field.empty()) { return shape; }
        ++shape.fields;
        // only a spec or the end of the field may follow '{'; an id or a name would appear here
        if(scan.error == ScanError::nested || (scan.field[1] != ':' && scan.field[1] != '}')) {
            shape.plainFields = false;
        }
        s = scan.rest;
    }
}

}   // namespace remote_fmt::detail
