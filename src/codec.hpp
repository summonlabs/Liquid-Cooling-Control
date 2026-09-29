#pragma once

// Internal canonical binary codec.
//
// The encoding is little-endian, field ordered and length prefixed.  It is used
// both for the durable store format and for request fingerprints, so the layout
// is a compatibility surface and must only change with a format version bump.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquidcooling/status.hpp"

namespace liquidcooling::detail {

class Writer final {
public:
    void u8(std::uint8_t value) { data_.push_back(value); }

    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value & 0xFFu));
        u8(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
    }

    void u32(std::uint32_t value) {
        u16(static_cast<std::uint16_t>(value & 0xFFFFu));
        u16(static_cast<std::uint16_t>((value >> 16u) & 0xFFFFu));
    }

    void u64(std::uint64_t value) {
        u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFull));
        u32(static_cast<std::uint32_t>((value >> 32u) & 0xFFFFFFFFull));
    }

    void boolean(bool value) { u8(value ? std::uint8_t{1} : std::uint8_t{0}); }

    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }
    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }

    void raw(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        data_.insert(data_.end(), bytes, bytes + size);
    }

    /// Length-prefixed text.  The caller must have bounded the length already.
    void text(std::string_view value) {
        u32(static_cast<std::uint32_t>(value.size()));
        raw(value.data(), value.size());
    }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return data_; }
    [[nodiscard]] std::vector<std::uint8_t>& bytes() noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

private:
    std::vector<std::uint8_t> data_{};
};

class Reader final {
public:
    explicit Reader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

    [[nodiscard]] bool ok() const noexcept { return failed_ == StatusCode::Ok; }
    [[nodiscard]] bool failed() const noexcept { return failed_ != StatusCode::Ok; }
    [[nodiscard]] Status status() const { return Status{failed_, message_}; }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - position_; }
    [[nodiscard]] bool at_end() const noexcept { return position_ == data_.size(); }

    void fail(StatusCode code, std::string message) {
        if (failed_ == StatusCode::Ok) {
            failed_ = code;
            message_ = std::move(message);
        }
    }

    [[nodiscard]] std::uint8_t u8() {
        if (remaining() < 1) {
            fail(StatusCode::StoreCorrupt, "unexpected end of record");
            return 0;
        }
        return data_[position_++];
    }

    [[nodiscard]] std::uint16_t u16() {
        const std::uint16_t low = u8();
        const std::uint16_t high = u8();
        return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8u));
    }

    [[nodiscard]] std::uint32_t u32() {
        const std::uint32_t low = u16();
        const std::uint32_t high = u16();
        return low | (high << 16u);
    }

    [[nodiscard]] std::uint64_t u64() {
        const std::uint64_t low = u32();
        const std::uint64_t high = u32();
        return low | (high << 32u);
    }

    [[nodiscard]] bool boolean() {
        const std::uint8_t raw_value = u8();
        if (raw_value > 1) {
            fail(StatusCode::StoreCorrupt, "boolean field is not 0 or 1");
            return false;
        }
        return raw_value == 1;
    }

    [[nodiscard]] std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    [[nodiscard]] std::int32_t i32() { return static_cast<std::int32_t>(u32()); }

    [[nodiscard]] std::span<const std::uint8_t> raw(std::size_t size) {
        if (remaining() < size) {
            fail(StatusCode::StoreCorrupt, "declared length exceeds the record body");
            return {};
        }
        const auto slice = data_.subspan(position_, size);
        position_ += size;
        return slice;
    }

    /// Bounded length-prefixed text; rejects undeclared long content.
    [[nodiscard]] std::string text(std::size_t max_length) {
        const std::uint32_t declared = u32();
        if (failed()) {
            return {};
        }
        if (declared > max_length) {
            fail(StatusCode::StoreBoundsExceeded, "declared text length exceeds the configured bound");
            return {};
        }
        const auto slice = raw(declared);
        if (failed()) {
            return {};
        }
        std::string out;
        out.reserve(declared);
        for (const std::uint8_t byte : slice) {
            if (byte == 0) {
                fail(StatusCode::StoreCorrupt, "text field contains a NUL byte");
                return {};
            }
            out.push_back(static_cast<char>(byte));
        }
        return out;
    }

    /// Skips a declared number of bytes, interpreting them as reserved.
    void reserved(std::size_t count, std::string_view field) {
        const auto slice = raw(count);
        if (failed()) {
            return;
        }
        for (const std::uint8_t byte : slice) {
            if (byte != 0) {
                fail(StatusCode::StoreCorrupt, std::string("reserved field ") + std::string(field) + " is non-zero");
                return;
            }
        }
    }

private:
    std::span<const std::uint8_t> data_{};
    std::size_t position_{0};
    StatusCode failed_{StatusCode::Ok};
    std::string message_{};
};

template <typename E, std::size_t N>
[[nodiscard]] E read_enum(Reader& reader, const std::array<std::string_view, N>& names, std::string_view what) {
    const std::uint8_t raw_value = reader.u8();
    if (reader.failed()) {
        return E{};
    }
    if (static_cast<std::size_t>(raw_value) >= N) {
        reader.fail(StatusCode::StoreCorrupt, std::string(what) + " has an undefined enumerator");
        return E{};
    }
    (void)names;
    return static_cast<E>(raw_value);
}

}  // namespace liquidcooling::detail
