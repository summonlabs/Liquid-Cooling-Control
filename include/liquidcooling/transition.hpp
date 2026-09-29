#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "liquidcooling/authority.hpp"
#include "liquidcooling/enum_support.hpp"
#include "liquidcooling/ids.hpp"
#include "liquidcooling/status.hpp"
#include "liquidcooling/time.hpp"
#include "liquidcooling/units.hpp"

namespace liquidcooling {

/// Every transition the runtime can attempt.
///
/// The action set is closed: unknown actions are rejected structurally rather
/// than interpreted.
enum class ActionKind : std::uint8_t {
    Unknown = 0,
    StartPump = 1,
    StopPump = 2,
    OpenValve = 3,
    CloseValve = 4,
    SetFlowTarget = 5,
    SetPressureTarget = 6,
    IsolateLoop = 7,
    ClearIsolation = 8,
    EnterService = 9,
    ExitService = 10,
    CompleteObligation = 11,
    AbandonAttempt = 12,
    ResetFault = 13,
};

inline constexpr std::size_t kActionKindCount = 14;
inline constexpr std::array<std::string_view, kActionKindCount> kActionKindNames = {
    "unknown",          "start-pump",       "stop-pump",     "open-valve",     "close-valve",
    "set-flow-target",  "set-pressure-target", "isolate-loop", "clear-isolation", "enter-service",
    "exit-service",     "complete-obligation", "abandon-attempt", "reset-fault"};

[[nodiscard]] inline std::string_view to_string(ActionKind value) noexcept {
    return detail::enum_name(value, kActionKindNames);
}
[[nodiscard]] inline Result<ActionKind> parse_action_kind(std::uint8_t value) {
    return detail::enum_parse<ActionKind>(value, kActionKindNames, "action kind");
}

/// How an action moves the hazard exposure of a loop.
enum class EffectClass : std::uint8_t {
    /// No physical actuation; a control-plane state change only.
    None = 0,
    /// Strictly reduces hazard and must remain available under broad authority.
    ReduceExposure = 1,
    /// Adds load, flow or stored energy and therefore needs production authority.
    IncreaseExposure = 2,
    /// Determined at plan time by comparing the requested value with the
    /// current target.
    DirectionRelative = 3,
};

inline constexpr std::size_t kEffectClassCount = 4;
inline constexpr std::array<std::string_view, kEffectClassCount> kEffectClassNames = {
    "none", "reduce-exposure", "increase-exposure", "direction-relative"};

[[nodiscard]] inline std::string_view to_string(EffectClass value) noexcept {
    return detail::enum_name(value, kEffectClassNames);
}
[[nodiscard]] inline Result<EffectClass> parse_effect_class(std::uint8_t value) {
    return detail::enum_parse<EffectClass>(value, kEffectClassNames, "effect class");
}

/// The capability an action demands.
enum class RequiredCapability : std::uint8_t {
    None = 0,
    Observe = 1,
    Isolate = 2,
    Production = 3,
    Service = 4,
};

inline constexpr std::size_t kRequiredCapabilityCount = 5;
inline constexpr std::array<std::string_view, kRequiredCapabilityCount> kRequiredCapabilityNames = {
    "none", "observe", "isolate", "production", "service"};

[[nodiscard]] inline std::string_view to_string(RequiredCapability value) noexcept {
    return detail::enum_name(value, kRequiredCapabilityNames);
}
[[nodiscard]] inline Result<RequiredCapability> parse_required_capability(std::uint8_t value) {
    return detail::enum_parse<RequiredCapability>(value, kRequiredCapabilityNames, "required capability");
}

/// Static properties of an action.
struct ActionDescriptor final {
    ActionKind kind{ActionKind::Unknown};
    std::string_view name{};
    EffectClass effect{EffectClass::None};
    RequiredCapability capability{RequiredCapability::None};
    /// The action targets a specific device.
    bool requires_device{false};
    /// The action carries a flow setpoint.
    bool requires_flow_target{false};
    /// The action carries a pressure setpoint.
    bool requires_pressure_target{false};
    /// The action carries a service obligation identity.
    bool requires_obligation{false};
    /// The action carries the identity of an attempt to abandon.
    bool requires_attempt{false};
    /// The action is dispatched to the adapter as a physical command.
    bool issues_command{false};
};

[[nodiscard]] const ActionDescriptor& action_descriptor(ActionKind kind) noexcept;

/// The capability implied by a required-capability enumerator.
[[nodiscard]] AuthorityCapabilities capability_set(RequiredCapability capability) noexcept;

/// A transition request exactly as supplied by a caller.
///
/// Every authority-bearing field is explicit: the caller states which loop,
/// which device generation and which state revision the request was planned
/// against, and which authority grant covers it.
struct TransitionRequest final {
    ActionKind action{ActionKind::Unknown};
    LoopId loop{};
    DeviceId target{};
    DeviceGeneration expected_device_generation{};
    FlowRate flow_target{};
    bool has_flow_target{false};
    Pressure pressure_target{};
    bool has_pressure_target{false};
    ObligationId obligation{};
    AttemptId attempt{};
    StateRevision expected_revision{};
    AuthorityToken authority{};
    IdempotencyKey idempotency_key{};
    std::string reason{};
};

/// A validated, immutable intent.
///
/// A plan records the exact state it was planned against.  Execution re-checks
/// that binding before any adapter call, which is how stale plans are fenced.
struct Plan final {
    PlanId id{};
    TransitionRequest request{};
    RequestFingerprint fingerprint{};
    StateRevision revision_at_plan{};
    ControlPlaneEpoch epoch_at_plan{};
    ControllerIncarnation incarnation_at_plan{};
    ConfigGeneration config_generation{};
    TopologyGeneration topology_generation{};
    DeviceGeneration device_generation{};
    TimestampNs planned_at{};
    EffectClass effect{EffectClass::None};
    RequiredCapability capability{RequiredCapability::None};
    /// Set when the action carries a physical command.
    bool issues_command{false};
    /// Canonical intent bytes covered by the fingerprint.
    std::vector<std::uint8_t> canonical{};
};

/// Canonical byte encoding of the semantic intent of a request.
///
/// The authority token, the idempotency key and the free-form reason are
/// excluded: they are not part of what the request means, so a retry with a
/// fresh token and the same intent replays instead of conflicting.
[[nodiscard]] std::vector<std::uint8_t> canonical_request_bytes(const TransitionRequest& request);

[[nodiscard]] RequestFingerprint fingerprint_request(const TransitionRequest& request);

/// Resolves an action that depends on the direction of a setpoint change.
[[nodiscard]] Result<EffectClass> resolve_effect(const ActionDescriptor& descriptor,
                                                 bool has_current_value,
                                                 std::int64_t current_value,
                                                 std::int64_t requested_value);

}  // namespace liquidcooling
