#pragma once

#include "remote_fmt/fmt_wrapper.hpp"
#include "remote_fmt/remote_fmt.hpp"
#include "remote_fmt/type_identifier.hpp"

#ifdef __GNUC__
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wextra-semi"
#endif

#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

#include <magic_enum/magic_enum.hpp>
#include <magic_enum/magic_enum_switch.hpp>

#ifdef __GNUC__
    #pragma GCC diagnostic pop
#endif
#ifdef __clang__
    #pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iterator>
#include <map>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace remote_fmt {
namespace detail {

    template<typename Iterator>
    struct ParseResult_ {
        std::string str;
        Iterator    pos;
    };

    template<typename Iterator>
    using ParseResult = std::optional<ParseResult_<Iterator>>;

    template<ExtendedTypeIdentifier>
    struct ExtendedTypeIdentifierParser;

    template<>
    struct ExtendedTypeIdentifierParser<ExtendedTypeIdentifier::styled> {
        // NOTE: This function is part of a recursive parsing system for nested data structures.
        // Recursion is bounded by the structure of the serialized data being parsed.
        template<typename Iterator,
                 typename Parser>
        static ParseResult<Iterator>
        parse(Iterator                               first,
              Iterator                               last,
              std::string_view                       replacementField,
              bool                                   in_map,
              bool                                   in_list,
              std::unordered_map<std::uint16_t,
                                 std::string> const& stringConstantsMap,
              Parser&                                parser) {
            if(1 > static_cast<std::size_t>(std::distance(first, last))) { return std::nullopt; }
            std::uint8_t const set = static_cast<std::uint8_t>(*first);
            ++first;

            if((set & static_cast<std::uint8_t>(0xC0)) != 0) { return std::nullopt; }
            bool const fg_rgb  = 0 != (set & static_cast<std::uint8_t>(1));
            bool const fg_term = 0 != (set & static_cast<std::uint8_t>(2));
            bool const bg_rgb  = 0 != (set & static_cast<std::uint8_t>(4));
            bool const bg_term = 0 != (set & static_cast<std::uint8_t>(8));
            bool const emp     = 0 != (set & static_cast<std::uint8_t>(16));

            if((fg_rgb && fg_term) || (bg_rgb && bg_term) || (fg_term && (bg_term || bg_rgb))
               || (bg_term && (fg_term || fg_rgb)))
            {
                return std::nullopt;
            }
            fmt::text_style style{};

            auto extractColor = [&](bool rgb, bool term, auto gen) {
                if(rgb) {
                    auto const opt_result = parser.extractSize(first, last, TypeSize::_4);
                    if(!opt_result) { return false; }
                    auto const color = opt_result->first;
                    if(color >= (1U << 25U)) { return false; }
                    first = opt_result->second;
                    style |= gen(static_cast<fmt::color>(color));
                }
                if(term) {
                    auto const opt_result_term = parser.extractSize(first, last, TypeSize::_1);
                    if(!opt_result_term) { return false; }

                    auto const color = opt_result_term->first;

                    if(!magic_enum::enum_contains<fmt::terminal_color>(
                         static_cast<std::underlying_type_t<fmt::terminal_color>>(color)))
                    {
                        return false;
                    }

                    first = opt_result_term->second;
                    style |= gen(static_cast<fmt::terminal_color>(color));
                }
                return true;
            };
            if(!extractColor(fg_rgb, fg_term, [](auto value) { return fmt::fg(value); })) {
                return std::nullopt;
            }
            if(!extractColor(bg_rgb, bg_term, [](auto value) { return fmt::bg(value); })) {
                return std::nullopt;
            }
            if(emp) {
                if(1 > static_cast<std::size_t>(std::distance(first, last))) {
                    return std::nullopt;
                }
                std::uint8_t const emp_value = static_cast<std::uint8_t>(*first);
                ++first;
                style |= fmt::text_style{static_cast<fmt::emphasis>(emp_value)};
            }

            auto const inner_result = parser.parseFromTypeId(first,
                                                             last,
                                                             replacementField,
                                                             in_list,
                                                             in_map,
                                                             stringConstantsMap);

            if(!inner_result) { return std::nullopt; }

            try {
                auto const styled_string = fmt::format("{}", fmt::styled(inner_result->str, style));
                first                    = inner_result->pos;
                return ParseResult_<Iterator>{styled_string, first};

            } catch(...) { return std::nullopt; }
        }
    };

    template<>
    struct ExtendedTypeIdentifierParser<ExtendedTypeIdentifier::optional> {
        // NOTE: This function is part of a recursive parsing system for optional value types.
        // Recursion is bounded by the structure of the serialized data being parsed.
        template<typename Iterator,
                 typename Parser>
        static ParseResult<Iterator>
        parse(Iterator                               first,
              Iterator                               last,
              std::string_view                       replacementField,
              bool                                   in_map,
              bool                                   in_list,
              std::unordered_map<std::uint16_t,
                                 std::string> const& stringConstantsMap,
              Parser&                                parser) {
            if(1 > static_cast<std::size_t>(std::distance(first, last))) { return std::nullopt; }
            std::uint8_t const isSet = static_cast<std::uint8_t>(*first);
            ++first;

            if(isSet != 0 && isSet != 1) { return std::nullopt; }

            if(isSet == 0) { return ParseResult_<Iterator>{"()", first}; }
            auto const inner_result = parser.parseFromTypeId(first,
                                                             last,
                                                             replacementField,
                                                             in_list,
                                                             in_map,
                                                             stringConstantsMap);

            if(!inner_result) { return std::nullopt; }
            return ParseResult_<Iterator>{inner_result->str, inner_result->pos};
        }
    };

    template<>
    struct ExtendedTypeIdentifierParser<ExtendedTypeIdentifier::expected> {
        template<typename Iterator,
                 typename Parser>
        static ParseResult<Iterator>
        parse(Iterator                               first,
              Iterator                               last,
              std::string_view                       replacementField,
              bool                                   in_map,
              bool                                   in_list,
              std::unordered_map<std::uint16_t,
                                 std::string> const& stringConstantsMap,
              Parser&                                parser) {
            if(1 > static_cast<std::size_t>(std::distance(first, last))) { return std::nullopt; }
            std::uint8_t const hasValue = static_cast<std::uint8_t>(*first);
            ++first;

            if(hasValue != 0 && hasValue != 1) { return std::nullopt; }

            auto const inner_result = parser.parseFromTypeId(first,
                                                             last,
                                                             replacementField,
                                                             in_list,
                                                             in_map,
                                                             stringConstantsMap);
            if(!inner_result) { return std::nullopt; }

            if(hasValue == 1) {
                return ParseResult_<Iterator>{fmt::format("expected({})", inner_result->str),
                                              inner_result->pos};
            }
            return ParseResult_<Iterator>{fmt::format("unexpected({})", inner_result->str),
                                          inner_result->pos};
        }
    };

    struct Parser {
        std::function<void(std::string_view)> errorMessagef;

        template<typename ErrorMessageF>
            requires(!std::is_same_v<std::decay_t<ErrorMessageF>,
                                     Parser>)
        explicit Parser(ErrorMessageF&& errorMessagef_)
          : errorMessagef{std::forward<ErrorMessageF>(errorMessagef_)} {}

        std::optional<std::string_view>
        getNextReplacementFieldFromFmtStringAndAppendStrings(std::string&      out,
                                                             std::string_view& fmtString) {
            while(!fmtString.empty()) {
                auto const curlyPos = std::ranges::find_if(fmtString, [](auto character) {
                    return character == '{' || character == '}';
                });

                if(curlyPos == fmtString.end()) {
                    out += fmtString;
                    fmtString = std::string_view{};
                    return std::nullopt;
                }

                assert(std::next(curlyPos) != fmtString.end());

                if(*(std::next(curlyPos)) == *curlyPos) {
                    out += std::string_view{fmtString.begin(), std::next(curlyPos)};
                    fmtString = std::string_view{std::next(curlyPos, 2), fmtString.end()};
                    continue;
                }

                assert(*curlyPos == '{');
                auto const closeCurlyPos = std::ranges::find(fmtString, '}');
                assert(closeCurlyPos != fmtString.end());

                out += std::string_view{fmtString.begin(), curlyPos};
                fmtString = std::string_view{std::next(closeCurlyPos), fmtString.end()};
                return std::string_view{curlyPos, std::next(closeCurlyPos)};
            }
            return std::nullopt;
        }

        using trivial_t
          = std::variant<std::uint64_t, std::int64_t, bool, char, void const*, float, double>;

        template<typename T,
                 typename Iterator>
        T extract(Iterator first,
                  Iterator last) {
            static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
            if(static_cast<std::size_t>(std::distance(first, last)) < sizeof(T)) {
                throw std::runtime_error("Insufficient data for extraction");
            }
            T value;
            std::memcpy(&value, &*first, sizeof(T));
            return value;
        }

        template<typename Iterator>
        std::optional<std::variant<double,
                                   float>>
        extractFloatingpoint(Iterator first,
                             Iterator last,
                             TypeSize typeSize) {
            if(typeSize == TypeSize::_4) { return extract<float>(first, last); }
            if(typeSize == TypeSize::_8) { return extract<double>(first, last); }
            return std::nullopt;
        }

        template<typename Iterator>
        std::optional<std::uint64_t> extractUnsigned(Iterator first,
                                                     Iterator last,
                                                     TypeSize typeSize) {
            switch(typeSize) {
            case TypeSize::_1: return extract<std::uint8_t>(first, last);
            case TypeSize::_2: return extract<std::uint16_t>(first, last);
            case TypeSize::_4: return extract<std::uint32_t>(first, last);
            case TypeSize::_8: return extract<std::uint64_t>(first, last);
            }
            return std::nullopt;
        }

        template<typename Iterator>
        std::optional<std::pair<std::size_t,
                                Iterator>>
        extractSize(Iterator first,
                    Iterator last,
                    TypeSize typeSize) {
            auto const byteCount = byteSize(typeSize);

            if(byteCount > static_cast<std::size_t>(std::distance(first, last))) {
                return std::nullopt;
            }

            auto optionalSize = extractUnsigned(first, last, typeSize);
            if(!optionalSize) { return std::nullopt; }

            return {
              {static_cast<std::size_t>(*optionalSize),
               std::next(first, static_cast<std::make_signed_t<std::size_t>>(byteCount))}
            };
        }

        template<typename Iterator>
        std::optional<std::int64_t> extractSigned(Iterator first,
                                                  Iterator last,
                                                  TypeSize typeSize) {
            switch(typeSize) {
            case TypeSize::_1: return extract<std::int8_t>(first, last);
            case TypeSize::_2: return extract<std::int16_t>(first, last);
            case TypeSize::_4: return extract<std::int32_t>(first, last);
            case TypeSize::_8: return extract<std::int64_t>(first, last);
            }
            return std::nullopt;
        }

        template<typename Iterator>
        std::optional<char> extractCharacter(Iterator first,
                                             Iterator last,
                                             TypeSize typeSize) {
            if(typeSize == TypeSize::_1) { return extract<char>(first, last); }
            return std::nullopt;
        }

        template<typename Iterator>
        std::optional<bool> extractBoolean(Iterator first,
                                           Iterator last,
                                           TypeSize typeSize) {
            auto const optionalUnsigned = extractUnsigned(first, last, typeSize);
            if(!optionalUnsigned) { return std::nullopt; }
            if(*optionalUnsigned != 0) { return true; }
            return false;
        }

        template<typename Iterator>
        std::optional<void const*> extractPointer(Iterator first,
                                                  Iterator last,
                                                  TypeSize typeSize) {
            auto const optionalUnsigned = extractUnsigned(first, last, typeSize);
            if(!optionalUnsigned) { return std::nullopt; }
            return std::bit_cast<void const*>(static_cast<std::uintptr_t>(*optionalUnsigned));
        }

        template<typename Iterator>
        std::optional<std::pair<trivial_t,
                                Iterator>>
        extractTrivial(Iterator    first,
                       Iterator    last,
                       TrivialType trivialType,
                       TypeSize    typeSize) {
            auto const byteCount = byteSize(typeSize);
            if(byteCount > static_cast<std::size_t>(std::distance(first, last))) {
                return std::nullopt;
            }
            std::optional<trivial_t> optionalTrivial;
            switch(trivialType) {
            case TrivialType::unsigned_:
                optionalTrivial = extractUnsigned(first, last, typeSize);
                break;
            case TrivialType::signed_:
                optionalTrivial = extractSigned(first, last, typeSize);
                break;
            case TrivialType::boolean:
                optionalTrivial = extractBoolean(first, last, typeSize);
                break;
            case TrivialType::character:
                optionalTrivial = extractCharacter(first, last, typeSize);
                break;
            case TrivialType::pointer:
                optionalTrivial = extractPointer(first, last, typeSize);
                break;
            case TrivialType::floatingpoint:
                {
                    auto optionalFloat = extractFloatingpoint(first, last, typeSize);
                    if(!optionalFloat) { return std::nullopt; }
                    std::visit([&](auto value) { optionalTrivial = value; }, *optionalFloat);
                }
                break;
            }
            if(!optionalTrivial) { return std::nullopt; }
            first += static_cast<std::make_signed_t<std::size_t>>(byteCount);
            return {
              {*optionalTrivial, first}
            };
        }

        template<typename Iterator>
        ParseResult<Iterator> extractAndFormatTrivial(Iterator         first,
                                                      Iterator         last,
                                                      std::string_view replacementField,
                                                      TrivialType      trivialType,
                                                      TypeSize         typeSize) {
            auto const optionalTrivial = extractTrivial(first, last, trivialType, typeSize);
            if(!optionalTrivial) { return std::nullopt; }
            first                  = optionalTrivial->second;
            auto const optionalStr = std::visit(
              [&](auto const& value) -> std::optional<std::string> {
                  try {
                      return fmt::format(fmt::runtime(replacementField), value);
                  } catch(std::exception const& e) {
                      errorMessagef(fmt::format(
                        "bad format for replacement field {:?}: {} (type: {}, size: {} bytes)",
                        replacementField,
                        e.what(),
                        magic_enum::enum_name(trivialType),
                        byteSize(typeSize)));
                      return std::nullopt;
                  }
              },
              optionalTrivial->first);
            if(!optionalStr) { return std::nullopt; }
            return {
              {*optionalStr, first}
            };
        }

        template<typename Iterator>
        ParseResult<Iterator> parseTrivial(Iterator         first,
                                           Iterator         last,
                                           std::string_view replacementField) {
            if(first == last) { return std::nullopt; }
            auto const trivialTypeId = parseTrivialTypeIdentifier(*first);
            if(!trivialTypeId) { return std::nullopt; }
            ++first;
            auto const [trivialType, typeSize] = *trivialTypeId;
            return extractAndFormatTrivial(first, last, replacementField, trivialType, typeSize);
        }

        template<typename Ratio>
        std::optional<std::string> formatTimeFixedRatio(std::int64_t     value,
                                                        TimeType         timeType,
                                                        std::string_view replacementField) {
            using duration = std::chrono::duration<std::int64_t, Ratio>;
            try {
                if(timeType == TimeType::duration) {
                    return fmt::format(fmt::runtime(replacementField), duration{value});
                }
            } catch(std::exception const& e) {
                errorMessagef(
                  fmt::format("bad format for replacement field {:?}: {} (timeType: {}, ratio: "
                              "{}/{}, value: {})",
                              replacementField,
                              e.what(),
                              magic_enum::enum_name(timeType),
                              Ratio::num,
                              Ratio::den,
                              value));
            }
            return std::nullopt;
        }

        std::optional<std::string> formatTime(std::uint64_t    num,
                                              std::uint64_t    den,
                                              std::int64_t     value,
                                              TimeType         timeType,
                                              std::string_view replacementField) {
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

            std::optional<std::string> oStr;
            bool                       failed = false;
            auto format_std_ratio = [&]<std::size_t I>(std::integral_constant<std::size_t, I>) {
                using ratio = typename std::tuple_element_t<I, std_ratios>::type;
                if(ratio::num == num && ratio::den == den) {
                    oStr = formatTimeFixedRatio<ratio>(value, timeType, replacementField);
                    if(!oStr) { failed = true; }
                    return false;
                }
                return true;
            };

            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                (format_std_ratio(std::integral_constant<std::size_t, Is>{}) && ...);
            }(std::make_index_sequence<std::tuple_size_v<std_ratios>>{});

            if(failed) { return std::nullopt; }
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

            //TODO not correct but otherwise the replacementField needs to be parsed further...
            try {
                auto const dur = std::chrono::duration<double>{
                  (static_cast<double>(value) * static_cast<double>(num))
                  / static_cast<double>(den)};
                return fmt::format(fmt::runtime(replacementField), dur);
            } catch(std::exception const& e) {
                errorMessagef(fmt::format(
                  "bad format for replacement field {:?}: {} (custom ratio: {}/{}, value: {})",
                  replacementField,
                  e.what(),
                  num,
                  den,
                  value));
                return std::nullopt;
            }
        }

        template<typename Iterator>
        ParseResult<Iterator> parseTime(Iterator         first,
                                        Iterator         last,
                                        std::string_view replacementField) {
            if(first == last) { return std::nullopt; }
            auto const trivialTypeId = parseTimeTypeIdentifier(*first);
            if(!trivialTypeId) { return std::nullopt; }
            ++first;
            auto const [timeType, numeratorTypeSize, denominatorTypeSize, timeSize]
              = *trivialTypeId;

            auto const byteCount
              = byteSize(numeratorTypeSize) + byteSize(denominatorTypeSize) + byteSize(timeSize);
            if(byteCount > static_cast<std::size_t>(std::distance(first, last))) {
                return std::nullopt;
            }

            auto const numeratorOpt = extractUnsigned(first, last, numeratorTypeSize);
            if(!numeratorOpt) { return std::nullopt; }
            std::uint64_t const numerator = *numeratorOpt;
            first += static_cast<std::make_signed_t<std::size_t>>(byteSize(numeratorTypeSize));
            auto const denominatorOpt = extractUnsigned(first, last, denominatorTypeSize);
            if(!denominatorOpt) { return std::nullopt; }
            std::uint64_t const denominator = *denominatorOpt;
            first += static_cast<std::make_signed_t<std::size_t>>(byteSize(denominatorTypeSize));
            auto const valueOpt = extractSigned(first, last, timeSizeToTypeSize(timeSize));
            if(!valueOpt) { return std::nullopt; }
            std::int64_t const value = *valueOpt;
            first += static_cast<std::make_signed_t<std::size_t>>(byteSize(timeSize));

            if(denominator == 0 || numerator == 0) { return std::nullopt; }

            auto const optionalTrivial
              = formatTime(numerator, denominator, value, timeType, replacementField);
            if(!optionalTrivial) { return std::nullopt; }

            return {
              {*optionalTrivial, first}
            };
        }

        template<typename Iterator>
        ParseResult<Iterator>
        parseCatalogedString(Iterator first,
                             Iterator,
                             std::size_t                            size,
                             RangeLayout                            rangeLayout,
                             std::string_view                       replacementField,
                             bool                                   in_list,
                             std::unordered_map<std::uint16_t,
                                                std::string> const& stringConstantsMap) {
            if(rangeLayout != RangeLayout::compact) { return std::nullopt; }

            auto const stringIt = stringConstantsMap.find(static_cast<std::uint16_t>(size));
            if(stringIt == stringConstantsMap.end()) {
                errorMessagef(fmt::format("cataloged string not found {}", size));
                return std::nullopt;
            }

            std::string const catalogedString = stringIt->second;

            try {
                auto const ret = [&]() {
                    if(in_list) {
                        return fmt::format(
                          fmt::runtime(replacementField),
                          (std::stringstream{} << std::quoted(catalogedString)).str());
                    }
                    return fmt::format(fmt::runtime(replacementField), catalogedString);
                }();
                return {
                  {ret, first}
                };
            } catch(std::exception const& e) {
                errorMessagef(
                  fmt::format("bad format for replacement field {:?}: {} (string: \"{}\", length: "
                              "{}, in_list: {})\n",
                              replacementField,
                              e.what(),
                              catalogedString,
                              catalogedString.length(),
                              in_list));
                return std::nullopt;
            }
        }

        template<typename Iterator>
        ParseResult<Iterator> parseString(Iterator         first,
                                          Iterator         last,
                                          std::size_t      size,
                                          RangeLayout      rangeLayout,
                                          std::string_view replacementField,
                                          bool             in_list) {
            if(size > static_cast<std::size_t>(std::distance(first, last))) { return std::nullopt; }
            if(rangeLayout != RangeLayout::compact) { return std::nullopt; }
            std::string parsedString;
            parsedString.resize(size);

            auto const string_end
              = std::next(first, static_cast<std::make_signed_t<std::size_t>>(size));
            std::transform(first, string_end, parsedString.begin(), [](auto byte) {
                return static_cast<char>(byte);
            });

            try {
                auto const ret = [&]() {
                    if(in_list) {
                        return fmt::format(
                          fmt::runtime(replacementField),
                          (std::stringstream{} << std::quoted(parsedString)).str());
                    }
                    return fmt::format(fmt::runtime(replacementField), parsedString);
                }();
                return {
                  {ret, string_end}
                };
            } catch(std::exception const& e) {
                errorMessagef(
                  fmt::format("bad format for replacement field {:?}: {} (string: \"{}\", size: "
                              "{}, in_list: {})",
                              replacementField,
                              e.what(),
                              parsedString,
                              size,
                              in_list));
                return std::nullopt;
            }
        }

        // NOTE: This function handles parsing of extended type identifiers.
        // Recursion is bounded by the depth of nested data structures in the serialized format.
        template<typename Iterator>
        ParseResult<Iterator>
        parseExtendedTypeIdentifier(Iterator                               first,
                                    Iterator                               last,
                                    std::size_t                            size,
                                    std::string_view                       replacementField,
                                    bool                                   in_map,
                                    bool                                   in_list,
                                    std::unordered_map<std::uint16_t,
                                                       std::string> const& stringConstantsMap) {
            if(size > std::numeric_limits<std::underlying_type_t<ExtendedTypeIdentifier>>::max()
               || !magic_enum::enum_contains<ExtendedTypeIdentifier>(
                 static_cast<std::underlying_type_t<ExtendedTypeIdentifier>>(size)))
            {
                return std::nullopt;
            }

            return magic_enum::enum_switch(
              [&](auto enumerator) {
                  static constexpr ExtendedTypeIdentifier eti = enumerator;
                  return ExtendedTypeIdentifierParser<eti>::parse(first,
                                                                  last,
                                                                  replacementField,
                                                                  in_map,
                                                                  in_list,
                                                                  stringConstantsMap,
                                                                  *this);
              },
              magic_enum::enum_value<ExtendedTypeIdentifier>(size));
        }

        struct RangeReplacementField {
            std::string rangeReplacementField;
            std::string childReplacementField;
        };

        std::optional<RangeReplacementField>
        fixRangeReplacementField(std::string_view replacementField) {
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
            rrf.rangeReplacementField += "}";

            rrf.childReplacementField = "{";
            rrf.childReplacementField += replacementField.substr(colonPos);

            return rrf;
        }

        // NOTE: This function parses tuple structures, which can contain nested elements.
        // Recursion depth is bounded by the nesting level of tuples in the serialized data.
        template<typename Iterator>
        ParseResult<Iterator>
        parseTuple(Iterator                               first,
                   Iterator                               last,
                   std::size_t                            size,
                   RangeLayout                            rangeLayout,
                   std::string_view                       replacementField,
                   bool                                   in_map,
                   std::unordered_map<std::uint16_t,
                                      std::string> const& stringConstantsMap) {
            if(rangeLayout != RangeLayout::on_ti_each) { return std::nullopt; }

            auto const oRangeRepField = fixRangeReplacementField(replacementField);
            if(!oRangeRepField) { return std::nullopt; }
            auto const& [rangeReplacementField, childReplacementField] = *oRangeRepField;

            if((in_map || rangeReplacementField.contains('m')) && size != 2) {
                return std::nullopt;
            }

            bool const printParenthesis = !in_map && !rangeReplacementField.contains('n')
                                       && !rangeReplacementField.contains('m');

            std::string tupleString = printParenthesis ? "(" : "";
            while(size != 0 && first != last) {
                auto const optionalStr = parseFromTypeId(first,
                                                         last,
                                                         childReplacementField,
                                                         true,
                                                         false,
                                                         stringConstantsMap);
                if(!optionalStr) { return std::nullopt; }
                tupleString += optionalStr->str;
                first = optionalStr->pos;
                --size;
                if(size != 0) {
                    if(in_map || rangeReplacementField.contains('m')) {
                        tupleString += ": ";
                    } else {
                        tupleString += ", ";
                    }
                }
            }

            if(size != 0) { return std::nullopt; }
            if(printParenthesis) { tupleString += ')'; }
            return {
              {tupleString, first}
            };
        }

        // NOTE: This function parses list/collection structures.
        // Recursion depth is bounded by the nesting level of lists in the serialized data.
        template<typename Iterator>
        ParseResult<Iterator> parseList(Iterator                               first,
                                        Iterator                               last,
                                        std::size_t                            size,
                                        RangeLayout                            rangeLayout,
                                        std::string_view                       replacementField,
                                        RangeType                              rangeType,
                                        std::unordered_map<std::uint16_t,
                                                           std::string> const& stringConstantsMap) {
            auto const oRangeRepField = fixRangeReplacementField(replacementField);
            if(!oRangeRepField) { return std::nullopt; }
            auto const& [rangeReplacementField, childReplacementField] = *oRangeRepField;

            bool const printParenthesis = !rangeReplacementField.contains('n');

            std::string listString;
            if(printParenthesis) {
                listString = rangeType == RangeType::list ? "[" : "{";
            } else {
                listString = "";
            }
            // Reserve space to reduce reallocations during string building
            listString.reserve((size * 10) + (printParenthesis ? 2 : 0));   // Rough estimate

            std::optional<std::tuple<TrivialType, TypeSize>> trivialTypeId;

            if(rangeLayout == RangeLayout::compact && size != 0 && first != last) {
                trivialTypeId = parseTrivialTypeIdentifier(*first);
                if(!trivialTypeId) { return std::nullopt; }

                ++first;
            }

            while(size != 0 && first != last) {
                auto const optionalStr = [&]() {
                    if(rangeLayout == RangeLayout::compact) {
                        auto [trivialType, typeSize] = *trivialTypeId;
                        return extractAndFormatTrivial(first,
                                                       last,
                                                       childReplacementField,
                                                       trivialType,
                                                       typeSize);
                    }
                    return parseFromTypeId(first,
                                           last,
                                           childReplacementField,
                                           true,
                                           rangeType == RangeType::map
                                             || rangeReplacementField.contains('m'),
                                           stringConstantsMap);
                }();
                if(!optionalStr) { return std::nullopt; }
                listString += optionalStr->str;
                first = optionalStr->pos;
                --size;
                if(size != 0) { listString += ", "; }
            }

            if(size != 0) { return std::nullopt; }
            if(printParenthesis) { listString += rangeType == RangeType::list ? ']' : '}'; }
            return {
              {listString, first}
            };
        }

        // NOTE: This function parses range structures which can contain nested elements.
        // Recursion depth is bounded by the nesting level of ranges in the serialized data.
        template<typename Iterator>
        ParseResult<Iterator>
        parseRange(Iterator                               first,
                   Iterator                               last,
                   std::string_view                       replacementField,
                   bool                                   in_list,
                   bool                                   in_map,
                   std::unordered_map<std::uint16_t,
                                      std::string> const& stringConstantsMap) {
            if(first == last) { return std::nullopt; }
            auto const rangeTypeId = parseRangeTypeIdentifier(*first);
            if(!rangeTypeId) { return std::nullopt; }
            ++first;
            auto const [rangeType, rangeSize, rangeLayout] = *rangeTypeId;

            auto optionalSize = extractSize(first, last, rangeSizeToTypeSize(rangeSize));
            if(!optionalSize) { return std::nullopt; }

            first = optionalSize->second;
            switch(rangeType) {
            case RangeType::cataloged_string:
                return parseCatalogedString(first,
                                            last,
                                            optionalSize->first,
                                            rangeLayout,
                                            replacementField,
                                            in_list,
                                            stringConstantsMap);
            case RangeType::string:
                return parseString(first,
                                   last,
                                   optionalSize->first,
                                   rangeLayout,
                                   replacementField,
                                   in_list);
            case RangeType::tuple:
                return parseTuple(first,
                                  last,
                                  optionalSize->first,
                                  rangeLayout,
                                  replacementField,
                                  in_map,
                                  stringConstantsMap);
            case RangeType::extendedTypeIdentifier:
                return parseExtendedTypeIdentifier(first,
                                                   last,
                                                   optionalSize->first,
                                                   replacementField,
                                                   in_map,
                                                   in_list,
                                                   stringConstantsMap);
            case RangeType::map: [[fallthrough]];
            case RangeType::set: [[fallthrough]];
            case RangeType::list:
                return parseList(first,
                                 last,
                                 optionalSize->first,
                                 rangeLayout,
                                 replacementField,
                                 rangeType,
                                 stringConstantsMap);
            }

            return std::nullopt;
        }

        // NOTE: This function dispatches parsing based on type identifiers.
        // Recursion depth is bounded by the complexity of nested type structures in the data.
        template<typename Iterator>
        ParseResult<Iterator> parseType(Iterator                               first,
                                        Iterator                               last,
                                        std::string_view                       replacementField,
                                        bool                                   in_list,
                                        bool                                   in_map,
                                        std::unordered_map<std::uint16_t,
                                                           std::string> const& stringConstantsMap) {
            if(first == last) { return std::nullopt; }
            auto const typeId = parseTypeIdentifier(*first);
            switch(typeId) {
            case TypeIdentifier::fmt_string: return std::nullopt;
            case TypeIdentifier::trivial:    return parseTrivial(first, last, replacementField);
            case TypeIdentifier::time:       return parseTime(first, last, replacementField);
            case TypeIdentifier::range:
                return parseRange(first,
                                  last,
                                  replacementField,
                                  in_list,
                                  in_map,
                                  stringConstantsMap);
            }
            return std::nullopt;
        }

        template<typename Iterator>
        ParseResult<Iterator>
        parseFmtString(Iterator                               first,
                       Iterator                               last,
                       FmtStringType                          type,
                       std::unordered_map<std::uint16_t,
                                          std::string> const& stringConstantsMap) {
            auto iterator             = first;
            auto optionalFmtRangeSize = parseFmtStringTypeIdentifier(*iterator, type);
            if(!optionalFmtRangeSize) { return std::nullopt; }
            ++iterator;
            auto optionalSize
              = extractSize(iterator, last, rangeSizeToTypeSize(*optionalFmtRangeSize));
            if(!optionalSize) { return std::nullopt; }

            iterator = optionalSize->second;

            auto const fmtStringSize = optionalSize->first;

            std::string fmtString;
            if(type == FmtStringType::normal || type == FmtStringType::sub) {
                if(fmtStringSize > static_cast<std::size_t>(std::distance(iterator, last))) {
                    return std::nullopt;
                }

                fmtString.resize(fmtStringSize);

                std::transform(
                  iterator,
                  std::next(iterator, static_cast<std::make_signed_t<std::size_t>>(fmtStringSize)),
                  fmtString.begin(),
                  [](auto byte) { return static_cast<char>(byte); });

                std::advance(iterator, fmtStringSize);
            } else {
                auto const fmtStringIt
                  = stringConstantsMap.find(static_cast<std::uint16_t>(fmtStringSize));
                if(fmtStringIt == stringConstantsMap.end()) {
                    errorMessagef(
                      fmt::format("cataloged format string not found {}", fmtStringSize));
                    return std::nullopt;
                }

                fmtString = fmtStringIt->second;
            }

            if(!checkReplacementFieldCount(fmtString)) { return std::nullopt; }

            if(!allCharsValid(fmtString)) { return std::nullopt; }
            return {
              {fmtString, iterator}
            };
        }

        // NOTE: This function handles format string parsing with nested arguments.
        // Recursion occurs when parsing nested format specifiers and is bounded by format complexity.
        template<typename Iterator>
        ParseResult<Iterator> parseFmt(Iterator                               first,
                                       Iterator                               last,
                                       FmtStringType                          type,
                                       std::unordered_map<std::uint16_t,
                                                          std::string> const& stringConstantsMap) {
            auto       iterator          = first;
            auto const optionalFmtString = parseFmtString(iterator, last, type, stringConstantsMap);

            if(!optionalFmtString) { return std::nullopt; }
            std::string_view fmtString = optionalFmtString->str;
            iterator                   = optionalFmtString->pos;

            std::string ret;
            // Reserve space based on format string length to reduce reallocations
            ret.reserve(fmtString.size() * 2);
            while(iterator != last) {
                auto const optionalReplacementField
                  = getNextReplacementFieldFromFmtStringAndAppendStrings(ret, fmtString);
                if(!optionalReplacementField) { break; }
                auto const optionalStr = parseFromTypeId(iterator,
                                                         last,
                                                         *optionalReplacementField,
                                                         false,
                                                         false,
                                                         stringConstantsMap);
                if(!optionalStr) { return std::nullopt; }
                iterator = optionalStr->pos;
                ret += optionalStr->str;
            }
            if(!fmtString.empty()) { return std::nullopt; }
            return {
              {ret, iterator}
            };
        }

        // NOTE: This is the main entry point for recursive parsing of data structures.
        // Recursion depth is naturally bounded by the structure of the serialized data being parsed.
        template<typename Iterator>
        ParseResult<Iterator>
        parseFromTypeId(Iterator                               first,
                        Iterator                               last,
                        std::string_view                       replacementField,
                        bool                                   in_list,
                        bool                                   in_map,
                        std::unordered_map<std::uint16_t,
                                           std::string> const& stringConstantsMap) {
            if(first == last) { return std::nullopt; }

            TypeIdentifier const typeId = static_cast<TypeIdentifier>(*first & std::byte{0x03});
            if(typeId == TypeIdentifier::fmt_string) {
                if(parseFmtStringTypeIdentifier(*first, FmtStringType::sub)) {
                    auto const optionalFmtTypeSize
                      = parseFmtStringTypeIdentifier(*first, FmtStringType::sub);
                    if(!optionalFmtTypeSize) { return std::nullopt; }
                    if(replacementField != "{}") { return std::nullopt; }
                    return parseFmt(first, last, FmtStringType::sub, stringConstantsMap);
                }
                auto const optionalFmtTypeSize
                  = parseFmtStringTypeIdentifier(*first, FmtStringType::cataloged_sub);
                if(!optionalFmtTypeSize) { return std::nullopt; }
                if(replacementField != "{}") { return std::nullopt; }
                return parseFmt(first, last, FmtStringType::cataloged_sub, stringConstantsMap);
            }
            return parseType(first, last, replacementField, in_list, in_map, stringConstantsMap);
        }
    };
}   // namespace detail

template<typename ErrorMessageF>
inline std::tuple<std::optional<std::string>,
                  std::span<std::byte const>,
                  std::size_t>
parse(std::span<std::byte const>             buffer,
      std::unordered_map<std::uint16_t,
                         std::string> const& stringConstantsMap,
      ErrorMessageF&&                        errorMessagef) {
    std::size_t unparsed_bytes{};
    while(!buffer.empty()) {
        auto const        iterator = std::ranges::find(buffer, protocol::Start_marker);
        std::size_t const offset
          = static_cast<std::size_t>(std::distance(buffer.begin(), iterator));
        buffer = buffer.subspan(offset);
        unparsed_bytes += offset;
        if(2 > buffer.size()
           || detail::parseFmtStringTypeIdentifier(buffer[1], detail::FmtStringType::normal)
           || detail::parseFmtStringTypeIdentifier(buffer[1],
                                                   detail::FmtStringType::cataloged_normal))
        {
            break;
        }
        unparsed_bytes += 1;
        buffer = buffer.subspan(1);
    }

    bool const contains_end = std::ranges::find(buffer, protocol::End_marker) != buffer.end();

    if(2 > buffer.size() || !contains_end) { return {std::nullopt, buffer, unparsed_bytes}; }

    detail::FmtStringType const fmtStringType = [&]() {
        if(detail::parseFmtStringTypeIdentifier(buffer[1], detail::FmtStringType::normal)) {
            return detail::FmtStringType::normal;
        }
        return detail::FmtStringType::cataloged_normal;
    }();

    detail::Parser parser{std::forward<ErrorMessageF>(errorMessagef)};
    auto const     optionalStr
      = parser.parseFmt(std::next(buffer.begin()), buffer.end(), fmtStringType, stringConstantsMap);

    if(!optionalStr) { return {std::nullopt, buffer, unparsed_bytes}; }
    if(optionalStr->pos == buffer.end() || *optionalStr->pos != protocol::End_marker) {
        return {std::nullopt, buffer, unparsed_bytes};
    }
    buffer = buffer.subspan(
      static_cast<std::size_t>(std::distance(buffer.begin(), optionalStr->pos + 1)));
    return {optionalStr->str, buffer, unparsed_bytes};
}
}   // namespace remote_fmt
