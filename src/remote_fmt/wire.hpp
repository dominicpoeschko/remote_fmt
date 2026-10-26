#pragma once

// What the encoder (remote_fmt.hpp) and the decoder (parser.hpp) agree on, and nothing else: the frame's markers and
// its CRC, the characters a string may hold, the bounds of a replacement field, and which frames a reader takes.
// The standard library only, so a reader includes it without the encoder and the encoder without fmt.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace remote_fmt {

// Protocol constants. A frame is
//   start | id (2 bytes, little endian) | the values, as the id's descriptor says | [CRC-8] | end
// No type bytes: the string behind the id (a call site's, or a cataloged one) carries the descriptor of every value
// (descriptor.hpp), so only data goes out.
namespace protocol {
    // A checked frame (REMOTE_FMT_FRAME_CHECK, the default): this start marker, the frame, then one byte of CRC-8 over
    // every byte from the start marker on, then the end marker.
    static constexpr std::byte checkedStartMarker{0x57};
    // an unchecked frame (REMOTE_FMT_FRAME_CHECK=0)
    static constexpr std::byte startMarker{0x58};
    static constexpr std::byte endMarker{0xAA};

    constexpr bool isStartMarker(std::byte b) {
        return b == startMarker || b == checkedStartMarker;
    }

    // The id that says "the string follows inline": a LEB128 length and its bytes (REMOTE_FMT_USE_CATALOG=false).
    // tools/extract_sites.py fails the link of an image with a call site at this id.
    inline constexpr std::uint16_t inlineId = 0xFFFF;

    // CRC-8/AUTOSAR (polynomial 0x2F, init 0xFF, final xor 0xFF): Hamming distance 4 up to 119 data bits and 3 up to
    // 2048 - every 1-, 2- and 3-bit error in a frame of up to 15 bytes, and every burst of 8 bits or less in any frame.
    // A table of 256 entries, one load a byte: 256 B of flash for ~85 cycles a line against the 16-entry nibble table
    // (two loads and ~10 instructions a byte).
    inline constexpr std::uint8_t crcInit = 0xFF;

    inline constexpr std::array<std::uint8_t, 256> crcTable = [] {
        std::array<std::uint8_t, 256> t{};
        for(unsigned n = 0; n < 256; ++n) {
            unsigned c = n;
            for(int k = 0; k < 8; ++k) {
                c = (c & 0x80U) != 0 ? ((c << 1U) ^ 0x2FU) & 0xFFU : (c << 1U) & 0xFFU;
            }
            t[n] = static_cast<std::uint8_t>(c);
        }
        return t;
    }();

    constexpr std::uint8_t crcStep(std::uint8_t crc,
                                   std::byte    b) {
        return crcTable[static_cast<unsigned>(crc) ^ std::to_integer<unsigned>(b)];
    }

    constexpr std::uint8_t crcOver(std::uint8_t               crc,
                                   std::span<std::byte const> bytes) {
        for(auto const b : bytes) { crc = crcStep(crc, b); }
        return crc;
    }

    constexpr std::uint8_t crcFinal(std::uint8_t crc) {
        return static_cast<std::uint8_t>(crc ^ 0xFFU);
    }
}   // namespace protocol

// What the reader knows about the image's frames, from its catalog ("FrameCheck", tools/extract_sites.py): `required`
// - the image checks every frame, so only checked frames (0x57) are frames, and an unchecked one is refused; `none` -
// an image built without the check: only 0x58 frames; `either` - nothing known (a catalog without "Sites" yet, tests).
enum class FrameCheck { either, required, none };

constexpr bool isFrameStart(std::byte  b,
                            FrameCheck expect) {
    switch(expect) {
    case FrameCheck::required: return b == protocol::checkedStartMarker;
    case FrameCheck::none:     return b == protocol::startMarker;
    case FrameCheck::either:   break;
    }
    return protocol::isStartMarker(b);
}

namespace detail {

    constexpr bool isValidChar(char character) {
        if(character > '~') { return false; }
        if(character < ' ') { return character == '\n'; }
        return true;
    }

    constexpr bool allCharsValid(std::string_view stringView) {
        return std::ranges::all_of(stringView, isValidChar);
    }

    // Inline strings travel the wire and stay ASCII; a cataloged one never does, so it may hold
    // UTF-8 (unit symbols) but still no control characters.
    constexpr bool isValidCatalogedChar(char character) {
        return static_cast<unsigned char>(character) >= 0x80 || isValidChar(character);
    }

    constexpr bool allCharsValidCataloged(std::string_view stringView) {
        return std::ranges::all_of(stringView, isValidCatalogedChar);
    }

    // A replacement field carrying no format spec at all. fmt applies its debug format to strings
    // and chars nested inside a range or tuple only while the spec is absent - an explicit spec
    // replaces it - so the parser has to distinguish this exact field from every other one.
    static constexpr std::string_view defaultReplacementField{"{}"};

    // Upper bound on any number inside a replacement field. fmt allocates width and precision
    // padding eagerly, so without a bound a 27-byte wire message carrying "{:2147483647}" makes
    // the host allocate 2 GiB in a single malloc - an amplification of roughly 80,000,000x from
    // data a device controls.
    //
    // 4096 is far above anything a log line legitimately needs and still leaves room for the
    // largest useful precision: the exact decimal expansion of a double runs to ~767 significant
    // digits, so "{:.1100f}" style specs keep working.
    //
    // Only the parse side enforces this. A device's own format strings are compiled in and
    // trusted; the untrusted input is the byte stream arriving at the host.
    static constexpr std::size_t maxReplacementFieldNumber{4096};

    // Checked per replacement field rather than over the whole format string: literal text
    // legitimately contains large numbers ("reboot count 1000000"), only the spec must be bounded.
    // A nested brace - a dynamic width or precision, "{:{}}" or "{0:{0}}", whose number would come
    // from an argument at run time and bypass this bound - never gets here: the scanner reports it
    // (format_grammar.hpp, ScanError::nested). "{0:{0}}" was the dangerous one: it reuses the
    // argument as its own width, so a single 8-byte integer off the wire asked fmt for a
    // multi-gigabyte allocation (found by fuzzing). Range element specs use colons, not braces, so
    // "{::>5}" and "{:::>4}" are unaffected.
    constexpr bool replacementFieldWithinLimits(std::string_view replacementField) {
        std::size_t value    = 0;
        bool        inNumber = false;
        for(char const character : replacementField) {
            if(character < '0' || character > '9') {
                inNumber = false;
                continue;
            }
            if(!inNumber) {
                value    = 0;
                inNumber = true;
            }
            value = (value * 10U) + static_cast<std::size_t>(character - '0');
            if(value > maxReplacementFieldNumber) { return false; }
        }
        return true;
    }

}   // namespace detail

}   // namespace remote_fmt
