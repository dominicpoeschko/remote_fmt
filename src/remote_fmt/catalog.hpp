#pragma once
#include <cstdint>

// A cataloged string travels as a 16-bit id:
// - catalog<CFS>(): 0x0000-0x7FFF, generated before the link (tools/generate_string_constants.py).
// - REMOTE_FMT_SITE(): 0x8000-0xFFFF, 0x8000 + the address of a tag whose symbol names the string
//   and the enclosing function; tools/extract_sites.py adds both to the catalog after the link.
//   The target's linker script needs
//     remote_fmt_sites 0 (INFO) : { KEEP(*(remote_fmt_sites remote_fmt_sites.*)) }
//   A host executable gets cmake/remote_fmt_sites_host.ld and counts from siteAnchor.

namespace remote_fmt {
using catalog_id = std::uint16_t;

template<typename CFS>
catalog_id catalog();

struct SiteId {
    catalog_id id;
};

namespace detail {
    inline constexpr catalog_id FirstSiteId = 0x8000;

#if UINTPTR_MAX > 0xFFFFFFFFU
    [[gnu::used, gnu::section("remote_fmt_sites")]] inline char siteAnchor{};
#endif

    inline catalog_id siteId(char const* tag) {
        auto const address = reinterpret_cast<std::uintptr_t>(tag);
#if UINTPTR_MAX > 0xFFFFFFFFU
        auto const offset = address - reinterpret_cast<std::uintptr_t>(&siteAnchor);
        return static_cast<catalog_id>(FirstSiteId | (offset & 0x7FFFU));
#else
        return static_cast<catalog_id>(address + FirstSiteId);
#endif
    }
}   // namespace detail
}   // namespace remote_fmt

#define REMOTE_FMT_DETAIL_STRINGIFY2(x) #x
#define REMOTE_FMT_DETAIL_STRINGIFY(x)  REMOTE_FMT_DETAIL_STRINGIFY2(x)

// a section per site: gcc gives "section type conflict" for comdat and plain statics in one
#ifdef __clang__
    #define REMOTE_FMT_DETAIL_COUNTER_PUSH                                                        \
        _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wc2y-extensions\"")
    #define REMOTE_FMT_DETAIL_COUNTER_POP _Pragma("clang diagnostic pop")
#else
    #define REMOTE_FMT_DETAIL_COUNTER_PUSH
    #define REMOTE_FMT_DETAIL_COUNTER_POP
#endif

// `REMOTE_FMT_SITE()(fmt)`: the id of `fmt` at this call site. The tag is writable because LTO
// merges identical constants. Without a catalog the string travels inline: no tag, id 0.
#if defined(REMOTE_FMT_USE_CATALOG) && !(REMOTE_FMT_USE_CATALOG)
    #define REMOTE_FMT_SITE() [](auto const&) -> ::remote_fmt::catalog_id { return 0; }
#else
    #define REMOTE_FMT_SITE()                                               \
        []<typename REMOTE_FMT_DETAIL_FMT>(                                 \
          REMOTE_FMT_DETAIL_FMT const&) -> ::remote_fmt::catalog_id {       \
            REMOTE_FMT_DETAIL_COUNTER_PUSH                                  \
            [[gnu::used,                                                    \
              gnu::section("remote_fmt_sites." REMOTE_FMT_DETAIL_STRINGIFY( \
                __COUNTER__))]] static char REMOTE_FMT_SITE_TAG{};          \
            REMOTE_FMT_DETAIL_COUNTER_POP                                   \
            return ::remote_fmt::detail::siteId(&REMOTE_FMT_SITE_TAG);      \
        }
#endif
