#pragma once

// The static descriptor of a log argument: what the host needs to read its value off the wire, as text built at
// compile time. Each formatter declares its own (`static consteval auto descriptor()`, next to the code that writes
// the value); describe.hpp's descriptorOf<T>() asks the formatter. The grammar (README.md "Descriptors"):
//
//   b c                  bool, char: 1 byte
//   u<n> i<n>            integer of n bytes: LEB128 for n = 4 (zigzag for i), else n bytes raw, little endian
//   f4 f8 p4 p8          float, double, pointer: raw
//   s                    string: LEB128 length + bytes
//   k<n>:<text>          a string constant: nothing on the wire
//   d<num>/<den><D>      duration / time_point: the count as D
//   O<D>  X<D><D>  v     optional, expected (engaged byte + value), void
//   V<n>:<D>...          variant: index byte + the alternative
//   L<D> S<D> M<D>       list, set, map: LEB128 size + elements
//   A<D>                 contiguous range of integers/floats/bytes: LEB128 size + the raw elements
//   (<D>...)             tuple
//   Q<n>:<symbol><D>     quantity: the number
//   Y<D>                 fmt::styled: flag byte, colours, value
//   F<n>:<fmt><k>:<D>... a sub format string with k arguments: the arguments
//   E<D><k>:<v>,<n>:<name>...   enum, G... bitflag enum: the raw value (D)
//   R<D><n>:<fmt><d>:<dim>,...<f>:<mask>,<k>:<v>,<n>:<name>...   register: the raw word as D
//   r<D><f>:<mask>,<n>:<name>...  Register::Flags: the raw word as D
//   #<16 hex>            a type entry of the catalog (its text is one of the above)
//   *                    self-described: the id of a sub format string that carries its own descriptor, its values
//
// Every free text is counted (<n>:<text>), so a descriptor needs no escaping. A call site's string is its format
// string, 0x1e, the descriptors of its arguments (the time stamp first).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_constant/string_constant.hpp>
#include <string_view>

namespace remote_fmt { namespace desc {

    // separates a format string from its arguments' descriptors in a cataloged string
    inline constexpr char separator = '\x1e';

    // A text built by a generator in two passes (count, then write): `Gen::build(TextBuilder&)`.
    struct TextBuilder {
        std::span<char> out{};
        bool            writing{};
        std::size_t     n{};

        constexpr void put(char c) {
            if(writing) { out[n] = c; }
            ++n;
        }

        constexpr void put(std::string_view s) {
            for(char const c : s) { put(c); }
        }

        constexpr void num(std::uint64_t v) {
            std::array<char, 20> digits{};
            std::size_t          k = 0;
            do {
                digits[k++] = static_cast<char>('0' + (v % 10U));
                v /= 10U;
            } while(v != 0);
            while(k != 0) { put(digits[--k]); }
        }

        constexpr void signedNum(std::int64_t v) {
            if(v < 0) {
                put('-');
                num(static_cast<std::uint64_t>(-(v + 1)) + 1U);
            } else {
                num(static_cast<std::uint64_t>(v));
            }
        }

        // <n>:<text>
        constexpr void counted(std::string_view s) {
            num(s.size());
            put(':');
            put(s);
        }
    };

    template<typename Gen>
    struct Built {
        static constexpr std::size_t size = [] {
            TextBuilder b{};
            Gen::build(b);
            return b.n;
        }();

        static constexpr std::array<char, size> chars = [] {
            std::array<char, size> a{};
            TextBuilder            b{a, true};
            Gen::build(b);
            return a;
        }();
    };

    // the text of a generator as a string constant
    template<typename Gen>
    consteval auto built() {
        return sc::create(
          [] { return std::string_view{Built<Gen>::chars.data(), Built<Gen>::size}; });
    }

    template<std::uint64_t N>
    struct NumberGen {
        static constexpr void build(TextBuilder& b) { b.num(N); }
    };

    template<std::uint64_t N>
    consteval auto number() {
        return built<NumberGen<N>>();
    }

    // <n>:<text>
    template<char... Cs>
    consteval auto counted(sc::StringConstant<Cs...>) {
        return number<sizeof...(Cs)>() + sc::StringConstant<':'>{} + sc::StringConstant<Cs...>{};
    }

    // FNV-1a, 64 bits: the key of a type entry (tools/extract_sites.py computes the same and fails the link on a
    // collision)
    consteval std::uint64_t fnv1a64(std::string_view text) {
        std::uint64_t h = 0xcbf29ce484222325ULL;
        for(char const c : text) {
            h ^= static_cast<std::uint8_t>(c);
            h *= 0x100000001b3ULL;
        }
        return h;
    }

    template<std::uint64_t H>
    struct HexGen {
        static constexpr void build(TextBuilder& b) {
            for(int shift = 60; shift >= 0; shift -= 4) {
                b.put("0123456789abcdef"[(H >> static_cast<unsigned>(shift)) & 0xFU]);
            }
        }
    };

    // #<16 hex digits>: the reference to a type entry
    template<typename Text>
    consteval auto typeKey() {
        return sc::StringConstant<'#'>{} + built<HexGen<fnv1a64(Text::stringView)>>();
    }

    // the descriptor of a single-byte-tag kind with its size: u4, i2, f8, p4
    template<char        Kind,
             std::size_t Size>
    consteval auto sized() {
        static_assert(Size == 1 || Size == 2 || Size == 4 || Size == 8, "1, 2, 4 or 8 bytes");
        return sc::StringConstant<Kind, static_cast<char>('0' + Size)>{};
    }

}}   // namespace remote_fmt::desc

namespace remote_fmt { namespace detail {
    // One tag per type entry: its symbol names the entry's text (`typeTag<sc::StringConstant<...>>()::
    // REMOTE_FMT_TYPE_TAG`), in a section that is not loaded on a target (the linker script's `remote_fmt_sites`
    // KEEP). A comdat function, so one per image whatever the number of TUs and Printers that write the type.
    template<typename Text>
    [[gnu::always_inline]] inline void typeTag() {
        [[gnu::used, gnu::section("remote_fmt_sites.types")]] static char REMOTE_FMT_TYPE_TAG{};
    }
}}   // namespace remote_fmt::detail
