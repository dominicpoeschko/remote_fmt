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
    void appendSized(TypeSize typeSize,
                     T        value,
                     Append   append) {
        switch(typeSize) {
        case TypeSize::_1: append(static_cast<std::uint8_t>(value)); break;
        case TypeSize::_2: append(static_cast<std::uint16_t>(value)); break;
        case TypeSize::_4: append(static_cast<std::uint32_t>(value)); break;
        case TypeSize::_8: append(static_cast<std::uint64_t>(value)); break;
        }
    }

    template<typename T,
             typename Append>
    void appendSized(TimeSize timeSize,
                     T        value,
                     Append   append) {
        switch(timeSize) {
        case TimeSize::_4: append(static_cast<std::int32_t>(value)); break;
        case TimeSize::_8: append(static_cast<std::int64_t>(value)); break;
        }
    }

    template<typename T,
             typename Append>
    void appendSized(RangeSize rangeSize,
                     T         value,
                     Append    append) {
        switch(rangeSize) {
        case RangeSize::_1: append(static_cast<std::uint8_t>(value)); break;
        case RangeSize::_2: append(static_cast<std::uint16_t>(value)); break;
        }
    }

    template<typename T>
    constexpr TypeSize typeToTypeSize() {
        if constexpr(sizeof(T) == 8) { return TypeSize::_8; }
        if constexpr(sizeof(T) == 4) { return TypeSize::_4; }
        if constexpr(sizeof(T) == 2) { return TypeSize::_2; }
        if constexpr(sizeof(T) == 1) { return TypeSize::_1; }
    }

    constexpr std::size_t byteSize(TypeSize typeSize) {
        switch(typeSize) {
        case TypeSize::_1: return 1;
        case TypeSize::_2: return 2;
        case TypeSize::_4: return 4;
        case TypeSize::_8: return 8;
        }
        return 0;
    }

    constexpr std::size_t byteSize(TimeSize timeSize) {
        switch(timeSize) {
        case TimeSize::_4: return 4;
        case TimeSize::_8: return 8;
        }
        return 0;
    }

    constexpr std::size_t byteSize(RangeSize rangeSize) {
        switch(rangeSize) {
        case RangeSize::_1: return 1;
        case RangeSize::_2: return 2;
        }
        return 0;
    }

    constexpr TypeSize timeSizeToTypeSize(TimeSize timeSize) {
        switch(timeSize) {
        case TimeSize::_4: return TypeSize::_4;
        case TimeSize::_8: return TypeSize::_8;
        }
        return TypeSize::_8;
    }

    constexpr TypeSize rangeSizeToTypeSize(RangeSize rangeSize) {
        switch(rangeSize) {
        case RangeSize::_1: return TypeSize::_1;
        case RangeSize::_2: return TypeSize::_2;
        }
        return TypeSize::_1;
    }

    template<std::integral T>
    constexpr TypeSize sizeToTypeSize(T size) {
        static_assert(!std::is_signed_v<T>, "Type must be unsigned!");
        if(size > std::numeric_limits<std::uint32_t>::max()) { return TypeSize::_8; }
        if(size > std::numeric_limits<std::uint16_t>::max()) { return TypeSize::_4; }
        if(size > std::numeric_limits<std::uint8_t>::max()) { return TypeSize::_2; }
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
        if(size > std::numeric_limits<std::uint8_t>::max()) { return RangeSize::_2; }
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
    constexpr std::byte castAndShift(T           value,
                                     std::size_t shift) {
        return static_cast<std::byte>(static_cast<std::uint8_t>(value)
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

    constexpr TypeIdentifier parseTypeIdentifier(std::byte value) {
        return static_cast<TypeIdentifier>(value & std::byte{0x03});
    }

    template<FmtStringType ft>
    static constexpr std::byte TypeId_v<TypeIdentifier::fmt_string, ft>{
      castAndShift(TypeIdentifier::fmt_string, 0) | castAndShift(ft, 4)};

    template<FmtStringType ft>
    constexpr std::byte fmtStringTypeIdentifier(RangeSize rangeSize) {
        return TypeId_v<TypeIdentifier::fmt_string, ft> | castAndShift(rangeSize, 2);
    }

    constexpr std::optional<RangeSize> parseFmtStringTypeIdentifier(std::byte     value,
                                                                    FmtStringType type) {
        TypeIdentifier const typeId = parseTypeIdentifier(value);
        if(typeId != detail::TypeIdentifier::fmt_string) { return std::nullopt; }

        FmtStringType const fmtType = static_cast<FmtStringType>((value & std::byte{0x30}) >> 4);
        if(fmtType != type) { return std::nullopt; }

        RangeSize const rangeSize = static_cast<RangeSize>((value & std::byte{0x04}) >> 2);

        if(value
           != (detail::castAndShift(static_cast<TypeIdentifier>(typeId), 0)
               | detail::castAndShift(static_cast<FmtStringType>(fmtType), 4)
               | detail::castAndShift(static_cast<RangeSize>(rangeSize), 2)))
        {
            return std::nullopt;
        }

        return rangeSize;
    }

    template<TrivialType tt, TypeSize ts>
    static constexpr std::byte TypeId_v<TypeIdentifier::trivial, tt, ts>{
      castAndShift(TypeIdentifier::trivial, 0) | castAndShift(static_cast<TypeSize>(ts), 2)
      | castAndShift(static_cast<TrivialType>(tt), 4)};

    template<TrivialType trivialType,
             TypeSize    typeSize>
    constexpr std::byte trivialTypeIdentifier() {
        return TypeId_v<TypeIdentifier::trivial,
                        static_cast<TrivialType>(trivialType),
                        static_cast<TypeSize>(typeSize)>;
    }

    constexpr std::optional<std::pair<TrivialType,
                                      TypeSize>>
    parseTrivialTypeIdentifier(std::byte value) {
        TypeIdentifier const typeId = parseTypeIdentifier(value);
        if(typeId != TypeIdentifier::trivial) { return std::nullopt; }

        TrivialType const trivialType = static_cast<TrivialType>((value & std::byte{0x70}) >> 4);
        if(static_cast<std::uint8_t>(trivialType)
           > static_cast<std::uint8_t>(TrivialType::floatingpoint))
        {
            return std::nullopt;
        }

        TypeSize const typeSize = static_cast<TypeSize>((value & std::byte{0x0C}) >> 2);

        if(value
           != (detail::castAndShift(static_cast<TypeIdentifier>(typeId), 0)
               | detail::castAndShift(static_cast<TrivialType>(trivialType), 4)
               | detail::castAndShift(static_cast<TypeSize>(typeSize), 2)))
        {
            return std::nullopt;
        }
        return {
          {trivialType, typeSize}
        };
    }

    template<RangeType rt, RangeLayout rl>
    static constexpr std::byte TypeId_v<TypeIdentifier::range, rt, rl>{
      castAndShift(TypeIdentifier::range, 0) | castAndShift(static_cast<RangeType>(rt), 4)
      | castAndShift(static_cast<RangeLayout>(rl), 7)};

    template<RangeType   rangeType,
             RangeLayout rangeLayout>
    constexpr std::byte rangeTypeIdentifier(RangeSize rangeSize) {
        return TypeId_v<TypeIdentifier::range,
                        static_cast<RangeType>(rangeType),
                        static_cast<RangeLayout>(rangeLayout)>
             | castAndShift(rangeSize, 2);
    }

    constexpr std::optional<std::tuple<RangeType,
                                       RangeSize,
                                       RangeLayout>>
    parseRangeTypeIdentifier(std::byte value) {
        TypeIdentifier const typeId = parseTypeIdentifier(value);
        if(typeId != TypeIdentifier::range) { return std::nullopt; }

        RangeSize const   rangeSize   = static_cast<RangeSize>((value & std::byte{0x04}) >> 2);
        RangeType const   rangeType   = static_cast<RangeType>((value & std::byte{0x70}) >> 4);
        RangeLayout const rangeLayout = static_cast<RangeLayout>((value & std::byte{0x80}) >> 7);

        if(static_cast<std::uint8_t>(rangeType)
           > static_cast<std::uint8_t>(RangeType::extendedTypeIdentifier))
        {
            return std::nullopt;
        }

        if(value
           != (detail::castAndShift(static_cast<TypeIdentifier>(typeId), 0)
               | detail::castAndShift(static_cast<RangeSize>(rangeSize), 2)
               | detail::castAndShift(static_cast<RangeType>(rangeType), 4)
               | detail::castAndShift(static_cast<RangeLayout>(rangeLayout), 7)))
        {
            return std::nullopt;
        }
        return {
          {rangeType, rangeSize, rangeLayout}
        };
    }

    template<TimeType tt, TypeSize num_ts, TypeSize den_ts>
    static constexpr std::byte TypeId_v<TypeIdentifier::time, tt, num_ts, den_ts>{
      castAndShift(TypeIdentifier::time, 0) | castAndShift(num_ts, 2) | castAndShift(den_ts, 4)
      | castAndShift(tt, 7)};

    template<TimeType timeType,
             TypeSize num_ts,
             TypeSize den_ts>
    constexpr std::byte timeTypeIdentifier(TimeSize timeSize) {
        return TypeId_v<TypeIdentifier::time, timeType, num_ts, den_ts> | castAndShift(timeSize, 6);
    }

    constexpr std::optional<std::tuple<TimeType,
                                       TypeSize,
                                       TypeSize,
                                       TimeSize>>
    parseTimeTypeIdentifier(std::byte value) {
        TypeIdentifier const typeId = parseTypeIdentifier(value);
        if(typeId != TypeIdentifier::time) { return std::nullopt; }

        TypeSize const num_ts   = static_cast<TypeSize>((value & std::byte{0x0C}) >> 2);
        TypeSize const den_ts   = static_cast<TypeSize>((value & std::byte{0x30}) >> 4);
        TimeSize const timeSize = static_cast<TimeSize>((value & std::byte{0x40}) >> 6);
        TimeType const timeType = static_cast<TimeType>((value & std::byte{0x80}) >> 7);

        if(value
           != (detail::castAndShift(static_cast<TypeIdentifier>(typeId), 0)
               | detail::castAndShift(static_cast<TypeSize>(num_ts), 2)
               | detail::castAndShift(static_cast<TypeSize>(den_ts), 4)
               | detail::castAndShift(static_cast<TimeSize>(timeSize), 6)
               | detail::castAndShift(static_cast<TimeType>(timeType), 7)))
        {
            return std::nullopt;
        }
        return {
          {timeType, num_ts, den_ts, timeSize}
        };
    }

    template<ExtendedTypeIdentifier eti,
             typename Append>
    void appendExtendedTypeIdentifier(Append append) {
        auto constexpr rangeSize = detail::sizeToRangeSize(static_cast<std::size_t>(eti));
        auto constexpr typeId
          = detail::rangeTypeIdentifier<detail::RangeType::extendedTypeIdentifier,
                                        detail::RangeLayout::compact>(rangeSize);
        append(typeId);
        appendSized(rangeSize, eti, append);
    }

}}   // namespace remote_fmt::detail
