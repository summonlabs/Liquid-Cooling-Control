#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "liquidcooling/status.hpp"

namespace liquidcooling::detail {

/// Looks up the stable name of an enumerator whose values are contiguous from 0.
template <typename E, std::size_t N>
[[nodiscard]] inline std::string_view enum_name(E value, const std::array<std::string_view, N>& table) noexcept {
    const auto index = static_cast<std::size_t>(value);
    return index < N ? table[index] : std::string_view{"unknown"};
}

/// Decodes an enumerator, rejecting any value outside the defined range.
template <typename E, std::size_t N>
[[nodiscard]] inline Result<E> enum_parse(std::uint8_t value,
                                          const std::array<std::string_view, N>& /*table*/,
                                          std::string_view what) {
    if (static_cast<std::size_t>(value) >= N) {
        return Status{StatusCode::InvalidEnumValue, std::string(what) + " has an undefined enumerator"};
    }
    return static_cast<E>(value);
}

}  // namespace liquidcooling::detail
