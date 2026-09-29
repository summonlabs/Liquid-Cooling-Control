#include "store_framing.hpp"

#include <array>
#include <cstring>

#include "codec.hpp"
#include "crc32c.hpp"

namespace liquidcooling::detail {
namespace {

constexpr std::string_view kTrailerMagic = "LCCEND01";

}  // namespace

std::vector<std::uint8_t> frame_file(std::string_view magic,
                                     JournalCommitSequence sequence,
                                     std::uint64_t record_count,
                                     const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> file;
    file.reserve(kStoreHeaderBytes + payload.size() + kStoreTrailerBytes);
    Writer header;
    header.raw(magic.data(), magic.size());
    header.u32(kStoreFormatVersion);
    header.u32(static_cast<std::uint32_t>(kStoreHeaderBytes));
    header.u32(0);
    header.u32(0);
    header.u64(sequence.value());
    header.u64(record_count);
    header.u64(static_cast<std::uint64_t>(payload.size()));
    header.u32(crc32c(payload.data(), payload.size()));
    header.u32(0);
    const auto partial = header.bytes();
    header.u32(crc32c(partial.data(), partial.size()));
    header.u32(0);
    file.insert(file.end(), header.bytes().begin(), header.bytes().end());
    file.insert(file.end(), payload.begin(), payload.end());
    Writer trailer;
    trailer.raw(kTrailerMagic.data(), kTrailerMagic.size());
    trailer.u64(sequence.value());
    file.insert(file.end(), trailer.bytes().begin(), trailer.bytes().end());
    return file;
}

Result<ParsedFrame> parse_framed_file(std::span<const std::uint8_t> bytes,
                                      std::string_view expected_magic,
                                      const StoreBounds& bounds) {
    if (bytes.size() < kStoreHeaderBytes + kStoreTrailerBytes) {
        return Status{StatusCode::StoreCorrupt, "store file is shorter than its fixed framing"};
    }
    if (bytes.size() > bounds.max_file_bytes) {
        return Status{StatusCode::StoreBoundsExceeded, "store file exceeds the configured maximum size"};
    }
    std::array<std::uint8_t, 8> magic{};
    std::memcpy(magic.data(), bytes.data(), magic.size());
    if (std::string_view(reinterpret_cast<const char*>(magic.data()), magic.size()) != expected_magic) {
        return Status{StatusCode::StoreCorrupt, "store file magic does not match its role"};
    }
    Reader header(bytes.subspan(0, kStoreHeaderBytes));
    (void)header.raw(8);
    const std::uint32_t version = header.u32();
    const std::uint32_t header_size = header.u32();
    const std::uint32_t reserved1 = header.u32();
    const std::uint32_t reserved2 = header.u32();
    const std::uint64_t sequence = header.u64();
    const std::uint64_t record_count = header.u64();
    const std::uint64_t payload_length = header.u64();
    const std::uint32_t payload_crc = header.u32();
    const std::uint32_t flags = header.u32();
    const std::uint32_t header_crc = header.u32();
    const std::uint32_t reserved3 = header.u32();
    if (header.failed()) {
        return header.status();
    }
    if (crc32c(bytes.data(), kStoreHeaderBytes - 8) != header_crc) {
        return Status{StatusCode::StoreCorrupt, "store file header failed its integrity check"};
    }
    if (version != kStoreFormatVersion) {
        return Status{StatusCode::StoreVersionUnsupported, "store file format version is not supported"};
    }
    if (header_size != kStoreHeaderBytes) {
        return Status{StatusCode::StoreVersionUnsupported, "store file header size is not supported"};
    }
    if (reserved1 != 0 || reserved2 != 0 || reserved3 != 0 || flags != 0) {
        return Status{StatusCode::StoreCorrupt, "store file header reserved fields must be zero"};
    }
    if (payload_length > bounds.max_file_bytes) {
        return Status{StatusCode::StoreBoundsExceeded, "declared payload length exceeds the configured maximum"};
    }
    const std::uint64_t expected_total =
        static_cast<std::uint64_t>(kStoreHeaderBytes) + payload_length + kStoreTrailerBytes;
    if (expected_total != static_cast<std::uint64_t>(bytes.size())) {
        return Status{StatusCode::StoreCorrupt,
                      "store file size does not match its declared payload length (truncated or trailing bytes)"};
    }
    if (record_count > bounds.max_records) {
        return Status{StatusCode::StoreBoundsExceeded, "declared record count exceeds the configured maximum"};
    }
    const auto payload = bytes.subspan(kStoreHeaderBytes, static_cast<std::size_t>(payload_length));
    if (crc32c(payload.data(), payload.size()) != payload_crc) {
        return Status{StatusCode::StoreCorrupt, "store file payload failed its integrity check"};
    }
    const auto trailer = bytes.subspan(bytes.size() - kStoreTrailerBytes, kStoreTrailerBytes);
    std::array<std::uint8_t, 8> trailer_magic{};
    std::memcpy(trailer_magic.data(), trailer.data(), trailer_magic.size());
    if (std::string_view(reinterpret_cast<const char*>(trailer_magic.data()), trailer_magic.size()) !=
        kTrailerMagic) {
        return Status{StatusCode::StoreCorrupt, "store file trailer magic is missing (truncated publication)"};
    }
    Reader trailer_reader(trailer.subspan(8));
    const std::uint64_t trailer_sequence = trailer_reader.u64();
    if (trailer_reader.failed() || trailer_sequence != sequence) {
        return Status{StatusCode::StoreCorrupt, "store file trailer does not agree with its header"};
    }
    ParsedFrame parsed;
    parsed.sequence = JournalCommitSequence::from_value(sequence);
    parsed.record_count = record_count;
    parsed.payload = payload;
    return parsed;
}

}  // namespace liquidcooling::detail
