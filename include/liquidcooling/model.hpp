#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "liquidcooling/enum_support.hpp"
#include "liquidcooling/ids.hpp"
#include "liquidcooling/status.hpp"
#include "liquidcooling/time.hpp"
#include "liquidcooling/units.hpp"

namespace liquidcooling {

// ---------------------------------------------------------------------------
// Device model
// ---------------------------------------------------------------------------

enum class DeviceKind : std::uint8_t {
    Unknown = 0,
    Cdu = 1,
    Pump = 2,
    Valve = 3,
    FlowSensor = 4,
    PressureSensor = 5,
    LeakSensor = 6,
    CoolantProbe = 7,
};

inline constexpr std::size_t kDeviceKindCount = 8;
inline constexpr std::array<std::string_view, kDeviceKindCount> kDeviceKindNames = {
    "unknown", "cdu", "pump", "valve", "flow-sensor", "pressure-sensor", "leak-sensor", "coolant-probe"};

/// Whether a device kind accepts actuation (as opposed to observation only).
[[nodiscard]] constexpr bool is_actuatable(DeviceKind kind) noexcept {
    return kind == DeviceKind::Pump || kind == DeviceKind::Valve || kind == DeviceKind::Cdu;
}

[[nodiscard]] inline std::string_view to_string(DeviceKind value) noexcept {
    return detail::enum_name(value, kDeviceKindNames);
}
[[nodiscard]] inline Result<DeviceKind> parse_device_kind(std::uint8_t value) {
    return detail::enum_parse<DeviceKind>(value, kDeviceKindNames, "device kind");
}

enum class LifecycleState : std::uint8_t {
    Unknown = 0,
    Present = 1,
    Absent = 2,
    Faulted = 3,
    Service = 4,
    Retired = 5,
};

inline constexpr std::size_t kLifecycleStateCount = 6;
inline constexpr std::array<std::string_view, kLifecycleStateCount> kLifecycleStateNames = {
    "unknown", "present", "absent", "faulted", "service", "retired"};

[[nodiscard]] inline std::string_view to_string(LifecycleState value) noexcept {
    return detail::enum_name(value, kLifecycleStateNames);
}
[[nodiscard]] inline Result<LifecycleState> parse_lifecycle_state(std::uint8_t value) {
    return detail::enum_parse<LifecycleState>(value, kLifecycleStateNames, "lifecycle state");
}

enum class ValvePosition : std::uint8_t {
    Unknown = 0,
    Closed = 1,
    Open = 2,
    Intermediate = 3,
    Faulted = 4,
};

inline constexpr std::size_t kValvePositionCount = 5;
inline constexpr std::array<std::string_view, kValvePositionCount> kValvePositionNames = {
    "unknown", "closed", "open", "intermediate", "faulted"};

[[nodiscard]] inline std::string_view to_string(ValvePosition value) noexcept {
    return detail::enum_name(value, kValvePositionNames);
}
[[nodiscard]] inline Result<ValvePosition> parse_valve_position(std::uint8_t value) {
    return detail::enum_parse<ValvePosition>(value, kValvePositionNames, "valve position");
}

enum class PumpState : std::uint8_t {
    Unknown = 0,
    Stopped = 1,
    Running = 2,
    Starting = 3,
    Faulted = 4,
};

inline constexpr std::size_t kPumpStateCount = 5;
inline constexpr std::array<std::string_view, kPumpStateCount> kPumpStateNames = {
    "unknown", "stopped", "running", "starting", "faulted"};

[[nodiscard]] inline std::string_view to_string(PumpState value) noexcept {
    return detail::enum_name(value, kPumpStateNames);
}
[[nodiscard]] inline Result<PumpState> parse_pump_state(std::uint8_t value) {
    return detail::enum_parse<PumpState>(value, kPumpStateNames, "pump state");
}

enum class LoopOperatingState : std::uint8_t {
    Unknown = 0,
    Stopped = 1,
    Running = 2,
    Degraded = 3,
    Isolating = 4,
    Isolated = 5,
    Faulted = 6,
    Service = 7,
};

inline constexpr std::size_t kLoopOperatingStateCount = 8;
inline constexpr std::array<std::string_view, kLoopOperatingStateCount> kLoopOperatingStateNames = {
    "unknown", "stopped", "running", "degraded", "isolating", "isolated", "faulted", "service"};

[[nodiscard]] inline std::string_view to_string(LoopOperatingState value) noexcept {
    return detail::enum_name(value, kLoopOperatingStateNames);
}
[[nodiscard]] inline Result<LoopOperatingState> parse_loop_operating_state(std::uint8_t value) {
    return detail::enum_parse<LoopOperatingState>(value, kLoopOperatingStateNames, "loop operating state");
}

enum class IsolationState : std::uint8_t {
    Unknown = 0,
    Open = 1,
    Isolating = 2,
    Isolated = 3,
};

inline constexpr std::size_t kIsolationStateCount = 4;
inline constexpr std::array<std::string_view, kIsolationStateCount> kIsolationStateNames = {
    "unknown", "open", "isolating", "isolated"};

[[nodiscard]] inline std::string_view to_string(IsolationState value) noexcept {
    return detail::enum_name(value, kIsolationStateNames);
}
[[nodiscard]] inline Result<IsolationState> parse_isolation_state(std::uint8_t value) {
    return detail::enum_parse<IsolationState>(value, kIsolationStateNames, "isolation state");
}

enum class LeakState : std::uint8_t {
    Unknown = 0,
    None = 1,
    Suspected = 2,
    Confirmed = 3,
};

inline constexpr std::size_t kLeakStateCount = 4;
inline constexpr std::array<std::string_view, kLeakStateCount> kLeakStateNames = {
    "unknown", "none", "suspected", "confirmed"};

[[nodiscard]] inline std::string_view to_string(LeakState value) noexcept {
    return detail::enum_name(value, kLeakStateNames);
}
[[nodiscard]] inline Result<LeakState> parse_leak_state(std::uint8_t value) {
    return detail::enum_parse<LeakState>(value, kLeakStateNames, "leak state");
}

/// True when the leak state can be relied upon to permit exposure-increasing work.
[[nodiscard]] constexpr bool leak_permits_increase(LeakState value) noexcept {
    return value == LeakState::None;
}

enum class CoolantQuality : std::uint8_t {
    Unknown = 0,
    Nominal = 1,
    Degraded = 2,
    OutOfSpec = 3,
};

inline constexpr std::size_t kCoolantQualityCount = 4;
inline constexpr std::array<std::string_view, kCoolantQualityCount> kCoolantQualityNames = {
    "unknown", "nominal", "degraded", "out-of-spec"};

[[nodiscard]] inline std::string_view to_string(CoolantQuality value) noexcept {
    return detail::enum_name(value, kCoolantQualityNames);
}
[[nodiscard]] inline Result<CoolantQuality> parse_coolant_quality(std::uint8_t value) {
    return detail::enum_parse<CoolantQuality>(value, kCoolantQualityNames, "coolant quality");
}

enum class ServiceMode : std::uint8_t {
    Production = 0,
    Service = 1,
};

inline constexpr std::size_t kServiceModeCount = 2;
inline constexpr std::array<std::string_view, kServiceModeCount> kServiceModeNames = {"production", "service"};

[[nodiscard]] inline std::string_view to_string(ServiceMode value) noexcept {
    return detail::enum_name(value, kServiceModeNames);
}
[[nodiscard]] inline Result<ServiceMode> parse_service_mode(std::uint8_t value) {
    return detail::enum_parse<ServiceMode>(value, kServiceModeNames, "service mode");
}

/// Origin of a piece of evidence, used to explain staleness decisions.
enum class EvidenceOrigin : std::uint8_t {
    Absent = 0,
    Adapter = 1,
    RecoveredFromStore = 2,
};

inline constexpr std::size_t kEvidenceOriginCount = 3;
inline constexpr std::array<std::string_view, kEvidenceOriginCount> kEvidenceOriginNames = {
    "absent", "adapter", "recovered-from-store"};

[[nodiscard]] inline std::string_view to_string(EvidenceOrigin value) noexcept {
    return detail::enum_name(value, kEvidenceOriginNames);
}
[[nodiscard]] inline Result<EvidenceOrigin> parse_evidence_origin(std::uint8_t value) {
    return detail::enum_parse<EvidenceOrigin>(value, kEvidenceOriginNames, "evidence origin");
}

/// Why a piece of evidence cannot be used for an authority decision.
enum class EvidenceUsability : std::uint8_t {
    Usable = 0,
    Absent = 1,
    Stale = 2,
    Unsupported = 3,
    Recovered = 4,
    Future = 5,
};

inline constexpr std::size_t kEvidenceUsabilityCount = 6;
inline constexpr std::array<std::string_view, kEvidenceUsabilityCount> kEvidenceUsabilityNames = {
    "usable", "absent", "stale", "unsupported", "recovered", "future"};

[[nodiscard]] inline std::string_view to_string(EvidenceUsability value) noexcept {
    return detail::enum_name(value, kEvidenceUsabilityNames);
}
[[nodiscard]] inline Result<EvidenceUsability> parse_evidence_usability(std::uint8_t value) {
    return detail::enum_parse<EvidenceUsability>(value, kEvidenceUsabilityNames, "evidence usability");
}

// ---------------------------------------------------------------------------
// Durable records
// ---------------------------------------------------------------------------

/// One actuatable or observable device registered in a loop.
struct DeviceRecord final {
    DeviceId id{};
    DeviceKind kind{DeviceKind::Unknown};
    DeviceGeneration generation{};
    LifecycleState lifecycle{LifecycleState::Unknown};
    ValvePosition valve_position{ValvePosition::Unknown};
    PumpState pump_state{PumpState::Unknown};
    /// Generation the reported valve position / pump state belongs to.
    DeviceGeneration state_generation{};
    /// Monotonic duty counter supplied as typed evidence by the plant layer.
    std::uint64_t duty_minutes{0};
};

/// Kind of protected service obligation attached to a device.
enum class ObligationKind : std::uint8_t {
    Unknown = 0,
    ServiceDue = 1,
    LockoutTag = 2,
    CalibrationDue = 3,
    CoolantQualificationDue = 4,
};

inline constexpr std::size_t kObligationKindCount = 5;
inline constexpr std::array<std::string_view, kObligationKindCount> kObligationKindNames = {
    "unknown", "service-due", "lockout-tag", "calibration-due", "coolant-qualification-due"};

[[nodiscard]] inline std::string_view to_string(ObligationKind value) noexcept {
    return detail::enum_name(value, kObligationKindNames);
}
[[nodiscard]] inline Result<ObligationKind> parse_obligation_kind(std::uint8_t value) {
    return detail::enum_parse<ObligationKind>(value, kObligationKindNames, "obligation kind");
}

/// A protected service obligation.  Lockout tags block every actuation on the
/// target; other obligations block exposure-increasing actuation only.
struct ServiceObligation final {
    ObligationId id{};
    DeviceId target{};
    ObligationKind kind{ObligationKind::Unknown};
    bool active{false};
    /// Duty threshold in minutes; zero means the obligation is not duty driven.
    std::uint64_t due_at_duty_minutes{0};
    TimestampNs raised_at{};
    std::string note{};
};

/// Convenience predicate: does this obligation forbid all actuation?
[[nodiscard]] constexpr bool obligation_forbids_all_actuation(ObligationKind kind) noexcept {
    return kind == ObligationKind::LockoutTag;
}

/// A registered cooling loop with its device membership and control state.
struct LoopRecord final {
    LoopId id{};
    std::string name{};
    TopologyGeneration topology_generation{};
    LoopOperatingState operating{LoopOperatingState::Unknown};
    IsolationState isolation{IsolationState::Unknown};
    ServiceMode service_mode{ServiceMode::Production};
    FlowRate flow_target{};
    Pressure pressure_target{};
    DeviceGeneration flow_target_generation{};
    DeviceGeneration pressure_target_generation{};
    std::vector<DeviceRecord> devices{};

    [[nodiscard]] const DeviceRecord* find_device(DeviceId id) const noexcept;
    [[nodiscard]] DeviceRecord* find_device(DeviceId id) noexcept;
};

/// An open service window: explicit service authority for one loop.
struct ServiceWindow final {
    ServiceWindowId id{};
    LoopId loop{};
    PrincipalId opened_by{};
    TimestampNs opened_at{};
    TimestampNs expires_at{};
    std::string note{};
};

}  // namespace liquidcooling
