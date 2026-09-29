#include "liquidcooling/runtime.hpp"

#include <algorithm>
#include <exception>
#include <set>
#include <utility>

#include "platform.hpp"

namespace liquidcooling {
namespace {

/// Verdict of comparing one post-command observation with the commanded intent.
enum class Verdict : std::uint8_t {
    Confirmed = 0,
    Contradicted = 1,
    Pending = 2,
    Unusable = 3,
};

struct EffectEvaluation final {
    Verdict verdict{Verdict::Unusable};
    StatusCode code{StatusCode::EffectNotObserved};
    std::string detail{};
    ObservationSequence sequence{};
    DeviceGeneration generation{};
};

[[nodiscard]] bool channel_usable(bool present, bool unsupported, bool recovered) noexcept {
    return present && !unsupported && !recovered;
}

template <typename T>
[[nodiscard]] bool usable(const Evidence<T>& channel) noexcept {
    return channel_usable(channel.present, channel.unsupported, channel.is_recovered());
}

[[nodiscard]] std::string owner_note() {
    return "liquidcooling pid=" + std::to_string(detail::current_process_id());
}

[[nodiscard]] EffectEvaluation unusable(StatusCode code, std::string detail) {
    EffectEvaluation evaluation;
    evaluation.verdict = Verdict::Unusable;
    evaluation.code = code;
    evaluation.detail = std::move(detail);
    return evaluation;
}

[[nodiscard]] EffectEvaluation evaluate_effect(const CommandAttempt& attempt,
                                               const LoopReading& reading,
                                               const PolicyLimits& limits) {
    EffectEvaluation evaluation;
    evaluation.sequence = reading.sequence;

    if (usable(reading.device_generation)) {
        evaluation.generation = reading.device_generation.value;
        // A loop-scoped command carries no device generation binding; a
        // device-scoped command must be observed on the generation it was
        // issued against, otherwise the effect cannot be attributed.
        if (attempt.generation.valid() && reading.device_generation.value != attempt.generation) {
            return unusable(StatusCode::EvidenceGenerationMismatch,
                            "observation belongs to device generation " +
                                std::to_string(reading.device_generation.value.value()) +
                                " but the attempt was issued against generation " +
                                std::to_string(attempt.generation.value()));
        }
    } else if (attempt.generation.valid()) {
        return unusable(StatusCode::EvidenceGenerationMismatch,
                        "the adapter did not report a usable device generation");
    }

    switch (attempt.action) {
        case ActionKind::StartPump: {
            if (!usable(reading.pump_state)) {
                return unusable(StatusCode::EvidenceAbsent, "no usable pump state was reported");
            }
            switch (reading.pump_state.value) {
                case PumpState::Running:
                    evaluation.verdict = Verdict::Confirmed;
                    evaluation.code = StatusCode::Ok;
                    evaluation.detail = "pump reports running";
                    return evaluation;
                case PumpState::Stopped:
                case PumpState::Faulted:
                    evaluation.verdict = Verdict::Contradicted;
                    evaluation.code = StatusCode::EffectContradicted;
                    evaluation.detail = "pump reports stopped after a start command";
                    return evaluation;
                case PumpState::Starting:
                    evaluation.verdict = Verdict::Pending;
                    evaluation.code = StatusCode::EffectPending;
                    evaluation.detail = "pump reports starting";
                    return evaluation;
                case PumpState::Unknown:
                    return unusable(StatusCode::EvidenceAbsent, "pump state is unknown");
            }
            return unusable(StatusCode::EvidenceAbsent, "pump state is unusable");
        }
        case ActionKind::StopPump: {
            if (!usable(reading.pump_state)) {
                return unusable(StatusCode::EvidenceAbsent, "no usable pump state was reported");
            }
            switch (reading.pump_state.value) {
                case PumpState::Stopped:
                    evaluation.verdict = Verdict::Confirmed;
                    evaluation.code = StatusCode::Ok;
                    evaluation.detail = "pump reports stopped";
                    return evaluation;
                case PumpState::Running:
                case PumpState::Starting:
                    evaluation.verdict = Verdict::Contradicted;
                    evaluation.code = StatusCode::EffectContradicted;
                    evaluation.detail = "pump still reports running after a stop command";
                    return evaluation;
                case PumpState::Faulted:
                    evaluation.verdict = Verdict::Confirmed;
                    evaluation.code = StatusCode::Ok;
                    evaluation.detail = "pump reports faulted and is not circulating";
                    return evaluation;
                case PumpState::Unknown:
                    return unusable(StatusCode::EvidenceAbsent, "pump state is unknown");
            }
            return unusable(StatusCode::EvidenceAbsent, "pump state is unusable");
        }
        case ActionKind::OpenValve:
        case ActionKind::CloseValve: {
            if (!usable(reading.valve_position)) {
                return unusable(StatusCode::EvidenceAbsent, "no usable valve position was reported");
            }
            const bool wants_open = attempt.action == ActionKind::OpenValve;
            switch (reading.valve_position.value) {
                case ValvePosition::Open:
                    evaluation.verdict = wants_open ? Verdict::Confirmed : Verdict::Contradicted;
                    evaluation.detail = wants_open ? "valve reports open" : "valve reports open after a close command";
                    break;
                case ValvePosition::Closed:
                    evaluation.verdict = wants_open ? Verdict::Contradicted : Verdict::Confirmed;
                    evaluation.detail =
                        wants_open ? "valve reports closed after an open command" : "valve reports closed";
                    break;
                case ValvePosition::Intermediate:
                    evaluation.verdict = Verdict::Pending;
                    evaluation.detail = "valve reports an intermediate position";
                    break;
                case ValvePosition::Faulted:
                    evaluation.verdict = Verdict::Contradicted;
                    evaluation.detail = "valve reports a faulted actuator";
                    break;
                case ValvePosition::Unknown:
                    return unusable(StatusCode::EvidenceAbsent, "valve position is unknown");
            }
            evaluation.code = evaluation.verdict == Verdict::Confirmed
                                 ? StatusCode::Ok
                                 : (evaluation.verdict == Verdict::Contradicted ? StatusCode::EffectContradicted
                                                                                : StatusCode::EffectPending);
            return evaluation;
        }
        case ActionKind::IsolateLoop: {
            if (!usable(reading.flow)) {
                return unusable(StatusCode::EvidenceAbsent, "no usable flow evidence was reported");
            }
            if (reading.flow.value.value() <= 0) {
                evaluation.verdict = Verdict::Confirmed;
                evaluation.code = StatusCode::Ok;
                evaluation.detail = "no flow is measurable on the loop";
                return evaluation;
            }
            evaluation.verdict = Verdict::Contradicted;
            evaluation.code = StatusCode::EffectContradicted;
            evaluation.detail = "flow is still measurable after an isolation command";
            return evaluation;
        }
        case ActionKind::SetFlowTarget: {
            if (!usable(reading.flow) || !usable(reading.supply_pressure)) {
                return unusable(StatusCode::EvidenceAbsent, "no usable flow or pressure evidence was reported");
            }
            if (reading.supply_pressure.value.value() <= 0) {
                return unusable(StatusCode::EffectNotObserved,
                                "the loop is not pressurised so a flow setpoint cannot be observed");
            }
            std::int64_t delta = reading.flow.value.value() - attempt.commanded_flow.value();
            if (delta < 0) {
                delta = -delta;
            }
            if (delta <= limits.flow_tolerance.value()) {
                evaluation.verdict = Verdict::Confirmed;
                evaluation.code = StatusCode::Ok;
                evaluation.detail = "measured flow matches the commanded setpoint";
                return evaluation;
            }
            evaluation.verdict = Verdict::Contradicted;
            evaluation.code = StatusCode::EffectContradicted;
            evaluation.detail = "measured flow does not match the commanded setpoint";
            return evaluation;
        }
        case ActionKind::SetPressureTarget: {
            if (!usable(reading.supply_pressure)) {
                return unusable(StatusCode::EvidenceAbsent, "no usable pressure evidence was reported");
            }
            if (reading.supply_pressure.value.value() <= 0) {
                return unusable(StatusCode::EffectNotObserved,
                                "the loop is not pressurised so a pressure setpoint cannot be observed");
            }
            std::int64_t delta = reading.supply_pressure.value.value() - attempt.commanded_pressure.value();
            if (delta < 0) {
                delta = -delta;
            }
            if (delta <= limits.pressure_tolerance.value()) {
                evaluation.verdict = Verdict::Confirmed;
                evaluation.code = StatusCode::Ok;
                evaluation.detail = "measured pressure matches the commanded setpoint";
                return evaluation;
            }
            evaluation.verdict = Verdict::Contradicted;
            evaluation.code = StatusCode::EffectContradicted;
            evaluation.detail = "measured pressure does not match the commanded setpoint";
            return evaluation;
        }
        case ActionKind::Unknown:
        case ActionKind::ClearIsolation:
        case ActionKind::EnterService:
        case ActionKind::ExitService:
        case ActionKind::CompleteObligation:
        case ActionKind::AbandonAttempt:
        case ActionKind::ResetFault:
            break;
    }
    return unusable(StatusCode::InternalError, "the action has no physical effect to verify");
}

[[nodiscard]] bool has_idempotency_locked(const DurableState& state, const IdempotencyKey& key) {
    for (const auto& record : state.idempotency) {
        if (record.key == key) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool is_production_action(EffectClass effect) noexcept {
    return effect == EffectClass::IncreaseExposure;
}

}  // namespace


void trim_attempts(DurableState& state, std::size_t max_retained) {
    if (state.attempts.size() <= max_retained) {
        return;
    }
    std::vector<CommandAttempt> kept;
    kept.reserve(state.attempts.size());
    for (const auto& attempt : state.attempts) {
        if (attempt.is_open()) {
            kept.push_back(attempt);
        }
    }
    // Retain the newest terminal attempts up to the bound, never dropping an
    // open attempt: an open attempt carries an unresolved physical obligation.
    const std::size_t terminal_budget = max_retained > kept.size() ? max_retained - kept.size() : 0;
    std::vector<CommandAttempt> terminal;
    for (const auto& attempt : state.attempts) {
        if (!attempt.is_open()) {
            terminal.push_back(attempt);
        }
    }
    const std::size_t drop = terminal.size() > terminal_budget ? terminal.size() - terminal_budget : 0;
    for (std::size_t i = drop; i < terminal.size(); ++i) {
        kept.push_back(terminal[i]);
    }
    std::sort(kept.begin(), kept.end(),
              [](const CommandAttempt& a, const CommandAttempt& b) { return a.id < b.id; });
    state.attempts = std::move(kept);
}

[[nodiscard]] LoopRecord* mutable_loop(DurableState& state, LoopId id) {
    for (auto& loop : state.loops) {
        if (loop.id == id) {
            return &loop;
        }
    }
    return nullptr;
}

[[nodiscard]] LoopReading* mutable_reading(DurableState& state, LoopId id) {
    for (auto& record : state.evidence) {
        if (record.loop == id) {
            return &record.reading;
        }
    }
    return nullptr;
}

/// Applies a transition that changes control-plane state without issuing a
/// physical command.  Every branch is total: an unknown or unhandled action
/// leaves the state unchanged.
void apply_control_plane_action(DurableState& state, const TransitionRequest& request) {
    LoopRecord* loop = mutable_loop(state, request.loop);
    if (loop == nullptr) {
        return;
    }
    switch (request.action) {
        case ActionKind::ClearIsolation:
            loop->isolation = IsolationState::Open;
            loop->operating = LoopOperatingState::Stopped;
            break;
        case ActionKind::EnterService:
            loop->service_mode = ServiceMode::Service;
            loop->operating = LoopOperatingState::Service;
            break;
        case ActionKind::ExitService:
            loop->service_mode = ServiceMode::Production;
            loop->operating = LoopOperatingState::Stopped;
            break;
        case ActionKind::CompleteObligation:
            for (auto& obligation : state.obligations) {
                if (obligation.id == request.obligation) {
                    obligation.active = false;
                    break;
                }
            }
            break;
        case ActionKind::AbandonAttempt:
            for (auto& attempt : state.attempts) {
                if (attempt.id == request.attempt) {
                    attempt.status = AttemptStatus::Abandoned;
                    attempt.outcome = StatusCode::Ok;
                    attempt.detail = "abandoned under explicit service authority";
                    break;
                }
            }
            break;
        case ActionKind::ResetFault:
            if (DeviceRecord* device = loop->find_device(request.target)) {
                device->lifecycle = LifecycleState::Present;
                device->valve_position = ValvePosition::Unknown;
                device->pump_state = PumpState::Unknown;
                device->state_generation = DeviceGeneration::invalid();
            }
            break;
        case ActionKind::Unknown:
        case ActionKind::StartPump:
        case ActionKind::StopPump:
        case ActionKind::OpenValve:
        case ActionKind::CloseValve:
        case ActionKind::SetFlowTarget:
        case ActionKind::SetPressureTarget:
        case ActionKind::IsolateLoop:
            break;
    }
}

/// Validates and absorbs one device reading into candidate state.
///
/// Precedence: identity, then time, then ordering, then content.  A reading that
/// fails any check is discarded whole; partially applied evidence would be worse
/// than no evidence because it could not be reasoned about.
[[nodiscard]] Result<void> absorb_reading(DurableState& state, LoopId loop, DeviceId focus,
                                          const LoopReading& reading, TimestampNs now, const Policy& policy,
                                          std::vector<Contradiction>& contradictions, bool& leak_indeterminate,
                                          bool& leak_confirmed) {
    if (reading.loop != loop) {
        return Status{StatusCode::EvidenceConflicting, "the adapter returned a reading for a different loop"};
    }
    const auto future_limit = checked_add(now, policy.limits.observation_future_tolerance);
    if (future_limit.ok() && future_limit.value() < reading.observed_at) {
        return Status{StatusCode::EvidenceFuture, "the adapter stamped the reading ahead of the runtime clock"};
    }
    LoopReading* stored = mutable_reading(state, loop);
    // A higher switchover generation means the adapter behind this loop was
    // replaced, so the observation sequence legitimately restarts.
    const bool adapter_replaced = stored != nullptr &&
                                  stored->switchover_generation < reading.switchover_generation;
    if (!adapter_replaced && stored != nullptr && stored->sequence.valid() && reading.sequence.valid() &&
        !(stored->sequence < reading.sequence)) {
        return Status{StatusCode::EvidenceStale,
                      "the adapter replayed or regressed the observation sequence for this loop"};
    }

    LoopRecord* loop_record = mutable_loop(state, loop);
    if (loop_record == nullptr) {
        return Status{StatusCode::UnknownObject, "the loop is not registered"};
    }
    if (focus.valid()) {
        if (DeviceRecord* device = loop_record->find_device(focus)) {
            if (usable(reading.device_present)) {
                if (!reading.device_present.value) {
                    device->lifecycle = LifecycleState::Absent;
                } else if (device->lifecycle == LifecycleState::Absent ||
                           device->lifecycle == LifecycleState::Unknown) {
                    device->lifecycle = LifecycleState::Present;
                }
            }
            if (usable(reading.device_generation) && reading.device_generation.value.valid()) {
                device->generation = reading.device_generation.value;
                device->state_generation = reading.device_generation.value;
            }
            if (device->kind == DeviceKind::Valve && usable(reading.valve_position)) {
                device->valve_position = reading.valve_position.value;
            }
            if (device->kind == DeviceKind::Pump && usable(reading.pump_state)) {
                device->pump_state = reading.pump_state.value;
            }
            if (usable(reading.duty_minutes)) {
                device->duty_minutes = reading.duty_minutes.value;
            }
        }
    }

    if (stored == nullptr) {
        LoopEvidenceRecord entry;
        entry.loop = loop;
        entry.reading = reading;
        state.evidence.push_back(std::move(entry));
        stored = &state.evidence.back().reading;
    } else if (adapter_replaced || !stored->sequence.valid()) {
        *stored = reading;
    } else {
        const ObservationSequence previous = stored->sequence;
        *stored = reading;
        if (!(previous < stored->sequence)) {
            stored->sequence = previous;
        }
    }

    bool any_pump_running = false;
    bool any_valve_open = false;
    for (const auto& device : loop_record->devices) {
        if (device.kind == DeviceKind::Pump && device.pump_state == PumpState::Running) {
            any_pump_running = true;
        }
        if (device.kind == DeviceKind::Valve && device.valve_position == ValvePosition::Open) {
            any_valve_open = true;
        }
    }

    const EvidenceUsability leak_usability =
        evaluate_usability(stored->leak, now, policy.limits.evidence_freshness);
    if (leak_usability != EvidenceUsability::Usable) {
        leak_indeterminate = true;
    } else if (stored->leak.value == LeakState::Confirmed) {
        leak_confirmed = true;
        contradictions.push_back(
            Contradiction{StatusCode::LeakConfirmed, "the loop reports a confirmed leak"});
    }

    const EvidenceUsability flow_usability =
        evaluate_usability(stored->flow, now, policy.limits.evidence_freshness);
    if (flow_usability == EvidenceUsability::Usable) {
        if (any_pump_running && stored->flow.value < policy.limits.minimum_effective_flow) {
            contradictions.push_back(Contradiction{
                StatusCode::EvidenceConflicting,
                "a pump reports running while measured flow is below the minimum effective flow"});
        }
        if (any_valve_open && any_pump_running && stored->flow.value.value() <= 0) {
            contradictions.push_back(Contradiction{
                StatusCode::EvidenceConflicting, "a valve reports open and a pump runs but no flow is measurable"});
        }
    }

    const EvidenceUsability pressure_usability =
        evaluate_usability(stored->supply_pressure, now, policy.limits.evidence_freshness);
    if (pressure_usability == EvidenceUsability::Usable &&
        policy.limits.supply_pressure.maximum < stored->supply_pressure.value) {
        contradictions.push_back(
            Contradiction{StatusCode::PressureOutOfRange, "measured supply pressure exceeds the envelope"});
    }
    if (loop_record->isolation == IsolationState::Isolated && flow_usability == EvidenceUsability::Usable &&
        stored->flow.value.value() > 0) {
        contradictions.push_back(Contradiction{
            StatusCode::EvidenceConflicting, "the loop is latched isolated but flow is measurable"});
    }

    if (loop_record->service_mode == ServiceMode::Service) {
        loop_record->operating = LoopOperatingState::Service;
    } else if (loop_record->isolation == IsolationState::Isolated) {
        loop_record->operating = LoopOperatingState::Isolated;
    } else if (leak_confirmed) {
        loop_record->operating = LoopOperatingState::Degraded;
    } else if (any_pump_running) {
        loop_record->operating =
            (flow_usability == EvidenceUsability::Usable &&
             policy.limits.minimum_effective_flow <= stored->flow.value)
                ? LoopOperatingState::Running
                : LoopOperatingState::Degraded;
    } else {
        loop_record->operating = LoopOperatingState::Stopped;
    }
    return ok_result();
}

Runtime::~Runtime() { (void)close(); }

Result<std::unique_ptr<Runtime>> Runtime::open(const RuntimeConfig& config) {
    const auto policy_status = validate(config.policy);
    if (!policy_status.ok()) {
        return policy_status.status();
    }
    if (config.adapter == nullptr) {
        return Status{StatusCode::MissingRequiredField, "a loop adapter is required"};
    }
    // The storage bounds must be able to hold everything the policy permits,
    // otherwise a legal state could become unencodable.
    {
        const auto& limits = config.policy.limits;
        const auto& bounds = config.store_bounds;
        const bool consistent = bounds.max_loops >= limits.max_loops &&
                                bounds.max_devices_per_loop >= limits.max_devices_per_loop &&
                                bounds.max_attempts >= limits.max_retained_attempts &&
                                bounds.max_idempotency_records >= limits.max_idempotency_records &&
                                bounds.max_obligations >= limits.max_obligations &&
                                bounds.max_service_windows >= limits.max_service_windows &&
                                bounds.max_revoked_tokens >= limits.max_revoked_tokens &&
                                bounds.max_journal_entries >= limits.max_journal_entries;
        if (!consistent) {
            return Status{StatusCode::InvalidArgument,
                          "store bounds must accommodate every policy retention bound"};
        }
    }
    if (config.store_root.empty()) {
        return Status{StatusCode::MissingRequiredField, "a store root is required"};
    }
    if (!config.topology_generation.valid()) {
        return Status{StatusCode::MissingRequiredField, "a non-zero topology generation is required"};
    }
    if (config.initial_loops.size() > config.policy.limits.max_loops) {
        return Status{StatusCode::TooManyObjects, "initial loop count exceeds the configured maximum"};
    }
    std::vector<LoopId> loop_ids;
    for (const auto& loop : config.initial_loops) {
        if (!loop.id.valid()) {
            return Status{StatusCode::DuplicateIdentity, "an initial loop has no identity"};
        }
        const auto name = validate_name(loop.name, "loop name");
        if (!name.ok()) {
            return name.status();
        }
        if (loop.devices.size() > config.policy.limits.max_devices_per_loop) {
            return Status{StatusCode::TooManyObjects, "loop device count exceeds the configured maximum"};
        }
        std::vector<DeviceId> device_ids;
        for (const auto& device : loop.devices) {
            if (!device.id.valid()) {
                return Status{StatusCode::DuplicateIdentity, "a loop device has no identity"};
            }
            if (std::find(device_ids.begin(), device_ids.end(), device.id) != device_ids.end()) {
                return Status{StatusCode::DuplicateIdentity, "duplicate device identity inside a loop"};
            }
            device_ids.push_back(device.id);
        }
        if (std::find(loop_ids.begin(), loop_ids.end(), loop.id) != loop_ids.end()) {
            return Status{StatusCode::DuplicateIdentity, "duplicate loop identity"};
        }
        loop_ids.push_back(loop.id);
    }
    if (config.initial_obligations.size() > config.policy.limits.max_obligations) {
        return Status{StatusCode::TooManyObjects, "initial obligation count exceeds the configured maximum"};
    }

    auto store = DurableStore::open(config.store_root, config.store_bounds, config.create_store_if_missing,
                                    config.recovery, owner_note());
    if (!store.ok()) {
        return store.status();
    }

    std::unique_ptr<Runtime> runtime(new Runtime());
    runtime->store_ = std::move(store.value());
    runtime->policy_ = config.policy;
    runtime->adapter_ = config.adapter;
    runtime->clock_ = config.clock != nullptr ? config.clock : std::make_shared<SteadyClock>();
    runtime->state_ = runtime->store_->state();

    const bool fresh = runtime->store_->diagnostics().fresh;
    if (fresh) {
        runtime->state_.loops = config.initial_loops;
        runtime->state_.obligations = config.initial_obligations;
        runtime->state_.topology_generation = config.topology_generation;
        runtime->state_.config_generation = config.policy.generation;
        runtime->state_.epoch = ControlPlaneEpoch::from_value(1);
        runtime->state_.revision = StateRevision::from_value(0);
        runtime->state_.commit_sequence = JournalCommitSequence::from_value(0);
        runtime->state_.last_incarnation = ControllerIncarnation::from_value(0);
        for (auto& loop : runtime->state_.loops) {
            loop.topology_generation = config.topology_generation;
            if (loop.isolation == IsolationState::Unknown) {
                loop.isolation = IsolationState::Open;
            }
        }
    } else if (config.accept_topology_change) {
        if (!(runtime->state_.topology_generation < config.topology_generation)) {
            return Status{StatusCode::TopologyGenerationMismatch,
                          "an accepted topology change requires a strictly newer topology generation"};
        }
        runtime->state_.loops = config.initial_loops;
        runtime->state_.topology_generation = config.topology_generation;
        runtime->state_.epoch = runtime->state_.epoch.next();
        for (auto& loop : runtime->state_.loops) {
            loop.topology_generation = config.topology_generation;
            if (loop.isolation == IsolationState::Unknown) {
                loop.isolation = IsolationState::Open;
            }
        }
        runtime->state_.evidence.clear();
    } else if (config.topology_generation != runtime->state_.topology_generation) {
        return Status{StatusCode::TopologyGenerationMismatch,
                      "the configured topology generation does not match the durable store"};
    }

    // Counters must strictly exceed every identity already present, including
    // those seeded from configuration, so a restart can never reuse one.
    for (const auto& obligation : runtime->state_.obligations) {
        if (!(runtime->state_.next_obligation_id > obligation.id)) {
            runtime->state_.next_obligation_id = obligation.id.next();
        }
    }
    for (const auto& window : runtime->state_.service_windows) {
        if (!(runtime->state_.next_service_window_id > window.id)) {
            runtime->state_.next_service_window_id = window.id.next();
        }
    }
    for (const auto& attempt : runtime->state_.attempts) {
        if (!(runtime->state_.next_attempt_id > attempt.id)) {
            runtime->state_.next_attempt_id = attempt.id.next();
        }
        if (!(runtime->state_.next_command_id > attempt.command)) {
            runtime->state_.next_command_id = attempt.command.next();
        }
        if (!(runtime->state_.next_plan_id > attempt.plan)) {
            runtime->state_.next_plan_id = attempt.plan.next();
        }
    }
    for (const auto& record : runtime->state_.idempotency) {
        if (!(runtime->state_.next_plan_id > record.plan)) {
            runtime->state_.next_plan_id = record.plan.next();
        }
    }
    for (const auto& token : runtime->state_.revoked_tokens) {
        if (!(runtime->state_.next_authority_token_id > token)) {
            runtime->state_.next_authority_token_id = token.next();
        }
    }

    // A new incarnation fences every authority token minted by a previous one.
    runtime->state_.last_incarnation = runtime->state_.last_incarnation.valid()
                                           ? runtime->state_.last_incarnation.next()
                                           : ControllerIncarnation::from_value(1);
    if (!runtime->state_.epoch.valid()) {
        runtime->state_.epoch = ControlPlaneEpoch::from_value(1);
    }
    runtime->state_.config_generation = config.policy.generation;
    // Revocations are meaningful only within one incarnation.
    runtime->state_.revoked_tokens.clear();
    // Observation sequencing is scoped to one controller incarnation: a
    // restarted controller also restarts its adapter session, so the recorded
    // sequence is re-baselined.  The recovered values themselves stay marked as
    // recovered and never authorise a transition.
    for (auto& record : runtime->state_.evidence) {
        record.reading.sequence = ObservationSequence::invalid();
    }
    // Recovered observations are never current physical evidence.
    (void)mark_evidence_recovered(runtime->state_);
    runtime->token_nonce_ = runtime->state_.next_authority_token_id.value();

    {
        DurableState seeded = runtime->state_;
        seeded.revision = seeded.revision.valid() ? seeded.revision.next() : StateRevision::from_value(1);
        JournalEntry entry;
        entry.action = ActionKind::Unknown;
        entry.outcome = StatusCode::Ok;
        entry.attempt_status = AttemptStatus::Unknown;
        entry.detail = fresh ? "store-created" : "runtime-opened";
        const auto persisted = runtime->persist_locked(std::move(seeded), &entry);
        if (!persisted.ok()) {
            return persisted.status();
        }
    }
    const auto verified = runtime->store_->verify();
    if (!verified.ok()) {
        return verified.status();
    }
    return runtime;
}

Result<void> Runtime::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        return ok_result();
    }
    shutting_down_ = true;
    Result<void> verified = ok_result();
    if (store_ != nullptr) {
        verified = store_->verify();
        final_store_diagnostics_ = store_->diagnostics();
        // Dropping the store releases the exclusive cross-process lock so that
        // another writer can take over immediately after close().
        store_.reset();
    }
    closed_ = true;
    issued_.clear();
    if (!verified.ok()) {
        return verified.status();
    }
    return ok_result();
}

const LoopRecord* Runtime::find_loop_locked(LoopId id) const {
    for (const auto& loop : state_.loops) {
        if (loop.id == id) {
            return &loop;
        }
    }
    return nullptr;
}

LoopRecord* Runtime::find_loop_locked(LoopId id) {
    for (auto& loop : state_.loops) {
        if (loop.id == id) {
            return &loop;
        }
    }
    return nullptr;
}

const LoopReading* Runtime::find_reading_locked(LoopId id) const {
    for (const auto& record : state_.evidence) {
        if (record.loop == id) {
            return &record.reading;
        }
    }
    return nullptr;
}

LoopReading* Runtime::find_reading_locked(LoopId id) {
    for (auto& record : state_.evidence) {
        if (record.loop == id) {
            return &record.reading;
        }
    }
    return nullptr;
}

CommandAttempt* Runtime::find_attempt_locked(AttemptId id) {
    for (auto& attempt : state_.attempts) {
        if (attempt.id == id) {
            return &attempt;
        }
    }
    return nullptr;
}

const CommandAttempt* Runtime::find_attempt_locked(AttemptId id) const {
    for (const auto& attempt : state_.attempts) {
        if (attempt.id == id) {
            return &attempt;
        }
    }
    return nullptr;
}

DeviceId Runtime::loop_focus_device_locked(const LoopRecord& loop) const {
    for (const auto& device : loop.devices) {
        if (device.kind == DeviceKind::Cdu) {
            return device.id;
        }
    }
    if (!loop.devices.empty()) {
        return loop.devices.front().id;
    }
    return DeviceId::invalid();
}

bool Runtime::service_window_open_locked(LoopId loop, TimestampNs now) const {
    for (const auto& window : state_.service_windows) {
        if (window.loop == loop && now < window.expires_at) {
            return true;
        }
    }
    return false;
}

EvidenceUsability Runtime::channel_usability(const Evidence<LeakState>& channel, TimestampNs now) const {
    return evaluate_usability(channel.observed_at, channel.present, channel.unsupported, channel.is_recovered(),
                              now, policy_.limits.evidence_freshness);
}


Result<void> Runtime::check_authority_locked(const AuthorityToken& token, LoopId loop,
                                             RequiredCapability capability, TimestampNs now) const {
    if (!token.is_set()) {
        return Status{StatusCode::AuthorityMissing, "no authority token was supplied"};
    }
    if (token.epoch != state_.epoch) {
        return Status{StatusCode::EpochMismatch, "the authority token was issued for a different epoch"};
    }
    if (token.incarnation != state_.last_incarnation) {
        return Status{StatusCode::IncarnationMismatch,
                      "the authority token was issued by a different controller incarnation"};
    }
    if (token.config_generation != policy_.generation) {
        return Status{StatusCode::ConfigGenerationMismatch,
                      "the authority token was issued under a different configuration generation"};
    }
    const auto* loop_record = find_loop_locked(loop);
    if (loop_record == nullptr) {
        return Status{StatusCode::UnknownObject, "the scoped loop is not registered"};
    }
    if (token.topology_generation != loop_record->topology_generation) {
        return Status{StatusCode::TopologyGenerationMismatch,
                      "the authority token was issued against a different topology generation"};
    }
    if (token.loop != loop) {
        return Status{StatusCode::AuthorityScopeMismatch, "the authority token does not cover this loop"};
    }
    if (std::find(state_.revoked_tokens.begin(), state_.revoked_tokens.end(), token.id) !=
        state_.revoked_tokens.end()) {
        return Status{StatusCode::AuthorityRevoked, "the authority token has been revoked"};
    }
    bool issued = false;
    for (const auto& record : issued_) {
        if (record.id == token.id) {
            issued = record.nonce == token.nonce;
            break;
        }
    }
    if (!issued) {
        return Status{StatusCode::AuthorityRevoked,
                      "the authority token was not issued by this controller incarnation"};
    }
    if (!(now < token.expires_at)) {
        return Status{StatusCode::AuthorityExpired, "the authority lease has elapsed"};
    }
    if (!token.capabilities.covers(capability_set(capability))) {
        return Status{StatusCode::AuthorityInsufficient,
                      "the authority token does not carry the capability this action requires"};
    }
    return ok_result();
}

Result<void> Runtime::persist_locked(DurableState candidate, const JournalEntry* entry) {
    if (store_ == nullptr) {
        return Status{StatusCode::StoreNotOpen, "the durable store is not open"};
    }
    const JournalCommitSequence next_sequence = state_.commit_sequence.valid()
                                                    ? state_.commit_sequence.next()
                                                    : JournalCommitSequence::from_value(1);
    std::vector<JournalEntry> entries = journal_;
    if (entry != nullptr) {
        JournalEntry record = *entry;
        record.commit = next_sequence;
        record.revision = candidate.revision;
        record.timestamp = clock_->now();
        entries.push_back(record);
    }
    if (entries.size() > policy_.limits.max_journal_entries) {
        entries.erase(entries.begin(),
                      entries.begin() + static_cast<std::ptrdiff_t>(entries.size() -
                                                                    policy_.limits.max_journal_entries));
    }
    const auto committed = store_->commit(std::move(candidate), entries);
    if (!committed.ok()) {
        return committed.status();
    }
    state_ = store_->state();
    journal_ = std::move(entries);
    return ok_result();
}

Result<LoopReading> Runtime::read_device_unlocked(std::unique_lock<std::mutex>& lock,
                                                  const ObservationRequest& request) {
    std::shared_ptr<ILoopAdapter> adapter = adapter_;
    lock.unlock();
    Result<LoopReading> reading = [&adapter, &request]() -> Result<LoopReading> {
        try {
            return adapter->read(request);
        } catch (const std::exception& error) {
            return Status{StatusCode::AdapterInternalError,
                          std::string("the adapter threw while reading: ") + error.what()};
        } catch (...) {
            return Status{StatusCode::AdapterInternalError, "the adapter threw an unknown exception while reading"};
        }
    }();
    lock.lock();
    return reading;
}

Result<Runtime::PlanOutcome> Runtime::plan_locked(const TransitionRequest& request, TimestampNs now) const {
    const ActionDescriptor& descriptor = action_descriptor(request.action);

    // ---- Stage 1: shape ---------------------------------------------------
    if (request.action == ActionKind::Unknown ||
        static_cast<std::size_t>(request.action) >= kActionKindCount) {
        return Status{StatusCode::UnsupportedAction, "the action is not defined"};
    }
    if (request.idempotency_key.empty()) {
        return Status{StatusCode::MissingRequiredField, "an idempotency key is required"};
    }
    if (!request.loop.valid()) {
        return Status{StatusCode::MissingRequiredField, "a loop identity is required"};
    }
    if (descriptor.requires_device && !request.target.valid()) {
        return Status{StatusCode::MissingRequiredField, "this action requires a device target"};
    }
    if (!descriptor.requires_device && request.target.valid()) {
        return Status{StatusCode::InvalidArgument, "this action is loop scoped and does not accept a device target"};
    }
    if (descriptor.requires_obligation && !request.obligation.valid()) {
        return Status{StatusCode::MissingRequiredField, "this action requires a service obligation identity"};
    }
    if (!descriptor.requires_obligation && request.obligation.valid()) {
        return Status{StatusCode::InvalidArgument, "this action does not accept a service obligation identity"};
    }
    if (descriptor.requires_attempt && !request.attempt.valid()) {
        return Status{StatusCode::MissingRequiredField, "this action requires an attempt identity"};
    }
    if (!descriptor.requires_attempt && request.attempt.valid()) {
        return Status{StatusCode::InvalidArgument, "this action does not accept an attempt identity"};
    }
    if (descriptor.requires_flow_target && !request.has_flow_target) {
        return Status{StatusCode::MissingRequiredField, "this action requires a flow target"};
    }
    if (!descriptor.requires_flow_target && request.has_flow_target) {
        return Status{StatusCode::InvalidArgument, "this action does not accept a flow target"};
    }
    if (descriptor.requires_pressure_target && !request.has_pressure_target) {
        return Status{StatusCode::MissingRequiredField, "this action requires a pressure target"};
    }
    if (!descriptor.requires_pressure_target && request.has_pressure_target) {
        return Status{StatusCode::InvalidArgument, "this action does not accept a pressure target"};
    }
    const auto reason = validate_reason(request.reason, "reason");
    if (!reason.ok()) {
        return reason.status();
    }
    if (request.has_flow_target) {
        const auto status = validate_domain(request.flow_target);
        if (!status.ok()) {
            return status.status();
        }
    }
    if (request.has_pressure_target) {
        const auto status = validate_domain(request.pressure_target);
        if (!status.ok()) {
            return status.status();
        }
    }
    if (!request.authority.is_set()) {
        return Status{StatusCode::AuthorityMissing, "no authority token was supplied"};
    }

    // ---- Stage 2: identity ------------------------------------------------
    const LoopRecord* loop = find_loop_locked(request.loop);
    if (loop == nullptr) {
        return Status{StatusCode::UnknownObject, "the loop is not registered"};
    }
    const DeviceRecord* device = nullptr;
    if (descriptor.requires_device) {
        device = loop->find_device(request.target);
        if (device == nullptr) {
            return Status{StatusCode::UnknownObject, "the device is not registered in this loop"};
        }
        switch (request.action) {
            case ActionKind::StartPump:
            case ActionKind::StopPump:
                if (device->kind != DeviceKind::Pump) {
                    return Status{StatusCode::WrongObjectKind, "this action targets a pump"};
                }
                break;
            case ActionKind::OpenValve:
            case ActionKind::CloseValve:
                if (device->kind != DeviceKind::Valve) {
                    return Status{StatusCode::WrongObjectKind, "this action targets a valve"};
                }
                break;
            case ActionKind::ResetFault:
                if (!is_actuatable(device->kind)) {
                    return Status{StatusCode::WrongObjectKind, "this action targets an actuatable device"};
                }
                break;
            default:
                break;
        }
    }
    const ServiceObligation* obligation = nullptr;
    if (descriptor.requires_obligation) {
        for (const auto& candidate : state_.obligations) {
            if (candidate.id == request.obligation) {
                obligation = &candidate;
                break;
            }
        }
        if (obligation == nullptr) {
            return Status{StatusCode::UnknownObject, "the service obligation is not registered"};
        }
    }
    if (descriptor.requires_attempt) {
        if (find_attempt_locked(request.attempt) == nullptr) {
            return Status{StatusCode::AttemptNotFound, "the attempt is not registered"};
        }
    }

    // ---- Stage 3: device generation ---------------------------------------
    if (descriptor.requires_device) {
        if (!request.expected_device_generation.valid()) {
            return Status{StatusCode::MissingRequiredField, "the expected device generation is required"};
        }
        if (request.expected_device_generation < device->generation) {
            return Status{StatusCode::StaleGeneration,
                          "the request was planned against an older device generation"};
        }
        if (device->generation < request.expected_device_generation) {
            return Status{StatusCode::FutureGeneration,
                          "the request names a device generation the runtime has not observed"};
        }
    }

    // ---- Stage 4: authority -----------------------------------------------
    const auto authority = check_authority_locked(request.authority, request.loop, descriptor.capability, now);
    if (!authority.ok()) {
        return authority.status();
    }

    // ---- Stage 5: state revision ------------------------------------------
    if (!request.expected_revision.valid()) {
        return Status{StatusCode::MissingRequiredField, "the expected state revision is required"};
    }
    if (request.expected_revision < state_.revision) {
        return Status{StatusCode::StaleRevision, "the request was planned against an older state revision"};
    }
    if (state_.revision < request.expected_revision) {
        return Status{StatusCode::FutureRevision, "the request names a state revision that does not exist"};
    }

    // ---- Stage 6: service mode and protected obligations -------------------
    EffectClass effect = descriptor.effect;
    bool direction_resolved = descriptor.effect != EffectClass::DirectionRelative;
    if (descriptor.effect == EffectClass::DirectionRelative) {
        const bool is_flow = request.action == ActionKind::SetFlowTarget;
        const bool has_current = is_flow ? loop->flow_target_generation.valid() : loop->pressure_target_generation.valid();
        const std::int64_t current_value = is_flow ? loop->flow_target.value() : loop->pressure_target.value();
        const std::int64_t requested_value = is_flow ? request.flow_target.value() : request.pressure_target.value();
        const auto resolved = resolve_effect(descriptor, has_current, current_value, requested_value);
        if (!resolved.ok()) {
            return resolved.status();
        }
        effect = resolved.value();
        direction_resolved = true;
    }
    (void)direction_resolved;

    if (is_production_action(effect) && loop->service_mode == ServiceMode::Service) {
        return Status{StatusCode::ServiceModeConflict,
                      "the loop is in service mode and refuses exposure-increasing actuation"};
    }
    for (const auto& candidate : state_.obligations) {
        bool blocking = candidate.active;
        if (!blocking && candidate.due_at_duty_minutes > 0 && device != nullptr &&
            device->duty_minutes >= candidate.due_at_duty_minutes) {
            blocking = true;
        }
        if (!blocking) {
            continue;
        }
        if (!request.target.valid() || candidate.target != request.target) {
            continue;
        }
        if (obligation_forbids_all_actuation(candidate.kind)) {
            return Status{StatusCode::ServiceObligationActive,
                          "a lockout tag forbids every actuation on this device"};
        }
        if (is_production_action(effect) && !service_window_open_locked(request.loop, now)) {
            return Status{StatusCode::ServiceObligationActive,
                          "a protected service obligation forbids exposure-increasing actuation"};
        }
    }
    if (request.action == ActionKind::EnterService && loop->isolation != IsolationState::Isolated) {
        return Status{StatusCode::LoopNotIsolated, "the loop must be isolated before it can enter service mode"};
    }
    if (request.action == ActionKind::ExitService) {
        for (const auto& attempt : state_.attempts) {
            if (attempt.loop == request.loop && attempt.is_open()) {
                return Status{StatusCode::UnresolvedAttemptBlocks,
                              "an open attempt must be resolved before service mode can be left"};
            }
        }
    }

    // ---- Stage 7: device presence -----------------------------------------
    if (descriptor.requires_device) {
        if (request.action != ActionKind::ResetFault) {
            if (device->lifecycle == LifecycleState::Absent) {
                return Status{StatusCode::DeviceAbsent, "the device is reported absent"};
            }
            if (device->lifecycle == LifecycleState::Retired) {
                return Status{StatusCode::DeviceAbsent, "the device is retired"};
            }
            if (device->lifecycle == LifecycleState::Faulted) {
                return Status{StatusCode::DeviceFaulted, "the device is latched faulted"};
            }
        } else if (device->lifecycle != LifecycleState::Faulted) {
            return Status{StatusCode::InvalidStateTransition, "the device is not latched faulted"};
        }
        const LoopReading* reading = find_reading_locked(request.loop);
        if (reading != nullptr && usable(reading->device_generation) &&
            reading->device_generation.value == device->generation && usable(reading->device_present) &&
            !reading->device_present.value) {
            return Status{StatusCode::DeviceAbsent, "the adapter reports the device absent"};
        }
    }

    // ---- Stage 8: open attempts -------------------------------------------
    if (descriptor.issues_command || descriptor.effect != EffectClass::ReduceExposure) {
        const RequestFingerprint fingerprint = fingerprint_request(request);
        for (const auto& attempt : state_.attempts) {
            if (attempt.loop != request.loop || !attempt.is_open()) {
                continue;
            }
            if (attempt.id == request.attempt) {
                continue;
            }
            if (effect == EffectClass::ReduceExposure) {
                continue;
            }
            if (attempt.action == request.action && attempt.fingerprint == fingerprint) {
                continue;
            }
            return Status{StatusCode::UnresolvedAttemptBlocks,
                          "an unresolved effect-bearing attempt forbids incompatible actuation"};
        }
    }
    {
        std::size_t open_attempts = 0;
        for (const auto& attempt : state_.attempts) {
            if (attempt.is_open()) {
                ++open_attempts;
            }
        }
        if (open_attempts >= policy_.limits.max_open_attempts && descriptor.issues_command) {
            return Status{StatusCode::AttemptLimitExceeded, "the number of open attempts reached its bound"};
        }
    }

    // ---- Stage 9: leak interlocks -----------------------------------------
    if (is_production_action(effect)) {
        const LoopReading* reading = find_reading_locked(request.loop);
        const Evidence<LeakState> absent = Evidence<LeakState>::make_absent("none");
        const Evidence<LeakState>& leak = reading != nullptr ? reading->leak : absent;
        const EvidenceUsability usability = channel_usability(leak, now);
        if (usability != EvidenceUsability::Usable) {
            if (policy_.interlock_enabled(InterlockKind::IndeterminateLeakBlocksIncrease)) {
                return Status{StatusCode::LeakStateIndeterminate,
                              std::string("leak evidence is ") + std::string(to_string(usability)) +
                                  " so exposure-increasing actuation is refused"};
            }
        } else {
            switch (leak.value) {
                case LeakState::Confirmed:
                    if (policy_.interlock_enabled(InterlockKind::LeakBlocksIncrease)) {
                        return Status{StatusCode::LeakConfirmed,
                                      "a leak is confirmed on the loop so exposure-increasing actuation is refused"};
                    }
                    break;
                case LeakState::Suspected:
                    if (policy_.interlock_enabled(InterlockKind::LeakBlocksIncrease)) {
                        return Status{StatusCode::InterlockBlocked,
                                      "a leak is suspected on the loop so exposure-increasing actuation is refused"};
                    }
                    break;
                case LeakState::None:
                case LeakState::Unknown:
                    break;
            }
        }
    }

    // ---- Stage 10: coolant quality ----------------------------------------
    if (is_production_action(effect) && policy_.interlock_enabled(InterlockKind::CoolantQualityBlocksStart)) {
        const LoopReading* reading = find_reading_locked(request.loop);
        const Evidence<CoolantQuality> absent = Evidence<CoolantQuality>::make_absent("none");
        const Evidence<CoolantQuality>& quality = reading != nullptr ? reading->coolant_quality : absent;
        const EvidenceUsability usability = evaluate_usability(quality, now, policy_.limits.evidence_freshness);
        if (usability != EvidenceUsability::Usable) {
            return Status{usability_status(usability), "coolant quality evidence is not usable"};
        }
        if (quality.value != CoolantQuality::Nominal) {
            return Status{StatusCode::CoolantQualityNotNominal,
                          std::string("coolant quality is ") + std::string(to_string(quality.value))};
        }
    }

    // ---- Stage 11: operating envelope -------------------------------------
    {
        const LoopReading* reading = find_reading_locked(request.loop);
        if (is_production_action(effect) && policy_.interlock_enabled(InterlockKind::FrozenEvidenceBlocksActuation)) {
            const Evidence<FlowRate> absent_flow = Evidence<FlowRate>::make_absent("none");
            const Evidence<FlowRate>& flow = reading != nullptr ? reading->flow : absent_flow;
            const EvidenceUsability usability = evaluate_usability(flow, now, policy_.limits.evidence_freshness);
            if (usability != EvidenceUsability::Usable) {
                return Status{usability_status(usability), "flow evidence is not usable"};
            }
        }
        if (reading != nullptr && is_production_action(effect)) {
            const EvidenceUsability pressure_usability =
                evaluate_usability(reading->supply_pressure, now, policy_.limits.evidence_freshness);
            if (pressure_usability == EvidenceUsability::Usable &&
                policy_.interlock_enabled(InterlockKind::OverPressureBlocksIncrease) &&
                policy_.limits.supply_pressure.maximum < reading->supply_pressure.value) {
                return Status{StatusCode::PressureOutOfRange,
                              "measured supply pressure is above the permitted envelope"};
            }
            const EvidenceUsability temperature_usability =
                evaluate_usability(reading->supply_temperature, now, policy_.limits.evidence_freshness);
            if (temperature_usability == EvidenceUsability::Usable &&
                !policy_.limits.coolant_temperature.contains(reading->supply_temperature.value)) {
                return Status{StatusCode::TemperatureOutOfRange,
                              "measured coolant temperature is outside the permitted envelope"};
            }
            const EvidenceUsability conductivity_usability =
                evaluate_usability(reading->conductivity, now, policy_.limits.evidence_freshness);
            if (conductivity_usability == EvidenceUsability::Usable &&
                !policy_.limits.conductivity.contains(reading->conductivity.value)) {
                return Status{StatusCode::ConductivityOutOfRange,
                              "measured coolant conductivity is outside the permitted envelope"};
            }
            const EvidenceUsability acidity_usability =
                evaluate_usability(reading->acidity, now, policy_.limits.evidence_freshness);
            if (acidity_usability == EvidenceUsability::Usable &&
                !policy_.limits.acidity.contains(reading->acidity.value)) {
                return Status{StatusCode::PhOutOfRange, "measured coolant pH is outside the permitted envelope"};
            }
        }
        if (request.action == ActionKind::SetFlowTarget) {
            if (!policy_.limits.flow.contains(request.flow_target)) {
                return Status{StatusCode::FlowOutOfRange, "the flow target is outside the permitted envelope"};
            }
            if (loop->flow_target_generation.valid()) {
                const auto difference = checked_abs_diff(request.flow_target, loop->flow_target);
                if (!difference.ok()) {
                    return difference.status();
                }
                if (policy_.limits.maximum_flow_step < difference.value()) {
                    return Status{StatusCode::FlowOutOfRange,
                                  "the flow target moves further than one permitted step"};
                }
            }
        }
        if (request.action == ActionKind::SetPressureTarget) {
            if (!policy_.limits.supply_pressure.contains(request.pressure_target)) {
                return Status{StatusCode::PressureOutOfRange,
                              "the pressure target is outside the permitted envelope"};
            }
            if (loop->pressure_target_generation.valid()) {
                const auto difference = checked_abs_diff(request.pressure_target, loop->pressure_target);
                if (!difference.ok()) {
                    return difference.status();
                }
                if (policy_.limits.maximum_pressure_step < difference.value()) {
                    return Status{StatusCode::PressureOutOfRange,
                                  "the pressure target moves further than one permitted step"};
                }
            }
        }
    }

    // ---- Stage 12: transition legality ------------------------------------
    if (descriptor.requires_device && device != nullptr) {
        // A position or state only counts as current when it was observed on the
        // device generation the plan is bound to.
        const bool state_is_current = device->state_generation.valid() &&
                                      device->state_generation == device->generation;
        switch (request.action) {
            case ActionKind::StartPump:
                if (loop->isolation == IsolationState::Isolated) {
                    return Status{StatusCode::LoopIsolated, "the loop is isolated"};
                }
                if (state_is_current && device->pump_state == PumpState::Running) {
                    return Status{StatusCode::AlreadyInTargetState, "the pump already reports running"};
                }
                break;
            case ActionKind::StopPump:
                if (state_is_current && device->pump_state == PumpState::Stopped) {
                    return Status{StatusCode::AlreadyInTargetState, "the pump already reports stopped"};
                }
                break;
            case ActionKind::OpenValve:
                if (loop->isolation == IsolationState::Isolated) {
                    return Status{StatusCode::LoopIsolated, "the loop is isolated"};
                }
                if (state_is_current && device->valve_position == ValvePosition::Open) {
                    return Status{StatusCode::AlreadyInTargetState, "the valve already reports open"};
                }
                break;
            case ActionKind::CloseValve:
                if (state_is_current && device->valve_position == ValvePosition::Closed) {
                    return Status{StatusCode::AlreadyInTargetState, "the valve already reports closed"};
                }
                break;
            default:
                break;
        }
    }
    switch (request.action) {
        case ActionKind::IsolateLoop:
            if (loop->isolation == IsolationState::Isolated) {
                return Status{StatusCode::AlreadyInTargetState, "the loop is already isolated"};
            }
            break;
        case ActionKind::ClearIsolation:
            if (loop->isolation != IsolationState::Isolated) {
                return Status{StatusCode::LoopNotIsolated, "the loop is not isolated"};
            }
            break;
        case ActionKind::EnterService:
            if (loop->service_mode == ServiceMode::Service) {
                return Status{StatusCode::AlreadyInTargetState, "the loop is already in service mode"};
            }
            break;
        case ActionKind::ExitService:
            if (loop->service_mode == ServiceMode::Production) {
                return Status{StatusCode::AlreadyInTargetState, "the loop is not in service mode"};
            }
            break;
        case ActionKind::CompleteObligation:
            if (!obligation->active) {
                return Status{StatusCode::AlreadyInTargetState, "the obligation is not active"};
            }
            if (request.reason.empty()) {
                return Status{StatusCode::MissingRequiredField,
                              "completing an obligation requires an attested reason"};
            }
            break;
        case ActionKind::AbandonAttempt: {
            const CommandAttempt* attempt = find_attempt_locked(request.attempt);
            if (!attempt->is_open()) {
                return Status{StatusCode::AttemptNotOpen, "the attempt is already resolved"};
            }
            if (request.reason.empty()) {
                return Status{StatusCode::MissingRequiredField,
                              "abandoning an attempt requires an attested reason"};
            }
            break;
        }
        default:
            break;
    }

    PlanOutcome outcome;
    Plan& plan = outcome.plan;
    // Provisional identity; the durable identity is consumed once, in apply().
    plan.id = PlanId::from_value(state_.next_plan_id.value());
    plan.request = request;
    plan.fingerprint = fingerprint_request(request);
    plan.canonical = canonical_request_bytes(request);
    plan.revision_at_plan = state_.revision;
    plan.epoch_at_plan = state_.epoch;
    plan.incarnation_at_plan = state_.last_incarnation;
    plan.config_generation = policy_.generation;
    plan.topology_generation = loop->topology_generation;
    plan.device_generation = device != nullptr ? device->generation : DeviceGeneration::invalid();
    plan.planned_at = now;
    plan.effect = effect;
    plan.capability = descriptor.capability;
    plan.issues_command = descriptor.issues_command;
    return outcome;
}

Result<Plan> Runtime::plan(const TransitionRequest& request) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_) {
        return Status{StatusCode::ShuttingDown, "the runtime is shutting down"};
    }
    const auto outcome = plan_locked(request, clock_->now());
    if (!outcome.ok()) {
        return outcome.status();
    }
    return outcome.value().plan;
}


Result<ExecutionRecord> Runtime::apply(const Plan& plan) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (shutting_down_) {
        return Status{StatusCode::ShuttingDown, "the runtime is shutting down"};
    }
    Plan effective = plan;
    if (effective.fingerprint != fingerprint_request(plan.request)) {
        return Status{StatusCode::MalformedRequest,
                      "the plan fingerprint does not match the request it carries"};
    }
    const IdempotencyKey& key = effective.request.idempotency_key;
    for (const auto& record : state_.idempotency) {
        if (!(record.key == key)) {
            continue;
        }
        if (!(record.fingerprint == effective.fingerprint)) {
            return Status{StatusCode::IdempotencyConflict,
                          "the idempotency key was already used for a different request"};
        }
        ExecutionRecord replay;
        replay.plan = effective;
        replay.idempotent_replay = true;
        replay.outcome = record.outcome;
        replay.deferred = !is_ok(record.outcome);
        replay.detail = "replayed the stored outcome without actuating again";
        if (const CommandAttempt* stored = find_attempt_locked(record.attempt)) {
            replay.attempt = *stored;
        }
        return replay;
    }
    if (state_.idempotency.size() >= policy_.limits.max_idempotency_records) {
        return Status{StatusCode::ResourceExhausted,
                      "the idempotency record bound is reached; no new key can be admitted safely"};
    }

    const TimestampNs now = clock_->now();
    const auto replanned = plan_locked(effective.request, now);
    if (!replanned.ok()) {
        return replanned.status();
    }
    const Plan& validated = replanned.value().plan;
    if (validated.effect != effective.effect || validated.capability != effective.capability) {
        return Status{StatusCode::StaleRevision,
                      "the plan resolves to a different effect against current state"};
    }

    const ActionDescriptor& descriptor = action_descriptor(effective.request.action);
    // The durable plan identity is consumed here, once, so that a plan identity
    // is never handed out twice even when a plan is only validated.
    effective.id = state_.next_plan_id;
    DurableState next = state_;
    next.next_plan_id = next.next_plan_id.next();
    CommandAttempt attempt;
    attempt.id = next.next_attempt_id;
    next.next_attempt_id = next.next_attempt_id.next();
    attempt.plan = effective.id;
    attempt.idempotency_key = key;
    attempt.fingerprint = effective.fingerprint;
    attempt.action = plan.request.action;
    attempt.effect = plan.effect;
    attempt.loop = plan.request.loop;
    attempt.target = plan.request.target;
    attempt.generation = plan.device_generation;
    attempt.commanded_flow = plan.request.flow_target;
    attempt.commanded_pressure = plan.request.pressure_target;
    attempt.epoch = state_.epoch;
    attempt.incarnation = state_.last_incarnation;
    attempt.revision_at_issue = next.revision.valid() ? next.revision.next() : StateRevision::from_value(1);
    attempt.issued_at = now;
    attempt.updated_at = now;

    const JournalCommitSequence predicted_commit =
        state_.commit_sequence.valid() ? state_.commit_sequence.next() : JournalCommitSequence::from_value(1);

    if (!descriptor.issues_command) {
        attempt.status = AttemptStatus::EffectVerified;
        attempt.outcome = StatusCode::Ok;
        attempt.detail = "control-plane transition applied without a physical command";
        attempt.effect_sequence = ObservationSequence::invalid();
        attempt.effect_generation = attempt.generation;
        attempt.issued_at_commit = predicted_commit;
        apply_control_plane_action(next, plan.request);
        next.attempts.push_back(attempt);
        next.idempotency.push_back(
            IdempotencyRecord{key, effective.fingerprint, effective.id, attempt.id, StatusCode::Ok, predicted_commit});
        trim_attempts(next, policy_.limits.max_retained_attempts);
        next.revision = attempt.revision_at_issue;

        JournalEntry entry;
        entry.action = attempt.action;
        entry.outcome = StatusCode::Ok;
        entry.attempt_status = AttemptStatus::EffectVerified;
        entry.loop = attempt.loop;
        entry.target = attempt.target;
        entry.attempt = attempt.id;
        entry.fingerprint = attempt.fingerprint;
        entry.detail = attempt.detail;
        const auto persisted = persist_locked(std::move(next), &entry);
        if (!persisted.ok()) {
            return persisted.status();
        }
        ExecutionRecord record;
        record.plan = effective;
        record.attempt = *find_attempt_locked(attempt.id);
        record.outcome = StatusCode::Ok;
        return record;
    }

    attempt.command = next.next_command_id;
    next.next_command_id = next.next_command_id.next();
    attempt.status = AttemptStatus::Issued;
    attempt.outcome = StatusCode::EffectPending;
    attempt.detail = "command issued to the adapter";
    attempt.issued_at_commit = predicted_commit;
    next.attempts.push_back(attempt);
    next.revision = attempt.revision_at_issue;
    trim_attempts(next, policy_.limits.max_retained_attempts);

    {
        JournalEntry entry;
        entry.action = attempt.action;
        entry.outcome = StatusCode::EffectPending;
        entry.attempt_status = AttemptStatus::Issued;
        entry.loop = attempt.loop;
        entry.target = attempt.target;
        entry.attempt = attempt.id;
        entry.fingerprint = attempt.fingerprint;
        entry.detail = "issued";
        const auto persisted = persist_locked(std::move(next), &entry);
        if (!persisted.ok()) {
            return persisted.status();
        }
    }

    ActuationCommand command;
    command.command = attempt.command;
    command.attempt = attempt.id;
    command.plan = effective.id;
    command.action = attempt.action;
    command.loop = attempt.loop;
    command.target = attempt.target;
    command.generation = attempt.generation;
    command.flow_target = attempt.commanded_flow;
    command.pressure_target = attempt.commanded_pressure;
    command.epoch = attempt.epoch;
    command.revision = attempt.revision_at_issue;
    command.issued_at = now;

    std::shared_ptr<ILoopAdapter> adapter = adapter_;
    lock.unlock();
    Result<CommandAck> ack = [&adapter, &command]() -> Result<CommandAck> {
        try {
            return adapter->actuate(command);
        } catch (const std::exception& error) {
            return Status{StatusCode::AdapterInternalError,
                          std::string("the adapter threw while actuating: ") + error.what()};
        } catch (...) {
            return Status{StatusCode::AdapterInternalError, "the adapter threw an unknown exception"};
        }
    }();
    lock.lock();

    const auto finish = [&](StatusCode outcome, AttemptStatus status, std::string detail, StatusCode adapter_code,
                            ObservationSequence effect_sequence, DeviceGeneration effect_generation,
                            bool deferred) -> Result<ExecutionRecord> {
        DurableState updated = state_;
        CommandAttempt* stored = nullptr;
        for (auto& candidate : updated.attempts) {
            if (candidate.id == attempt.id) {
                stored = &candidate;
                break;
            }
        }
        if (stored == nullptr) {
            return Status{StatusCode::InternalError, "the attempt record disappeared during execution"};
        }
        if (!stored->is_open()) {
            ExecutionRecord existing;
            existing.plan = effective;
            existing.attempt = *stored;
            existing.outcome = stored->outcome;
            existing.deferred = !is_ok(stored->outcome);
            existing.detail = "the attempt was resolved concurrently; the stored outcome is reported";
            return existing;
        }
        stored->status = status;
        stored->outcome = outcome;
        stored->adapter_code = adapter_code;
        stored->detail = std::move(detail);
        stored->updated_at = clock_->now();
        stored->effect_sequence = effect_sequence;
        stored->effect_generation = effect_generation;
        updated.revision = updated.revision.next();
        if (!has_idempotency_locked(updated, key)) {
            updated.idempotency.push_back(
                IdempotencyRecord{key, effective.fingerprint, effective.id, attempt.id, outcome, predicted_commit});
        }
        JournalEntry entry;
        entry.action = attempt.action;
        entry.outcome = outcome;
        entry.attempt_status = status;
        entry.loop = attempt.loop;
        entry.target = attempt.target;
        entry.attempt = attempt.id;
        entry.fingerprint = attempt.fingerprint;
        entry.detail = stored->detail;
        const auto persisted = persist_locked(std::move(updated), &entry);
        if (!persisted.ok()) {
            return persisted.status();
        }
        ExecutionRecord record;
        record.plan = effective;
        record.attempt = *find_attempt_locked(attempt.id);
        record.outcome = outcome;
        record.deferred = deferred;
        record.detail = record.attempt.detail;
        return record;
    };

    if (ack.code() == StatusCode::AdapterUnavailable || ack.code() == StatusCode::AdapterInternalError) {
        return finish(ack.code(), AttemptStatus::Unresolved,
                      std::string("adapter call failed: ") + std::string(to_string(ack.code())), ack.code(),
                      ObservationSequence::invalid(), DeviceGeneration::invalid(), true);
    }
    if (!ack.ok()) {
        return finish(ack.code(), AttemptStatus::Unresolved,
                      std::string("adapter call failed: ") + std::string(to_string(ack.code())), ack.code(),
                      ObservationSequence::invalid(), DeviceGeneration::invalid(), true);
    }
    const CommandAck& acknowledgement = ack.value();
    if (acknowledgement.error != AdapterErrorCode::None) {
        const StatusCode code = to_status(acknowledgement.error);
        // A definite refusal cannot have moved the plant, so the attempt is
        // closed as failed.  A generation or internal error does not prove the
        // absence of a physical effect, so the attempt stays unresolved and
        // keeps fencing incompatible actuation.
        const bool definite_refusal = acknowledgement.error == AdapterErrorCode::Rejected ||
                                      acknowledgement.error == AdapterErrorCode::Busy ||
                                      acknowledgement.error == AdapterErrorCode::DeviceMissing ||
                                      acknowledgement.error == AdapterErrorCode::Unsupported;
        return finish(code, definite_refusal ? AttemptStatus::Failed : AttemptStatus::Unresolved,
                      acknowledgement.detail.empty() ? std::string(to_string(code)) : acknowledgement.detail, code,
                      acknowledgement.ack_sequence, acknowledgement.generation, true);
    }
    if (acknowledgement.command != command.command || acknowledgement.attempt != command.attempt) {
        return finish(StatusCode::AdapterInternalError, AttemptStatus::Unresolved,
                      "the adapter acknowledged a different command identity",
                      StatusCode::AdapterInternalError, acknowledgement.ack_sequence,
                      acknowledgement.generation, true);
    }
    if (acknowledgement.generation != attempt.generation) {
        return finish(StatusCode::EvidenceGenerationMismatch, AttemptStatus::Unresolved,
                      "the adapter acknowledged the command against a different device generation",
                      StatusCode::EvidenceGenerationMismatch, acknowledgement.ack_sequence,
                      acknowledgement.generation, true);
    }

    {
        DurableState updated = state_;
        CommandAttempt* stored = nullptr;
        for (auto& candidate : updated.attempts) {
            if (candidate.id == attempt.id) {
                stored = &candidate;
                break;
            }
        }
        if (stored == nullptr) {
            return Status{StatusCode::InternalError, "the attempt record disappeared during execution"};
        }
        stored->status = AttemptStatus::Acknowledged;
        stored->ack_sequence = acknowledgement.ack_sequence;
        stored->ack_generation = acknowledgement.generation;
        stored->updated_at = clock_->now();
        stored->detail = "acknowledged; effect not yet established";
        updated.revision = updated.revision.next();
        JournalEntry entry;
        entry.action = attempt.action;
        entry.outcome = StatusCode::EffectPending;
        entry.attempt_status = AttemptStatus::Acknowledged;
        entry.loop = attempt.loop;
        entry.target = attempt.target;
        entry.attempt = attempt.id;
        entry.fingerprint = attempt.fingerprint;
        entry.detail = "acknowledged";
        const auto persisted = persist_locked(std::move(updated), &entry);
        if (!persisted.ok()) {
            return persisted.status();
        }
    }

    const auto verification = verify_locked(lock, attempt.id, clock_->now());
    const AttemptStatus final_status =
        verification.ok() ? verification.value().status : AttemptStatus::Unresolved;
    const StatusCode final_code = verification.ok() ? verification.value().code : verification.status().code();
    const std::string detail = verification.ok() ? verification.value().detail : verification.status().message();

    DurableState updated = state_;
    CommandAttempt* stored = nullptr;
    for (auto& candidate : updated.attempts) {
        if (candidate.id == attempt.id) {
            stored = &candidate;
            break;
        }
    }
    if (stored == nullptr) {
        return Status{StatusCode::InternalError, "the attempt record disappeared during verification"};
    }
    // The idempotency record must exist for every attempt that reached the
    // adapter, whether the attempt was resolved here or by verification, so that
    // a lost-response retry can never actuate a second time.
    const bool needs_record = !has_idempotency_locked(updated, key);
    if (stored->is_open()) {
        stored->status = final_status;
        stored->outcome = final_code;
        stored->detail = detail;
        stored->updated_at = clock_->now();
    }
    if (stored->is_open() || needs_record) {
        updated.revision = updated.revision.next();
        if (needs_record) {
            updated.idempotency.push_back(IdempotencyRecord{key, effective.fingerprint, effective.id, attempt.id,
                                                            stored->outcome, predicted_commit});
        }
        JournalEntry entry;
        entry.action = attempt.action;
        entry.outcome = stored->outcome;
        entry.attempt_status = stored->status;
        entry.loop = attempt.loop;
        entry.target = attempt.target;
        entry.attempt = attempt.id;
        entry.fingerprint = attempt.fingerprint;
        entry.detail = stored->detail;
        const auto persisted = persist_locked(std::move(updated), &entry);
        if (!persisted.ok()) {
            return persisted.status();
        }
    }

    ExecutionRecord result;
    result.plan = effective;
    result.attempt = *find_attempt_locked(attempt.id);
    result.outcome = result.attempt.outcome;
    result.deferred = !is_ok(result.attempt.outcome);
    result.detail = result.attempt.detail;
    return result;
}

std::optional<Result<ExecutionRecord>> Runtime::try_replay_locked(const TransitionRequest& request,
                                                                 const RequestFingerprint& fingerprint) const {
    for (const auto& record : state_.idempotency) {
        if (!(record.key == request.idempotency_key)) {
            continue;
        }
        if (!(record.fingerprint == fingerprint)) {
            return Result<ExecutionRecord>{Status{StatusCode::IdempotencyConflict,
                                                  "the idempotency key was already used for a different request"}};
        }
        ExecutionRecord replay;
        // The plan identity of a replay is the identity that was durably
        // consumed by the original request.
        replay.plan.id = record.plan;
        replay.plan.request = request;
        replay.plan.fingerprint = fingerprint;
        replay.idempotent_replay = true;
        replay.outcome = record.outcome;
        replay.deferred = !is_ok(record.outcome);
        replay.detail = "replayed the stored outcome without actuating again";
        if (const CommandAttempt* stored = find_attempt_locked(record.attempt)) {
            replay.attempt = *stored;
        }
        return Result<ExecutionRecord>{replay};
    }
    return std::nullopt;
}

Result<ExecutionRecord> Runtime::execute(const TransitionRequest& request) {
    // A lost-response retry is resolved before planning so that it replays even
    // though the state revision and device generation have moved on.
    const RequestFingerprint fingerprint = fingerprint_request(request);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutting_down_) {
            return Status{StatusCode::ShuttingDown, "the runtime is shutting down"};
        }
        const auto replay = try_replay_locked(request, fingerprint);
        if (replay.has_value()) {
            return *replay;
        }
    }
    const auto planned = plan(request);
    if (!planned.ok()) {
        return planned.status();
    }
    return apply(planned.value());
}


Result<VerificationRecord> Runtime::verify_locked(std::unique_lock<std::mutex>& lock, AttemptId id, TimestampNs now) {
    CommandAttempt* attempt = find_attempt_locked(id);
    if (attempt == nullptr) {
        return Status{StatusCode::AttemptNotFound, "the attempt is not registered"};
    }
    VerificationRecord record;
    record.attempt = id;
    record.status = attempt->status;
    if (!attempt->is_open()) {
        record.code = attempt->outcome;
        record.detail = "the attempt is already resolved";
        record.evidence_sequence = attempt->effect_sequence;
        record.evidence_generation = attempt->effect_generation;
        return record;
    }
    if (!action_descriptor(attempt->action).issues_command) {
        return Status{StatusCode::InternalError, "this attempt has no physical effect to verify"};
    }

    DeviceId focus = attempt->target;
    if (!focus.valid()) {
        if (const LoopRecord* loop = find_loop_locked(attempt->loop)) {
            focus = loop_focus_device_locked(*loop);
        }
    }
    ObservationRequest request;
    request.loop = attempt->loop;
    request.target = focus;
    request.generation = attempt->generation;

    const AttemptId attempt_id = attempt->id;
    const DeviceGeneration attempt_generation = attempt->generation;
    const ActionKind attempt_action = attempt->action;
    const FlowRate commanded_flow = attempt->commanded_flow;
    const Pressure commanded_pressure = attempt->commanded_pressure;
    const ObservationSequence ack_sequence = attempt->ack_sequence;
    const TimestampNs issued_at = attempt->issued_at;
    attempt = nullptr;

    const auto reading = read_device_unlocked(lock, request);
    if (!reading.ok()) {
        DurableState updated = state_;
        CommandAttempt* stored = nullptr;
        for (auto& candidate : updated.attempts) {
            if (candidate.id == attempt_id) {
                stored = &candidate;
                break;
            }
        }
        if (stored != nullptr && stored->is_open()) {
            stored->status = AttemptStatus::Unresolved;
            stored->outcome = reading.code();
            stored->adapter_code = reading.code();
            stored->detail = std::string("observation failed: ") + std::string(reading.status().message());
            stored->updated_at = now;
            updated.revision = updated.revision.next();
            const auto persisted = persist_locked(std::move(updated), nullptr);
            if (!persisted.ok()) {
                return persisted.status();
            }
        }
        record.status = AttemptStatus::Unresolved;
        record.code = reading.code();
        record.detail = "the runtime could not read the device, so the effect stays unresolved";
        return record;
    }

    const LoopReading& value = reading.value();
    const auto evaluation = evaluate_effect(
        [&]() {
            CommandAttempt probe;
            probe.action = attempt_action;
            probe.generation = attempt_generation;
            probe.commanded_flow = commanded_flow;
            probe.commanded_pressure = commanded_pressure;
            return probe;
        }(),
        value, policy_.limits);

    const auto future_limit = checked_add(now, policy_.limits.observation_future_tolerance);
    const bool observation_is_current =
        !future_limit.ok() || !(future_limit.value() < value.observed_at);
    const bool observation_is_after_command =
        (!ack_sequence.valid() || ack_sequence < value.sequence) && observation_is_current;
    AttemptStatus status = AttemptStatus::Unresolved;
    StatusCode code = evaluation.code;
    std::string detail = evaluation.detail;
    if (!observation_is_after_command) {
        status = AttemptStatus::Unresolved;
        if (!observation_is_current) {
            code = StatusCode::EvidenceFuture;
            detail = "the observation is stamped ahead of the runtime clock, so it cannot prove effect";
        } else {
            code = StatusCode::EvidenceStale;
            detail = "the observation was published before the command acknowledgement, so it cannot prove effect";
        }
    } else {
        switch (evaluation.verdict) {
            case Verdict::Confirmed:
                status = AttemptStatus::EffectVerified;
                code = StatusCode::Ok;
                break;
            case Verdict::Contradicted: {
                const auto deadline = checked_add(issued_at, policy_.limits.effect_deadline);
                const TimestampNs limit = deadline.ok() ? deadline.value()
                                                        : TimestampNs::from_nanos(std::numeric_limits<std::int64_t>::max());
                if (!(now < limit)) {
                    status = AttemptStatus::Contradicted;
                    code = StatusCode::EffectContradicted;
                } else {
                    status = AttemptStatus::Unresolved;
                    code = StatusCode::EffectPending;
                    detail += " (the effect deadline has not elapsed yet)";
                }
                break;
            }
            case Verdict::Pending:
                status = AttemptStatus::Unresolved;
                code = StatusCode::EffectPending;
                break;
            case Verdict::Unusable:
                status = AttemptStatus::Unresolved;
                code = evaluation.code;
                break;
        }
    }

    DurableState updated = state_;
    CommandAttempt* stored = nullptr;
    for (auto& candidate : updated.attempts) {
        if (candidate.id == attempt_id) {
            stored = &candidate;
            break;
        }
    }
    if (stored == nullptr) {
        return Status{StatusCode::InternalError, "the attempt record disappeared during verification"};
    }
    // The post-command observation is real evidence about the device, so it is
    // absorbed into the loop state as well as used to judge the effect.
    {
        std::vector<Contradiction> absorbed_contradictions;
        bool leak_indeterminate = false;
        bool leak_confirmed = false;
        (void)absorb_reading(updated, request.loop, request.target, value, now, policy_,
                             absorbed_contradictions, leak_indeterminate, leak_confirmed);
    }
    stored = nullptr;
    for (auto& candidate : updated.attempts) {
        if (candidate.id == attempt_id) {
            stored = &candidate;
            break;
        }
    }
    if (stored == nullptr) {
        return Status{StatusCode::InternalError, "the attempt record disappeared during verification"};
    }
    if (stored->is_open()) {
        stored->status = status;
        stored->outcome = code;
        stored->detail = detail;
        stored->updated_at = now;
        stored->observed_at = value.observed_at;
        if (status == AttemptStatus::EffectVerified && attempt_action == ActionKind::SetFlowTarget) {
            if (LoopRecord* loop_record = mutable_loop(updated, request.loop)) {
                loop_record->flow_target = commanded_flow;
                loop_record->flow_target_generation = value.device_generation.present
                                                          ? value.device_generation.value
                                                          : DeviceGeneration::from_value(1);
            }
        }
        if (status == AttemptStatus::EffectVerified && attempt_action == ActionKind::SetPressureTarget) {
            if (LoopRecord* loop_record = mutable_loop(updated, request.loop)) {
                loop_record->pressure_target = commanded_pressure;
                loop_record->pressure_target_generation = value.device_generation.present
                                                              ? value.device_generation.value
                                                              : DeviceGeneration::from_value(1);
            }
        }
        if (status == AttemptStatus::EffectVerified && attempt_action == ActionKind::IsolateLoop) {
            // The isolation latch follows proven physical effect, never the
            // command acknowledgement.
            if (LoopRecord* loop_record = mutable_loop(updated, request.loop)) {
                loop_record->isolation = IsolationState::Isolated;
                loop_record->operating = LoopOperatingState::Isolated;
                // A loop-scoped command moves every device, but only the focus
                // device was observed.  The other devices' discrete states are
                // unbound from the current generation until they are re-read, so
                // they can never be mistaken for current position evidence.
                for (auto& device : loop_record->devices) {
                    if (device.id != request.target) {
                        device.state_generation = DeviceGeneration::invalid();
                    }
                }
            }
        }
        if (status == AttemptStatus::EffectVerified) {
            stored->effect_sequence = value.sequence;
            stored->effect_generation = value.device_generation.present ? value.device_generation.value
                                                                        : attempt_generation;
        }
        updated.revision = updated.revision.next();
        // The idempotency record is written in the same commit that resolves the
        // attempt, so one transition costs three durable commits rather than
        // four and a lost-response retry can never actuate a second time.
        if (!has_idempotency_locked(updated, stored->idempotency_key)) {
            const JournalCommitSequence predicted =
                state_.commit_sequence.valid() ? state_.commit_sequence.next()
                                               : JournalCommitSequence::from_value(1);
            updated.idempotency.push_back(IdempotencyRecord{stored->idempotency_key, stored->fingerprint,
                                                            stored->plan, stored->id, code, predicted});
        }
        JournalEntry entry;
        entry.action = attempt_action;
        entry.outcome = code;
        entry.attempt_status = status;
        entry.loop = request.loop;
        entry.target = request.target;
        entry.attempt = attempt_id;
        entry.detail = detail;
        const auto persisted = persist_locked(std::move(updated), &entry);
        if (!persisted.ok()) {
            return persisted.status();
        }
    }
    record.status = status;
    record.code = code;
    record.detail = detail;
    record.evidence_sequence = value.sequence;
    record.evidence_generation = value.device_generation.present ? value.device_generation.value
                                                                 : DeviceGeneration::invalid();
    return record;
}

Result<VerificationRecord> Runtime::verify(AttemptId attempt) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (shutting_down_) {
        return Status{StatusCode::ShuttingDown, "the runtime is shutting down"};
    }
    return verify_locked(lock, attempt, clock_->now());
}

Result<ObservationOutcome> Runtime::observe(LoopId loop) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (shutting_down_) {
        return Status{StatusCode::ShuttingDown, "the runtime is shutting down"};
    }
    LoopRecord* record = find_loop_locked(loop);
    if (record == nullptr) {
        return Status{StatusCode::UnknownObject, "the loop is not registered"};
    }
    std::vector<DeviceId> device_ids;
    device_ids.reserve(record->devices.size());
    for (const auto& device : record->devices) {
        device_ids.push_back(device.id);
    }
    if (device_ids.empty()) {
        return Status{StatusCode::UnknownObject, "the loop has no registered devices to observe"};
    }
    std::shared_ptr<ILoopAdapter> adapter = adapter_;
    lock.unlock();
    std::vector<Result<LoopReading>> readings;
    readings.reserve(device_ids.size());
    for (const DeviceId id : device_ids) {
        ObservationRequest request;
        request.loop = loop;
        request.target = id;
        readings.push_back([&adapter, &request]() -> Result<LoopReading> {
            try {
                return adapter->read(request);
            } catch (const std::exception& error) {
                return Status{StatusCode::AdapterInternalError,
                              std::string("the adapter threw while reading: ") + error.what()};
            } catch (...) {
                return Status{StatusCode::AdapterInternalError, "the adapter threw an unknown exception"};
            }
        }());
    }
    lock.lock();

    const TimestampNs now = clock_->now();
    ObservationOutcome outcome;
    outcome.loop = loop;
    outcome.accepted = false;
    outcome.code = StatusCode::Ok;

    DurableState next = state_;
    std::vector<Contradiction> contradictions;
    std::size_t accepted = 0;
    for (std::size_t index = 0; index < readings.size(); ++index) {
        const auto& reading = readings[index];
        if (!reading.ok()) {
            if (is_ok(outcome.code)) {
                outcome.code = reading.code();
            }
            continue;
        }
        const auto applied = absorb_reading(next, loop, device_ids[index], reading.value(), now, policy_,
                                            contradictions, outcome.leak_indeterminate, outcome.leak_confirmed);
        if (!applied.ok()) {
            if (is_ok(outcome.code)) {
                outcome.code = applied.code();
            }
            continue;
        }
        ++accepted;
        outcome.sequence = reading.value().sequence;
        outcome.observed_at = reading.value().observed_at;
        if (reading.value().device_generation.present) {
            outcome.generation = reading.value().device_generation.value;
        }
    }
    outcome.contradictions = contradictions;
    outcome.accepted = accepted > 0;
    if (accepted > 0) {
        next.revision = next.revision.next();
        const auto persisted = persist_locked(std::move(next), nullptr);
        if (!persisted.ok()) {
            return persisted.status();
        }
        if (const LoopReading* stored = find_reading_locked(loop)) {
            outcome.sequence = stored->sequence;
            outcome.observed_at = stored->observed_at;
        }
    } else if (is_ok(outcome.code)) {
        outcome.code = StatusCode::EvidenceAbsent;
    }
    outcome.revision = state_.revision;
    if (!outcome.accepted) {
        return Status{outcome.code, "no observation for the loop could be accepted"};
    }
    return outcome;
}

Result<AuthorityToken> Runtime::issue_authority(const AuthorityRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_) {
        return Status{StatusCode::ShuttingDown, "the runtime is shutting down"};
    }
    const PrincipalRecord* principal = policy_.principals.find(request.principal);
    if (principal == nullptr) {
        return Status{StatusCode::UnknownObject, "the principal is not configured"};
    }
    if (!principal->enabled) {
        return Status{StatusCode::AuthorityRevoked, "the principal is disabled"};
    }
    if (find_loop_locked(request.loop) == nullptr) {
        return Status{StatusCode::UnknownObject, "the loop is not registered"};
    }
    const auto lease_status = validate_domain(request.lease);
    if (!lease_status.ok()) {
        return lease_status.status();
    }
    if (request.lease.value() <= 0) {
        return Status{StatusCode::ValueOutOfRange, "the authority lease must be positive"};
    }
    if (policy_.limits.authority_lease_max < request.lease) {
        return Status{StatusCode::ValueOutOfRange, "the authority lease exceeds the configured maximum"};
    }
    if (issued_.size() >= policy_.limits.max_issued_tokens) {
        // Deterministic eviction: the token closest to expiry loses, ties broken
        // by the lower identity.  An evicted token is refused as revoked.
        auto victim = std::min_element(issued_.begin(), issued_.end(),
                                       [](const IssuedAuthority& a, const IssuedAuthority& b) {
                                           if (a.expires_at != b.expires_at) {
                                               return a.expires_at < b.expires_at;
                                           }
                                           return a.id < b.id;
                                       });
        if (victim != issued_.end()) {
            issued_.erase(victim);
        }
    }
    const TimestampNs now = clock_->now();
    const auto expiry = checked_add(now, request.lease);
    if (!expiry.ok()) {
        return expiry.status();
    }

    AuthorityToken token;
    token.id = state_.next_authority_token_id;
    token.principal = principal->id;
    token.capabilities = principal->capabilities;
    token.loop = request.loop;
    token.epoch = state_.epoch;
    token.incarnation = state_.last_incarnation;
    token.config_generation = policy_.generation;
    token.topology_generation = find_loop_locked(request.loop)->topology_generation;
    token.revision_at_issue = state_.revision;
    token.issued_at = now;
    token.expires_at = expiry.value();
    ++token_nonce_;
    token.nonce = token_nonce_;

    DurableState next = state_;
    next.next_authority_token_id = next.next_authority_token_id.next();
    next.revision = next.revision.next();
    const auto persisted = persist_locked(std::move(next), nullptr);
    if (!persisted.ok()) {
        return persisted.status();
    }
    issued_.push_back(IssuedAuthority{token.id, token.nonce, token.principal, token.expires_at});
    return token;
}

Result<void> Runtime::revoke_authority(AuthorityTokenId token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_) {
        return Status{StatusCode::ShuttingDown, "the runtime is shutting down"};
    }
    if (!token.valid()) {
        return Status{StatusCode::MissingRequiredField, "a token identity is required"};
    }
    const bool already_revoked =
        std::find(state_.revoked_tokens.begin(), state_.revoked_tokens.end(), token) !=
        state_.revoked_tokens.end();
    // Revocation is idempotent, so repeating it must not consume additional room.
    if (!already_revoked && state_.revoked_tokens.size() >= policy_.limits.max_revoked_tokens) {
        return Status{StatusCode::ResourceExhausted, "the revocation record bound is reached"};
    }
    if (!already_revoked) {
        DurableState next = state_;
        next.revoked_tokens.push_back(token);
        next.revision = next.revision.next();
        const auto persisted = persist_locked(std::move(next), nullptr);
        if (!persisted.ok()) {
            return persisted.status();
        }
    }
    issued_.erase(std::remove_if(issued_.begin(), issued_.end(),
                                 [token](const IssuedAuthority& record) { return record.id == token; }),
                  issued_.end());
    return ok_result();
}

Result<void> Runtime::abandon(AttemptId attempt, const AuthorityToken& authority, std::string reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_) {
        return Status{StatusCode::ShuttingDown, "the runtime is shutting down"};
    }
    const CommandAttempt* existing = find_attempt_locked(attempt);
    if (existing == nullptr) {
        return Status{StatusCode::AttemptNotFound, "the attempt is not registered"};
    }
    if (!existing->is_open()) {
        return Status{StatusCode::AttemptNotOpen, "the attempt is already resolved"};
    }
    const auto reason_status = validate_reason(reason, "reason");
    if (!reason_status.ok()) {
        return reason_status.status();
    }
    if (reason.empty()) {
        return Status{StatusCode::MissingRequiredField, "abandoning an attempt requires an attested reason"};
    }
    const LoopId loop = existing->loop;
    const auto authority_status =
        check_authority_locked(authority, loop, RequiredCapability::Service, clock_->now());
    if (!authority_status.ok()) {
        return authority_status.status();
    }

    DurableState next = state_;
    for (auto& candidate : next.attempts) {
        if (candidate.id == attempt) {
            candidate.status = AttemptStatus::Abandoned;
            candidate.outcome = StatusCode::Ok;
            candidate.detail = "abandoned under explicit service authority: " + reason;
            candidate.updated_at = clock_->now();
            break;
        }
    }
    next.revision = next.revision.next();
    JournalEntry entry;
    entry.action = ActionKind::AbandonAttempt;
    entry.outcome = StatusCode::Ok;
    entry.attempt_status = AttemptStatus::Abandoned;
    entry.loop = loop;
    entry.attempt = attempt;
    entry.detail = reason;
    const auto persisted = persist_locked(std::move(next), &entry);
    if (!persisted.ok()) {
        return persisted.status();
    }
    return ok_result();
}

Result<ServiceWindow> Runtime::open_service_window(LoopId loop, const AuthorityToken& authority,
                                                    Duration duration, std::string note) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_) {
        return Status{StatusCode::ShuttingDown, "the runtime is shutting down"};
    }
    if (find_loop_locked(loop) == nullptr) {
        return Status{StatusCode::UnknownObject, "the loop is not registered"};
    }
    const auto note_status = validate_reason(note, "note");
    if (!note_status.ok()) {
        return note_status.status();
    }
    if (note.empty()) {
        return Status{StatusCode::MissingRequiredField, "opening a service window requires a note"};
    }
    const auto duration_status = validate_domain(duration);
    if (!duration_status.ok()) {
        return duration_status.status();
    }
    if (duration.value() <= 0) {
        return Status{StatusCode::ValueOutOfRange, "a service window must have a positive duration"};
    }
    if (policy_.limits.service_window_max < duration) {
        return Status{StatusCode::ValueOutOfRange, "the service window exceeds the configured maximum"};
    }
    const auto authority_status =
        check_authority_locked(authority, loop, RequiredCapability::Service, clock_->now());
    if (!authority_status.ok()) {
        return authority_status.status();
    }
    if (state_.service_windows.size() >= policy_.limits.max_service_windows) {
        return Status{StatusCode::ResourceExhausted, "the service window bound is reached"};
    }
    const TimestampNs now = clock_->now();
    const auto expiry = checked_add(now, duration);
    if (!expiry.ok()) {
        return expiry.status();
    }

    ServiceWindow window;
    window.id = state_.next_service_window_id;
    window.loop = loop;
    window.opened_by = authority.principal;
    window.opened_at = now;
    window.expires_at = expiry.value();
    window.note = note;

    DurableState next = state_;
    next.next_service_window_id = next.next_service_window_id.next();
    next.service_windows.push_back(window);
    next.revision = next.revision.next();
    JournalEntry entry;
    entry.action = ActionKind::EnterService;
    entry.outcome = StatusCode::Ok;
    entry.attempt_status = AttemptStatus::Unknown;
    entry.loop = loop;
    entry.detail = "service window opened: " + note;
    const auto persisted = persist_locked(std::move(next), &entry);
    if (!persisted.ok()) {
        return persisted.status();
    }
    return window;
}

Result<void> Runtime::close_service_window(ServiceWindowId window, const AuthorityToken& authority) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_) {
        return Status{StatusCode::ShuttingDown, "the runtime is shutting down"};
    }
    const ServiceWindow* existing = nullptr;
    for (const auto& candidate : state_.service_windows) {
        if (candidate.id == window) {
            existing = &candidate;
            break;
        }
    }
    if (existing == nullptr) {
        return Status{StatusCode::UnknownObject, "the service window is not registered"};
    }
    const auto authority_status =
        check_authority_locked(authority, existing->loop, RequiredCapability::Service, clock_->now());
    if (!authority_status.ok()) {
        return authority_status.status();
    }
    const LoopId loop = existing->loop;
    DurableState next = state_;
    for (auto iterator = next.service_windows.begin(); iterator != next.service_windows.end(); ++iterator) {
        if (iterator->id == window) {
            next.service_windows.erase(iterator);
            break;
        }
    }
    next.revision = next.revision.next();
    JournalEntry entry;
    entry.action = ActionKind::ExitService;
    entry.outcome = StatusCode::Ok;
    entry.attempt_status = AttemptStatus::Unknown;
    entry.loop = loop;
    entry.detail = "service window closed";
    const auto persisted = persist_locked(std::move(next), &entry);
    if (!persisted.ok()) {
        return persisted.status();
    }
    return ok_result();
}

Result<std::vector<ServiceWindow>> Runtime::service_windows(LoopId loop) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ServiceWindow> result;
    for (const auto& window : state_.service_windows) {
        if (window.loop == loop) {
            result.push_back(window);
        }
    }
    return result;
}

Result<void> Runtime::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_) {
        return Status{StatusCode::ShuttingDown, "the runtime is shutting down"};
    }
    return persist_locked(state_, nullptr);
}

Result<RuntimeStatus> Runtime::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeStatus result;
    result.incarnation = state_.last_incarnation;
    result.epoch = state_.epoch;
    result.revision = state_.revision;
    result.config_generation = policy_.generation;
    result.topology_generation = state_.topology_generation;
    result.loop_count = state_.loops.size();
    for (const auto& attempt : state_.attempts) {
        if (attempt.is_open()) {
            ++result.open_attempts;
        }
    }
    result.retained_attempts = state_.attempts.size();
    result.obligations = state_.obligations.size();
    result.service_windows = state_.service_windows.size();
    result.issued_authority = issued_.size();
    result.idempotency_records = state_.idempotency.size();
    result.shutting_down = shutting_down_;
    result.store = store_ != nullptr ? store_->diagnostics() : final_store_diagnostics_;
    return result;
}

Result<std::vector<LoopRecord>> Runtime::loops() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.loops;
}

Result<LoopStatus> Runtime::loop_status(LoopId loop) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const LoopRecord* record = find_loop_locked(loop);
    if (record == nullptr) {
        return Status{StatusCode::UnknownObject, "the loop is not registered"};
    }
    LoopStatus result;
    result.loop = *record;
    if (const LoopReading* reading = find_reading_locked(loop)) {
        result.evidence = *reading;
    }
    for (const auto& attempt : state_.attempts) {
        if (attempt.loop == loop && attempt.is_open()) {
            result.open_attempts.push_back(attempt);
        }
    }
    const TimestampNs now = clock_->now();
    for (const auto& obligation : state_.obligations) {
        if (!obligation.active) {
            continue;
        }
        bool applies = false;
        for (const auto& device : record->devices) {
            if (device.id == obligation.target) {
                applies = true;
                break;
            }
        }
        if (applies) {
            ++result.active_obligations;
        }
    }
    result.service_window_open = service_window_open_locked(loop, now);
    result.revision = state_.revision;
    result.epoch = state_.epoch;
    result.incarnation = state_.last_incarnation;
    result.config_generation = policy_.generation;
    result.evidence_usability = evaluate_usability(result.evidence.leak, now, policy_.limits.evidence_freshness);
    result.evidence_usable = result.evidence_usability == EvidenceUsability::Usable;
    result.leak_indeterminate = result.evidence_usability != EvidenceUsability::Usable;
    result.leak_confirmed = result.evidence_usability == EvidenceUsability::Usable &&
                            result.evidence.leak.value == LeakState::Confirmed;
    return result;
}

Result<std::vector<CommandAttempt>> Runtime::attempts(LoopId loop) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CommandAttempt> result;
    for (const auto& attempt : state_.attempts) {
        if (attempt.loop == loop) {
            result.push_back(attempt);
        }
    }
    return result;
}

Result<std::vector<JournalEntry>> Runtime::journal() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return journal_;
}

}  // namespace liquidcooling
