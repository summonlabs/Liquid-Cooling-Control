#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "liquidcooling/adapter.hpp"
#include "liquidcooling/time.hpp"

namespace liquidcooling {

/// How the synthetic plant answers a command.
enum class SyntheticAckMode : std::uint8_t {
    Accept = 0,
    Reject = 1,
    Unavailable = 2,
    Busy = 3,
    InternalFailure = 4,
    DeviceMissing = 5,
    GenerationMismatch = 6,
};

inline constexpr std::size_t kSyntheticAckModeCount = 7;
inline constexpr std::array<std::string_view, kSyntheticAckModeCount> kSyntheticAckModeNames = {
    "accept", "reject", "unavailable", "busy", "internal-failure", "device-missing", "generation-mismatch"};

[[nodiscard]] inline std::string_view to_string(SyntheticAckMode value) noexcept {
    return detail::enum_name(value, kSyntheticAckModeNames);
}
[[nodiscard]] inline Result<SyntheticAckMode> parse_synthetic_ack_mode(std::uint8_t value) {
    return detail::enum_parse<SyntheticAckMode>(value, kSyntheticAckModeNames, "synthetic ack mode");
}

/// A device exposed by the synthetic plant.
struct SyntheticDevice final {
    DeviceId id{};
    DeviceKind kind{DeviceKind::Unknown};
    DeviceGeneration generation{};
};

/// One scripted response.  A step is consumed by each accepted actuation.
struct SyntheticStep final {
    SyntheticAckMode ack_mode{SyntheticAckMode::Accept};
    /// Observation batches served before the commanded effect becomes visible.
    std::uint32_t effect_delay_samples{0};
    /// When false the command is acknowledged but never takes effect.
    bool applies_effect{true};
    /// Report a valve/pump state opposite to the command.
    bool contradictory_position{false};
    /// Report supply pressure above any sane policy maximum.
    bool pressure_excursion{false};
    /// Replay the previous observation sequence instead of advancing it.
    bool stale_sequence{false};
    /// Leave the loop reporting a confirmed leak after this command.
    bool leak_confirmed{false};
    /// Report the commanded device absent.
    bool device_absent{false};
    /// Stamp observations one hour ahead of the injected clock.
    bool future_timestamp{false};
    /// Declare the leak channel unsupported.
    bool unsupported_leak_channel{false};
    /// Report these duty minutes from this step onwards when apply_duty_minutes is set.
    std::uint64_t duty_minutes{0};
    bool apply_duty_minutes{false};
    /// Run the loop below the minimum effective flow while a pump runs.
    bool starved_flow{false};
    /// Report coolant quality from this step onwards.
    CoolantQuality coolant_quality{CoolantQuality::Nominal};
    /// Evidence generation to advertise from this step onwards.
    EvidenceGeneration evidence_generation{};
    /// Switchover generation to advertise from this step onwards.
    SwitchoverGeneration switchover_generation{};
};

/// Baseline physical description of the synthetic plant.
struct SyntheticPlantConfig final {
    LoopId loop{};
    std::string vendor{"synthetic"};
    std::string model{"loop-sim-1"};
    std::vector<SyntheticDevice> devices{};
    FlowRate nominal_flow{FlowRate::from_value(120'000)};
    Pressure nominal_supply{Pressure::from_value(350'000)};
    Pressure nominal_return{Pressure::from_value(180'000)};
    Pressure excursion_pressure{Pressure::from_value(4'000'000)};
    Temperature nominal_supply_temperature{Temperature::from_value(18'000)};
    Temperature nominal_return_temperature{Temperature::from_value(32'000)};
    Conductivity nominal_conductivity{Conductivity::from_value(450)};
    Acidity nominal_acidity{Acidity::from_value(7'400)};
    LeakState initial_leak{LeakState::None};
    CoolantQuality initial_quality{CoolantQuality::Nominal};
    std::uint64_t initial_duty_minutes{0};
    EvidenceGeneration evidence_generation{EvidenceGeneration::from_value(1)};
};

/// Deterministic synthetic loop/CDU/pump/valve plant.
///
/// The plant is a simulation.  It exercises control semantics and is never
/// evidence of behaviour on real hardware.  All scheduling is driven by the
/// injected clock and by explicit step scripts, so runs are reproducible.
class SyntheticAdapter final : public ILoopAdapter {
public:
    SyntheticAdapter(SyntheticPlantConfig config, std::shared_ptr<Clock> clock);

    [[nodiscard]] AdapterDescriptor describe() const override;
    [[nodiscard]] Result<CommandAck> actuate(const ActuationCommand& command) override;
    [[nodiscard]] Result<LoopReading> read(const ObservationRequest& request) override;

    // --- Script control -----------------------------------------------------
    void push_step(SyntheticStep step);
    void set_steady(SyntheticStep step);
    [[nodiscard]] SyntheticStep steady() const;

    // --- Plant control ------------------------------------------------------
    void set_leak_state(LeakState state);
    void set_coolant_quality(CoolantQuality quality);
    void set_device_present(DeviceId id, bool present);
    void set_unavailable(bool unavailable);
    void set_duty_minutes(std::uint64_t minutes);
    void bump_generation(DeviceId id);
    void set_device_generation(DeviceId id, DeviceGeneration generation);
    void set_evidence_generation(EvidenceGeneration generation);
    void bump_switchover();
    void set_flow(FlowRate flow);
    void set_supply_pressure(Pressure pressure);
    void set_pump_state(DeviceId id, PumpState state);
    void set_valve_position(DeviceId id, ValvePosition position);
    /// Forces the next read to replay the previous sequence number.
    void force_stale_sequence_once();

    // --- Inspection ---------------------------------------------------------
    [[nodiscard]] std::size_t actuation_count() const noexcept;
    [[nodiscard]] std::size_t read_count() const noexcept;
    [[nodiscard]] std::vector<ActuationCommand> commands() const;
    [[nodiscard]] ValvePosition valve_position(DeviceId id) const;
    [[nodiscard]] PumpState pump_state(DeviceId id) const;
    [[nodiscard]] FlowRate measured_flow() const;
    [[nodiscard]] Pressure measured_supply_pressure() const;
    [[nodiscard]] bool any_pump_running() const;
    [[nodiscard]] bool all_valves_closed() const;
    [[nodiscard]] DeviceGeneration generation_of(DeviceId id) const;
    [[nodiscard]] bool has_device(DeviceId id) const;
    /// Applies any pending effect without waiting for a read.
    void settle();

private:
    struct PendingEffect final {
        bool active{false};
        ActionKind action{ActionKind::Unknown};
        DeviceId target{};
        FlowRate flow{};
        Pressure pressure{};
        std::uint32_t remaining{0};
        bool applies{true};
        bool contradictory{false};
    };

    void apply_effect_locked(const PendingEffect& effect);
    [[nodiscard]] ObservationSequence advance_sequence_locked();
    [[nodiscard]] DeviceGeneration generation_of_locked(DeviceId id) const;

    mutable std::mutex mutex_{};
    SyntheticPlantConfig config_{};
    std::shared_ptr<Clock> clock_{};

    std::vector<SyntheticStep> steps_{};
    std::size_t next_step_{0};
    SyntheticStep steady_{};

    std::unordered_map<std::uint64_t, ValvePosition> valve_positions_{};
    std::unordered_map<std::uint64_t, PumpState> pump_states_{};
    std::unordered_map<std::uint64_t, bool> present_{};
    std::unordered_map<std::uint64_t, DeviceGeneration> generations_{};
    std::unordered_map<std::uint64_t, DeviceKind> kinds_{};

    PendingEffect pending_{};
    FlowRate flow_{};
    Pressure supply_pressure_{};
    Pressure return_pressure_{};
    Temperature supply_temperature_{};
    Temperature return_temperature_{};
    Conductivity conductivity_{};
    Acidity acidity_{};
    LeakState leak_{LeakState::None};
    CoolantQuality coolant_quality_{CoolantQuality::Nominal};
    std::uint64_t duty_minutes_{0};
    EvidenceGeneration evidence_generation_{};
    SwitchoverGeneration switchover_generation_{};
    ObservationSequence sequence_{};
    ObservationSequence last_published_{};
    bool stale_sequence_once_{false};
    bool stale_sequence_{false};
    bool unavailable_{false};
    bool future_timestamp_{false};
    bool unsupported_leak_{false};
    bool excursion_{false};
    bool starved_flow_{false};
    std::size_t actuations_{0};
    std::size_t reads_{0};
    std::vector<ActuationCommand> commands_{};
};

}  // namespace liquidcooling
