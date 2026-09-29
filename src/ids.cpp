#include "liquidcooling/ids.hpp"

#include <array>
#include <cstdio>

namespace liquidcooling {
namespace {

[[nodiscard]] bool is_key_char(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x21u || u > 0x7Eu) {
        return false;
    }
    return c != '"' && c != '\\' && c != '\'' && c != ':' && c != ',' && c != ';';
}

[[nodiscard]] bool is_name_char(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    const bool alnum = (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z');
    return alnum || c == '-' || c == '_';
}

constexpr std::uint64_t kFnvOffsetA = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvOffsetB = 0x9e3779b97f4a7c15ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

}  // namespace

Result<IdempotencyKey> IdempotencyKey::create(std::string_view text) {
    if (text.empty()) {
        return Status{StatusCode::MissingRequiredField, "idempotency key must not be empty"};
    }
    if (text.size() > kMaxIdempotencyKeyLength) {
        return Status{StatusCode::StringTooLong, "idempotency key exceeds the maximum length"};
    }
    for (const char c : text) {
        if (!is_key_char(c)) {
            return Status{StatusCode::InvalidEncoding,
                          "idempotency key must be printable ASCII without separators or whitespace"};
        }
    }
    IdempotencyKey key;
    key.text_.assign(text);
    return key;
}

std::string RequestFingerprint::to_hex() const {
    std::array<char, 40> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%016llx%016llx",
                                      static_cast<unsigned long long>(high),
                                      static_cast<unsigned long long>(low));
    if (written <= 0) {
        return {};
    }
    return std::string(buffer.data(), static_cast<std::size_t>(written));
}

RequestFingerprint fingerprint_bytes(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint64_t a = kFnvOffsetA;
    std::uint64_t b = kFnvOffsetB;
    for (std::size_t i = 0; i < size; ++i) {
        a ^= static_cast<std::uint64_t>(data[i]);
        a *= kFnvPrime;
        b ^= static_cast<std::uint64_t>(data[i]) + (i & 0xFFu);
        b *= kFnvPrime;
    }
    // Length is mixed in so that trailing-zero truncation cannot collide.
    a ^= static_cast<std::uint64_t>(size);
    a *= kFnvPrime;
    b ^= static_cast<std::uint64_t>(size) * kFnvPrime;
    return RequestFingerprint{a, b};
}

Result<std::string> validate_name(std::string_view text, std::string_view field) {
    if (text.empty()) {
        return Status{StatusCode::MissingRequiredField, std::string(field) + " must not be empty"};
    }
    if (text.size() > kMaxNameLength) {
        return Status{StatusCode::StringTooLong, std::string(field) + " exceeds the maximum length"};
    }
    for (const char c : text) {
        if (!is_name_char(c)) {
            return Status{StatusCode::InvalidEncoding,
                          std::string(field) + " must contain only ASCII letters, digits, '-' and '_'"};
        }
    }
    return std::string(text);
}

Result<std::string> validate_reason(std::string_view text, std::string_view field) {
    if (text.size() > kMaxReasonLength) {
        return Status{StatusCode::StringTooLong, std::string(field) + " exceeds the maximum length"};
    }
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20u || u == 0x7Fu) {
            return Status{StatusCode::InvalidEncoding, std::string(field) + " must not contain control characters"};
        }
    }
    return std::string(text);
}

}  // namespace liquidcooling
