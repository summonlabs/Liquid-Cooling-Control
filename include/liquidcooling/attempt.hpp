#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "liquidcooling/enum_support.hpp"
#include "liquidcooling/ids.hpp"
#include "liquidcooling/status.hpp"
#include "liquidcooling/time.hpp"
#include "liquidcooling/transition.hpp"

namespace liquidcooling {

/// Lifecycle of one command attempt.
///
/// Acknowledgement is never proof of effect: Acknowledged means the adapter
/// accepted the command, EffectVerified means a later observation from the same
/// device generation confirmed the intended physical result.
enum class AttemptStatus : std::uint8_t {
    Unknown = 0,
    Planned = 1,
    Issued = 2,
    Acknowledged = 3,
    EffectVerified = 4,
    Failed = 5,
    Contradicted = 6,
    Unresolved = 7,
    Abandoned = 8,
};

inline constexpr std::size_t kAttemptStatusCount = 9;
inline constexpr std::array<std::string_view, kAttemptStatusCount> kAttemptStatusNames = {
    "unknown", "planned", "issued", "acknowledged", "effect-verified", "failed", "contradicted", "unresolved",
    "abandoned"};

[[nodiscard]] inline std::string_view to_string(AttemptStatus value) noexcept {
    return detail::enum_name(value, kAttemptStatusNames);
}
[[nodiscard]] inline Result<AttemptStatus> parse_attempt_status(std::uint8_t value) {
    return detail::enum_parse<AttemptStatus>(value, kAttemptStatusNames, "attempt status");
}

/// True while the attempt still constrains subsequent actuation.
[[nodiscard]] constexpr bool attempt_is_open(AttemptStatus status) noexcept {
    return status == AttemptStatus::Planned || status == AttemptStatus::Issued ||
           status == AttemptStatus::Acknowledged || status == AttemptStatus::Unresolved;
}

/// True once the attempt can no longer change without explicit operator action.
[[nodiscard]] constexpr bool attempt_is_terminal(AttemptStatus status) noexcept {
    return status == AttemptStatus::EffectVerified || status == AttemptStatus::Failed ||
           status == AttemptStatus::Contradicted || status == AttemptStatus::Abandoned;
}

/// A durable record of one actuation attempt.
struct CommandAttempt final {
    AttemptId id{};
    PlanId plan{};
    CommandId command{};
    IdempotencyKey idempotency_key{};
    RequestFingerprint fingerprint{};
    ActionKind action{ActionKind::Unknown};
    EffectClass effect{EffectClass::None};
    LoopId loop{};
    DeviceId target{};
    DeviceGeneration generation{};
    /// Setpoints carried by the command, retained for effect comparison.
    FlowRate commanded_flow{};
    Pressure commanded_pressure{};
    ControlPlaneEpoch epoch{};
    ControllerIncarnation incarnation{};
    AttemptStatus status{AttemptStatus::Unknown};

    StateRevision revision_at_issue{};
    JournalCommitSequence issued_at_commit{};

    TimestampNs issued_at{};
    TimestampNs updated_at{};
    TimestampNs observed_at{};

    ObservationSequence ack_sequence{};
    ObservationSequence effect_sequence{};
    DeviceGeneration ack_generation{};
    DeviceGeneration effect_generation{};

    /// Adapter outcome, translated into the runtime status vocabulary.
    StatusCode adapter_code{StatusCode::Ok};
    /// Terminal or pending runtime outcome.
    StatusCode outcome{StatusCode::Ok};
    std::string detail{};

    [[nodiscard]] bool is_open() const noexcept { return attempt_is_open(status); }
    [[nodiscard]] bool is_terminal() const noexcept { return attempt_is_terminal(status); }
};

/// An idempotency record: the durable binding from a caller key to one intent.
struct IdempotencyRecord final {
    IdempotencyKey key{};
    RequestFingerprint fingerprint{};
    PlanId plan{};
    AttemptId attempt{};
    StatusCode outcome{StatusCode::Ok};
    JournalCommitSequence committed_at{};
};

/// The complete result of planning and executing one transition.
struct ExecutionRecord final {
    Plan plan{};
    CommandAttempt attempt{};
    /// True when the call replayed a stored result instead of actuating again.
    bool idempotent_replay{false};
    /// Runtime outcome of this call.
    StatusCode outcome{StatusCode::Ok};
    /// True when the call terminated with a code that is not Ok; the attempt is
    /// still durable and still constrains later actuation.
    bool deferred{false};
    std::string detail{};
};

/// The result of attempting to verify the physical effect of an attempt.
struct VerificationRecord final {
    AttemptId attempt{};
    AttemptStatus status{AttemptStatus::Unknown};
    StatusCode code{StatusCode::Ok};
    ObservationSequence evidence_sequence{};
    DeviceGeneration evidence_generation{};
    std::string detail{};
};

}  // namespace liquidcooling
