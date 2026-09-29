#include <string>
#include <vector>

#include "liquidcooling/liquidcooling.hpp"
#include "testing.hpp"

using namespace liquidcooling;

TEST(model, enum_names_cover_every_defined_value) {
    for (std::uint8_t value = 0; value < kDeviceKindCount; ++value) {
        const auto parsed = parse_device_kind(value);
        CHECK(parsed.ok());
        CHECK(to_string(parsed.value()) != "unknown" || value == 0);
    }
    CHECK(parse_device_kind(kDeviceKindCount).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_device_kind(255).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_lifecycle_state(kLifecycleStateCount).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_valve_position(kValvePositionCount).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_pump_state(kPumpStateCount).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_loop_operating_state(kLoopOperatingStateCount).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_isolation_state(kIsolationStateCount).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_leak_state(kLeakStateCount).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_coolant_quality(kCoolantQualityCount).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_service_mode(kServiceModeCount).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_evidence_origin(kEvidenceOriginCount).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_evidence_usability(kEvidenceUsabilityCount).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_obligation_kind(kObligationKindCount).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_interlock_kind(kInterlockKindCount).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_action_kind(kActionKindCount).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_effect_class(kEffectClassCount).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_required_capability(kRequiredCapabilityCount).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_attempt_status(kAttemptStatusCount).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_adapter_error_code(kAdapterErrorCodeCount).code() == StatusCode::InvalidEnumValue);
    CHECK(parse_synthetic_ack_mode(kSyntheticAckModeCount).code() == StatusCode::InvalidEnumValue);
}

TEST(model, leak_semantics_are_conservative) {
    CHECK(leak_permits_increase(LeakState::None));
    CHECK(!leak_permits_increase(LeakState::Unknown));
    CHECK(!leak_permits_increase(LeakState::Suspected));
    CHECK(!leak_permits_increase(LeakState::Confirmed));
}

TEST(model, only_actuatable_kinds_accept_commands) {
    CHECK(is_actuatable(DeviceKind::Pump));
    CHECK(is_actuatable(DeviceKind::Valve));
    CHECK(is_actuatable(DeviceKind::Cdu));
    CHECK(!is_actuatable(DeviceKind::FlowSensor));
    CHECK(!is_actuatable(DeviceKind::LeakSensor));
    CHECK(!is_actuatable(DeviceKind::Unknown));
}

TEST(model, evidence_default_is_absent_not_zero) {
    const Evidence<FlowRate> channel;
    CHECK(channel.is_absent());
    CHECK(!channel.present);
    CHECK(!channel.unsupported);
    CHECK_EQ(channel.origin, EvidenceOrigin::Absent);
    CHECK_EQ(evaluate_usability(channel, TimestampNs::from_nanos(100), Duration::from_value(10)),
             EvidenceUsability::Absent);

    const Evidence<FlowRate> unsupported = Evidence<FlowRate>::make_unsupported("adapter");
    CHECK(unsupported.is_unsupported());
    CHECK_EQ(evaluate_usability(unsupported, TimestampNs::from_nanos(100), Duration::from_value(10)),
             EvidenceUsability::Unsupported);

    const Evidence<FlowRate> sample = Evidence<FlowRate>::make_present(
        FlowRate::from_value(10), ObservationSequence::from_value(1), DeviceGeneration::from_value(1),
        TimestampNs::from_nanos(100), "adapter");
    CHECK_EQ(evaluate_usability(sample, TimestampNs::from_nanos(105), Duration::from_value(10)),
             EvidenceUsability::Usable);
    CHECK_EQ(evaluate_usability(sample, TimestampNs::from_nanos(111), Duration::from_value(10)),
             EvidenceUsability::Stale);
    CHECK_EQ(evaluate_usability(sample, TimestampNs::from_nanos(99), Duration::from_value(10)),
             EvidenceUsability::Future);

    Evidence<FlowRate> recovered = sample;
    recovered.origin = EvidenceOrigin::RecoveredFromStore;
    CHECK_EQ(evaluate_usability(recovered, TimestampNs::from_nanos(105), Duration::from_value(10)),
             EvidenceUsability::Recovered);
    CHECK_EQ(usability_status(EvidenceUsability::Recovered), StatusCode::RecoveredEvidenceRequiresRefresh);
    CHECK_EQ(usability_status(EvidenceUsability::Usable), StatusCode::Ok);
    CHECK_EQ(usability_status(EvidenceUsability::Stale), StatusCode::EvidenceStale);
}

TEST(model, loop_record_device_lookup) {
    LoopRecord loop;
    loop.id = LoopId::from_value(1);
    loop.devices.push_back(DeviceRecord{DeviceId::from_value(7), DeviceKind::Pump});
    CHECK(loop.find_device(DeviceId::from_value(7)) != nullptr);
    CHECK(loop.find_device(DeviceId::from_value(8)) == nullptr);
    CHECK_EQ(loop.find_device(DeviceId::from_value(7))->kind, DeviceKind::Pump);
}

TEST(model, action_descriptor_table_is_consistent) {
    for (std::uint8_t raw = 0; raw < kActionKindCount; ++raw) {
        const auto kind = parse_action_kind(raw);
        REQUIRE(kind.ok());
        const ActionDescriptor& descriptor = action_descriptor(kind.value());
        CHECK_EQ(descriptor.kind, kind.value());
        CHECK_EQ(static_cast<std::uint8_t>(descriptor.effect), raw == 0 ? 0 : static_cast<std::uint8_t>(descriptor.effect));
        CHECK(!to_string(kind.value()).empty());
    }
    // Hazard-reducing actions are always dispatched with isolation authority,
    // never with production authority alone.
    CHECK_EQ(action_descriptor(ActionKind::StopPump).capability, RequiredCapability::Isolate);
    CHECK_EQ(action_descriptor(ActionKind::CloseValve).capability, RequiredCapability::Isolate);
    CHECK_EQ(action_descriptor(ActionKind::IsolateLoop).capability, RequiredCapability::Isolate);
    CHECK_EQ(action_descriptor(ActionKind::StartPump).capability, RequiredCapability::Production);
    CHECK_EQ(action_descriptor(ActionKind::OpenValve).capability, RequiredCapability::Production);
    CHECK_EQ(action_descriptor(ActionKind::EnterService).capability, RequiredCapability::Service);
    CHECK_EQ(action_descriptor(ActionKind::AbandonAttempt).capability, RequiredCapability::Service);
    CHECK_EQ(action_descriptor(ActionKind::StartPump).effect, EffectClass::IncreaseExposure);
    CHECK_EQ(action_descriptor(ActionKind::StopPump).effect, EffectClass::ReduceExposure);
    CHECK(action_descriptor(ActionKind::StartPump).issues_command);
    CHECK(!action_descriptor(ActionKind::EnterService).issues_command);
    CHECK(!action_descriptor(ActionKind::ClearIsolation).issues_command);
}

TEST(model, capability_sets_do_not_form_a_privilege_ladder) {
    const AuthorityCapabilities service = AuthorityCapabilities::service_engineer();
    CHECK(service.service);
    CHECK(service.isolate);
    CHECK(!service.production);
    CHECK(!service.covers(AuthorityCapabilities::production_operator()));

    const AuthorityCapabilities isolation = AuthorityCapabilities::isolation_only();
    CHECK(isolation.isolate);
    CHECK(!isolation.production);
    CHECK(!isolation.service);

    const AuthorityCapabilities production = AuthorityCapabilities::production_operator();
    CHECK(production.production);
    CHECK(!production.service);

    CHECK(AuthorityCapabilities::full().covers(AuthorityCapabilities::service_engineer()));
    CHECK(AuthorityCapabilities::full().covers(AuthorityCapabilities::production_operator()));
    CHECK(!AuthorityCapabilities::none().covers(AuthorityCapabilities::auditor()));

    CHECK(capability_set(RequiredCapability::Isolate).isolate);
    CHECK(!capability_set(RequiredCapability::Isolate).production);
    CHECK(capability_set(RequiredCapability::Production).production);
    CHECK(capability_set(RequiredCapability::Service).service);
    CHECK(!capability_set(RequiredCapability::Service).production);
}

TEST(model, effect_resolution_for_directional_actions) {
    const ActionDescriptor& flow = action_descriptor(ActionKind::SetFlowTarget);
    CHECK_EQ(resolve_effect(flow, false, 0, 100).value(), EffectClass::IncreaseExposure);
    CHECK_EQ(resolve_effect(flow, true, 100, 200).value(), EffectClass::IncreaseExposure);
    CHECK_EQ(resolve_effect(flow, true, 200, 100).value(), EffectClass::ReduceExposure);
    CHECK_EQ(resolve_effect(flow, true, 100, 100).code(), StatusCode::TargetUnchanged);

    const ActionDescriptor& pump = action_descriptor(ActionKind::StartPump);
    CHECK_EQ(resolve_effect(pump, false, 0, 0).value(), EffectClass::IncreaseExposure);
}

TEST(model, attempt_status_classification) {
    CHECK(attempt_is_open(AttemptStatus::Planned));
    CHECK(attempt_is_open(AttemptStatus::Issued));
    CHECK(attempt_is_open(AttemptStatus::Acknowledged));
    CHECK(attempt_is_open(AttemptStatus::Unresolved));
    CHECK(!attempt_is_open(AttemptStatus::EffectVerified));
    CHECK(!attempt_is_open(AttemptStatus::Contradicted));
    CHECK(!attempt_is_open(AttemptStatus::Failed));
    CHECK(!attempt_is_open(AttemptStatus::Abandoned));
    CHECK(attempt_is_terminal(AttemptStatus::EffectVerified));
    CHECK(attempt_is_terminal(AttemptStatus::Abandoned));
    CHECK(!attempt_is_terminal(AttemptStatus::Unresolved));
    CHECK(!attempt_is_open(AttemptStatus::Unknown));
    CHECK(!attempt_is_terminal(AttemptStatus::Unknown));
}

TEST(model, policy_defaults_and_validation) {
    Policy policy = Policy::defaults(ConfigGeneration::from_value(1));
    CHECK(validate(policy).ok());
    for (std::uint8_t raw = 0; raw < kInterlockKindCount; ++raw) {
        const auto kind = parse_interlock_kind(raw);
        REQUIRE(kind.ok());
        CHECK(policy.interlock_enabled(kind.value()));
    }
    CHECK(!policy.generation.valid() == false);
    CHECK_EQ(policy.generation.value(), 1u);
    CHECK_EQ(policy.principals.generation.value(), 1u);

    Policy bad_generation = policy;
    bad_generation.generation = ConfigGeneration::invalid();
    CHECK_CODE(validate(bad_generation), StatusCode::MissingRequiredField);

    Policy bad_range = policy;
    bad_range.limits.flow.minimum = FlowRate::from_value(5000);
    bad_range.limits.flow.maximum = FlowRate::from_value(1000);
    CHECK_CODE(validate(bad_range), StatusCode::InvalidArgument);

    Policy bad_freshness = policy;
    bad_freshness.limits.evidence_freshness = Duration::from_value(0);
    CHECK_CODE(validate(bad_freshness), StatusCode::InvalidArgument);

    Policy bad_bounds = policy;
    bad_bounds.limits.max_loops = 0;
    CHECK_CODE(validate(bad_bounds), StatusCode::InvalidArgument);

    Policy bad_step = policy;
    bad_step.limits.maximum_flow_step = FlowRate::from_value(0);
    CHECK_CODE(validate(bad_step), StatusCode::InvalidArgument);

    Policy bad_deadline = policy;
    bad_deadline.limits.effect_deadline = Duration::from_value(0);
    CHECK_CODE(validate(bad_deadline), StatusCode::InvalidArgument);

    Policy duplicate_principals = policy;
    duplicate_principals.principals.principals.push_back(
        PrincipalRecord{PrincipalId::from_value(1), "operator", AuthorityCapabilities::auditor(), true});
    duplicate_principals.principals.principals.push_back(
        PrincipalRecord{PrincipalId::from_value(1), "operator-two", AuthorityCapabilities::auditor(), true});
    CHECK_CODE(validate(duplicate_principals), StatusCode::DuplicateIdentity);

    Policy unnamed = policy;
    unnamed.principals.principals.push_back(
        PrincipalRecord{PrincipalId::from_value(9), "bad name", AuthorityCapabilities::auditor(), true});
    CHECK_CODE(validate(unnamed), StatusCode::InvalidEncoding);
}

TEST(model, principal_registry_lookup) {
    Policy policy = Policy::defaults(ConfigGeneration::from_value(1));
    policy.principals.principals.push_back(
        PrincipalRecord{PrincipalId::from_value(4), "ops", AuthorityCapabilities::production_operator(), true});
    CHECK(policy.principals.find(PrincipalId::from_value(4)) != nullptr);
    CHECK(policy.principals.find(PrincipalId::from_value(5)) == nullptr);
    CHECK_EQ(policy.principals.find(PrincipalId::from_value(4))->name, std::string("ops"));
}
