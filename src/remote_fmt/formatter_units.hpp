#pragma once

// mp-units quantities and quantity points (REMOTE_FMT_USE_MP_UNITS): the number goes out in the quantity's own unit,
// the unit symbol is in the descriptor.

#include "config.hpp"

#if REMOTE_FMT_USE_MP_UNITS
    #include "describe.hpp"
    #include "fmt_check.hpp"

    #include <array>
    #include <cstddef>
    #include <mp-units/framework/quantity.h>
    #include <mp-units/framework/quantity_point.h>
    #include <mp-units/framework/unit.h>
    #include <string_constant/string_constant.hpp>
    #include <string_view>
    #include <type_traits>
    #include <utility>

namespace remote_fmt {

namespace detail {

    // The separator is part of the stored symbol, so the host has nothing to decide: it prints
    // the number and then this. mp-units' own space_before_unit_symbol says whether the unit
    // takes one -- true for nearly everything, false for the dimensionless `one` and for the
    // angle units, which is why a ratio does not come out as "5 " nor an angle as "45 deg".
    // Static storage, so the consteval generator below can hand out a view of it.
    template<auto U>
    inline constexpr auto unitSymbolStorage = [] {
        constexpr auto sym   = mp_units::unit_symbol<mp_units::unit_symbol_formatting{}>(U);
        constexpr bool space = mp_units::space_before_unit_symbol<U>;
        std::array<char, sym.size() + (space ? 1 : 0)> out{};
        std::size_t                                    i = 0;
        if constexpr(space) { out[i++] = ' '; }
        for(std::size_t k = 0; k < sym.size(); ++k) { out[i++] = sym[k]; }
        return out;
    }();

    /// The unit symbol as a StringConstant: it lives in the descriptor, so nothing of it goes on the wire.
    template<auto U>
    consteval auto unitSymbolConstant() {
        return sc::create([] {
            return std::string_view{unitSymbolStorage<U>.data(), unitSymbolStorage<U>.size()};
        });
    }

    // The spec is checked against the representation, which is what it ends up applied to.
    // host_type itself only exists when the check is enabled, so all of this is guarded.
    #if REMOTE_FMT_USE_FMT_CHECK
    /// The quantity a quantity_point is logged as. Not `quantity<R, Rep>`: quantity_from_zero
    /// restores the point's own unit only when that is non-truncating, and subtracting the origin
    /// can widen the representation, so the representation has to come from the call rather than
    /// be assumed.
    template<typename QP>
    using quantityFromZero_t
      = std::remove_cvref_t<decltype(std::declval<QP const&>().quantity_from_zero())>;

    template<auto R, typename Rep>
    struct HostType<mp_units::quantity<R, Rep>> {
        using type = host_type_t<Rep>;
    };

    template<auto R, auto PO, typename Rep>
    struct HostType<mp_units::quantity_point<R, PO, Rep>> {
        using type
          = host_type_t<typename quantityFromZero_t<mp_units::quantity_point<R, PO, Rep>>::rep>;
    };
    #endif

}   // namespace detail

/// A quantity goes out as its numerical value in its own unit; the unit symbol is in the descriptor
/// (`Q<n>:<symbol><D>`). The value keeps its own representation -- an integral quantity stays integral, so nothing on
/// the target converts to float merely to log it -- and the parser applies the caller's replacement field to the
/// number, so "{:.2f}" means what it usually means and the symbol follows. A sub format string could not do that: it
/// renders to a string before the outer spec is applied, and ".2f" on a string is an error.
template<auto R, typename Rep>
struct formatter<mp_units::quantity<R, Rep>> {
    static consteval auto descriptor() {
        return sc::StringConstant<'Q'>{}
             + desc::counted(detail::unitSymbolConstant<mp_units::get_unit(R)>())
             + descriptorOf<Rep>();
    }

    template<typename Printer>
    constexpr void format(mp_units::quantity<R,
                                             Rep> const& q,
                          Printer&                       printer) const {
        formatter<Rep>{}.format(q.numerical_value_in(mp_units::get_unit(R)), printer);
    }
};

/// mp-units gives quantity_point no text output of its own/// mp-units gives quantity_point no text output of its own: a point is meaningless without its
/// origin, so the documented idiom is to write `.quantity_from_zero()` at the call site. Logging
/// is the case where that ceremony buys nothing, so it happens here -- and it goes on the wire as
/// an ordinary quantity.
template<auto R, auto PO, typename Rep>
struct formatter<mp_units::quantity_point<R, PO, Rep>> {
    using Quantity = std::remove_cvref_t<
      decltype(std::declval<mp_units::quantity_point<R, PO, Rep> const&>().quantity_from_zero())>;

    static consteval auto descriptor() { return descriptorOf<Quantity>(); }

    template<typename Printer>
    constexpr void format(mp_units::quantity_point<R,
                                                   PO,
                                                   Rep> const& qp,
                          Printer&                             printer) const {
        formatter<Quantity>{}.format(qp.quantity_from_zero(), printer);
    }
};

}   // namespace remote_fmt
#endif
