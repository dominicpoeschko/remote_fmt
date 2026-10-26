#pragma once

// The kinds of argument the formatters (formatters.hpp) and the fmt check (fmt_check.hpp) tell apart. The standard
// library only.

#include <concepts>
#include <cstddef>
#include <ranges>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace remote_fmt { namespace detail {

    template<typename T>
    concept IsTupleLike = requires { typename std::tuple_size<T>::type; };

    template<typename T>
    concept IsTupleLikeButNotRange = IsTupleLike<T> && !std::ranges::range<T>;

    template<typename T>
    concept IsStringLike = std::is_convertible_v<T, std::string_view> && requires(T value) {
        { value.size() } -> std::convertible_to<std::size_t>;
        { value.data() } -> std::convertible_to<char const*>;
        { value.starts_with(std::string_view{}) } -> std::convertible_to<bool>;
    };

    template<typename T>
    concept IsRangeButNotStringLike = std::ranges::range<T> && !IsStringLike<T>;

    template<typename T>
    concept IsMap = requires { typename T::mapped_type; };

    template<typename T>
    concept IsSet = requires { typename T::key_type; } && !IsMap<T>;

}}   // namespace remote_fmt::detail
