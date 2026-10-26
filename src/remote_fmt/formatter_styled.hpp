#pragma once

// fmt::styled(...) arguments (REMOTE_FMT_USE_STYLED): the style goes out with the value (a flag byte, the colours, the
// emphasis), the value's shape is static.

#include "config.hpp"

#if REMOTE_FMT_USE_STYLED
    #include "describe.hpp"
    #include "detail/fmt_wall.hpp"
    #include "fmt_check.hpp"

    #include <cstdint>
    #include <string_constant/string_constant.hpp>

REMOTE_FMT_DETAIL_FMT_WALL_BEGIN
    #include <fmt/color.h>
REMOTE_FMT_DETAIL_FMT_WALL_END

namespace remote_fmt {

    #if REMOTE_FMT_USE_FMT_CHECK
namespace detail {
    // Both fmt and parser.hpp forward the spec to the payload's formatter.
    template<typename T>
    struct HostType<fmt::detail::styled_arg<T>> {
        using type = host_type_t<T>;
    };
}   // namespace detail
    #endif

template<typename T>
struct formatter<fmt::detail::styled_arg<T>> {
    static consteval auto descriptor() { return sc::StringConstant<'Y'>{} + descriptorOf<T>(); }

    template<typename Printer>
    constexpr void format(fmt::detail::styled_arg<T> const& styledArg,
                          Printer&                          printer) const {
        auto const& style = styledArg.style;

        std::uint8_t flags{};
        if(style.has_foreground()) {
            if(!style.get_foreground().is_terminal_color()) {
                flags |= 1U;
            } else {
                flags |= 2U;
            }
        }

        if(style.has_background()) {
            if(!style.get_background().is_terminal_color()) {
                flags |= 4U;
            } else {
                flags |= 8U;
            }
        }
        if(style.has_emphasis()) { flags |= 16U; }

        detail::Access::printHelper(printer, flags);

        auto addColor = [&](auto color) {
            if(!color.is_terminal_color()) {
                detail::Access::printHelper(printer, static_cast<std::uint32_t>(color.value()));
            } else {
                detail::Access::printHelper(printer, static_cast<std::uint8_t>(color.value()));
            }
        };

        if(style.has_foreground()) { addColor(style.get_foreground()); }
        if(style.has_background()) { addColor(style.get_background()); }
        if(style.has_emphasis()) {
            detail::Access::printHelper(printer, static_cast<std::uint8_t>(style.get_emphasis()));
        }

        formatter<T>{}.format(styledArg.value, printer);
    }
};

}   // namespace remote_fmt
#endif
