#include "liquidcooling/transition.hpp"

#include <array>

#include "codec.hpp"

namespace liquidcooling {
namespace {

using detail::Writer;

// Indexed by ActionKind; the index must equal the enumerator value.
constexpr std::array<ActionDescriptor, kActionKindCount> kActionDescriptors = {{
    {ActionKind::Unknown, "unknown", EffectClass::None, RequiredCapability::None, false, false, false, false, false,
     false},
    {ActionKind::StartPump, "start-pump", EffectClass::IncreaseExposure, RequiredCapability::Production, true, false,
     false, false, false, true},
    {ActionKind::StopPump, "stop-pump", EffectClass::ReduceExposure, RequiredCapability::Isolate, true, false, false,
     false, false, true},
    {ActionKind::OpenValve, "open-valve", EffectClass::IncreaseExposure, RequiredCapability::Production, true, false,
     false, false, false, true},
    {ActionKind::CloseValve, "close-valve", EffectClass::ReduceExposure, RequiredCapability::Isolate, true, false,
     false, false, false, true},
    {ActionKind::SetFlowTarget, "set-flow-target", EffectClass::DirectionRelative, RequiredCapability::Production,
     false, true, false, false, false, true},
    {ActionKind::SetPressureTarget, "set-pressure-target", EffectClass::DirectionRelative,
     RequiredCapability::Production, false, false, true, false, false, true},
    {ActionKind::IsolateLoop, "isolate-loop", EffectClass::ReduceExposure, RequiredCapability::Isolate, false, false,
     false, false, false, true},
    {ActionKind::ClearIsolation, "clear-isolation", EffectClass::IncreaseExposure, RequiredCapability::Production,
     false, false, false, false, false, false},
    {ActionKind::EnterService, "enter-service", EffectClass::None, RequiredCapability::Service, false, false, false,
     false, false, false},
    {ActionKind::ExitService, "exit-service", EffectClass::None, RequiredCapability::Service, false, false, false,
     false, false, false},
    {ActionKind::CompleteObligation, "complete-obligation", EffectClass::None, RequiredCapability::Service, false,
     false, false, true, false, false},
    {ActionKind::AbandonAttempt, "abandon-attempt", EffectClass::None, RequiredCapability::Service, false, false,
     false, false, true, false},
    {ActionKind::ResetFault, "reset-fault", EffectClass::None, RequiredCapability::Production, true, false, false,
     false, false, false},
}};

static_assert(kActionDescriptors.size() == kActionKindCount, "action descriptor table size");
static_assert(kActionDescriptors[static_cast<std::size_t>(ActionKind::StartPump)].kind == ActionKind::StartPump,
              "action descriptor table must be indexed by action kind");

}  // namespace

const ActionDescriptor& action_descriptor(ActionKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    if (index >= kActionDescriptors.size()) {
        return kActionDescriptors[0];
    }
    return kActionDescriptors[index];
}

AuthorityCapabilities capability_set(RequiredCapability capability) noexcept {
    switch (capability) {
        case RequiredCapability::None:
            return AuthorityCapabilities::none();
        case RequiredCapability::Observe:
            return AuthorityCapabilities::auditor();
        case RequiredCapability::Isolate:
            return AuthorityCapabilities::isolation_only();
        case RequiredCapability::Production:
            return AuthorityCapabilities::production_operator();
        case RequiredCapability::Service:
            return AuthorityCapabilities::service_engineer();
    }
    return AuthorityCapabilities::none();
}

std::vector<std::uint8_t> canonical_request_bytes(const TransitionRequest& request) {
    Writer writer;
    writer.u8(static_cast<std::uint8_t>(request.action));
    writer.u64(request.loop.value());
    writer.u64(request.target.value());
    writer.u32(request.expected_device_generation.value());
    writer.i64(request.flow_target.value());
    writer.boolean(request.has_flow_target);
    writer.i64(request.pressure_target.value());
    writer.boolean(request.has_pressure_target);
    writer.u64(request.obligation.value());
    writer.u64(request.attempt.value());
    writer.u64(request.expected_revision.value());
    return writer.bytes();
}

RequestFingerprint fingerprint_request(const TransitionRequest& request) {
    const std::vector<std::uint8_t> bytes = canonical_request_bytes(request);
    return fingerprint_bytes(bytes.data(), bytes.size());
}

Result<EffectClass> resolve_effect(const ActionDescriptor& descriptor, bool has_current_value,
                                   std::int64_t current_value, std::int64_t requested_value) {
    if (descriptor.effect != EffectClass::DirectionRelative) {
        return descriptor.effect;
    }
    if (!has_current_value) {
        // With no recorded target the request establishes one; that is an
        // exposure-increasing act and is authorised as such.
        return EffectClass::IncreaseExposure;
    }
    if (requested_value > current_value) {
        return EffectClass::IncreaseExposure;
    }
    if (requested_value < current_value) {
        return EffectClass::ReduceExposure;
    }
    return Status{StatusCode::TargetUnchanged, "requested setpoint equals the current target"};
}

}  // namespace liquidcooling
