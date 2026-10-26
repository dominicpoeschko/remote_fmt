#pragma once

// The encoder's writer: Printer puts a frame (wire.hpp) - the id of a described string, then its arguments through
// their formatters - into a backend; format_to, the sub format string of a formatter that declares no descriptor.

#include "catalog.hpp"
#include "check.hpp"
#include "config.hpp"
#include "describe.hpp"
#include "detail/Lifetimebound.hpp"
#include "wire.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <string_constant/string_constant.hpp>
#include <string_view>
#include <type_traits>
#include <utility>

namespace remote_fmt {

// REMOTE_FMT_FRAME_CHECK (config.hpp, default 1): every frame carries the CRC-8 of wire.hpp. Its marker tells
// tools/extract_sites.py, which writes "FrameCheck" into the string catalog: a reader then requires the check, and
// refuses an unchecked frame instead of trusting it.

// A test build counts the bytes the encoder puts into a frame's CRC (each exactly once); nothing otherwise.
#ifndef REMOTE_FMT_CRC_HOOK
    #define REMOTE_FMT_CRC_HOOK(bytes) static_cast<void>(bytes)
#endif

namespace detail {

    // The generated id of a string constant, defined by the generated catalog, which a TU never sees: the one place
    // that silences clang's warning about it.
#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wundefined-func-template"
#endif
    template<typename CFS>
    [[gnu::always_inline]] inline catalog_id catalogIdOf() {
        return catalog<CFS>();
    }
#ifdef __clang__
    #pragma clang diagnostic pop
#endif
}   // namespace detail

template<typename Printer,
         char... Chars,
         typename... Args>
static constexpr auto format_to(Printer&                     printer,
                                sc::StringConstant<Chars...> fmt,
                                Args&&... args) {
    // checked once, in format(): a consteval check runs again at every call site it is written at
    return printer.format(fmt, std::forward<Args>(args)...);
}

template<typename ComBackend>
struct Printer {
private:
    static constexpr std::size_t stagingBytes = REMOTE_FMT_STAGING_BYTES;
    static constexpr bool        frameCheck   = REMOTE_FMT_FRAME_CHECK != 0;

    static_assert(stagingBytes == 0 || stagingBytes >= 10,
                  "a field (a 64-bit varint) must fit the staging bytes");

    // Every byte of a frame goes into its CRC once, here, as it is written; the record's end writes its last staged
    // bytes, the CRC and the end marker with Count false (endRecord has the staged bytes in the CRC already).
    template<bool        Count  = true,
             std::size_t Extent = std::dynamic_extent>
    void constexpr write(std::span<std::byte const,
                                   Extent> span) {
        if constexpr(frameCheck && Count) {
            crc = protocol::crcOver(crc, span);
            REMOTE_FMT_CRC_HOOK(span.size());
        }
        if constexpr(requires { ComBackend::write(span); }) {
            ComBackend::write(span);
        } else {
            comBackend.write(span);
        }
    }

    constexpr void flush() {
        if constexpr(stagingBytes != 0) {
            if(staged != 0) {
                write(std::span<std::byte const>{staging}.first(staged));
                staged = 0;
            }
        }
    }

    template<std::size_t Extent = std::dynamic_extent>
    void constexpr lowprint(std::span<std::byte const,
                                      Extent> span) {
        if constexpr(stagingBytes == 0) {
            write(span);
        } else {
            // the too-big case first: with a span of known size gcc then sees the copy below is dead, which it cannot
            // from `stagingBytes - staged` (it does not know staged <= stagingBytes; g++ 16 -O3 -Warray-bounds)
            if(span.size() > stagingBytes) {
                flush();
                write(span);
                return;
            }
            if(span.size() > stagingBytes - staged) { flush(); }
            std::ranges::copy(span,
                              std::next(staging.begin(), static_cast<std::ptrdiff_t>(staged)));
            staged += span.size();
        }
    }

    template<std::ranges::contiguous_range R>
        requires std::is_trivially_copyable_v<std::ranges::range_value_t<R>>
    constexpr void lowprint(R const& range) {
        lowprint(std::as_bytes(std::span{range}));
    }

    // one field of 1, 2, 4 or 8 bytes: out of line and passed in a register, so a field costs a
    // call at its formatter and nothing is copied through the stack
    template<std::unsigned_integral U>
    [[gnu::noinline]] constexpr void append(U const value) {
        auto const bytes = std::bit_cast<std::array<std::byte, sizeof(U)>>(value);
        if constexpr(stagingBytes == 0) {
            write(std::span<std::byte const, sizeof(U)>{bytes});
        } else {
            if(stagingBytes - staged < sizeof(U)) { flush(); }
            std::ranges::copy(bytes,
                              std::next(staging.begin(), static_cast<std::ptrdiff_t>(staged)));
            staged += sizeof(U);
        }
    }

    // LEB128: seven bits a byte, the high bit set on every byte but the last. Inlined into the two out-of-line
    // writers below (once per Printer, like append): an integer argument is a register and a call at its formatter.
    template<typename U>
    [[gnu::always_inline]] constexpr void leb128(U value) {
        if constexpr(stagingBytes == 0) {
            std::array<std::byte, (sizeof(U) * 8 + 6) / 7> bytes{};
            std::size_t                                    n = 0;
            while(value >= 0x80U) {
                bytes[n++] = static_cast<std::byte>(static_cast<std::uint8_t>(value) | 0x80U);
                value >>= 7U;
            }
            bytes[n++] = static_cast<std::byte>(static_cast<std::uint8_t>(value));
            write(std::span<std::byte const>{bytes}.first(n));
        } else {
            if(stagingBytes - staged < (sizeof(U) * 8 + 6) / 7) { flush(); }
            while(value >= 0x80U) {
                staging[staged++]
                  = static_cast<std::byte>(static_cast<std::uint8_t>(value) | 0x80U);
                value >>= 7U;
            }
            staging[staged++] = static_cast<std::byte>(static_cast<std::uint8_t>(value));
        }
    }

    [[gnu::noinline]] constexpr void varint32(std::uint32_t value) { leb128(value); }

    // a 64-bit length (size_t on a 64-bit host), nearly always below 2^32: the 32-bit loop then
    [[gnu::noinline]] constexpr void varint64(std::uint64_t value) {
        if((value >> 32U) == 0) {
            varint32(static_cast<std::uint32_t>(value));
        } else {
            leb128(value);
        }
    }

    template<typename Type>
    using FieldBits = std::conditional_t<
      sizeof(Type) == 1,
      std::uint8_t,
      std::conditional_t<sizeof(Type) == 2,
                         std::uint16_t,
                         std::conditional_t<sizeof(Type) == 4, std::uint32_t, std::uint64_t>>>;

    constexpr void printHelper(bool value) { append(static_cast<std::uint8_t>(value ? 1 : 0)); }

    template<typename Type>
        requires(std::is_trivially_copyable_v<Type>
                 && !std::is_same_v<Type,
                                    bool>)
    constexpr void printHelper(Type const& value) {
        if constexpr(sizeof(Type) == 1 || sizeof(Type) == 2 || sizeof(Type) == 4
                     || sizeof(Type) == 8)
        {
            append(std::bit_cast<FieldBits<Type>>(value));
        } else {
            lowprint(std::span<Type const, 1>{std::addressof(value), 1});
        }
    }

    friend struct detail::Access;

    template<typename Printer,
             char... Chars,
             typename... Args>
    friend constexpr auto format_to(Printer&                     printer,
                                    sc::StringConstant<Chars...> fmt,
                                    Args&&... args);

    // The reference to a described string (`fmt` 0x1e descriptors): its catalog id, or without a catalog inlineId,
    // its length and its bytes.
    template<char... Chars>
    [[gnu::always_inline]] constexpr void stringRef(sc::StringConstant<Chars...> text) {
        if constexpr(useCatalog) {
            append(static_cast<std::uint16_t>(detail::catalogIdOf<decltype(text)>()));
        } else {
            auto constexpr stringView = std::string_view{decltype(text){}};
            append(protocol::inlineId);
            detail::Access::varint(*this, stringView.size());
            lowprint(stringView);
        }
    }

    // a sub format string, `*` in its formatter's descriptor: the described string's id, then the arguments
    template<char... Chars,
             typename... Args>
    constexpr void format(sc::StringConstant<Chars...> fmt,
                          Args&&... args) {
        checkFormatString<decltype(args)...>(fmt);
        stringRef(described<Args...>(fmt));
        formatArguments(std::forward<Args>(args)...);
    }

    [[no_unique_address]] ComBackend comBackend{};
    // Default-initialized (`Printer printer;`, not `Printer{}`): only staged bytes are read, and
    // zeroing them would cost a memclr per message.
    std::array<std::byte, stagingBytes> staging;
    std::size_t                         staged{};

    // the running CRC of the frame's bytes written so far (REMOTE_FMT_FRAME_CHECK)
    struct NoCrc {};

    [[no_unique_address]] std::conditional_t<frameCheck, std::uint8_t, NoCrc> crc{};

public:
    constexpr Printer() = default;

    template<typename Cb>
        requires std::is_same_v<std::remove_cvref_t<Cb>,
                                std::remove_cvref_t<ComBackend>>
    constexpr explicit Printer(Cb&& callback) : comBackend{std::forward<Cb>(callback)} {}

    [[nodiscard]] ComBackend const& getComBackend() const REMOTE_FMT_LIFETIMEBOUND {
        return comBackend;
    }

    ComBackend& getComBackend() REMOTE_FMT_LIFETIMEBOUND { return comBackend; }

    // The writer vocabulary for a formatter (remote_fmt::formatter<T>::format(value, printer)): out(), value(x),
    // formatArguments(xs...). Nothing else of the wire is public.

    // the printer itself, as fmt's ctx.out(): `return format_to(printer.out(), "{} {}"_sc, a, b);`
    constexpr Printer& out() REMOTE_FMT_LIFETIMEBOUND { return *this; }

    // one argument, through its formatter: the value only (its descriptor is the formatter's)
    template<typename T>
    [[gnu::always_inline]] constexpr void value(T&& x) {
        formatter<std::remove_cvref_t<T>>{}.format(std::forward<T>(x), *this);
    }

    template<char... Chars,
             typename... Args>
    constexpr void print(sc::StringConstant<Chars...> fmt,
                         Args&&... args) {
        printAt(NoSite{}, fmt, std::forward<Args>(args)...);
    }

    // `site` must be the id of described<Args...>(fmt) (REMOTE_FMT_SITE()(described<...>(fmt))): the catalog's
    // string for it says how to read the values
    template<char... Chars,
             typename... Args>
    constexpr void print(SiteId                       site,
                         sc::StringConstant<Chars...> fmt,
                         Args&&... args) {
        printAt(site, fmt, std::forward<Args>(args)...);
    }

private:
    struct NoSite {};

    // with a SiteId the string never instantiates catalog<>, so it gets no generated id
    template<typename Site,
             char... Chars,
             typename... Args>
    constexpr void printAt([[maybe_unused]] Site        site,
                           sc::StringConstant<Chars...> fmt,
                           Args&&... args) {
        checkFormatString<decltype(args)...>(fmt);

        if constexpr(useCatalog) {
            // the string is only its id at run time: one body per argument list, not per string
            catalog_id id{};
            if constexpr(std::is_same_v<Site, SiteId>) {
                id = site.id;
            } else {
                id = detail::catalogIdOf<decltype(described<Args...>(fmt))>();
            }
            printCataloged(id, std::forward<Args>(args)...);
        } else {
            beginRecord();
            stringRef(described<Args...>(fmt));
            formatArguments(std::forward<Args>(args)...);
            endRecord();
        }
    }

    template<typename... Args>
    constexpr void printCataloged(catalog_id const id,
                                  Args&&... args) {
        beginCataloged(SiteId{id});
        formatArguments(std::forward<Args>(args)...);
        endCataloged();
    }

public:
    // A cataloged record in three pieces, for a caller that shares its start and its end between
    // argument lists (uc_log's RecordFrame): beginCataloged, any number of formatArguments, then
    // endCataloged. The site's id must be that of the described string of everything formatted in
    // between (described<Args...>(fmt)). checkCataloged is the compile-time half, which needs the
    // string and makes no code.
    constexpr void beginCataloged(SiteId site) {
        static_assert(useCatalog, "beginCataloged needs the catalog");
        beginRecord();
        append(static_cast<std::uint16_t>(site.id));
    }

    template<typename... Args>
    constexpr void formatArguments(Args&&... args) {
        (formatter<std::remove_cvref_t<Args>>{}.format(std::forward<Args>(args), *this), ...);
    }

    constexpr void endCataloged() { endRecord(); }

    // The same record without the catalog (REMOTE_FMT_USE_CATALOG=false, host builds): beginInline with the
    // described string of everything formatted before endInline (described<Args...>(fmt)), which the record then
    // carries inline instead of its id.
    template<char... Chars>
    constexpr void beginInline(sc::StringConstant<Chars...> text) {
        static_assert(!useCatalog, "beginInline is the record without the catalog");
        beginRecord();
        stringRef(text);
    }

    constexpr void endInline() { endRecord(); }

    template<typename... Args,
             char... Chars>
    static consteval void checkCataloged(sc::StringConstant<Chars...> fmt) {
        checkFormatString<Args...>(fmt);
    }

private:
    constexpr void beginRecord() {
        if constexpr(requires { ComBackend::initTransfer(); }) {
            ComBackend::initTransfer();
        } else if constexpr(requires { comBackend.initTransfer(); }) {
            comBackend.initTransfer();
        }
        if constexpr(frameCheck) {
            crc = protocol::crcInit;
            printHelper(protocol::checkedStartMarker);
        } else {
            printHelper(protocol::startMarker);
        }
    }

    constexpr void endRecord() {
        // the CRC (over the bytes still staged too; the CRC byte itself is not in it) and the end marker
        std::array<std::byte, 2> tail{};
        std::size_t              n = 0;
        if constexpr(frameCheck) {
            auto const pending = std::span<std::byte const>{staging}.first(staged);
            tail[n++] = static_cast<std::byte>(protocol::crcFinal(protocol::crcOver(crc, pending)));
            REMOTE_FMT_CRC_HOOK(pending.size());
        }
        tail[n++] = protocol::endMarker;
        // written uncounted: the staged bytes are in the CRC already, and the CRC and the end marker are no part of it
        if constexpr(stagingBytes == 0) {
            write<false>(std::span<std::byte const>{tail}.first(n));
        } else {
            if(stagingBytes - staged < n) {
                write<false>(std::span<std::byte const>{staging}.first(staged));
                staged = 0;
            }
            std::ranges::copy(std::span<std::byte const>{tail}.first(n),
                              std::next(staging.begin(), static_cast<std::ptrdiff_t>(staged)));
            staged += n;
            write<false>(std::span<std::byte const>{staging}.first(staged));
            staged = 0;
        }

        if constexpr(requires { ComBackend::finalizeTransfer(); }) {
            ComBackend::finalizeTransfer();
        } else if constexpr(requires { comBackend.finalizeTransfer(); }) {
            comBackend.finalizeTransfer();
        }
    }

    template<typename Site,
             char... Chars,
             typename... Args>
    static constexpr void staticPrintAt(Site                         site,
                                        sc::StringConstant<Chars...> fmt,
                                        Args&&... args) {
        // checked once, in printAt(): a consteval check runs again at every call site it is written at
        static_assert(
          requires { ComBackend::write(std::span<std::byte const>{}); },
          "staticPrint needs static ComBackend");

        Printer printer;
        printer.printAt(site, fmt, std::forward<Args>(args)...);
    }

public:
    template<char... Chars,
             typename... Args>
    static constexpr void staticPrint(sc::StringConstant<Chars...> fmt,
                                      Args&&... args) {
        staticPrintAt(NoSite{}, fmt, std::forward<Args>(args)...);
    }

    template<char... Chars,
             typename... Args>
    static constexpr void staticPrint(SiteId                       site,
                                      sc::StringConstant<Chars...> fmt,
                                      Args&&... args) {
        staticPrintAt(site, fmt, std::forward<Args>(args)...);
    }
};

}   // namespace remote_fmt
