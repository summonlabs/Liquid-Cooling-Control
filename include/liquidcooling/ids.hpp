#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "liquidcooling/status.hpp"

namespace liquidcooling {

/// A generational scalar: an integer whose zero value means "never assigned".
///
/// The tag makes semantically different scalars distinct types, so an epoch can
/// never be passed where a generation is expected.
template <typename Tag, typename Rep = std::uint64_t>
class Scalar final {
public:
    using rep = Rep;
    using tag = Tag;

    constexpr Scalar() noexcept = default;

    [[nodiscard]] static constexpr Scalar from_value(Rep value) noexcept {
        Scalar s;
        s.value_ = value;
        return s;
    }

    /// The single canonical "unset" value.
    [[nodiscard]] static constexpr Scalar invalid() noexcept { return Scalar{}; }

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != Rep{0}; }

    /// The next value in the identity sequence.  Precondition: this is valid and
    /// not the maximum representable value.
    [[nodiscard]] constexpr Scalar next() const noexcept { return from_value(static_cast<Rep>(value_ + 1)); }
    [[nodiscard]] constexpr bool is_last() const noexcept { return value_ == static_cast<Rep>(-1); }

    [[nodiscard]] constexpr bool operator==(const Scalar&) const noexcept = default;
    [[nodiscard]] constexpr auto operator<=>(const Scalar&) const noexcept = default;

private:
    Rep value_{};
};

struct LoopIdTag;
struct DeviceIdTag;
struct SensorIdTag;
struct PlanIdTag;
struct AttemptIdTag;
struct CommandIdTag;
struct ObligationIdTag;
struct ServiceWindowIdTag;
struct AuthorityTokenIdTag;
struct PrincipalIdTag;
struct ControlPlaneEpochTag;
struct ControllerIncarnationTag;
struct DeviceGenerationTag;
struct ConfigGenerationTag;
struct TopologyGenerationTag;
struct EvidenceGenerationTag;
struct StateRevisionTag;
struct JournalCommitSequenceTag;
struct ObservationSequenceTag;
struct SwitchoverGenerationTag;

using LoopId = Scalar<LoopIdTag>;
using DeviceId = Scalar<DeviceIdTag>;
using SensorId = Scalar<SensorIdTag>;
using PlanId = Scalar<PlanIdTag>;
using AttemptId = Scalar<AttemptIdTag>;
using CommandId = Scalar<CommandIdTag>;
using ObligationId = Scalar<ObligationIdTag>;
using ServiceWindowId = Scalar<ServiceWindowIdTag>;
using AuthorityTokenId = Scalar<AuthorityTokenIdTag>;
using PrincipalId = Scalar<PrincipalIdTag>;

/// Monotonic across controller restarts; survives in the durable store.
using ControlPlaneEpoch = Scalar<ControlPlaneEpochTag>;
/// Identifies one running controller instance bound to a store.
using ControllerIncarnation = Scalar<ControllerIncarnationTag>;
/// The hardware/software generation of one device as reported by its adapter.
using DeviceGeneration = Scalar<DeviceGenerationTag, std::uint32_t>;
/// Generation of the policy configuration in force.
using ConfigGeneration = Scalar<ConfigGenerationTag>;
/// Generation of the loop/device topology in force.
using TopologyGeneration = Scalar<TopologyGenerationTag>;
/// Generation of an evidence set published by an adapter.
using EvidenceGeneration = Scalar<EvidenceGenerationTag>;
/// The runtime's mutation counter; every durable mutation advances it.
using StateRevision = Scalar<StateRevisionTag>;
/// Commit sequence of the authoritative store snapshot.
using JournalCommitSequence = Scalar<JournalCommitSequenceTag>;
/// Monotonic sequence of observation batches returned by an adapter.
using ObservationSequence = Scalar<ObservationSequenceTag>;
/// Generation of an adapter switchover (adapter replacement) event.
using SwitchoverGeneration = Scalar<SwitchoverGenerationTag>;

/// Maximum length of an idempotency key.
inline constexpr std::size_t kMaxIdempotencyKeyLength = 128;
/// Maximum length of any diagnostic or reason string.
inline constexpr std::size_t kMaxReasonLength = 256;
/// Maximum length of a principal or vendor name.
inline constexpr std::size_t kMaxNameLength = 64;

/// A caller-supplied key that makes an actuation request replay-safe.
///
/// The key is validated at construction: it must be non-empty, at most
/// kMaxIdempotencyKeyLength bytes, and restricted to printable ASCII without
/// whitespace so that it can be compared and logged without ambiguity.
class IdempotencyKey final {
public:
    IdempotencyKey() = default;

    [[nodiscard]] static Result<IdempotencyKey> create(std::string_view text);

    [[nodiscard]] const std::string& str() const noexcept { return text_; }
    [[nodiscard]] bool empty() const noexcept { return text_.empty(); }
    [[nodiscard]] bool operator==(const IdempotencyKey&) const noexcept = default;
    [[nodiscard]] auto operator<=>(const IdempotencyKey&) const noexcept = default;

private:
    std::string text_{};
};

/// A 128-bit hash over the canonical byte encoding of a request.
///
/// Equal fingerprints mean the canonical encodings are identical with the same
/// probability as a 128-bit collision.  Fingerprints are never used as proof of
/// physical effect; they only bind an idempotency key to one request shape.
struct RequestFingerprint final {
    std::uint64_t high{0};
    std::uint64_t low{0};

    [[nodiscard]] bool operator==(const RequestFingerprint&) const noexcept = default;
    [[nodiscard]] std::string to_hex() const;
};

/// A deterministic 128-bit fingerprint (FNV-1a over the byte range, two
/// independent offset bases).
[[nodiscard]] RequestFingerprint fingerprint_bytes(const std::uint8_t* data, std::size_t size) noexcept;

/// A strict, bounded validator for identifier text supplied by callers.
///
/// Accepts 1..kMaxNameLength bytes of ASCII letters, digits, '-' and '_'.
[[nodiscard]] Result<std::string> validate_name(std::string_view text, std::string_view field);

/// Validates and truncation-checks a free-form diagnostic string.
[[nodiscard]] Result<std::string> validate_reason(std::string_view text, std::string_view field);

}  // namespace liquidcooling

namespace std {

template <typename Tag, typename Rep>
struct hash<liquidcooling::Scalar<Tag, Rep>> {
    [[nodiscard]] std::size_t operator()(const liquidcooling::Scalar<Tag, Rep>& value) const noexcept {
        auto mixed = static_cast<std::uint64_t>(value.value());
        mixed ^= mixed >> 33u;
        mixed *= 0xff51afd7ed558ccdULL;
        mixed ^= mixed >> 33u;
        return static_cast<std::size_t>(mixed);
    }
};

template <>
struct hash<liquidcooling::IdempotencyKey> {
    [[nodiscard]] std::size_t operator()(const liquidcooling::IdempotencyKey& key) const noexcept {
        return std::hash<std::string>{}(key.str());
    }
};

}  // namespace std
