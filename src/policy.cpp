#include "liquidcooling/policy.hpp"

namespace liquidcooling {

Policy Policy::defaults(ConfigGeneration generation) {
    Policy policy;
    policy.generation = generation;
    policy.principals.generation = generation;
    policy.interlocks.fill(true);
    return policy;
}

Result<void> validate(const PolicyLimits& limits) {
    if (!limits.flow.valid() || !limits.supply_pressure.valid() || !limits.coolant_temperature.valid() ||
        !limits.conductivity.valid() || !limits.acidity.valid()) {
        return Status{StatusCode::InvalidArgument, "policy limit range has a maximum below its minimum"};
    }
    const auto flow_status = validate_domain(limits.flow.minimum);
    if (!flow_status.ok()) {
        return flow_status.status();
    }
    const auto flow_max_status = validate_domain(limits.flow.maximum);
    if (!flow_max_status.ok()) {
        return flow_max_status.status();
    }
    if (!limits.flow.contains(limits.minimum_effective_flow) || !limits.flow.contains(limits.maximum_flow_step)) {
        return Status{StatusCode::InvalidArgument, "flow thresholds must lie inside the flow range"};
    }
    if (limits.minimum_effective_flow.value() <= 0) {
        return Status{StatusCode::InvalidArgument, "minimum effective flow must be positive"};
    }
    if (limits.maximum_flow_step.value() <= 0) {
        return Status{StatusCode::InvalidArgument, "maximum flow step must be positive"};
    }
    if (limits.maximum_pressure_step.value() <= 0) {
        return Status{StatusCode::InvalidArgument, "maximum pressure step must be positive"};
    }
    if (limits.effect_deadline.value() <= 0) {
        return Status{StatusCode::InvalidArgument, "effect deadline must be positive"};
    }
    const auto deadline_status = validate_domain(limits.effect_deadline);
    if (!deadline_status.ok()) {
        return deadline_status.status();
    }
    if (limits.flow_tolerance.value() < 0 ||
        limits.flow_tolerance.value() > limits.flow.maximum.value()) {
        return Status{StatusCode::InvalidArgument, "flow tolerance must be non-negative and within the flow range"};
    }
    if (limits.pressure_tolerance.value() < 0 ||
        limits.pressure_tolerance.value() > limits.supply_pressure.maximum.value()) {
        return Status{StatusCode::InvalidArgument, "pressure tolerance must be non-negative and within range"};
    }
    if (limits.evidence_freshness.value() <= 0) {
        return Status{StatusCode::InvalidArgument, "evidence freshness window must be positive"};
    }
    const auto freshness_status = validate_domain(limits.evidence_freshness);
    if (!freshness_status.ok()) {
        return freshness_status.status();
    }
    const auto tolerance_status = validate_domain(limits.observation_future_tolerance);
    if (!tolerance_status.ok()) {
        return tolerance_status.status();
    }
    const auto lease_status = validate_domain(limits.authority_lease_max);
    if (!lease_status.ok()) {
        return lease_status.status();
    }
    if (limits.authority_lease_max.value() <= 0) {
        return Status{StatusCode::InvalidArgument, "authority lease maximum must be positive"};
    }
    const auto window_status = validate_domain(limits.service_window_max);
    if (!window_status.ok()) {
        return window_status.status();
    }
    if (limits.service_window_max.value() <= 0) {
        return Status{StatusCode::InvalidArgument, "service window maximum must be positive"};
    }
    if (limits.max_loops == 0 || limits.max_loops > 4096) {
        return Status{StatusCode::InvalidArgument, "max_loops must be in 1..4096"};
    }
    if (limits.max_devices_per_loop == 0 || limits.max_devices_per_loop > 4096) {
        return Status{StatusCode::InvalidArgument, "max_devices_per_loop must be in 1..4096"};
    }
    if (limits.max_open_attempts == 0 || limits.max_open_attempts > 4096) {
        return Status{StatusCode::InvalidArgument, "max_open_attempts must be in 1..4096"};
    }
    if (limits.max_retained_attempts < limits.max_open_attempts || limits.max_retained_attempts > 65536) {
        return Status{StatusCode::InvalidArgument, "max_retained_attempts must be at least max_open_attempts and at most 65536"};
    }
    if (limits.max_obligations == 0 || limits.max_obligations > 65536) {
        return Status{StatusCode::InvalidArgument, "max_obligations must be in 1..65536"};
    }
    if (limits.max_service_windows == 0 || limits.max_service_windows > 4096) {
        return Status{StatusCode::InvalidArgument, "max_service_windows must be in 1..4096"};
    }
    if (limits.max_issued_tokens == 0 || limits.max_issued_tokens > 1'048'576) {
        return Status{StatusCode::InvalidArgument, "max_issued_tokens must be in 1..1048576"};
    }
    if (limits.max_idempotency_records == 0 || limits.max_idempotency_records > 1'048'576) {
        return Status{StatusCode::InvalidArgument, "max_idempotency_records must be in 1..1048576"};
    }
    if (limits.max_journal_entries == 0 || limits.max_journal_entries > 65536) {
        return Status{StatusCode::InvalidArgument, "max_journal_entries must be in 1..65536"};
    }
    if (limits.max_revoked_tokens == 0 || limits.max_revoked_tokens > 1'048'576) {
        return Status{StatusCode::InvalidArgument, "max_revoked_tokens must be in 1..1048576"};
    }
    return ok_result();
}

Result<void> validate(const Policy& policy) {
    if (!policy.generation.valid()) {
        return Status{StatusCode::MissingRequiredField, "policy generation must be non-zero"};
    }
    const auto limits_status = validate(policy.limits);
    if (!limits_status.ok()) {
        return limits_status.status();
    }
    return policy.principals.validate();
}

}  // namespace liquidcooling
