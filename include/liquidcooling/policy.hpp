#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "liquidcooling/authority.hpp"
#include "liquidcooling/enum_support.hpp"
#include "liquidcooling/ids.hpp"
#include "liquidcooling/status.hpp"
#include "liquidcooling/units.hpp"

namespace liquidcooling {

/// Named interlocks.  Every interlock is a pure predicate over current evidence,
/// authority and durable state; the evaluation order is the enumerator order and
/// is therefore deterministic.
enum class InterlockKind : std::uint8_t {
    LeakBlocksIncrease = 0,
    IndeterminateLeakBlocksIncrease = 1,
    CoolantQualityBlocksStart = 2,
    OverPressureBlocksIncrease = 3,
    FrozenEvidenceBlocksActuation = 4,
    UnresolvedAttemptBlocksIncompatible = 5,
    ServiceModeBlocksProduction = 6,
    DeviceAbsenceBlocksActuation = 7,
};

inline constexpr std::size_t kInterlockKindCount = 8;
inline constexpr std::array<std::string_view, kInterlockKindCount> kInterlockKindNames = {
    "leak-blocks-increase",
    "indeterminate-leak-blocks-increase",
    "coolant-quality-blocks-start",
    "over-pressure-blocks-increase",
    "frozen-evidence-blocks-actuation",
    "unresolved-attempt-blocks-incompatible",
    "service-mode-blocks-production",
    "device-absence-blocks-actuation",
};

[[nodiscard]] inline std::string_view to_string(InterlockKind value) noexcept {
    return detail::enum_name(value, kInterlockKindNames);
}
[[nodiscard]] inline Result<InterlockKind> parse_interlock_kind(std::uint8_t value) {
    return detail::enum_parse<InterlockKind>(value, kInterlockKindNames, "interlock kind");
}

/// Exact-integer operating envelope and resource bounds.
struct PolicyLimits final {
    QuantityRange<FlowRate> flow{FlowRate::from_value(0), FlowRate::from_value(3'000'000)};
    QuantityRange<Pressure> supply_pressure{Pressure::from_value(10'000), Pressure::from_value(1'500'000)};
    QuantityRange<Temperature> coolant_temperature{Temperature::from_value(-10'000), Temperature::from_value(60'000)};
    QuantityRange<Conductivity> conductivity{Conductivity::from_value(0), Conductivity::from_value(200'000)};
    QuantityRange<Acidity> acidity{Acidity::from_value(6'000), Acidity::from_value(9'000)};

    /// Flow below this value while a pump reports running is a contradiction.
    FlowRate minimum_effective_flow{FlowRate::from_value(500)};

    /// Age beyond which evidence may not authorise a transition.
    Duration evidence_freshness{Duration::from_value(5'000'000'000)};
    /// How far ahead of the runtime clock an adapter sample may be stamped.
    Duration observation_future_tolerance{Duration::from_value(1'000'000'000)};
    /// Longest authority lease that may be minted.
    Duration authority_lease_max{Duration::from_value(3'600'000'000'000)};
    /// Longest service window that may be opened.
    Duration service_window_max{Duration::from_value(86'400'000'000'000)};
    /// Largest accepted change of the flow setpoint in one transition.
    FlowRate maximum_flow_step{FlowRate::from_value(500'000)};
    /// Largest accepted change of the pressure setpoint in one transition.
    Pressure maximum_pressure_step{Pressure::from_value(200'000)};

    /// Window after issuing a command during which a not-yet-visible effect is
    /// still treated as pending rather than as a contradiction.
    Duration effect_deadline{Duration::from_value(5'000'000'000)};
    /// Absolute flow tolerance for setpoint effect verification.
    FlowRate flow_tolerance{FlowRate::from_value(2'000)};
    /// Absolute pressure tolerance for setpoint effect verification.
    Pressure pressure_tolerance{Pressure::from_value(5'000)};

    std::size_t max_loops{64};
    std::size_t max_devices_per_loop{64};
    std::size_t max_open_attempts{64};
    std::size_t max_retained_attempts{256};
    std::size_t max_obligations{256};
    std::size_t max_service_windows{64};
    std::size_t max_issued_tokens{4096};
    std::size_t max_idempotency_records{1024};
    std::size_t max_journal_entries{512};
    std::size_t max_revoked_tokens{4096};
};

/// The complete configuration in force.  The generation is stamped on every
/// authority token and plan so that a configuration change fences in-flight work.
struct Policy final {
    ConfigGeneration generation{};
    PolicyLimits limits{};
    std::array<bool, kInterlockKindCount> interlocks{};
    PrincipalRegistry principals{};

    /// The shipped default policy with every interlock enabled.
    [[nodiscard]] static Policy defaults(ConfigGeneration generation);

    [[nodiscard]] bool interlock_enabled(InterlockKind kind) const noexcept {
        return interlocks[static_cast<std::size_t>(kind)];
    }
};

[[nodiscard]] Result<void> validate(const PolicyLimits& limits);
[[nodiscard]] Result<void> validate(const Policy& policy);

}  // namespace liquidcooling
