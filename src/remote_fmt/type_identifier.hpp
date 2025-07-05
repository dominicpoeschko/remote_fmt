#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace remote_fmt { namespace detail {

    enum class TypeIdentifier : std::uint8_t { trivial, range, time, fmt_string };
    enum class FmtStringType : std::uint8_t { sub, normal, cataloged_sub, cataloged_normal };
    enum class TrivialType : std::uint8_t {
        unsigned_,
        signed_,
        boolean,
        character,
        pointer,
        floatingpoint
    };
    enum class RangeType : std::uint8_t {
        list,
        map,
        set,
        string,
        cataloged_string,
        tuple,
        extendedTypeIdentifier
    };
    enum class RangeLayout : std::uint8_t { compact, on_ti_each };
    enum class TimeType : std::uint8_t { duration, time_point };
    enum class TypeSize : std::uint8_t { _1, _2, _4, _8 };
    enum class RangeSize : std::uint8_t { _1, _2 };
    enum class TimeSize : std::uint8_t { _4, _8 };
    enum class ExtendedTypeIdentifier : std::uint8_t { styled, optional };

    template<typename T,
             typename Append>
    void appendSized(TypeSize ts,
                     T        v,
                     Append   append) {
        switch(ts) {
        case TypeSize::_1: append(static_cast<std::uint8_t>(v)); break;
        case TypeSize::_2: append(static_cast<std::uint16_t>(v)); break;
        case TypeSize::_4: append(static_cast<std::uint32_t>(v)); break;
        case TypeSize::_8: append(static_cast<std::uint64_t>(v)); break;
        }
    }

    template<typename T,
             typename Append>
    void appendSized(TimeSize ts,
                     T        v,
                     Append   append) {
        switch(ts) {
        case TimeSize::_4: append(static_cast<std::int32_t>(v)); break;
        case TimeSize::_8: append(static_cast<std::int64_t>(v)); break;
        }
    }

    template<typename T,
             typename Append>
    void appendSized(RangeSize rs,
                     T         v,
                     Append    append) {
        switch(rs) {
        case RangeSize::_1: append(static_cast<std::uint8_t>(v)); break;
        case RangeSize::_2: append(static_cast<std::uint16_t>(v)); break;
        }
    }

    template<typename T>
    constexpr TypeSize typeToTypeSize() {
        if constexpr(sizeof(T) == 8) {
            return TypeSize::_8;
        }
        if constexpr(sizeof(T) == 4) {
            return TypeSize::_4;
        }
        if constexpr(sizeof(T) == 2) {
            return TypeSize::_2;
        }
        if constexpr(sizeof(T) == 1) {
            return TypeSize::_1;
        }
    }

    constexpr std::size_t byteSize(TypeSize ts) {
        switch(ts) {
        case TypeSize::_1: return 1;
        case TypeSize::_2: return 2;
        case TypeSize::_4: return 4;
        case TypeSize::_8: return 8;
        }
        return 0;
    }

    constexpr std::size_t byteSize(TimeSize ts) {
        switch(ts) {
        case TimeSize::_4: return 4;
        case TimeSize::_8: return 8;
        }
        return 0;
    }

    constexpr std::size_t byteSize(RangeSize rs) {
        switch(rs) {
        case RangeSize::_1: return 1;
        case RangeSize::_2: return 2;
        }
        return 0;
    }

    constexpr TypeSize timeSizeToTypeSize(TimeSize ts) {
        switch(ts) {
        case TimeSize::_4: return TypeSize::_4;
        case TimeSize::_8: return TypeSize::_8;
        }
        return TypeSize::_8;
    }

    constexpr TypeSize rangeSizeToTypeSize(RangeSize rs) {
        switch(rs) {
        case RangeSize::_1: return TypeSize::_1;
        case RangeSize::_2: return TypeSize::_2;
        }
        return TypeSize::_1;
    }

    template<std::integral T>
    constexpr TypeSize sizeToTypeSize(T size) {
        static_assert(!std::is_signed_v<T>, "Type must be unsigned!");
        if(size > std::numeric_limits<std::uint32_t>::max()) {
            return TypeSize::_8;
        }
        if(size > std::numeric_limits<std::uint16_t>::max()) {
            return TypeSize::_4;
        }
        if(size > std::numeric_limits<std::uint8_t>::max()) {
            return TypeSize::_2;
        }
        return TypeSize::_1;
    }

    template<std::integral T>
    constexpr TimeSize sizeToTimeSize(T size) {
        static_assert(std::is_signed_v<T>, "Type must be signed!");
        if(size > std::numeric_limits<std::int32_t>::max()
           || size < std::numeric_limits<std::int32_t>::min())
        {
            return TimeSize::_8;
        }
        return TimeSize::_4;
    }

    template<std::integral T>
    constexpr RangeSize sizeToRangeSize(T size) {
        static_assert(!std::is_signed_v<T>, "Type must be unsigned!");
        if(size > std::numeric_limits<std::uint8_t>::max()) {
            return RangeSize::_2;
        }
        return RangeSize::_1;
    }

    template<TypeSize ts>
    using typeSize_unsigned_t = std::conditional_t<
      ts == TypeSize::_1,
      std::uint8_t,
      std::conditional_t<
        ts == TypeSize::_2,
        std::uint16_t,
        std::conditional_t<ts == TypeSize::_4,
                           std::uint32_t,
                           std::conditional_t<ts == TypeSize::_8, std::uint64_t, void>>>>;

    template<RangeSize rs>
    using rangeSize_unsigned_t
      = std::conditional_t<rs == RangeSize::_1, std::uint8_t, std::uint16_t>;

    template<typename T>
    constexpr std::byte castAndShift(T           v,
                                     std::size_t shift) {
        return static_cast<std::byte>(static_cast<std::uint8_t>(v)
                                      << static_cast<std::uint8_t>(shift));
    }

    template<auto... vs>
    struct Fail {
        static_assert(sizeof...(vs) == 0,
                      "Precondition failed!");
        static_assert(sizeof...(vs) != 0,
                      "Precondition failed!");
    };

    template<TypeIdentifier ti, auto... vs>
    static constexpr std::byte TypeId_v{Fail<vs...>::value};

    constexpr TypeIdentifier parseTypeIdentifier(std::byte v) {
        return static_cast<TypeIdentifier>(v & std::byte{0x03});
    }

    template<FmtStringType ft>
    static constexpr std::byte TypeId_v<TypeIdentifier::fmt_string, ft>{
      castAndShift(TypeIdentifier::fmt_string, 0) | castAndShift(ft, 4)};

    template<FmtStringType ft>
    constexpr std::byte fmtStringTypeIdentifier(RangeSize rs) {
        return TypeId_v<TypeIdentifier::fmt_string, ft> | castAndShift(rs, 2);
    }

    constexpr std::optional<RangeSize> parseFmtStringTypeIdentifier(std::byte     v,
                                                                    FmtStringType type) {
        TypeIdentifier const ti = parseTypeIdentifier(v);
        if(ti != detail::TypeIdentifier::fmt_string) {
            return std::nullopt;
        }

        FmtStringType const ft = static_cast<FmtStringType>((v & std::byte{0x30}) >> 4);
        if(ft != type) {
            return std::nullopt;
        }

        RangeSize const ts = static_cast<RangeSize>((v & std::byte{0x04}) >> 2);

        if(v
           != (detail::castAndShift(ti, 0) | detail::castAndShift(ft, 4)
               | detail::castAndShift(ts, 2)))
        {
            return std::nullopt;
        }

        return ts;
    }

    template<TrivialType tt, TypeSize ts>
    static constexpr std::byte TypeId_v<TypeIdentifier::trivial, tt, ts>{
      castAndShift(TypeIdentifier::trivial, 0) | castAndShift(ts, 2) | castAndShift(tt, 4)};

    template<TrivialType tt,
             TypeSize    ts>
    constexpr std::byte trivialTypeIdentifier() {
        return TypeId_v<TypeIdentifier::trivial, tt, ts>;
    }

    constexpr std::optional<std::pair<TrivialType,
                                      TypeSize>>
    parseTrivialTypeIdentifier(std::byte v) {
        TypeIdentifier const ti = parseTypeIdentifier(v);
        if(ti != TypeIdentifier::trivial) {
            return std::nullopt;
        }

        TrivialType const tt = static_cast<TrivialType>((v & std::byte{0x70}) >> 4);
        if(static_cast<std::uint8_t>(tt) > static_cast<std::uint8_t>(TrivialType::floatingpoint)) {
            return std::nullopt;
        }

        TypeSize const ts = static_cast<TypeSize>((v & std::byte{0x0C}) >> 2);

        if(v
           != (detail::castAndShift(ti, 0) | detail::castAndShift(tt, 4)
               | detail::castAndShift(ts, 2)))
        {
            return std::nullopt;
        }
        return {
          {tt, ts}
        };
    }

    template<RangeType rt, RangeLayout rl>
    static constexpr std::byte TypeId_v<TypeIdentifier::range, rt, rl>{
      castAndShift(TypeIdentifier::range, 0) | castAndShift(rt, 4) | castAndShift(rl, 7)};

    template<RangeType   rt,
             RangeLayout rl>
    constexpr std::byte rangeTypeIdentifier(RangeSize rs) {
        return TypeId_v<TypeIdentifier::range, rt, rl> | castAndShift(rs, 2);
    }

    constexpr std::optional<std::tuple<RangeType,
                                       RangeSize,
                                       RangeLayout>>
    parseRangeTypeIdentifier(std::byte v) {
        TypeIdentifier const ti = parseTypeIdentifier(v);
        if(ti != TypeIdentifier::range) {
            return std::nullopt;
        }

        RangeSize const   rs = static_cast<RangeSize>((v & std::byte{0x04}) >> 2);
        RangeType const   rt = static_cast<RangeType>((v & std::byte{0x70}) >> 4);
        RangeLayout const rl = static_cast<RangeLayout>((v & std::byte{0x80}) >> 7);

        if(static_cast<std::uint8_t>(rt)
           > static_cast<std::uint8_t>(RangeType::extendedTypeIdentifier))
        {
            return std::nullopt;
        }

        if(v
           != (detail::castAndShift(ti, 0) | detail::castAndShift(rs, 2)
               | detail::castAndShift(rt, 4) | detail::castAndShift(rl, 7)))
        {
            return std::nullopt;
        }
        return {
          {rt, rs, rl}
        };
    }

    template<TimeType tt, TypeSize num_ts, TypeSize den_ts>
    static constexpr std::byte TypeId_v<TypeIdentifier::time, tt, num_ts, den_ts>{
      castAndShift(TypeIdentifier::time, 0) | castAndShift(num_ts, 2) | castAndShift(den_ts, 4)
      | castAndShift(tt, 7)};

    template<TimeType tt,
             TypeSize num_ts,
             TypeSize den_ts>
    constexpr std::byte timeTypeIdentifier(TimeSize ts) {
        return TypeId_v<TypeIdentifier::time, tt, num_ts, den_ts> | castAndShift(ts, 6);
    }

    constexpr std::optional<std::tuple<TimeType,
                                       TypeSize,
                                       TypeSize,
                                       TimeSize>>
    parseTimeTypeIdentifier(std::byte v) {
        TypeIdentifier const ti = parseTypeIdentifier(v);
        if(ti != TypeIdentifier::time) {
            return std::nullopt;
        }

        TypeSize const num_ts = static_cast<TypeSize>((v & std::byte{0x0C}) >> 2);
        TypeSize const den_ts = static_cast<TypeSize>((v & std::byte{0x30}) >> 4);
        TimeSize const ts     = static_cast<TimeSize>((v & std::byte{0x40}) >> 6);
        TimeType const tt     = static_cast<TimeType>((v & std::byte{0x80}) >> 7);

        if(v
           != (detail::castAndShift(ti, 0) | detail::castAndShift(num_ts, 2)
               | detail::castAndShift(den_ts, 4) | detail::castAndShift(ts, 6)
               | detail::castAndShift(tt, 7)))
        {
            return std::nullopt;
        }
        return {
          {tt, num_ts, den_ts, ts}
        };
    }

    template<ExtendedTypeIdentifier eti,
             typename Append>
    void appendExtendedTypeIdentifier(Append append) {
        auto constexpr rs = detail::sizeToRangeSize(static_cast<std::size_t>(eti));
        auto constexpr ti = detail::rangeTypeIdentifier<detail::RangeType::extendedTypeIdentifier,
                                                        detail::RangeLayout::compact>(rs);
        append(ti);
        appendSized(rs, eti, append);
    }

}}   // namespace remote_fmt::detail
