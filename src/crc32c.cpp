#include "crc32c.hpp"

#include <array>

namespace liquidcooling::detail {
namespace {

constexpr std::uint32_t kPolynomial = 0x82F63B78u;

constexpr std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1u) != 0u ? (value >> 1u) ^ kPolynomial : value >> 1u;
        }
        table[i] = value;
    }
    return table;
}

constexpr auto kTable = make_table();

}  // namespace

std::uint32_t crc32c(const void* data, std::size_t size, std::uint32_t seed) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = ~seed;
    for (std::size_t i = 0; i < size; ++i) {
        crc = kTable[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8u);
    }
    return ~crc;
}

}  // namespace liquidcooling::detail
