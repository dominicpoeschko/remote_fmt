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

    template<typename It>
    struct ParseResult_ {
        std::string str;
        It          pos;
    };

    template<typename It>
    using ParseResult = std::optional<ParseResult_<It>>;

    template<ExtendedTypeIdentifier>
    struct ExtendedTypeIdentifierParser;

    template<>
    struct ExtendedTypeIdentifierParser<ExtendedTypeIdentifier::styled> {
        template<typename It,
                 typename Parser>
        static ParseResult<It> parse(It                                     first,
                                     It                                     last,
                                     std::string_view                       replacementField,
                                     bool                                   in_map,
                                     bool                                   in_list,
                                     std::unordered_map<std::uint16_t,
                                                        std::string> const& stringConstantsMap,
                                     Parser&                                parser) {
            if(1 > static_cast<std::size_t>(std::distance(first, last))) {
                return std::nullopt;
            }
            std::uint8_t const set = static_cast<std::uint8_t>(*first);
            ++first;

            if((set & 0xC0) != 0) {
                return std::nullopt;
            }
            bool const fg_rgb  = 0 != (set & 1);
            bool const fg_term = 0 != (set & 2);
            bool const bg_rgb  = 0 != (set & 4);
            bool const bg_term = 0 != (set & 8);
            bool const emp     = 0 != (set & 16);

            if((fg_rgb && fg_term) || (bg_rgb && bg_term)) {
                return std::nullopt;
            }
            fmt::text_style style{};

            auto extractColor = [&](bool rgb, bool term, auto gen) {
                if(rgb) {
                    auto const o = parser.extractSize(first, last, TypeSize::_4);
                    if(!o) {
                        return false;
                    }
                    first = o->second;
                    style |= gen(static_cast<fmt::color>(o->first));
                }
                if(term) {
                    auto const o = parser.extractSize(first, last, TypeSize::_1);
                    if(!o) {
                        return false;
                    }
                    first = o->second;
                    style |= gen(static_cast<fmt::terminal_color>(o->first));
                }
                return true;
            };
            if(!extractColor(fg_rgb, fg_term, [](auto v) { return fmt::fg(v); })) {
                return std::nullopt;
            }
            if(!extractColor(bg_rgb, bg_term, [](auto v) { return fmt::bg(v); })) {
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

            if(!inner_result) {
                return std::nullopt;
            }
            auto const s = fmt::format("{}", fmt::styled(inner_result->str, style));
            first        = inner_result->pos;
            return ParseResult_<It>{s, first};
        }
    };

    template<>
    struct ExtendedTypeIdentifierParser<ExtendedTypeIdentifier::optional> {
        template<typename It,
                 typename Parser>
        static ParseResult<It> parse(It                                     first,
                                     It                                     last,
                                     std::string_view                       replacementField,
                                     bool                                   in_map,
                                     bool                                   in_list,
                                     std::unordered_map<std::uint16_t,
                                                        std::string> const& stringConstantsMap,
                                     Parser&                                parser) {
            if(1 > static_cast<std::size_t>(std::distance(first, last))) {
                return std::nullopt;
            }
            std::uint8_t const isSet = static_cast<std::uint8_t>(*first);
            ++first;

            if(isSet != 0 && isSet != 1) {
                return std::nullopt;
            }

            if(isSet == 0) {
                return ParseResult_<It>{"()", first};
            }
            auto const inner_result = parser.parseFromTypeId(first,
                                                             last,
                                                             replacementField,
                                                             in_list,
                                                             in_map,
                                                             stringConstantsMap);

            if(!inner_result) {
                return std::nullopt;
            }
            return ParseResult_<It>{inner_result->str, inner_result->pos};
        }
    };

    struct Parser {
        std::function<void(std::string_view)> errorMessagef;

        template<typename ErrorMessageF>
        Parser(ErrorMessageF&& errorMessagef_)
          : errorMessagef{std::forward<ErrorMessageF>(errorMessagef_)} {}

        std::optional<std::string_view>
        getNextReplacementFieldFromFmtStringAndAppendStrings(std::string&      out,
                                                             std::string_view& fmtString) {
            while(!fmtString.empty()) {
                auto const curlyPos = std::find_if(fmtString.begin(), fmtString.end(), [](auto c) {
                    return c == '{' || c == '}';
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
                auto const closeCurlyPos = std::find(fmtString.begin(), fmtString.end(), '}');
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
                 typename It>
        T extract(It first,
                  It last) {
            static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
            if(static_cast<std::size_t>(std::distance(first, last)) < sizeof(T)) {
                throw std::runtime_error("Insufficient data for extraction");
            }
            T v;
            std::memcpy(&v, &*first, sizeof(T));
            return v;
        }

        template<typename It>
        std::optional<std::variant<double,
                                   float>>
        extractFloatingpoint(It       first,
                             It       last,
                             TypeSize ts) {
            if(ts == TypeSize::_4) {
                return extract<float>(first, last);
            }
            if(ts == TypeSize::_8) {
                return extract<double>(first, last);
            }
            return std::nullopt;
        }

        template<typename It>
        std::optional<std::uint64_t> extractUnsigned(It       first,
                                                     It       last,
                                                     TypeSize ts) {
            switch(ts) {
            case TypeSize::_1: return extract<std::uint8_t>(first, last);
            case TypeSize::_2: return extract<std::uint16_t>(first, last);
            case TypeSize::_4: return extract<std::uint32_t>(first, last);
            case TypeSize::_8: return extract<std::uint64_t>(first, last);
            }
            return std::nullopt;
        }

        template<typename It>
        std::optional<std::pair<std::size_t,
                                It>>
        extractSize(It       first,
                    It       last,
                    TypeSize ts) {
            auto const bs = byteSize(ts);

            if(bs > static_cast<std::size_t>(std::distance(first, last))) {
                return std::nullopt;
            }

            auto optionalSize = extractUnsigned(first, last, ts);
            if(!optionalSize) {
                return std::nullopt;
            }

            return {
              {static_cast<std::size_t>(*optionalSize),
               std::next(first, static_cast<std::make_signed_t<std::size_t>>(bs))}
            };
        }

        template<typename It>
        std::optional<std::int64_t> extractSigned(It       first,
                                                  It       last,
                                                  TypeSize ts) {
            switch(ts) {
            case TypeSize::_1: return extract<std::int8_t>(first, last);
            case TypeSize::_2: return extract<std::int16_t>(first, last);
            case TypeSize::_4: return extract<std::int32_t>(first, last);
            case TypeSize::_8: return extract<std::int64_t>(first, last);
            }
            return std::nullopt;
        }

        template<typename It>
        std::optional<char> extractCharacter(It       first,
                                             It       last,
                                             TypeSize ts) {
            if(ts == TypeSize::_1) {
                return extract<char>(first, last);
            }
            return std::nullopt;
        }

        template<typename It>
        std::optional<bool> extractBoolean(It       first,
                                           It       last,
                                           TypeSize ts) {
            auto const oU = extractUnsigned(first, last, ts);
            if(!oU) {
                return std::nullopt;
            }
            if(*oU != 0) {
                return true;
            }
            return false;
        }

        template<typename It>
        std::optional<void const*> extractPointer(It       first,
                                                  It       last,
                                                  TypeSize ts) {
            auto const oU = extractUnsigned(first, last, ts);
            if(!oU) {
                return std::nullopt;
            }
            return std::bit_cast<void const*>(static_cast<std::uintptr_t>(*oU));
        }

        template<typename It>
        std::optional<std::pair<trivial_t,
                                It>>
        extractTrivial(It          first,
                       It          last,
                       TrivialType tt,
                       TypeSize    ts) {
            auto const bs = byteSize(ts);
            if(bs > static_cast<std::size_t>(std::distance(first, last))) {
                return std::nullopt;
            }
            std::optional<trivial_t> optionalTrivial;
            switch(tt) {
            case TrivialType::unsigned_: optionalTrivial = extractUnsigned(first, last, ts); break;
            case TrivialType::signed_:   optionalTrivial = extractSigned(first, last, ts); break;
            case TrivialType::boolean:   optionalTrivial = extractBoolean(first, last, ts); break;
            case TrivialType::character: optionalTrivial = extractCharacter(first, last, ts); break;
            case TrivialType::pointer:   optionalTrivial = extractPointer(first, last, ts); break;
            case TrivialType::floatingpoint:
                {
                    auto of = extractFloatingpoint(first, last, ts);
                    if(!of) {
                        return std::nullopt;
                    }
                    std::visit([&](auto v) { optionalTrivial = v; }, *of);
                }
                break;
            }
            if(!optionalTrivial) {
                return std::nullopt;
            }
            first += static_cast<std::make_signed_t<std::size_t>>(bs);
            return {
              {*optionalTrivial, first}
            };
        }

        template<typename It>
        ParseResult<It> extractAndFormatTrivial(It               first,
                                                It               last,
                                                std::string_view replacementField,
                                                TrivialType      tt,
                                                TypeSize         ts) {
            auto const optionalTrivial = extractTrivial(first, last, tt, ts);
            if(!optionalTrivial) {
                return std::nullopt;
            }
            first                  = optionalTrivial->second;
            auto const optionalStr = std::visit(
              [&](auto const& v) -> std::optional<std::string> {
                  try {
                      return fmt::format(fmt::runtime(replacementField), v);
                  } catch(std::exception const& e) {
                      errorMessagef(fmt::format("bad format {}", e.what()));
                      return std::nullopt;
                  }
              },
              optionalTrivial->first);
            if(!optionalStr) {
                return std::nullopt;
            }
            return {
              {*optionalStr, first}
            };
        }

        template<typename It>
        ParseResult<It> parseTrivial(It               first,
                                     It               last,
                                     std::string_view replacementField) {
            if(first == last) {
                return std::nullopt;
            }
            auto const trivialTypeId = parseTrivialTypeIdentifier(*first);
            if(!trivialTypeId) {
                return std::nullopt;
            }
            ++first;
            auto const [tt, ts] = *trivialTypeId;
            return extractAndFormatTrivial(first, last, replacementField, tt, ts);
        }

        template<typename Ratio>
        std::optional<std::string> formatTimeFixedRatio(std::int64_t     value,
                                                        TimeType         tt,
                                                        std::string_view replacementField) {
            using duration = std::chrono::duration<std::int64_t, Ratio>;
            try {
                if(tt == TimeType::duration) {
                    return fmt::format(fmt::runtime(replacementField), duration{value});
                }
            } catch(std::exception const& e) {
                errorMessagef(fmt::format("bad format {}", e.what()));
            }
            return std::nullopt;
        }

        std::optional<std::string> formatTime(std::uint64_t    num,
                                              std::uint64_t    den,
                                              std::int64_t     value,
                                              TimeType         tt,
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
                    oStr = formatTimeFixedRatio<ratio>(value, tt, replacementField);
                    if(!oStr) {
                        failed = true;
                    }
                    return false;
                }
                return true;
            };

            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                (format_std_ratio(std::integral_constant<std::size_t, Is>{}) && ...);
            }(std::make_index_sequence<std::tuple_size_v<std_ratios>>{});

            if(failed) {
                return std::nullopt;
            }
            if(oStr) {
                return oStr;
            }

            if(replacementField == "{}" || replacementField == "{:%Q%q}") {
                if(den == 1) {
                    return fmt::format("{}[{}]s", value, num);
                }
                return fmt::format("{}[{}/{}]s", value, num, den);
            }
            if(replacementField == "{:%Q}") {
                return fmt::format("{}", value);
            }
            if(replacementField == "{:%q}") {
                if(den == 1) {
                    return fmt::format("[{}]s", num);
                }
                return fmt::format("[{}/{}]s", num, den);
            }

            //TODO not correct but otherwise the replacementField needs to be parsed further...
            try {
                auto const dur = std::chrono::duration<double>{
                  (static_cast<double>(value) * static_cast<double>(num))
                  / static_cast<double>(den)};
                return fmt::format(fmt::runtime(replacementField), dur);
            } catch(std::exception const& e) {
                errorMessagef(fmt::format("bad format {}", e.what()));
                return std::nullopt;
            }
        }

        template<typename It>
        ParseResult<It> parseTime(It               first,
                                  It               last,
                                  std::string_view replacementField) {
            if(first == last) {
                return std::nullopt;
            }
            auto const trivialTypeId = parseTimeTypeIdentifier(*first);
            if(!trivialTypeId) {
                return std::nullopt;
            }
            ++first;
            auto const [tt, num_ts, den_ts, ts] = *trivialTypeId;

            auto const bs = byteSize(num_ts) + byteSize(den_ts) + byteSize(ts);
            if(bs > static_cast<std::size_t>(std::distance(first, last))) {
                return std::nullopt;
            }

            std::uint64_t const num = *extractUnsigned(first, last, num_ts);
            first += static_cast<std::make_signed_t<std::size_t>>(byteSize(num_ts));
            std::uint64_t const den = *extractUnsigned(first, last, den_ts);
            first += static_cast<std::make_signed_t<std::size_t>>(byteSize(den_ts));
            std::int64_t const value = *extractSigned(first, last, timeSizeToTypeSize(ts));
            first += static_cast<std::make_signed_t<std::size_t>>(byteSize(ts));

            if(den == 0 || num == 0) {
                return std::nullopt;
            }

            auto const optionalTrivial = formatTime(num, den, value, tt, replacementField);
            if(!optionalTrivial) {
                return std::nullopt;
            }

            return {
              {*optionalTrivial, first}
            };
        }

        template<typename It>
        ParseResult<It>
        parseCatalogedString(It first,
                             It,
                             std::size_t                            size,
                             RangeLayout                            rl,
                             std::string_view                       replacementField,
                             bool                                   in_list,
                             std::unordered_map<std::uint16_t,
                                                std::string> const& stringConstantsMap) {
            if(rl != RangeLayout::compact) {
                return std::nullopt;
            }

            auto const stringIt = stringConstantsMap.find(static_cast<std::uint16_t>(size));
            if(stringIt == stringConstantsMap.end()) {
                errorMessagef(fmt::format("cataloged string not found {}", size));
                return std::nullopt;
            }

            std::string const s = stringIt->second;

            try {
                auto const ret = [&]() {
                    if(in_list) {
                        return fmt::format(fmt::runtime(replacementField),
                                           (std::stringstream{} << std::quoted(s)).str());
                    }
                    return fmt::format(fmt::runtime(replacementField), s);
                }();
                return {
                  {ret, first}
                };
            } catch(std::exception const& e) {
                errorMessagef(fmt::format("bad format {}\n", e.what()));
                return std::nullopt;
            }
        }

        template<typename It>
        ParseResult<It> parseString(It               first,
                                    It               last,
                                    std::size_t      size,
                                    RangeLayout      rl,
                                    std::string_view replacementField,
                                    bool             in_list) {
            if(size > static_cast<std::size_t>(std::distance(first, last))) {
                return std::nullopt;
            }
            if(rl != RangeLayout::compact) {
                return std::nullopt;
            }
            std::string s;
            s.resize(size);

            auto const string_end
              = std::next(first, static_cast<std::make_signed_t<std::size_t>>(size));
            std::transform(first, string_end, s.begin(), [](auto b) {
                return static_cast<char>(b);
            });

            try {
                auto const ret = [&]() {
                    if(in_list) {
                        return fmt::format(fmt::runtime(replacementField),
                                           (std::stringstream{} << std::quoted(s)).str());
                    }
                    return fmt::format(fmt::runtime(replacementField), s);
                }();
                return {
                  {ret, string_end}
                };
            } catch(std::exception const& e) {
                errorMessagef(fmt::format("bad format {}", e.what()));
                return std::nullopt;
            }
        }

        template<typename It>
        ParseResult<It>
        parseExtendedTypeIdentifier(It                                     first,
                                    It                                     last,
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
              [&](auto e) {
                  static constexpr ExtendedTypeIdentifier eti = e;
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

        template<typename It>
        ParseResult<It> parseTuple(It                                     first,
                                   It                                     last,
                                   std::size_t                            size,
                                   RangeLayout                            rl,
                                   std::string_view                       replacementField,
                                   bool                                   in_map,
                                   std::unordered_map<std::uint16_t,
                                                      std::string> const& stringConstantsMap) {
            if(rl != RangeLayout::on_ti_each) {
                return std::nullopt;
            }

            auto const oRangeRepField = fixRangeReplacementField(replacementField);
            if(!oRangeRepField) {
                return std::nullopt;
            }
            auto const& [rangeReplacementField, childReplacementField] = *oRangeRepField;

            if((in_map || rangeReplacementField.find('m') != std::string_view::npos) && size != 2) {
                return std::nullopt;
            }

            bool const printParenthesis = !in_map
                                       && rangeReplacementField.find('n') == std::string_view::npos
                                       && rangeReplacementField.find('m') == std::string_view::npos;

            std::string s = printParenthesis ? "(" : "";
            while(size != 0 && first != last) {
                auto const optionalStr = parseFromTypeId(first,
                                                         last,
                                                         childReplacementField,
                                                         true,
                                                         false,
                                                         stringConstantsMap);
                if(!optionalStr) {
                    return std::nullopt;
                }
                s += optionalStr->str;
                first = optionalStr->pos;
                --size;
                if(size != 0) {
                    if(in_map || rangeReplacementField.find('m') != std::string_view::npos) {
                        s += ": ";
                    } else {
                        s += ", ";
                    }
                }
            }

            if(size != 0) {
                return std::nullopt;
            }
            if(printParenthesis) {
                s += ')';
            }
            return {
              {s, first}
            };
        }

        template<typename It>
        ParseResult<It> parseList(It                                     first,
                                  It                                     last,
                                  std::size_t                            size,
                                  RangeLayout                            rl,
                                  std::string_view                       replacementField,
                                  RangeType                              rt,
                                  std::unordered_map<std::uint16_t,
                                                     std::string> const& stringConstantsMap) {
            auto const oRangeRepField = fixRangeReplacementField(replacementField);
            if(!oRangeRepField) {
                return std::nullopt;
            }
            auto const& [rangeReplacementField, childReplacementField] = *oRangeRepField;

            bool const printParenthesis = rangeReplacementField.find('n') == std::string_view::npos;

            std::string s = printParenthesis ? rt == RangeType::list ? "[" : "{" : "";
            // Reserve space to reduce reallocations during string building
            s.reserve(size * 10 + (printParenthesis ? 2 : 0));   // Rough estimate

            std::optional<std::tuple<TrivialType, TypeSize>> trivialTypeId;

            if(rl == RangeLayout::compact && size != 0 && first != last) {
                trivialTypeId = parseTrivialTypeIdentifier(*first);
                if(!trivialTypeId) {
                    return std::nullopt;
                }

                ++first;
            }

            while(size != 0 && first != last) {
                auto const optionalStr = [&]() {
                    if(rl == RangeLayout::compact) {
                        auto [tt, ts] = *trivialTypeId;
                        return extractAndFormatTrivial(first, last, childReplacementField, tt, ts);
                    }
                    return parseFromTypeId(first,
                                           last,
                                           childReplacementField,
                                           true,
                                           rt == RangeType::map
                                             || rangeReplacementField.find('m')
                                                  != std::string_view::npos,
                                           stringConstantsMap);
                }();
                if(!optionalStr) {
                    return std::nullopt;
                }
                s += optionalStr->str;
                first = optionalStr->pos;
                --size;
                if(size != 0) {
                    s += ", ";
                }
            }

            if(size != 0) {
                return std::nullopt;
            }
            if(printParenthesis) {
                s += rt == RangeType::list ? ']' : '}';
            }
            return {
              {s, first}
            };
        }

        template<typename It>
        ParseResult<It> parseRange(It                                     first,
                                   It                                     last,
                                   std::string_view                       replacementField,
                                   bool                                   in_list,
                                   bool                                   in_map,
                                   std::unordered_map<std::uint16_t,
                                                      std::string> const& stringConstantsMap) {
            if(first == last) {
                return std::nullopt;
            }
            auto const rangeTypeId = parseRangeTypeIdentifier(*first);
            if(!rangeTypeId) {
                return std::nullopt;
            }
            ++first;
            auto const [rt, rs, rl] = *rangeTypeId;

            auto optionalSize = extractSize(first, last, rangeSizeToTypeSize(rs));
            if(!optionalSize) {
                return std::nullopt;
            }

            first = optionalSize->second;
            switch(rt) {
            case RangeType::cataloged_string:
                return parseCatalogedString(first,
                                            last,
                                            optionalSize->first,
                                            rl,
                                            replacementField,
                                            in_list,
                                            stringConstantsMap);
            case RangeType::string:
                return parseString(first, last, optionalSize->first, rl, replacementField, in_list);
            case RangeType::tuple:
                return parseTuple(first,
                                  last,
                                  optionalSize->first,
                                  rl,
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
                                 rl,
                                 replacementField,
                                 rt,
                                 stringConstantsMap);
            }

            return std::nullopt;
        }

        template<typename It>
        ParseResult<It> parseType(It                                     first,
                                  It                                     last,
                                  std::string_view                       replacementField,
                                  bool                                   in_list,
                                  bool                                   in_map,
                                  std::unordered_map<std::uint16_t,
                                                     std::string> const& stringConstantsMap) {
            if(first == last) {
                return std::nullopt;
            }
            auto const ti = parseTypeIdentifier(*first);
            switch(ti) {
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

        template<typename It>
        ParseResult<It> parseFmtString(It                                     first,
                                       It                                     last,
                                       FmtStringType                          type,
                                       std::unordered_map<std::uint16_t,
                                                          std::string> const& stringConstantsMap) {
            auto it            = first;
            auto oFmtRangeSize = parseFmtStringTypeIdentifier(*it, type);
            if(!oFmtRangeSize) {
                return std::nullopt;
            }
            ++it;
            auto optionalSize = extractSize(it, last, rangeSizeToTypeSize(*oFmtRangeSize));
            if(!optionalSize) {
                return std::nullopt;
            }

            it = optionalSize->second;

            auto const fmtStringSize = optionalSize->first;

            std::string fmtString;
            if(type == FmtStringType::normal || type == FmtStringType::sub) {
                if(fmtStringSize > static_cast<std::size_t>(std::distance(it, last))) {
                    return std::nullopt;
                }

                fmtString.resize(fmtStringSize);

                std::transform(
                  it,
                  std::next(it, static_cast<std::make_signed_t<std::size_t>>(fmtStringSize)),
                  fmtString.begin(),
                  [](auto b) { return static_cast<char>(b); });

                std::advance(it, fmtStringSize);
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

            if(!checkReplacementFieldCount(fmtString)) {
                return std::nullopt;
            }

            if(!allCharsValid(fmtString)) {
                return std::nullopt;
            }
            return {
              {fmtString, it}
            };
        }

        template<typename It>
        ParseResult<It> parseFmt(It                                     first,
                                 It                                     last,
                                 FmtStringType                          type,
                                 std::unordered_map<std::uint16_t,
                                                    std::string> const& stringConstantsMap) {
            auto       it         = first;
            auto const oFmtString = parseFmtString(it, last, type, stringConstantsMap);

            if(!oFmtString) {
                return std::nullopt;
            }
            std::string_view fmtString = oFmtString->str;
            it                         = oFmtString->pos;

            std::string ret;
            // Reserve space based on format string length to reduce reallocations
            ret.reserve(fmtString.size() * 2);
            while(it != last) {
                auto const oReplacementField
                  = getNextReplacementFieldFromFmtStringAndAppendStrings(ret, fmtString);
                if(!oReplacementField) {
                    break;
                }
                auto const optionalStr
                  = parseFromTypeId(it, last, *oReplacementField, false, false, stringConstantsMap);
                if(!optionalStr) {
                    return std::nullopt;
                }
                it = optionalStr->pos;
                ret += optionalStr->str;
            }
            if(!fmtString.empty()) {
                return std::nullopt;
            }
            return {
              {ret, it}
            };
        }

        template<typename It>
        ParseResult<It> parseFromTypeId(It                                     first,
                                        It                                     last,
                                        std::string_view                       replacementField,
                                        bool                                   in_list,
                                        bool                                   in_map,
                                        std::unordered_map<std::uint16_t,
                                                           std::string> const& stringConstantsMap) {
            TypeIdentifier const ti = static_cast<TypeIdentifier>(*first & std::byte{0x03});
            if(ti == TypeIdentifier::fmt_string) {
                if(parseFmtStringTypeIdentifier(*first, FmtStringType::sub)) {
                    auto const oFmtTypeSize
                      = parseFmtStringTypeIdentifier(*first, FmtStringType::sub);
                    if(!oFmtTypeSize) {
                        return std::nullopt;
                    }
                    if(replacementField != "{}") {
                        return std::nullopt;
                    }
                    return parseFmt(first, last, FmtStringType::sub, stringConstantsMap);
                }
                auto const oFmtTypeSize
                  = parseFmtStringTypeIdentifier(*first, FmtStringType::cataloged_sub);
                if(!oFmtTypeSize) {
                    return std::nullopt;
                }
                if(replacementField != "{}") {
                    return std::nullopt;
                }
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
        auto const        it = std::find(buffer.begin(), buffer.end(), protocol::Start_marker);
        std::size_t const s  = static_cast<std::size_t>(std::distance(buffer.begin(), it));
        buffer               = buffer.subspan(s);
        unparsed_bytes += s;
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

    bool const contains_end
      = std::find(buffer.begin(), buffer.end(), protocol::End_marker) != buffer.end();

    if(2 > buffer.size() || !contains_end) {
        return {std::nullopt, buffer, unparsed_bytes};
    }

    detail::FmtStringType const fmtStringType = [&]() {
        if(detail::parseFmtStringTypeIdentifier(buffer[1], detail::FmtStringType::normal)) {
            return detail::FmtStringType::normal;
        }
        return detail::FmtStringType::cataloged_normal;
    }();

    detail::Parser parser{std::forward<ErrorMessageF>(errorMessagef)};
    auto const     optionalStr
      = parser.parseFmt(std::next(buffer.begin()), buffer.end(), fmtStringType, stringConstantsMap);

    if(!optionalStr) {
        return {std::nullopt, buffer, unparsed_bytes};
    }
    if(optionalStr->pos == buffer.end() || *optionalStr->pos != protocol::End_marker) {
        return {std::nullopt, buffer, unparsed_bytes};
    }
    buffer = buffer.subspan(
      static_cast<std::size_t>(std::distance(buffer.begin(), optionalStr->pos + 1)));
    return {optionalStr->str, buffer, unparsed_bytes};
}
}   // namespace remote_fmt
