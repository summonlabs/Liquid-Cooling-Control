#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "liquidcooling/ids.hpp"
#include "liquidcooling/model.hpp"
#include "liquidcooling/status.hpp"
#include "liquidcooling/time.hpp"
#include "liquidcooling/units.hpp"

namespace liquidcooling {

/// A single evidence channel value together with its provenance.
///
/// Absent, unsupported, stale and future are distinct from any physical value.
/// A default-constructed Evidence is absent, never zero-valued usable data.
/// Recovered evidence (loaded from the durable store) is never treated as
/// current physical evidence until revalidated by an adapter read.
template <typename T>
struct Evidence final {
    /// True when a value has ever been recorded for this channel.
    bool present{false};
    /// True when the adapter explicitly declared the channel unsupported.
    bool unsupported{false};
    T value{};
    ObservationSequence sequence{};
    DeviceGeneration generation{};
    TimestampNs observed_at{};
    EvidenceOrigin origin{EvidenceOrigin::Absent};
    std::string source{};

    [[nodiscard]] bool is_absent() const noexcept { return !present && !unsupported; }
    [[nodiscard]] bool is_unsupported() const noexcept { return unsupported; }
    [[nodiscard]] bool is_recovered() const noexcept { return origin == EvidenceOrigin::RecoveredFromStore; }

    static Evidence make_unsupported(std::string_view who) {
        Evidence e;
        e.unsupported = true;
        e.source.assign(who);
        return e;
    }

    static Evidence make_absent(std::string_view who) {
        Evidence e;
        e.source.assign(who);
        return e;
    }

    static Evidence make_present(T v, ObservationSequence seq, DeviceGeneration gen, TimestampNs at,
                                 std::string_view who) {
        Evidence e;
        e.present = true;
        e.value = v;
        e.sequence = seq;
        e.generation = gen;
        e.observed_at = at;
        e.origin = EvidenceOrigin::Adapter;
        e.source.assign(who);
        return e;
    }
};

/// Evaluates whether evidence may be used for an authority decision.
///
/// The order of the tests is the documented precedence and is deterministic.
[[nodiscard]] inline EvidenceUsability evaluate_usability(TimestampNs observed_at,
                                                          bool present,
                                                          bool unsupported,
                                                          bool recovered,
                                                          TimestampNs now,
                                                          Duration freshness_window) noexcept {
    if (unsupported) {
        return EvidenceUsability::Unsupported;
    }
    if (!present) {
        return EvidenceUsability::Absent;
    }
    if (recovered) {
        return EvidenceUsability::Recovered;
    }
    if (observed_at > now) {
        return EvidenceUsability::Future;
    }
    if (elapsed(observed_at, now) > freshness_window) {
        return EvidenceUsability::Stale;
    }
    return EvidenceUsability::Usable;
}

template <typename T>
[[nodiscard]] EvidenceUsability evaluate_usability(const Evidence<T>& evidence, TimestampNs now,
                                                   Duration freshness_window) noexcept {
    return evaluate_usability(evidence.observed_at, evidence.present, evidence.unsupported,
                              evidence.is_recovered(), now, freshness_window);
}

/// Maps an unusable evidence state onto the status code reported to callers.
[[nodiscard]] StatusCode usability_status(EvidenceUsability usability) noexcept;

/// A full set of loop evidence as read from an adapter at one instant.
///
/// Every channel is optional and independently typed; a missing channel is never
/// coerced to zero.
struct LoopReading final {
    LoopId loop{};
    /// Monotonic sequence assigned by the adapter to this batch.
    ObservationSequence sequence{};
    /// Generation of the adapter evidence set at the time of the read.
    EvidenceGeneration evidence_generation{};
    SwitchoverGeneration switchover_generation{};
    TimestampNs observed_at{};

    Evidence<bool> device_present{};
    Evidence<DeviceGeneration> device_generation{};
    Evidence<ValvePosition> valve_position{};
    Evidence<PumpState> pump_state{};
    Evidence<FlowRate> flow{};
    Evidence<Pressure> supply_pressure{};
    Evidence<Pressure> return_pressure{};
    Evidence<Temperature> supply_temperature{};
    Evidence<Temperature> return_temperature{};
    Evidence<Conductivity> conductivity{};
    Evidence<Acidity> acidity{};
    Evidence<LeakState> leak{};
    Evidence<CoolantQuality> coolant_quality{};
    Evidence<std::uint64_t> duty_minutes{};
};

/// A specific, machine-checkable disagreement between evidence channels or
/// between a command intent and the resulting observation.
struct Contradiction final {
    StatusCode code{StatusCode::Ok};
    std::string detail{};
};

}  // namespace liquidcooling
