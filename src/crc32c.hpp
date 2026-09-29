#pragma once

#include <cstddef>
#include <cstdint>

namespace liquidcooling::detail {

/// CRC-32C (Castagnoli), used for the durable store integrity check.
[[nodiscard]] std::uint32_t crc32c(const void* data, std::size_t size, std::uint32_t seed = 0) noexcept;

}  // namespace liquidcooling::detail
