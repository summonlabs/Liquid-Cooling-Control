#pragma once

// Fixed framing shared by the authoritative snapshot file and the audit journal
// file.  A framed file is:
//
//   header (64 bytes) | payload (record stream) | trailer (16 bytes)
//
// The header carries a format version, the commit sequence, the record count,
// the payload length and two independent integrity checks.  The trailer repeats
// the commit sequence so that a truncated publication is detected even when the
// header survived.  Declared lengths are validated against the bounds before any
// allocation or slicing.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "liquidcooling/store.hpp"

namespace liquidcooling::detail {

struct ParsedFrame final {
    JournalCommitSequence sequence{};
    std::uint64_t record_count{0};
    std::span<const std::uint8_t> payload{};
};

/// Builds the complete framed byte image of a payload.
[[nodiscard]] std::vector<std::uint8_t> frame_file(std::string_view magic,
                                                   JournalCommitSequence sequence,
                                                   std::uint64_t record_count,
                                                   const std::vector<std::uint8_t>& payload);

/// Validates the framing of a byte image and returns a view of its payload.
[[nodiscard]] Result<ParsedFrame> parse_framed_file(std::span<const std::uint8_t> bytes,
                                                    std::string_view expected_magic,
                                                    const StoreBounds& bounds);

}  // namespace liquidcooling::detail
