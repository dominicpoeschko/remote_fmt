#pragma once

// Reading a descriptor (descriptor.hpp's grammar) on the host: one descriptor skipped, whether a value of it can take
// no bytes, the type references of a catalog resolved. The standard library only: the parser renders with it and the
// catalog reader (catalog_helpers.hpp) resolves with it, which reaches no fmt.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace remote_fmt { namespace detail {

    // Reading a descriptor (descriptor.hpp's grammar). Every function advances `d` past what it read and returns
    // nullopt / false on a descriptor it cannot read: a catalog is the build's own, but an inline one came off the
    // wire.
    namespace grammar {
        inline std::optional<std::uint64_t> number(std::string_view& d) {
            std::uint64_t v      = 0;
            std::size_t   digits = 0;
            while(!d.empty() && d.front() >= '0' && d.front() <= '9') {
                auto const digit = static_cast<std::uint64_t>(d.front() - '0');
                if(v > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
                    return std::nullopt;
                }
                v = (v * 10U) + digit;
                d.remove_prefix(1);
                ++digits;
            }
            if(digits == 0) { return std::nullopt; }
            return v;
        }

        // An enumerator's value as EnumText writes it: an unsigned one in full (up to 2^64 - 1), a signed one with
        // '-' when negative (down to -2^63). The 64 bits of the value, a negative one sign-extended - what
        // integerBits makes of a value on the wire, so the two compare as they are.
        inline std::optional<std::uint64_t> enumeratorValue(std::string_view& d) {
            bool const negative = !d.empty() && d.front() == '-';
            if(negative) { d.remove_prefix(1); }
            auto const magnitude = number(d);
            if(!magnitude) { return std::nullopt; }
            if(!negative) { return magnitude; }
            if(*magnitude > (std::uint64_t{1} << 63U)) { return std::nullopt; }
            // two's complement without a wrapping add (the integer sanitizer reports one): -m is ~(m - 1); -0 is 0
            return *magnitude == 0U ? std::uint64_t{0} : ~(*magnitude - 1U);
        }

        inline bool literal(std::string_view& d,
                            char              c) {
            if(d.empty() || d.front() != c) { return false; }
            d.remove_prefix(1);
            return true;
        }

        // <n>:<text>
        inline std::optional<std::string_view> counted(std::string_view& d) {
            auto const n = number(d);
            if(!n || !literal(d, ':') || *n > d.size()) { return std::nullopt; }
            auto const text = d.substr(0, static_cast<std::size_t>(*n));
            d.remove_prefix(static_cast<std::size_t>(*n));
            return text;
        }

        // the size digit of u/i/f/p
        inline std::optional<std::size_t> size(std::string_view& d) {
            if(d.empty()) { return std::nullopt; }
            char const c = d.front();
            d.remove_prefix(1);
            if(c == '1' || c == '2' || c == '4' || c == '8') {
                return static_cast<std::size_t>(c - '0');
            }
            return std::nullopt;
        }

        inline constexpr std::size_t maxDepth = 64;

        // `<k>:` then k entries `<value>,<n>:<name>`
        inline bool skipNames(std::string_view& d) {
            auto const k = number(d);
            if(!k || !literal(d, ':')) { return false; }
            for(std::uint64_t i = 0; i != *k; ++i) {
                if(!enumeratorValue(d) || !literal(d, ',') || !counted(d)) { return false; }
            }
            return true;
        }

        // The one structural walk of a descriptor: its tag and its own text are read off `d`, and every descriptor
        // nested in it is handed to `onChild(d, depth + 1)`, which consumes exactly that one from `d` (skips it,
        // copies it, ...). False if the descriptor is malformed, nests deeper than maxDepth or a child says so.
        // skip, take, canBeEmpty and resolve are this walk with different children; tools/descriptor.py mirrors it.
        template<typename OnChild>
        bool walk(std::string_view& d,
                  std::size_t       depth,
                  OnChild&&         onChild) {
            if(d.empty() || depth > maxDepth) { return false; }
            auto const child    = [&] { return onChild(d, depth + 1); };
            auto const children = [&](std::optional<std::uint64_t> const n) {
                if(!n || !literal(d, ':')) { return false; }
                for(std::uint64_t i = 0; i != *n; ++i) {
                    if(!child()) { return false; }
                }
                return true;
            };
            char const tag = d.front();
            d.remove_prefix(1);
            switch(tag) {
            case 'b':
            case 'c':
            case 's':
            case 'v':
            case '*': return true;
            case 'u':
            case 'i':
            case 'f':
            case 'p': return size(d).has_value();
            case 'k': return counted(d).has_value();
            case 'd': return number(d) && literal(d, '/') && number(d) && child();
            case 'O':
            case 'L':
            case 'S':
            case 'M':
            case 'A':
            case 'Y': return child();
            case 'X':
                {   // a key, then its value: two children in this order
                    bool const key = child();
                    return key && child();
                }
            case 'V': return children(number(d));
            case '(':
                while(!literal(d, ')')) {
                    if(!child()) { return false; }
                }
                return true;
            case 'Q': return counted(d) && child();
            case 'F': return counted(d) && children(number(d));
            case 'E':
            case 'G': return child() && skipNames(d);
            case 'R':
                {
                    if(!child() || !counted(d)) { return false; }
                    auto const dims = number(d);
                    if(!dims || !literal(d, ':')) { return false; }
                    for(std::uint64_t i = 0; i != *dims; ++i) {
                        if(!number(d) || !literal(d, ',')) { return false; }
                    }
                    auto const fields = number(d);
                    if(!fields || !literal(d, ':')) { return false; }
                    for(std::uint64_t i = 0; i != *fields; ++i) {
                        if(!number(d) || !literal(d, ',') || !skipNames(d)) { return false; }
                    }
                    return true;
                }
            case 'r':
                {
                    if(!child()) { return false; }
                    auto const fields = number(d);
                    if(!fields || !literal(d, ':')) { return false; }
                    for(std::uint64_t i = 0; i != *fields; ++i) {
                        if(!number(d) || !literal(d, ',') || !counted(d)) { return false; }
                    }
                    return true;
                }
            case '#':
                if(d.size() < 16) { return false; }
                d.remove_prefix(16);
                return true;
            default: return false;
            }
        }

        // Skips one descriptor; false if it is malformed or nests deeper than maxDepth.
        inline bool skip(std::string_view& d,
                         std::size_t       depth = 0) {
            return walk(d, depth, [](std::string_view& child, std::size_t childDepth) {
                return skip(child, childDepth);
            });
        }

        // One complete descriptor off the front of `d`: its text, or nullopt if it is malformed (d then stands
        // anywhere inside it).
        inline std::optional<std::string_view> take(std::string_view& d,
                                                    std::size_t       depth = 0) {
            std::string_view const before = d;
            if(!skip(d, depth)) { return std::nullopt; }
            return before.substr(0, before.size() - d.size());
        }

        // Whether a value of this descriptor can take no bytes at all (string constants, void, a tuple of them): a
        // range of them is then bounded by a count, not by the bytes that are left.
        inline bool canBeEmpty(std::string_view d,
                               std::size_t      depth = 0) {
            if(d.empty() || depth > maxDepth) { return true; }
            char const tag = d.front();
            d.remove_prefix(1);
            auto const allEmpty = [&](std::uint64_t count, bool untilParen) {
                for(std::uint64_t i = 0; untilParen || i != count; ++i) {
                    if(untilParen && literal(d, ')')) { return true; }
                    auto const one = take(d, depth + 1);
                    if(!one) { return true; }
                    if(!canBeEmpty(*one, depth + 1)) { return false; }
                }
                return true;
            };
            switch(tag) {
            case 'k':
            case 'v':
            case '#': return true;
            case '(': return allEmpty(0, true);
            case 'F':
                {
                    if(!counted(d)) { return true; }
                    auto const k = number(d);
                    if(!k || !literal(d, ':')) { return true; }
                    return allEmpty(*k, false);
                }
            default: return false;
            }
        }

        // Replaces every `#<hex>` type reference in a descriptor list by the entry's text (recursively), as the
        // catalog reader does once per string. False: a reference with no entry, or a malformed descriptor.
        inline bool resolve(std::string_view                       d,
                            std::unordered_map<std::string,
                                               std::string> const& types,
                            std::string&                           out,
                            std::size_t                            depth = 0);

        // One descriptor copied to `out` as walk reads it, a `#` type reference replaced by its entry (resolved in
        // turn).
        inline bool resolveOne(std::string_view&                      d,
                               std::unordered_map<std::string,
                                                  std::string> const& types,
                               std::string&                           out,
                               std::size_t                            depth) {
            if(depth > maxDepth) { return false; }
            if(!d.empty() && d.front() == '#') {
                if(d.size() < 17) { return false; }
                auto const it = types.find(std::string{d.substr(1, 16)});
                if(it == types.end()) { return false; }
                d.remove_prefix(17);
                std::string_view entry{it->second};
                return resolveOne(entry, types, out, depth + 1) && entry.empty();
            }
            // the descriptor's own text as it is, each child resolved in its place
            std::string_view const whole  = d;
            std::size_t            copied = 0;
            auto const             copyTo = [&](std::string_view const rest) {
                auto const at = whole.size() - rest.size();
                out.append(whole.substr(copied, at - copied));
                copied = at;
            };
            auto const resolvedChild = [&](std::string_view& child, std::size_t const childDepth) {
                copyTo(child);
                if(!resolveOne(child, types, out, childDepth)) { return false; }
                copied = whole.size() - child.size();
                return true;
            };
            if(!walk(d, depth, resolvedChild)) { return false; }
            copyTo(d);
            return true;
        }

        inline bool resolve(std::string_view                       d,
                            std::unordered_map<std::string,
                                               std::string> const& types,
                            std::string&                           out,
                            std::size_t                            depth) {
            while(!d.empty()) {
                if(!resolveOne(d, types, out, depth)) { return false; }
            }
            return true;
        }
    }   // namespace grammar
}}   // namespace remote_fmt::detail
