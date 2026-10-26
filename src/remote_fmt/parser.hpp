#pragma once

// The host half of remote_fmt: frames back to text. A frame carries an id and values; the string
// behind the id (the catalog's, or an inline one) is the format string, 0x1e and the descriptors of the values
// (descriptor.hpp). The parser walks descriptor and bytes together - once to find the frame's exact end and check it,
// then once to render it with fmt - and renders each kind as fmt would (or as the protocol defines it: optional,
// expected, variant, enums, registers, quantities).

#include "remote_fmt/catalog.hpp"
#include "remote_fmt/descriptor.hpp"
#include "remote_fmt/descriptor_grammar.hpp"
#include "remote_fmt/detail/Lifetimebound.hpp"
#include "remote_fmt/detail/enchantum.hpp"
#include "remote_fmt/fmt_wrapper.hpp"
#include "remote_fmt/format_grammar.hpp"
#include "remote_fmt/wire.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace remote_fmt {

namespace detail {

    struct FillAlignWidth {
        std::string padField;     ///< "{:*>10}" -- what pads the finished text, "{}" for nothing
        std::string valueField;   ///< the caller's field with fill, align and width taken out
    };

    /// Split a replacement field into the part that positions the finished text and the part that
    /// renders the value. A quantity is one argument made of two pieces, so "{:>10}" has to pad
    /// "1.5 V" as a whole while ".2f" still reaches the number alone.
    ///
    /// The zero flag is the exception and is left where it is: "{:08.2f}" asks for sign-aware zero
    /// padding, which is a property of the number ("00001.50 V"), and zero-padding a string that
    /// ends in a unit symbol would be nonsense. Dynamic widths ("{:{}}") never get here - they are
    /// rejected on the target at compile time and again by the parser's scan (ScanError::nested).
    inline FillAlignWidth splitFillAlignWidth(std::string_view replacementField) {
        FillAlignWidth result{"{}", std::string{replacementField}};

        if(!replacementField.starts_with("{:") || !replacementField.ends_with('}')) {
            return result;
        }
        std::string_view spec = replacementField.substr(2, replacementField.size() - 3);

        auto const isAlign = [](char c) { return c == '<' || c == '>' || c == '^'; };

        // [[fill]align]: a leading character is a fill only if an align follows it, which is also
        // what keeps "{:0>10}" from being read as the zero flag.
        std::string_view fillAlign;
        if(spec.size() >= 2 && isAlign(spec[1])) {
            fillAlign = spec.substr(0, 2);
            spec      = spec.substr(2);
        } else if(!spec.empty() && isAlign(spec[0])) {
            fillAlign = spec.substr(0, 1);
            spec      = spec.substr(1);
        }

        // [sign][#][0][width]: everything from here on stays with the number except the width.
        std::size_t i = 0;
        if(i < spec.size() && (spec[i] == '+' || spec[i] == '-' || spec[i] == ' ')) { ++i; }
        if(i < spec.size() && spec[i] == '#') { ++i; }
        if(i < spec.size() && spec[i] == '0') {
            // Zero padding: the width belongs to the number too. Nothing to split off.
            return result;
        }

        std::size_t const widthStart = i;
        while(i < spec.size() && spec[i] >= '0' && spec[i] <= '9') { ++i; }
        std::string_view const width = spec.substr(widthStart, i - widthStart);

        if(fillAlign.empty() && width.empty()) { return result; }

        // A quantity takes the spec its representation would have taken, and a number aligns
        // right, so a bare width right-aligns rather than defaulting to a string's left.
        result.padField = "{:";
        result.padField += fillAlign.empty() ? std::string_view{">"} : fillAlign;
        result.padField += width;
        result.padField += '}';

        result.valueField = "{:";
        result.valueField += spec.substr(0, widthStart);
        result.valueField += spec.substr(i);
        result.valueField += '}';

        return result;
    }

    // The bytes of one frame, read from the front.
    struct Input {
        std::span<std::byte const> bytes;
        std::size_t                pos{};

        [[nodiscard]] std::size_t left() const { return bytes.size() - pos; }

        std::optional<std::uint8_t> byte() {
            if(left() < 1) { return std::nullopt; }
            return std::to_integer<std::uint8_t>(bytes[pos++]);
        }

        // n bytes, little endian
        std::optional<std::uint64_t> raw(std::size_t n) {
            if(left() < n) { return std::nullopt; }
            std::uint64_t v = 0;
            for(std::size_t i = 0; i != n; ++i) {
                v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[pos + i]))
                  << (8U * i);
            }
            pos += n;
            return v;
        }

        // LEB128, at most 10 bytes; one that does not fit 64 bits (or runs past the data) is nullopt
        std::optional<std::uint64_t> varint() {
            std::uint64_t v = 0;
            for(unsigned shift = 0; shift < 70; shift += 7) {
                auto const b = byte();
                if(!b) { return std::nullopt; }
                auto const part = static_cast<std::uint64_t>(*b & 0x7FU);
                if(shift == 63 && part > 1) { return std::nullopt; }
                v |= part << shift;
                if((*b & 0x80U) == 0) { return v; }
            }
            return std::nullopt;
        }
    };

    struct Parser {
        using Strings = std::unordered_map<std::uint16_t, std::string>;

        std::function<void(std::string_view)> errorMessagef;
        Strings const*                        strings{};

        // Why the last walk stopped: out of bytes (the frame may still be arriving), or bytes or a descriptor that
        // make no frame, or a frame whose text fmt refused (reported through errorMessagef).
        enum class Stop { none, shortData, malformed, render };
        Stop stop{Stop::none};
        // false: only walk (validate the structure, find the end); true: render too
        bool render{};

        // Nesting is whatever the descriptor says, and an inline descriptor came off the wire: a list of a list of a
        // list ... costs a frame of value() on the stack per level. Nothing legitimate nests past a handful of levels.
        static constexpr std::size_t maxNestingDepth = 64;
        // the elements of a range whose element can take no bytes (string constants): bounded by this, not the data
        static constexpr std::uint64_t maxEmptyElements = 4096;

        std::size_t nestingDepth{};

        struct NestingGuard {
            std::size_t& depth;

            explicit NestingGuard(std::size_t& depth_ REMOTE_FMT_LIFETIMEBOUND) : depth{depth_} {
                ++depth;
            }

            NestingGuard(NestingGuard const&) = delete;

            ~NestingGuard() { --depth; }
        };

        template<typename ErrorMessageF>
            requires(!std::is_same_v<std::decay_t<ErrorMessageF>,
                                     Parser>)
        explicit Parser(ErrorMessageF&&         errorMessagef_,
                        Strings const* strings_ REMOTE_FMT_LIFETIMEBOUND = nullptr)
          : errorMessagef{std::forward<ErrorMessageF>(errorMessagef_)}
          , strings{strings_} {}

        using Text = std::optional<std::string>;

        Text shortData() {
            if(stop == Stop::none) { stop = Stop::shortData; }
            return std::nullopt;
        }

        Text malformed() {
            if(stop == Stop::none) { stop = Stop::malformed; }
            return std::nullopt;
        }

        Text renderError(std::string const& message) {
            errorMessagef(message);
            if(stop == Stop::none) { stop = Stop::render; }
            return std::nullopt;
        }

        // fmt::format with a field off the wire or the catalog; `what` describes the value for the message
        template<typename T,
                 typename What>
        Text formatted(std::string_view field,
                       T const&         value,
                       What&&           what) {
            if(!render) { return std::string{}; }
            try {
                return fmt::format(fmt::runtime(field), value);
            } catch(std::exception const& e) {
                return renderError(fmt::format("bad format for replacement field {:?}: {} ({})",
                                               field,
                                               e.what(),
                                               what()));
            }
        }

        // a string as fmt would print it here: its debug form inside a range or tuple when the field is plain
        Text stringValue(std::string_view field,
                         std::string_view value,
                         bool             inList,
                         std::string_view kind) {
            if(!render) { return std::string{}; }
            if(inList && field == defaultReplacementField) { return fmt::format("{:?}", value); }
            return formatted(field, value, [&] {
                return fmt::format("{}: \"{}\", length: {}, in_list: {}",
                                   kind,
                                   value,
                                   value.size(),
                                   inList);
            });
        }

        // ---- scalars

        Text integer(std::string_view field,
                     bool             isSigned,
                     std::size_t      size,
                     Input&           in) {
            std::int64_t asSigned{};
            auto const   bits = integerBits(isSigned, size, in, asSigned);
            if(!bits) { return std::nullopt; }
            if(isSigned) { return signedValue(field, asSigned, size); }
            return unsignedValue(field, *bits, size);
        }

        // the three readers of an unsigned number (u, an `A` element, an enum without a name for it): one text
        Text unsignedValue(std::string_view field,
                           std::uint64_t    bits,
                           std::size_t      size) {
            return formatted(field, bits, [&] {
                return fmt::format("type: unsigned, size: {} bytes", size);
            });
        }

        // b and an `A` element of b
        Text boolValue(std::string_view field,
                       std::uint64_t    bits) {
            if(bits > 1) { return malformed(); }
            return formatted(field, bits != 0, [] {
                return std::string{"type: bool, size: 1 bytes"};
            });
        }

        Text signedValue(std::string_view field,
                         std::int64_t     value,
                         std::size_t      size) {
            return formatted(field, value, [&] {
                return fmt::format("type: signed, size: {} bytes", size);
            });
        }

        // An integer of `size` bytes: 4 bytes as LEB128 (zigzag for a signed one), every other width raw, little
        // endian (formatters.hpp formatter<integral>). The bits, and in asSigned the value a signed one stands for.
        std::optional<std::uint64_t> integerBits(bool          isSigned,
                                                 std::size_t   size,
                                                 Input&        in,
                                                 std::int64_t& asSigned) {
            if(size == 4) {
                auto const v = in.varint();
                if(!v) {
                    if(in.left() == 0) {
                        shortData();
                    } else {
                        malformed();
                    }
                    return std::nullopt;
                }
                if((*v >> 32U) != 0) {
                    malformed();
                    return std::nullopt;
                }
                if(isSigned) {
                    asSigned
                      = static_cast<std::int64_t>(*v >> 1U) ^ -static_cast<std::int64_t>(*v & 1U);
                    return static_cast<std::uint64_t>(asSigned);
                }
                asSigned = static_cast<std::int64_t>(*v);
                return v;
            }
            auto const v = in.raw(size);
            if(!v) {
                shortData();
                return std::nullopt;
            }
            if(isSigned) {
                // sign-extended from the top bit of its width
                auto const unused = static_cast<unsigned>(64U - (8U * size));
                asSigned          = static_cast<std::int64_t>(*v << unused) >> unused;
                return static_cast<std::uint64_t>(asSigned);
            }
            asSigned = static_cast<std::int64_t>(*v);
            return v;
        }

        // ---- time

        using std_ratios = std::tuple<std::atto,
                                      std::femto,
                                      std::pico,
                                      std::nano,
                                      std::micro,
                                      std::milli,
                                      std::centi,
                                      std::ratio<1>,
                                      std::deci,
                                      std::deca,
                                      std::hecto,
                                      std::kilo,
                                      std::mega,
                                      std::giga,
                                      std::tera,
                                      std::peta,
                                      std::exa,
                                      std::chrono::nanoseconds::period,
                                      std::chrono::microseconds::period,
                                      std::chrono::milliseconds::period,
                                      std::chrono::seconds::period,
                                      std::chrono::minutes::period,
                                      std::chrono::hours::period,
                                      std::chrono::days::period,
                                      std::chrono::weeks::period,
                                      std::chrono::months::period,
                                      std::chrono::years::period>;

        // fmt's chrono decomposition (%j, %H, ...) static_casts the day count from
        // floating point to int before range checking it - UB for huge values (fmt
        // 12.2.0, chrono.h to_nonnegative_int, found by fuzzing). Reject values whose
        // day count cannot fit an int before handing them to fmt. %Q/%q and specs
        // without conversion specifiers only print the count and are always safe.
        template<typename Rep>
        bool timeValueSafeForFmt(std::uint64_t    num,
                                 std::uint64_t    den,
                                 Rep              value,
                                 std::string_view replacementField) {
            if(!replacementField.contains('%') || replacementField == "{:%Q}"
               || replacementField == "{:%q}" || replacementField == "{:%Q%q}")
            {
                return true;
            }
            static constexpr double maxSeconds
              = 86400.0 * static_cast<double>(std::numeric_limits<std::int32_t>::max());
            double const seconds
              = (static_cast<double>(value) * static_cast<double>(num)) / static_cast<double>(den);
            if(seconds >= -maxSeconds && seconds <= maxSeconds) { return true; }
            errorMessagef(
              fmt::format("time value out of range (ratio: {}/{}, value: {})", num, den, value));
            return false;
        }

        template<typename Rep>
        Text formatTime(std::uint64_t    num,
                        std::uint64_t    den,
                        Rep              value,
                        std::string_view replacementField) {
            if(!render) { return std::string{}; }
            if constexpr(std::is_floating_point_v<Rep>) {
                if(!timeValueSafeForFmt(num, den, value, replacementField)) {
                    // said there, with the ratio and the value: one message per lost line
                    if(stop == Stop::none) { stop = Stop::render; }
                    return std::nullopt;
                }
            }
            Text       oStr;
            bool       failed = false;
            auto const fixed  = [&]<std::size_t I>(std::integral_constant<std::size_t, I>) {
                using ratio = typename std::tuple_element_t<I, std_ratios>::type;
                if(ratio::num != static_cast<std::intmax_t>(num)
                   || ratio::den != static_cast<std::intmax_t>(den))
                {
                    return true;
                }
                // A time_point is its duration since the clock's epoch, as the custom-ratio path below renders it.
                try {
                    oStr = fmt::format(fmt::runtime(replacementField),
                                       std::chrono::duration<Rep, ratio>{value});
                } catch(std::exception const& e) {
                    errorMessagef(fmt::format(
                      "bad format for replacement field {:?}: {} (ratio: {}/{}, value: {})",
                      replacementField,
                      e.what(),
                      num,
                      den,
                      value));
                    failed = true;
                }
                return false;
            };
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                (fixed(std::integral_constant<std::size_t, Is>{}) && ...);
            }(std::make_index_sequence<std::tuple_size_v<std_ratios>>{});

            if(failed) {
                if(stop == Stop::none) { stop = Stop::render; }
                return std::nullopt;
            }
            if(oStr) { return oStr; }

            if(replacementField == "{}" || replacementField == "{:%Q%q}") {
                if(den == 1) { return fmt::format("{}[{}]s", value, num); }
                return fmt::format("{}[{}/{}]s", value, num, den);
            }
            if(replacementField == "{:%Q}") { return fmt::format("{}", value); }
            if(replacementField == "{:%q}") {
                if(den == 1) { return fmt::format("[{}]s", num); }
                return fmt::format("[{}/{}]s", num, den);
            }

            // Integer reps are converted to a double duration here, so they need the
            // same guard as floating point values.
            if(!timeValueSafeForFmt(num, den, value, replacementField)) {
                if(stop == Stop::none) { stop = Stop::render; }
                return std::nullopt;
            }

            // A ratio fmt has no type for, under any other spec: the value as a duration<double> of seconds, which a
            // conversion (%H:%M, %S) or a precision then reads as seconds. A %Q or %q inside a longer spec thus gives
            // the seconds and "s", not the count of the custom unit: doing better would mean parsing the spec here.
            try {
                auto const dur = std::chrono::duration<double>{
                  (static_cast<double>(value) * static_cast<double>(num))
                  / static_cast<double>(den)};
                return fmt::format(fmt::runtime(replacementField), dur);
            } catch(std::exception const& e) {
                return renderError(fmt::format(
                  "bad format for replacement field {:?}: {} (custom ratio: {}/{}, value: {})",
                  replacementField,
                  e.what(),
                  num,
                  den,
                  value));
            }
        }

        Text time(std::string_view& d,
                  Input&            in,
                  std::string_view  field) {
            auto const num = grammar::number(d);
            if(!num || !grammar::literal(d, '/')) { return malformed(); }
            auto const den = grammar::number(d);
            if(!den || *num == 0 || *den == 0 || d.empty()) { return malformed(); }
            char const kind = d.front();
            d.remove_prefix(1);
            auto const size = grammar::size(d);
            if(!size) { return malformed(); }
            if(kind == 'f') {
                auto const bits = in.raw(*size);
                if(!bits) { return shortData(); }
                double const value
                  = *size == 4
                    ? static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(*bits)))
                  : *size == 8 ? std::bit_cast<double>(*bits)
                               : 0.0;
                if(*size != 4 && *size != 8) { return malformed(); }
                return formatTime(*num, *den, value, field);
            }
            if(kind != 'i' && kind != 'u') { return malformed(); }
            std::int64_t asSigned{};
            if(!integerBits(kind == 'i', *size, in, asSigned)) { return std::nullopt; }
            return formatTime(*num, *den, asSigned, field);
        }

        // ---- containers

        struct RangeReplacementField {
            std::string rangeReplacementField;
            std::string childReplacementField;
        };

        static RangeReplacementField fixRangeReplacementField(std::string_view replacementField) {
            RangeReplacementField rrf;
            if(!replacementField.starts_with("{:")) {
                rrf.rangeReplacementField = "{}";
                rrf.childReplacementField = "{}";
                return rrf;
            }
            auto const colonPos = replacementField.find(':', 2);
            if(colonPos == std::string_view::npos) {
                rrf.rangeReplacementField = replacementField;
                rrf.childReplacementField = "{}";
                return rrf;
            }
            rrf.rangeReplacementField = replacementField.substr(0, colonPos);
            rrf.rangeReplacementField += '}';

            rrf.childReplacementField = "{";
            rrf.childReplacementField += replacementField.substr(colonPos);

            return rrf;
        }

        Text tuple(std::string_view& d,
                   Input&            in,
                   std::string_view  field,
                   bool              inMap) {
            // the element descriptors first: a pair is checked for its size
            std::vector<std::string_view> elements;
            while(!grammar::literal(d, ')')) {
                auto const element = grammar::take(d);
                if(!element) { return malformed(); }
                elements.push_back(*element);
            }
            auto const [rangeField, childField] = fixRangeReplacementField(field);
            bool const asMap                    = inMap || rangeField.contains('m');
            if(render && asMap && elements.size() != 2) {
                return renderError(
                  fmt::format("bad format for replacement field {:?}: a map element of {} fields",
                              field,
                              elements.size()));
            }
            bool const parenthesis
              = !inMap && !rangeField.contains('n') && !rangeField.contains('m');

            std::string out = parenthesis ? "(" : "";
            for(std::size_t i = 0; i != elements.size(); ++i) {
                std::string_view element = elements[i];
                auto const       text    = value(element, in, childField, true, false);
                if(!text) { return std::nullopt; }
                out += *text;
                if(i + 1 != elements.size()) { out += asMap ? ": " : ", "; }
            }
            if(parenthesis) { out += ')'; }
            return out;
        }

        Text range(char              kind,
                   std::string_view& d,
                   Input&            in,
                   std::string_view  field) {
            auto const taken = grammar::take(d);
            if(!taken) { return malformed(); }
            std::string_view const element = *taken;

            auto const size = in.varint();
            if(!size) { return in.left() == 0 ? shortData() : malformed(); }
            if(grammar::canBeEmpty(element) ? *size > maxEmptyElements : *size > in.left()) {
                return *size > in.left() && !grammar::canBeEmpty(element) ? shortData()
                                                                          : malformed();
            }

            auto const [rangeField, childField] = fixRangeReplacementField(field);
            bool const parenthesis              = !rangeField.contains('n');
            bool const list                     = kind == 'L' || kind == 'A';

            std::string out;
            if(parenthesis) { out = list ? "[" : "{"; }
            if(render) { out.reserve((static_cast<std::size_t>(*size) * 10) + 2); }

            if(kind == 'A') {
                // raw elements of a plain type: the element's size from its descriptor
                std::string_view probe = element;
                if(probe.empty()) { return malformed(); }
                char const scalar = probe.front();
                probe.remove_prefix(1);
                std::size_t width = 1;
                if(scalar == 'u' || scalar == 'i' || scalar == 'f') {
                    auto const s = grammar::size(probe);
                    if(!s) { return malformed(); }
                    width = *s;
                } else if(scalar != 'b' && scalar != 'c') {
                    return malformed();
                }
                if(*size > in.left() / width) { return shortData(); }
                for(std::uint64_t i = 0; i != *size; ++i) {
                    auto const text = rawScalar(scalar, width, in, childField, true);
                    if(!text) { return std::nullopt; }
                    out += *text;
                    if(i + 1 != *size) { out += ", "; }
                }
            } else {
                bool const inMap = kind == 'M' || rangeField.contains('m');
                for(std::uint64_t i = 0; i != *size; ++i) {
                    std::string_view e    = element;
                    auto const       text = value(e, in, childField, true, inMap);
                    if(!text) { return std::nullopt; }
                    out += *text;
                    if(i + 1 != *size) { out += ", "; }
                }
            }
            if(parenthesis) { out += list ? ']' : '}'; }
            return out;
        }

        // one element of an `A` range: a fixed-width scalar
        Text rawScalar(char             scalar,
                       std::size_t      width,
                       Input&           in,
                       std::string_view field,
                       bool             inList) {
            auto const bits = in.raw(width);
            if(!bits) { return shortData(); }
            switch(scalar) {
            case 'b': return boolValue(field, *bits);
            case 'c': return rawCharacter(static_cast<char>(*bits), field, inList);
            case 'u': return unsignedValue(field, *bits, width);
            case 'i':
                {
                    auto const shift = 64U - (8U * static_cast<unsigned>(width));
                    auto const v     = static_cast<std::int64_t>(*bits << shift) >> shift;
                    return signedValue(field, v, width);
                }
            case 'f':
                if(width == 4) {
                    return formatted(field,
                                     std::bit_cast<float>(static_cast<std::uint32_t>(*bits)),
                                     [] { return std::string{"type: float"}; });
                }
                if(width == 8) {
                    return formatted(field, std::bit_cast<double>(*bits), [] {
                        return std::string{"type: double"};
                    });
                }
                return malformed();
            default: return malformed();
            }
        }

        // ---- names

        struct Name {
            std::uint64_t    bits;   // grammar::enumeratorValue
            std::string_view name;
        };

        static bool readNames(std::string_view&  d,
                              std::vector<Name>& names) {
            auto const k = grammar::number(d);
            if(!k || !grammar::literal(d, ':')) { return false; }
            for(std::uint64_t i = 0; i != *k; ++i) {
                auto const v = grammar::enumeratorValue(d);
                if(!v || !grammar::literal(d, ',')) { return false; }
                auto const name = grammar::counted(d);
                if(!name) { return false; }
                names.push_back({*v, *name});
            }
            return true;
        }

        Text enumeration(bool              bitflag,
                         std::string_view& d,
                         Input&            in,
                         std::string_view  field,
                         bool              inList) {
            if(d.empty()) { return malformed(); }
            char const kind = d.front();
            d.remove_prefix(1);
            auto const size = grammar::size(d);
            if(!size || (kind != 'i' && kind != 'u')) { return malformed(); }
            std::vector<Name> names;
            if(!readNames(d, names)) { return malformed(); }
            std::int64_t asSigned{};
            auto const   bits = integerBits(kind == 'i', *size, in, asSigned);
            if(!bits) { return std::nullopt; }
            if(!render) { return std::string{}; }

            auto const asNumber = [&]() -> Text {
                if(kind == 'i') { return signedValue(field, asSigned, *size); }
                return unsignedValue(field, *bits, *size);
            };
            if(!bitflag) {
                for(auto const& n : names) {
                    if(n.bits == *bits) { return stringValue(field, n.name, inList, "enumerator"); }
                }
                return asNumber();
            }
            // As enchantum::contains_bitflag decides it: a bit no enumerator has, or 0 without a zero flag, is a
            // number; 0 with one is its name; anything else the names of the flags it holds, joined with '|'.
            std::uint64_t ors  = 0;
            Name const*   zero = nullptr;
            for(auto const& n : names) {
                ors |= n.bits;
                if(n.bits == 0 && zero == nullptr) { zero = &n; }
            }
            auto const v = *bits;
            if(v == 0) {
                if(zero != nullptr) { return stringValue(field, zero->name, inList, "bitflag"); }
                return asNumber();
            }
            if((v & ~ors) != 0) { return asNumber(); }
            std::string joined;
            for(auto const& n : names) {
                auto const flag = n.bits;
                if(flag != 0 && (v & flag) == flag) {
                    if(!joined.empty()) { joined += '|'; }
                    joined += n.name;
                }
            }
            if(joined.empty()) { return asNumber(); }
            return stringValue(field, joined, inList, "bitflag");
        }

        // ---- format strings

        // Renders `fmtString` with one argument per field, `argument(field)` producing each one's text, in order.
        // Called once per field; `argument` with an empty field tells "no field left" (more arguments than fields).
        template<typename Argument>
        Text formatString(std::string_view fmtString,
                          std::size_t      arguments,
                          Argument&&       argument) {
            std::string out;
            if(render) { out.reserve(fmtString.size() * 2); }
            std::string_view rest   = fmtString;
            std::size_t      fields = 0;
            while(true) {
                auto const next = nextField(rest);
                if(next.malformed()) {
                    if(render) {
                        return renderError(fmt::format("malformed format string {:?}", fmtString));
                    }
                    return malformed();
                }
                if(render) { appendLiteral(out, next.literal); }
                if(next.field.empty()) { break; }
                rest = next.rest;
                if(next.error == ScanError::nested || !replacementFieldWithinLimits(next.field)) {
                    if(render) {
                        return renderError(
                          fmt::format("replacement field {:?} is a dynamic spec or carries a "
                                      "number above the limit of {}",
                                      next.field,
                                      maxReplacementFieldNumber));
                    }
                    return malformed();
                }
                if(fields == arguments) {
                    if(render) {
                        return renderError(fmt::format(
                          "malformed format string {:?}: more fields than its {} arguments",
                          fmtString,
                          arguments));
                    }
                    return malformed();
                }
                ++fields;
                auto const text = argument(next.field);
                if(!text) { return std::nullopt; }
                out += *text;
            }
            if(fields != arguments) {
                if(render) {
                    return renderError(
                      fmt::format("malformed format string {:?}: {} fields for {} arguments",
                                  fmtString,
                                  fields,
                                  arguments));
                }
                return malformed();
            }
            return out;
        }

        // A list of descriptors, one per argument
        static std::optional<std::vector<std::string_view>> splitDescriptors(std::string_view d) {
            std::vector<std::string_view> out;
            while(!d.empty()) {
                auto const one = grammar::take(d);
                if(!one) { return std::nullopt; }
                out.push_back(*one);
            }
            return out;
        }

        // the format string with its arguments read off the wire by their descriptors
        Text described(std::string_view                     fmtString,
                       std::vector<std::string_view> const& descriptors,
                       Input&                               in) {
            std::size_t next = 0;
            return formatString(fmtString, descriptors.size(), [&](std::string_view field) -> Text {
                std::string_view d = descriptors[next++];
                return value(d, in, field, false, false);
            });
        }

        /// A sub format string is already text by the time the outer replacement field is seen:
        /// every one of its arguments is rendered. So a presentation type that needs a value - ".2f", "#x", "%H" - has
        /// nothing left to act on and is an error, and pushing it down into the sub string's own fields is no answer
        /// either, since "{} {}" would apply it to both. Everything fmt accepts on a string, though - width, fill,
        /// align, precision-as-truncation, "s" - is meaningful and is applied here rather than thrown away.
        ///
        /// in_list deliberately plays no part: the result is a formatted representation, not a
        /// string value, so it does not pick up fmt's debug quoting inside a range.
        Text applyFieldToSubFmtString(std::string_view field,
                                      Text const&      rendered) {
            if(!rendered) { return std::nullopt; }
            if(!render || field == defaultReplacementField) { return rendered; }
            try {
                return fmt::format(fmt::runtime(field), *rendered);
            } catch(std::exception const& e) {
                return renderError(fmt::format(
                  "bad format for replacement field {:?}: {} - a formatter built on format_to "
                  "renders to a string on the host, so only a string spec can apply to it "
                  "(sub format string result: {:?})",
                  field,
                  e.what(),
                  *rendered));
            }
        }

        struct DescribedString {
            std::string_view              fmtString;
            std::vector<std::string_view> descriptors;
        };

        // A described string - the catalog's, or an inline one off the wire - split at its separator; its format
        // string checked with the grammar and its characters checked (an inline one must stay ASCII).
        std::optional<DescribedString> split(std::string_view text,
                                             bool             isInline) {
            auto const at = text.find(desc::separator);
            if(at == std::string_view::npos) { return std::nullopt; }
            DescribedString out{text.substr(0, at), {}};
            if(!formatShape(out.fmtString).wellFormed) { return std::nullopt; }
            if(!(isInline ? allCharsValid(out.fmtString) : allCharsValidCataloged(out.fmtString))) {
                return std::nullopt;
            }
            auto descriptors = splitDescriptors(text.substr(at + 1));
            if(!descriptors) {
                errorMessagef(fmt::format(
                  "a descriptor this parser cannot read (nesting deeper than {} levels, or "
                  "malformed): {:?}",
                  grammar::maxDepth,
                  text.substr(at + 1, 120)));
                return std::nullopt;
            }
            out.descriptors = std::move(*descriptors);
            return out;
        }

        // the string an id names: the catalog's, or (inlineId) the one that follows on the wire
        std::optional<std::string_view> stringOf(std::uint16_t id,
                                                 Input&        in,
                                                 bool&         isInline) {
            isInline = id == protocol::inlineId;
            if(isInline) {
                auto const length = in.varint();
                if(!length) {
                    if(in.left() == 0) {
                        shortData();
                    } else {
                        malformed();
                    }
                    return std::nullopt;
                }
                if(*length > in.left()) {
                    shortData();
                    return std::nullopt;
                }
                auto const text = in.bytes.subspan(in.pos, static_cast<std::size_t>(*length));
                in.pos += text.size();
                return std::string_view{reinterpret_cast<char const*>(text.data()), text.size()};
            }
            if(strings != nullptr) {
                if(auto const it = strings->find(id); it != strings->end()) {
                    return std::string_view{it->second};
                }
            }
            if(render) { errorMessagef(fmt::format("cataloged format string not found {}", id)); }
            malformed();
            return std::nullopt;
        }

        // `*`: a sub format string by its id (or inline), then its arguments
        Text selfDescribed(Input&           in,
                           std::string_view field) {
            auto const id = in.raw(2);
            if(!id) { return shortData(); }
            bool       isInline{};
            auto const text = stringOf(static_cast<std::uint16_t>(*id), in, isInline);
            if(!text) { return std::nullopt; }
            auto const parts = split(*text, isInline);
            if(!parts) { return malformed(); }
            return applyFieldToSubFmtString(field,
                                            described(parts->fmtString, parts->descriptors, in));
        }

        // ---- one value

        // Every recursive path goes through here - elements of ranges and tuples, wrapped values, the arguments of a
        // sub format string - so this is the one place the depth has to be counted.
        Text value(std::string_view& d,
                   Input&            in,
                   std::string_view  field,
                   bool              inList,
                   bool              inMap) {
            if(nestingDepth >= maxNestingDepth) {
                // said in either walk: the frame is refused in the first
                errorMessagef(fmt::format("nesting deeper than {} levels", maxNestingDepth));
                return malformed();
            }
            NestingGuard const guard{nestingDepth};

            if(d.empty()) { return malformed(); }
            char const tag = d.front();
            d.remove_prefix(1);
            switch(tag) {
            case 'b':
                {
                    auto const b = in.byte();
                    if(!b) { return shortData(); }
                    return boolValue(field, *b);
                }
            case 'c':
                {
                    auto const b = in.byte();
                    if(!b) { return shortData(); }
                    return rawCharacter(static_cast<char>(*b), field, inList);
                }
            case 'u':
            case 'i':
                {
                    auto const size = grammar::size(d);
                    if(!size) { return malformed(); }
                    return integer(field, tag == 'i', *size, in);
                }
            case 'f':
                {
                    auto const size = grammar::size(d);
                    if(!size || (*size != 4 && *size != 8)) { return malformed(); }
                    return rawScalar('f', *size, in, field, inList);
                }
            case 'p':
                {
                    auto const size = grammar::size(d);
                    if(!size || (*size != 4 && *size != 8)) { return malformed(); }
                    auto const bits = in.raw(*size);
                    if(!bits) { return shortData(); }
                    return formatted(
                      field,
                      std::bit_cast<void const*>(static_cast<std::uintptr_t>(*bits)),
                      [&] { return fmt::format("type: pointer, size: {} bytes", *size); });
                }
            case 's':
                {
                    auto const length = in.varint();
                    if(!length) { return in.left() == 0 ? shortData() : malformed(); }
                    if(*length > in.left()) { return shortData(); }
                    auto const bytes = in.bytes.subspan(in.pos, static_cast<std::size_t>(*length));
                    in.pos += bytes.size();
                    std::string_view const text{reinterpret_cast<char const*>(bytes.data()),
                                                bytes.size()};
                    return stringValue(field, text, inList, "string");
                }
            case 'k':
                {
                    auto const text = grammar::counted(d);
                    if(!text) { return malformed(); }
                    return stringValue(field, *text, inList, "string");
                }
            case 'd': return time(d, in, field);
            case 'O':
                {
                    auto const engaged = in.byte();
                    if(!engaged) { return shortData(); }
                    if(*engaged > 1) { return malformed(); }
                    if(*engaged == 0) {
                        if(!grammar::skip(d)) { return malformed(); }
                        return render ? Text{"none"} : Text{std::string{}};
                    }
                    // in_list forced true rather than forwarded: a wrapper payload is a nested position,
                    // so fmt keeps applying its debug format inside optional(...) - optional("s") and
                    // optional('c'), not optional(s) and optional(c).
                    auto const inner = value(d, in, field, true, inMap);
                    if(!inner) { return std::nullopt; }
                    return render ? fmt::format("optional({})", *inner) : std::string{};
                }
            case 'X':
                {
                    auto const hasValue = in.byte();
                    if(!hasValue) { return shortData(); }
                    if(*hasValue > 1) { return malformed(); }
                    if(*hasValue == 0 && !grammar::skip(d)) { return malformed(); }
                    // in_list forced true for the same reason as optional: fmt writes expected("ok") and
                    // unexpected("err"), keeping the debug format inside the wrapper.
                    auto const inner = value(d, in, field, true, inMap);
                    if(!inner) { return std::nullopt; }
                    if(*hasValue == 1 && !grammar::skip(d)) { return malformed(); }
                    if(!render) { return std::string{}; }
                    // A void value renders as the empty string, so this yields fmt's "expected()".
                    return fmt::format("{}({})",
                                       *hasValue == 1 ? "expected" : "unexpected",
                                       *inner);
                }
            case 'v': return std::string{};
            case 'V':
                {
                    auto const n = grammar::number(d);
                    if(!n || !grammar::literal(d, ':')) { return malformed(); }
                    auto const index = in.byte();
                    if(!index) { return shortData(); }
                    if(*index >= *n) { return malformed(); }
                    for(std::uint64_t i = 0; i != *index; ++i) {
                        if(!grammar::skip(d)) { return malformed(); }
                    }
                    auto const inner = value(d, in, field, true, inMap);
                    if(!inner) { return std::nullopt; }
                    for(std::uint64_t i = *index + 1; i < *n; ++i) {
                        if(!grammar::skip(d)) { return malformed(); }
                    }
                    return render ? fmt::format("variant({})", *inner) : std::string{};
                }
            case 'L':
            case 'S':
            case 'M':
            case 'A': return range(tag, d, in, field);
            case '(': return tuple(d, in, field, inMap);
            case 'Q': return quantity(d, in, field, inList, inMap);
            case 'Y': return styled(d, in, field, inList, inMap);
            case 'F':
                {
                    auto const fmtString = grammar::counted(d);
                    auto const k         = grammar::number(d);
                    if(!fmtString || !k || !grammar::literal(d, ':')) { return malformed(); }
                    std::vector<std::string_view> descriptors;
                    for(std::uint64_t i = 0; i != *k; ++i) {
                        auto const one = grammar::take(d);
                        if(!one) { return malformed(); }
                        descriptors.push_back(*one);
                    }
                    if(!formatShape(*fmtString).wellFormed) { return malformed(); }
                    return applyFieldToSubFmtString(field, described(*fmtString, descriptors, in));
                }
            case 'E':
            case 'G': return enumeration(tag == 'G', d, in, field, inList);
            case 'R': return reg(d, in, field);
            case 'r': return flags(d, in, field, inList);
            case '*': return selfDescribed(in, field);
            case '#':
                // said in the first walk, which refuses the frame
                errorMessagef(
                  "a type reference without its entry: the catalog has no \"Types\" entry for it "
                  "(a "
                  "catalog of another build?)");
                return malformed();
            default: return malformed();
            }
        }

        Text rawCharacter(char             c,
                          std::string_view field,
                          bool             inList) {
            if(!render) { return std::string{}; }
            // A char nested in a range or tuple gets fmt's debug format ('x', with control bytes escaped),
            // matching what fmt does for the same container. Only chars: fmt leaves the other trivial types
            // alone inside containers.
            if(inList && field == defaultReplacementField) { return fmt::format("{:?}", c); }
            return formatted(field, c, [] { return std::string{"type: char, size: 1 bytes"}; });
        }

        /// The value is rendered with the caller's own replacement field, so a quantity takes the spec its
        /// representation would have taken: "{:.2f}" on a float quantity, "{:#x}" on an integral one. Fill, align and
        /// width are held back and applied to number and symbol together, so "{:>10}" positions "1.5 V" the way it
        /// would position any other argument.
        Text quantity(std::string_view& d,
                      Input&            in,
                      std::string_view  field,
                      bool              inList,
                      bool              inMap) {
            auto const symbol = grammar::counted(d);
            if(!symbol) { return malformed(); }
            auto const [padField, valueField] = splitFillAlignWidth(field);
            auto const number                 = value(d, in, valueField, inList, inMap);
            if(!number) { return std::nullopt; }
            if(!render) { return std::string{}; }
            auto text = fmt::format("{}{}", *number, *symbol);   // not const: returned by move
            if(padField == "{}") { return text; }
            try {
                return fmt::format(fmt::runtime(padField), text);
            } catch(std::exception const& e) {
                return renderError(
                  fmt::format("bad format for replacement field {:?}: {} (quantity: \"{}\")",
                              field,
                              e.what(),
                              text));
            }
        }

        Text styled(std::string_view& d,
                    Input&            in,
                    std::string_view  field,
                    bool              inList,
                    bool              inMap) {
            auto const set = in.byte();
            if(!set) { return shortData(); }
            if((*set & 0xC0U) != 0) { return malformed(); }
            bool const fgRgb  = 0 != (*set & 1U);
            bool const fgTerm = 0 != (*set & 2U);
            bool const bgRgb  = 0 != (*set & 4U);
            bool const bgTerm = 0 != (*set & 8U);
            bool const emp    = 0 != (*set & 16U);

            if((fgRgb && fgTerm) || (bgRgb && bgTerm) || (fgTerm && (bgTerm || bgRgb))
               || (bgTerm && (fgTerm || fgRgb)))
            {
                return malformed();
            }
            fmt::text_style style{};
            bool            bad   = false;
            bool            ran   = false;
            auto const      color = [&](bool rgb, bool term, auto gen) {
                if(rgb) {
                    auto const c = in.raw(4);
                    if(!c) {
                        ran = true;
                        return;
                    }
                    if(*c >= (1U << 25U)) {
                        bad = true;
                        return;
                    }
                    style |= gen(static_cast<fmt::color>(*c));
                }
                if(term) {
                    auto const c = in.byte();
                    if(!c) {
                        ran = true;
                        return;
                    }
                    if(!enchantum::contains<fmt::terminal_color>(
                         static_cast<std::underlying_type_t<fmt::terminal_color>>(*c)))
                    {
                        bad = true;
                        return;
                    }
                    style |= gen(static_cast<fmt::terminal_color>(*c));
                }
            };
            color(fgRgb, fgTerm, [](auto v) { return fmt::fg(v); });
            if(!bad && !ran) {
                color(bgRgb, bgTerm, [](auto v) { return fmt::bg(v); });
            }
            if(ran) { return shortData(); }
            if(bad) { return malformed(); }
            if(emp) {
                auto const e = in.byte();
                if(!e) { return shortData(); }
                style |= fmt::text_style{static_cast<fmt::emphasis>(*e)};
            }
            auto const inner = value(d, in, field, inList, inMap);
            if(!inner) { return std::nullopt; }
            if(!render) { return std::string{}; }
            try {
                return fmt::format("{}", fmt::styled(*inner, style));
            } catch(std::exception const& e) {
                return renderError(fmt::format("bad style: {}", e.what()));
            }
        }

        // The value descriptor of a register or Flags: an unsigned integer (`u<size>`); nullopt for anything else
        static std::optional<std::size_t> wordSize(std::string_view& d) {
            if(!grammar::literal(d, 'u')) { return std::nullopt; }
            return grammar::size(d);
        }

        // the largest value of a word of `size` bytes: no mask may reach past it
        static constexpr std::uint64_t wordMax(std::size_t size) {
            return size >= 8 ? ~std::uint64_t{} : (std::uint64_t{1} << (8U * size)) - 1U;
        }

        // A register: the raw word (its value descriptor), cut into its fields by the masks, each a number or its
        // value's name, rendered with the register's own format string (dims first), as a sub format string.
        Text reg(std::string_view& d,
                 Input&            in,
                 std::string_view  field) {
            auto const size = wordSize(d);
            if(!size) { return malformed(); }
            auto const fmtString = grammar::counted(d);
            if(!fmtString) { return malformed(); }
            auto const dimCount = grammar::number(d);
            if(!dimCount || !grammar::literal(d, ':')) { return malformed(); }
            std::vector<std::uint64_t> dims;
            for(std::uint64_t i = 0; i != *dimCount; ++i) {
                auto const dim = grammar::number(d);
                if(!dim || !grammar::literal(d, ',')) { return malformed(); }
                dims.push_back(*dim);
            }
            auto const fieldCount = grammar::number(d);
            if(!fieldCount || !grammar::literal(d, ':')) { return malformed(); }

            struct Field {
                std::uint64_t     mask;
                std::vector<Name> names;
            };

            std::vector<Field> fields;
            for(std::uint64_t i = 0; i != *fieldCount; ++i) {
                auto const mask = grammar::number(d);
                if(!mask || *mask > wordMax(*size) || !grammar::literal(d, ',')) {
                    return malformed();
                }
                Field f{*mask, {}};
                if(!readNames(d, f.names)) { return malformed(); }
                fields.push_back(std::move(f));
            }
            std::int64_t unusedSigned{};
            auto const   raw = integerBits(false, *size, in, unusedSigned);
            if(!raw) { return std::nullopt; }
            if(!render) { return std::string{}; }
            if(!formatShape(*fmtString).wellFormed) { return malformed(); }

            auto const  word = *raw;
            std::size_t next = 0;
            auto const  text = formatString(
              *fmtString,
              dims.size() + fields.size(),
              [&](std::string_view f) -> Text {
                  auto const i = next++;
                  if(i < dims.size()) {
                      return formatted(f, dims[i], [] { return std::string{"register dim"}; });
                  }
                  auto const& fd = fields[i - dims.size()];
                  auto const  v = fd.mask == 0 ? 0U : (word & fd.mask) >> std::countr_zero(fd.mask);
                  for(auto const& n : fd.names) {
                      if(n.bits == v) { return stringValue(f, n.name, false, "field value"); }
                  }
                  return formatted(f, v, [] { return std::string{"register field"}; });
              });
            return applyFieldToSubFmtString(field, text);
        }

        // Register::Flags: the names of the set single-bit fields of the raw word (its value descriptor) joined with
        // '|', "none" when none is set
        Text flags(std::string_view& d,
                   Input&            in,
                   std::string_view  field,
                   bool              inList) {
            auto const size = wordSize(d);
            if(!size) { return malformed(); }
            auto const count = grammar::number(d);
            if(!count || !grammar::literal(d, ':')) { return malformed(); }

            struct Flag {
                std::uint64_t    mask;
                std::string_view name;
            };

            std::vector<Flag> flagList;
            for(std::uint64_t i = 0; i != *count; ++i) {
                auto const mask = grammar::number(d);
                if(!mask || *mask > wordMax(*size) || !grammar::literal(d, ',')) {
                    return malformed();
                }
                auto const name = grammar::counted(d);
                if(!name) { return malformed(); }
                flagList.push_back({*mask, *name});
            }
            std::int64_t unusedSigned{};
            auto const   raw = integerBits(false, *size, in, unusedSigned);
            if(!raw) { return std::nullopt; }
            if(!render) { return std::string{}; }
            std::string joined;
            for(auto const& f : flagList) {
                if((*raw & f.mask) != 0) {
                    if(!joined.empty()) { joined += '|'; }
                    joined += f.name;
                }
            }
            if(joined.empty()) { joined = "none"; }
            return stringValue(field, joined, inList, "bitflag");
        }
    };
}   // namespace detail

struct ParsedMessage {
    std::optional<std::string> message;
    std::span<std::byte const> remaining;
    std::size_t                discarded{};
    std::optional<catalog_id>  catalogId;
};

// What a parse error is about, for an error callback that takes it as its second argument. `stream`: bytes that make
// no frame, or a frame that does not check out - skipped, the reader resyncs past them. `format`: a whole frame whose
// text the host could not render (fmt refused a replacement field or a style, a time value out of range, ...) -
// consumed, its line lost to its format string.
enum class ParseError : std::uint8_t { stream, format };

// An error callback is `(text)` or `(text, ParseError)`: this calls either.
template<typename ErrorMessageF>
inline void sayParseError(ErrorMessageF&     errorMessagef,
                          std::string const& text,
                          ParseError         kind) {
    if constexpr(std::is_invocable_v<ErrorMessageF&, std::string const&, ParseError>) {
        errorMessagef(text, kind);
    } else {
        errorMessagef(text);
    }
}

// One frame off the front of `buffer` (after any bytes that make none): its text, the rest, the bytes skipped, the id.
// No message and the buffer as it was (minus the skipped bytes): the frame at its front is still arriving.
// Every problem is said through `errorMessagef`: `(text)`, or `(text, ParseError)` to tell a stream error from a
// line lost to its format.
template<typename ErrorMessageF>
inline ParsedMessage parseMessage(std::span<std::byte const>             buffer,
                                  std::unordered_map<std::uint16_t,
                                                     std::string> const& stringConstantsMap,
                                  ErrorMessageF&&                        errorMessagef,
                                  FrameCheck expect = FrameCheck::either) {
    std::size_t unparsedBytes{};
    auto const  isStart = [expect](std::byte b) { return isFrameStart(b, expect); };
    // a frame of the other kind than the catalog says: refused, said once a call
    bool       foreignTold = false;
    auto const foreign     = [&](std::span<std::byte const> at) {
        if(foreignTold || at.size() < 3 || isStart(at[0])) { return; }
        if(expect == FrameCheck::either || !protocol::isStartMarker(at[0])) { return; }
        catalog_id const id = static_cast<catalog_id>(std::to_integer<unsigned>(at[1])
                                                      | (std::to_integer<unsigned>(at[2]) << 8U));
        if(id != protocol::inlineId && !stringConstantsMap.contains(id)) { return; }
        foreignTold = true;
        sayParseError(
          errorMessagef,
          expect == FrameCheck::required
            ? std::
                string{"an unchecked frame from an image whose catalog says it checks its frames: "
                       "refused (the catalog is not this image's?)"}
            : std::string{"a checked frame from an image whose catalog says it has no frame check: "
                          "refused (the catalog is not this image's?)"},
          ParseError::stream);
    };
    bool       unknownTold = false;
    auto const skipOne     = [&] {
        unparsedBytes += 1;
        buffer = buffer.subspan(1);
    };
    while(true) {
        for(std::size_t k = 0; k + 2 < buffer.size() && !foreignTold && !isStart(buffer[k]); ++k) {
            foreign(buffer.subspan(k));
        }
        auto const        iterator = std::ranges::find_if(buffer, isStart);
        std::size_t const offset
          = static_cast<std::size_t>(std::distance(buffer.begin(), iterator));
        buffer = buffer.subspan(offset);
        unparsedBytes += offset;

        if(buffer.size() < 3) { return {std::nullopt, buffer, unparsedBytes, {}}; }
        bool const       checked = buffer[0] == protocol::checkedStartMarker;
        catalog_id const id      = static_cast<catalog_id>(
          std::to_integer<unsigned>(buffer[1]) | (std::to_integer<unsigned>(buffer[2]) << 8U));
        // a known id or an inline string: anything else is no frame start (resync) - said once a call, as an image
        // newer than its catalog would lose every new line in silence otherwise
        if(id != protocol::inlineId && !stringConstantsMap.contains(id)) {
            if(!unknownTold) {
                unknownTold = true;
                sayParseError(
                  errorMessagef,
                  fmt::format("cataloged format string not found {}: skipped as no frame (the "
                              "catalog is not this image's?)",
                              id),
                  ParseError::stream);
            }
            skipOne();
            continue;
        }

        // what the parser says in the first walk is about the bytes; in the second, the frame is whole and checked,
        // so what it says there is about rendering its text
        ParseError     phase = ParseError::stream;
        detail::Parser parser{
          [&](std::string_view m) { sayParseError(errorMessagef, std::string{m}, phase); },
          &stringConstantsMap};
        // first walk: the frame's structure and its exact end, nothing rendered
        detail::Input                                  in{buffer, 3};
        bool                                           isInline{};
        auto const                                     text = parser.stringOf(id, in, isInline);
        std::optional<detail::Parser::DescribedString> parts;
        if(text) { parts = parser.split(*text, isInline); }
        if(text && !parts) { parser.stop = detail::Parser::Stop::malformed; }
        if(parts) {
            for(auto d : parts->descriptors) {
                if(!parser.value(d, in, detail::defaultReplacementField, false, false)) { break; }
            }
        }
        if(parser.stop == detail::Parser::Stop::shortData) {
            return {std::nullopt, buffer, unparsedBytes, {}};
        }
        if(parser.stop != detail::Parser::Stop::none) {
            skipOne();
            continue;
        }
        std::size_t end = in.pos;
        if(checked) {
            // the CRC byte, then the end marker; a frame that does not add up is no frame: skip its start
            // marker and look for the next one, rather than wait for bytes that would not help
            if(buffer.size() < end + 2) { return {std::nullopt, buffer, unparsedBytes, {}}; }
            auto const want
              = protocol::crcFinal(protocol::crcOver(protocol::crcInit, buffer.first(end)));
            if(std::to_integer<std::uint8_t>(buffer[end]) != want
               || buffer[end + 1] != protocol::endMarker)
            {
                sayParseError(
                  errorMessagef,
                  fmt::format("frame check failed: a damaged frame of {} bytes dropped", end + 2),
                  ParseError::stream);
                skipOne();
                continue;
            }
            ++end;
        } else {
            if(buffer.size() < end + 1) { return {std::nullopt, buffer, unparsedBytes, {}}; }
            if(buffer[end] != protocol::endMarker) {
                skipOne();
                continue;
            }
        }
        ++end;   // the end marker

        // second walk: the text
        parser.render = true;
        parser.stop   = detail::Parser::Stop::none;
        phase         = ParseError::format;
        detail::Input second{buffer.first(end), 3};
        auto const    again    = parser.stringOf(id, second, isInline);
        auto const    rendered = again && parts
                                 ? parser.described(parts->fmtString, parts->descriptors, second)
                                 : std::optional<std::string>{};
        buffer                 = buffer.subspan(end);
        if(!rendered) {
            // a frame that is whole and checks out, but whose text fmt refused (reported): consumed, on to the next
            continue;
        }
        return {rendered,
                buffer,
                unparsedBytes,
                id == protocol::inlineId ? std::optional<catalog_id>{}
                                         : std::optional<catalog_id>{id}};
    }
}
}   // namespace remote_fmt
