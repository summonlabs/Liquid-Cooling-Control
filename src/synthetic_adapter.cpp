#include "liquidcooling/synthetic_adapter.hpp"

#include <algorithm>
#include <utility>

namespace liquidcooling {
namespace {

constexpr std::size_t kMaxRecordedCommands = 4096;

}  // namespace

SyntheticAdapter::SyntheticAdapter(SyntheticPlantConfig config, std::shared_ptr<Clock> clock)
    : config_(std::move(config)), clock_(std::move(clock)) {
    if (!clock_) {
        clock_ = std::make_shared<SteadyClock>();
    }
    for (const auto& device : config_.devices) {
        kinds_.emplace(device.id.value(), device.kind);
        generations_.emplace(device.id.value(), device.generation);
        present_.emplace(device.id.value(), true);
        if (device.kind == DeviceKind::Valve) {
            valve_positions_.emplace(device.id.value(), ValvePosition::Closed);
        } else if (device.kind == DeviceKind::Pump) {
            pump_states_.emplace(device.id.value(), PumpState::Stopped);
        }
    }
    flow_ = config_.nominal_flow;
    supply_pressure_ = config_.nominal_supply;
    return_pressure_ = config_.nominal_return;
    supply_temperature_ = config_.nominal_supply_temperature;
    return_temperature_ = config_.nominal_return_temperature;
    conductivity_ = config_.nominal_conductivity;
    acidity_ = config_.nominal_acidity;
    leak_ = config_.initial_leak;
    coolant_quality_ = config_.initial_quality;
    duty_minutes_ = config_.initial_duty_minutes;
    evidence_generation_ = config_.evidence_generation;
    steady_.duty_minutes = config_.initial_duty_minutes;
    steady_.apply_duty_minutes = true;
    steady_.coolant_quality = config_.initial_quality;
    steady_.evidence_generation = config_.evidence_generation;
}

AdapterDescriptor SyntheticAdapter::describe() const {
    std::lock_guard<std::mutex> lock(mutex_);
    AdapterDescriptor descriptor;
    descriptor.vendor = config_.vendor;
    descriptor.model = config_.model;
    descriptor.protocol_version = 1;
    descriptor.evidence_generation = evidence_generation_;
    descriptor.switchover_generation = switchover_generation_;
    return descriptor;
}

void SyntheticAdapter::push_step(SyntheticStep step) { 
    std::lock_guard<std::mutex> lock(mutex_);
    steps_.push_back(std::move(step));
}

void SyntheticAdapter::set_steady(SyntheticStep step) {
    std::lock_guard<std::mutex> lock(mutex_);
    steady_ = std::move(step);
}

SyntheticStep SyntheticAdapter::steady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return steady_;
}

void SyntheticAdapter::set_leak_state(LeakState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    leak_ = state;
}

void SyntheticAdapter::set_coolant_quality(CoolantQuality quality) {
    std::lock_guard<std::mutex> lock(mutex_);
    coolant_quality_ = quality;
}

void SyntheticAdapter::set_device_present(DeviceId id, bool present) {
    std::lock_guard<std::mutex> lock(mutex_);
    present_[id.value()] = present;
}

void SyntheticAdapter::set_unavailable(bool unavailable) {
    std::lock_guard<std::mutex> lock(mutex_);
    unavailable_ = unavailable;
}

void SyntheticAdapter::set_duty_minutes(std::uint64_t minutes) {
    std::lock_guard<std::mutex> lock(mutex_);
    duty_minutes_ = minutes;
}

void SyntheticAdapter::bump_generation(DeviceId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = generations_.find(id.value());
    if (it != generations_.end()) {
        it->second = it->second.next();
    }
}

void SyntheticAdapter::set_device_generation(DeviceId id, DeviceGeneration generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    generations_[id.value()] = generation;
}

void SyntheticAdapter::set_evidence_generation(EvidenceGeneration generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    evidence_generation_ = generation;
}

void SyntheticAdapter::bump_switchover() {
    std::lock_guard<std::mutex> lock(mutex_);
    switchover_generation_ = switchover_generation_.next();
}

void SyntheticAdapter::set_flow(FlowRate flow) {
    std::lock_guard<std::mutex> lock(mutex_);
    flow_ = flow;
}

void SyntheticAdapter::set_supply_pressure(Pressure pressure) {
    std::lock_guard<std::mutex> lock(mutex_);
    supply_pressure_ = pressure;
}

void SyntheticAdapter::set_pump_state(DeviceId id, PumpState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = pump_states_.find(id.value());
    if (it != pump_states_.end()) {
        it->second = state;
    }
}

void SyntheticAdapter::set_valve_position(DeviceId id, ValvePosition position) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = valve_positions_.find(id.value());
    if (it != valve_positions_.end()) {
        it->second = position;
    }
}

void SyntheticAdapter::force_stale_sequence_once() {
    std::lock_guard<std::mutex> lock(mutex_);
    stale_sequence_once_ = true;
}

std::size_t SyntheticAdapter::actuation_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return actuations_;
}

std::size_t SyntheticAdapter::read_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return reads_;
}

std::vector<ActuationCommand> SyntheticAdapter::commands() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return commands_;
}

ValvePosition SyntheticAdapter::valve_position(DeviceId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = valve_positions_.find(id.value());
    return it == valve_positions_.end() ? ValvePosition::Unknown : it->second;
}

PumpState SyntheticAdapter::pump_state(DeviceId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = pump_states_.find(id.value());
    return it == pump_states_.end() ? PumpState::Unknown : it->second;
}

bool SyntheticAdapter::any_pump_running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : pump_states_) {
        if (entry.second == PumpState::Running) {
            return true;
        }
    }
    return false;
}

bool SyntheticAdapter::all_valves_closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : valve_positions_) {
        if (entry.second != ValvePosition::Closed) {
            return false;
        }
    }
    return true;
}

FlowRate SyntheticAdapter::measured_flow() const {
    std::lock_guard<std::mutex> lock(mutex_);
    bool running = false;
    for (const auto& entry : pump_states_) {
        if (entry.second == PumpState::Running) {
            running = true;
            break;
        }
    }
    if (!running || starved_flow_) {
        return FlowRate::from_value(0);
    }
    return flow_;
}

Pressure SyntheticAdapter::measured_supply_pressure() const {
    std::lock_guard<std::mutex> lock(mutex_);
    bool running = false;
    for (const auto& entry : pump_states_) {
        if (entry.second == PumpState::Running) {
            running = true;
            break;
        }
    }
    if (!running) {
        return Pressure::from_value(0);
    }
    return excursion_ ? config_.excursion_pressure : supply_pressure_;
}

DeviceGeneration SyntheticAdapter::generation_of(DeviceId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = generations_.find(id.value());
    return it == generations_.end() ? DeviceGeneration::invalid() : it->second;
}

bool SyntheticAdapter::has_device(DeviceId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kinds_.find(id.value()) != kinds_.end();
}

void SyntheticAdapter::settle() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.active && pending_.applies) {
        apply_effect_locked(pending_);
    }
    pending_ = PendingEffect{};
}

void SyntheticAdapter::apply_effect_locked(const PendingEffect& effect) {
    switch (effect.action) {
        case ActionKind::StartPump: {
            auto it = pump_states_.find(effect.target.value());
            if (it != pump_states_.end()) {
                it->second = effect.contradictory ? PumpState::Stopped : PumpState::Running;
            }
            break;
        }
        case ActionKind::StopPump: {
            auto it = pump_states_.find(effect.target.value());
            if (it != pump_states_.end()) {
                it->second = effect.contradictory ? PumpState::Running : PumpState::Stopped;
            }
            break;
        }
        case ActionKind::OpenValve: {
            auto it = valve_positions_.find(effect.target.value());
            if (it != valve_positions_.end()) {
                it->second = effect.contradictory ? ValvePosition::Closed : ValvePosition::Open;
            }
            break;
        }
        case ActionKind::CloseValve: {
            auto it = valve_positions_.find(effect.target.value());
            if (it != valve_positions_.end()) {
                it->second = effect.contradictory ? ValvePosition::Open : ValvePosition::Closed;
            }
            break;
        }
        case ActionKind::IsolateLoop: {
            for (auto& entry : valve_positions_) {
                entry.second = effect.contradictory ? ValvePosition::Open : ValvePosition::Closed;
            }
            for (auto& entry : pump_states_) {
                entry.second = effect.contradictory ? PumpState::Running : PumpState::Stopped;
            }
            break;
        }
        case ActionKind::SetFlowTarget:
            flow_ = effect.flow;
            break;
        case ActionKind::SetPressureTarget:
            supply_pressure_ = effect.pressure;
            return_pressure_ = effect.pressure;
            break;
        case ActionKind::Unknown:
        case ActionKind::ClearIsolation:
        case ActionKind::EnterService:
        case ActionKind::ExitService:
        case ActionKind::CompleteObligation:
        case ActionKind::AbandonAttempt:
        case ActionKind::ResetFault:
            break;
    }
}

ObservationSequence SyntheticAdapter::advance_sequence_locked() {
    if (stale_sequence_once_) {
        stale_sequence_once_ = false;
        stale_sequence_ = true;
    }
    if (stale_sequence_) {
        if (!sequence_.valid()) {
            sequence_ = ObservationSequence::from_value(1);
        }
        return sequence_;
    }
    sequence_ = sequence_.valid() ? sequence_.next() : ObservationSequence::from_value(1);
    last_published_ = sequence_;
    return sequence_;
}

Result<CommandAck> SyntheticAdapter::actuate(const ActuationCommand& command) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++actuations_;
    if (commands_.size() < kMaxRecordedCommands) {
        commands_.push_back(command);
    } else {
        commands_.erase(commands_.begin());
        commands_.push_back(command);
    }

    SyntheticStep step = steady_;
    if (next_step_ < steps_.size()) {
        step = steps_[next_step_];
        ++next_step_;
    }

    if (unavailable_ || step.ack_mode == SyntheticAckMode::Unavailable) {
        return Status{StatusCode::AdapterUnavailable, "synthetic adapter reports itself unavailable"};
    }

    CommandAck ack;
    ack.command = command.command;
    ack.attempt = command.attempt;
    ack.generation = command.generation;
    ack.ack_sequence = last_published_;
    ack.acked_at = clock_->now();

    switch (step.ack_mode) {
        case SyntheticAckMode::Accept:
            break;
        case SyntheticAckMode::Reject:
            ack.error = AdapterErrorCode::Rejected;
            ack.detail = "synthetic plant rejected the command";
            return ack;
        case SyntheticAckMode::Busy:
            ack.error = AdapterErrorCode::Busy;
            ack.detail = "synthetic plant is busy";
            return ack;
        case SyntheticAckMode::InternalFailure:
            ack.error = AdapterErrorCode::Internal;
            ack.detail = "synthetic plant reported an internal failure";
            return ack;
        case SyntheticAckMode::DeviceMissing:
            ack.error = AdapterErrorCode::DeviceMissing;
            ack.detail = "synthetic plant does not expose the target device";
            return ack;
        case SyntheticAckMode::GenerationMismatch:
            ack.error = AdapterErrorCode::GenerationMismatch;
            ack.detail = "synthetic plant is on a different device generation";
            return ack;
        case SyntheticAckMode::Unavailable:
            break;
    }

    if (command.target.valid() && kinds_.find(command.target.value()) == kinds_.end()) {
        ack.error = AdapterErrorCode::DeviceMissing;
        ack.detail = "synthetic plant does not expose the target device";
        return ack;
    }

    // Plant-level side effects carried by the step become visible from the next
    // observation onwards.
    if (step.leak_confirmed) {
        leak_ = LeakState::Confirmed;
    }
    if (step.unsupported_leak_channel) {
        unsupported_leak_ = true;
    }
    if (step.stale_sequence) {
        stale_sequence_ = true;
    }
    if (step.future_timestamp) {
        future_timestamp_ = true;
    }
    if (step.pressure_excursion) {
        excursion_ = true;
    }
    if (step.starved_flow) {
        starved_flow_ = true;
    }
    if (step.apply_duty_minutes) {
        duty_minutes_ = step.duty_minutes;
    }
    if (step.coolant_quality != CoolantQuality::Unknown) {
        coolant_quality_ = step.coolant_quality;
    }
    if (step.evidence_generation.valid()) {
        evidence_generation_ = step.evidence_generation;
    }
    if (step.switchover_generation.valid()) {
        switchover_generation_ = step.switchover_generation;
    }
    if (step.device_absent) {
        if (command.target.valid()) {
            present_[command.target.value()] = false;
        } else {
            for (auto& entry : present_) {
                entry.second = false;
            }
        }
    }

    PendingEffect effect;
    effect.active = true;
    effect.action = command.action;
    effect.target = command.target;
    effect.flow = command.flow_target;
    effect.pressure = command.pressure_target;
    effect.remaining = step.effect_delay_samples;
    effect.applies = step.applies_effect;
    effect.contradictory = step.contradictory_position;
    pending_ = effect;

    return ack;
}

Result<LoopReading> SyntheticAdapter::read(const ObservationRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unavailable_) {
        return Status{StatusCode::AdapterUnavailable, "synthetic adapter reports itself unavailable"};
    }
    ++reads_;

    if (pending_.active) {
        if (!pending_.applies) {
            pending_ = PendingEffect{};
        } else if (pending_.remaining > 0) {
            --pending_.remaining;
        } else {
            apply_effect_locked(pending_);
            pending_ = PendingEffect{};
        }
    }

    const ObservationSequence sequence = advance_sequence_locked();

    DeviceId focus = request.target;
    if (!focus.valid() && !config_.devices.empty()) {
        focus = config_.devices.front().id;
    }

    TimestampNs observed_at = clock_->now();
    if (future_timestamp_) {
        const auto shifted = checked_add(observed_at, Duration::from_value(3'600'000'000'000));
        if (shifted.ok()) {
            observed_at = shifted.value();
        }
    }

    const DeviceGeneration generation =
        focus.valid() ? generation_of_locked(focus) : DeviceGeneration::from_value(1);
    const std::string_view source{config_.vendor};

    LoopReading reading;
    reading.loop = request.loop.valid() ? request.loop : config_.loop;
    reading.sequence = sequence;
    reading.evidence_generation = evidence_generation_;
    reading.switchover_generation = switchover_generation_;
    reading.observed_at = observed_at;

    bool is_present = true;
    const auto presence_it = present_.find(focus.value());
    if (presence_it != present_.end()) {
        is_present = presence_it->second;
    }
    reading.device_present = Evidence<bool>::make_present(is_present, sequence, generation, observed_at, source);
    reading.device_generation =
        Evidence<DeviceGeneration>::make_present(generation, sequence, generation, observed_at, source);

    const auto kind_it = kinds_.find(focus.value());
    const DeviceKind kind = kind_it == kinds_.end() ? DeviceKind::Unknown : kind_it->second;

    if (kind == DeviceKind::Valve) {
        const auto it = valve_positions_.find(focus.value());
        const ValvePosition position = it == valve_positions_.end() ? ValvePosition::Unknown : it->second;
        reading.valve_position =
            Evidence<ValvePosition>::make_present(position, sequence, generation, observed_at, source);
    }
    if (kind == DeviceKind::Pump) {
        const auto it = pump_states_.find(focus.value());
        const PumpState state = it == pump_states_.end() ? PumpState::Unknown : it->second;
        reading.pump_state = Evidence<PumpState>::make_present(state, sequence, generation, observed_at, source);
    }

    bool running = false;
    for (const auto& entry : pump_states_) {
        if (entry.second == PumpState::Running) {
            running = true;
            break;
        }
    }

    const FlowRate reported_flow = (!running || starved_flow_) ? FlowRate::from_value(0) : flow_;
    reading.flow = Evidence<FlowRate>::make_present(reported_flow, sequence, generation, observed_at, source);

    const Pressure reported_supply =
        running ? (excursion_ ? config_.excursion_pressure : supply_pressure_) : Pressure::from_value(0);
    reading.supply_pressure =
        Evidence<Pressure>::make_present(reported_supply, sequence, generation, observed_at, source);
    reading.return_pressure = Evidence<Pressure>::make_present(
        running ? return_pressure_ : Pressure::from_value(0), sequence, generation, observed_at, source);
    reading.supply_temperature =
        Evidence<Temperature>::make_present(supply_temperature_, sequence, generation, observed_at, source);
    reading.return_temperature =
        Evidence<Temperature>::make_present(return_temperature_, sequence, generation, observed_at, source);
    reading.conductivity =
        Evidence<Conductivity>::make_present(conductivity_, sequence, generation, observed_at, source);
    reading.acidity = Evidence<Acidity>::make_present(acidity_, sequence, generation, observed_at, source);
    reading.coolant_quality =
        Evidence<CoolantQuality>::make_present(coolant_quality_, sequence, generation, observed_at, source);
    reading.duty_minutes =
        Evidence<std::uint64_t>::make_present(duty_minutes_, sequence, generation, observed_at, source);

    if (unsupported_leak_) {
        reading.leak = Evidence<LeakState>::make_unsupported(source);
    } else {
        reading.leak = Evidence<LeakState>::make_present(leak_, sequence, generation, observed_at, source);
    }

    last_published_ = sequence;
    return reading;
}

DeviceGeneration SyntheticAdapter::generation_of_locked(DeviceId id) const {
    const auto it = generations_.find(id.value());
    return it == generations_.end() ? DeviceGeneration::invalid() : it->second;
}

}  // namespace liquidcooling
