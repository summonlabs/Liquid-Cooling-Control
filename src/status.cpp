#include "liquidcooling/status.hpp"

namespace liquidcooling {
namespace {

constexpr std::string_view kCodeNames[] = {
    "ok",
    "invalid-argument",
    "malformed-request",
    "string-too-long",
    "value-out-of-range",
    "unsupported-action",
    "invalid-enum-value",
    "missing-required-field",
    "duplicate-identity",
    "too-many-objects",
    "invalid-encoding",
    "unknown-object",
    "wrong-object-kind",
    "stale-generation",
    "future-generation",
    "stale-revision",
    "future-revision",
    "epoch-mismatch",
    "incarnation-mismatch",
    "config-generation-mismatch",
    "topology-generation-mismatch",
    "evidence-generation-mismatch",
    "superseded-authority",
    "authority-missing",
    "authority-expired",
    "authority-revoked",
    "authority-scope-mismatch",
    "authority-insufficient",
    "service-mode-conflict",
    "evidence-absent",
    "evidence-stale",
    "evidence-unsupported",
    "evidence-conflicting",
    "evidence-future",
    "leak-state-indeterminate",
    "leak-confirmed",
    "coolant-quality-not-nominal",
    "flow-out-of-range",
    "pressure-out-of-range",
    "temperature-out-of-range",
    "conductivity-out-of-range",
    "ph-out-of-range",
    "interlock-blocked",
    "service-obligation-active",
    "device-not-present",
    "device-faulted",
    "device-absent",
    "service-window-required",
    "invalid-state-transition",
    "already-in-target-state",
    "target-unchanged",
    "loop-isolated",
    "loop-not-isolated",
    "unresolved-attempt-blocks",
    "attempt-not-found",
    "attempt-not-open",
    "idempotency-conflict",
    "attempt-limit-exceeded",
    "adapter-unavailable",
    "adapter-rejected",
    "adapter-device-missing",
    "adapter-generation-mismatch",
    "adapter-busy",
    "adapter-internal-error",
    "effect-not-observed",
    "effect-contradicted",
    "effect-pending",
    "store-missing",
    "store-corrupt",
    "store-locked",
    "store-io-error",
    "store-version-unsupported",
    "store-bounds-exceeded",
    "store-path-invalid",
    "store-journal-lagging",
    "store-not-open",
    "shutting-down",
    "resource-exhausted",
    "recovered-evidence-requires-refresh",
    "internal-error",
};

struct CodeEntry {
    StatusCode code;
    std::string_view name;
    std::string_view description;
};

// Ordered by numeric value; validated by a unit test.
constexpr CodeEntry kCodes[] = {
    {StatusCode::Ok, "ok", "operation completed"},
    {StatusCode::InvalidArgument, "invalid-argument", "an argument failed structural validation"},
    {StatusCode::MalformedRequest, "malformed-request", "the request is not well formed"},
    {StatusCode::StringTooLong, "string-too-long", "a bounded string exceeded its limit"},
    {StatusCode::ValueOutOfRange, "value-out-of-range", "a quantity is outside its physical domain"},
    {StatusCode::UnsupportedAction, "unsupported-action", "the action is not recognised by this runtime"},
    {StatusCode::InvalidEnumValue, "invalid-enum-value", "an encoded enumerator is not defined"},
    {StatusCode::MissingRequiredField, "missing-required-field", "a field required by the action is absent"},
    {StatusCode::DuplicateIdentity, "duplicate-identity", "an identity appears more than once where uniqueness is required"},
    {StatusCode::TooManyObjects, "too-many-objects", "a bounded collection exceeded its capacity"},
    {StatusCode::InvalidEncoding, "invalid-encoding", "a string or byte sequence is not valid for its declared encoding"},
    {StatusCode::UnknownObject, "unknown-object", "the referenced object is not registered"},
    {StatusCode::WrongObjectKind, "wrong-object-kind", "the referenced object exists but has a different kind"},
    {StatusCode::StaleGeneration, "stale-generation", "the bound generation is older than the current generation"},
    {StatusCode::FutureGeneration, "future-generation", "the bound generation is newer than the current generation"},
    {StatusCode::StaleRevision, "stale-revision", "the bound state revision is older than the current revision"},
    {StatusCode::FutureRevision, "future-revision", "the bound state revision is newer than the current revision"},
    {StatusCode::EpochMismatch, "epoch-mismatch", "the bound control-plane epoch is not the current epoch"},
    {StatusCode::IncarnationMismatch, "incarnation-mismatch", "the bound controller incarnation is not the running incarnation"},
    {StatusCode::ConfigGenerationMismatch, "config-generation-mismatch", "the bound configuration generation is not current"},
    {StatusCode::TopologyGenerationMismatch, "topology-generation-mismatch", "the bound topology generation is not current"},
    {StatusCode::EvidenceGenerationMismatch, "evidence-generation-mismatch", "evidence was produced for a different device generation"},
    {StatusCode::SupersededAuthority, "superseded-authority", "the authority was superseded by a newer grant"},
    {StatusCode::AuthorityMissing, "authority-missing", "no authority was supplied"},
    {StatusCode::AuthorityExpired, "authority-expired", "the authority lease has elapsed"},
    {StatusCode::AuthorityRevoked, "authority-revoked", "the authority was revoked or was never issued"},
    {StatusCode::AuthorityScopeMismatch, "authority-scope-mismatch", "the authority does not cover the requested loop"},
    {StatusCode::AuthorityInsufficient, "authority-insufficient", "the authority lacks the capability the action requires"},
    {StatusCode::ServiceModeConflict, "service-mode-conflict", "production actuation was attempted while the loop is in service mode"},
    {StatusCode::EvidenceAbsent, "evidence-absent", "no evidence has ever been obtained for the channel"},
    {StatusCode::EvidenceStale, "evidence-stale", "the newest evidence is older than the freshness window"},
    {StatusCode::EvidenceUnsupported, "evidence-unsupported", "the adapter cannot provide this evidence channel"},
    {StatusCode::EvidenceConflicting, "evidence-conflicting", "evidence channels disagree in an unresolvable way"},
    {StatusCode::EvidenceFuture, "evidence-future", "evidence carries a sequence or timestamp ahead of the runtime"},
    {StatusCode::LeakStateIndeterminate, "leak-state-indeterminate", "leak evidence is absent, stale or unsupported"},
    {StatusCode::LeakConfirmed, "leak-confirmed", "a leak is confirmed on the loop"},
    {StatusCode::CoolantQualityNotNominal, "coolant-quality-not-nominal", "coolant quality evidence is not nominal"},
    {StatusCode::FlowOutOfRange, "flow-out-of-range", "flow evidence or target is outside the permitted envelope"},
    {StatusCode::PressureOutOfRange, "pressure-out-of-range", "pressure evidence or target is outside the permitted envelope"},
    {StatusCode::TemperatureOutOfRange, "temperature-out-of-range", "temperature evidence is outside the permitted envelope"},
    {StatusCode::ConductivityOutOfRange, "conductivity-out-of-range", "coolant conductivity is outside the permitted envelope"},
    {StatusCode::PhOutOfRange, "ph-out-of-range", "coolant pH is outside the permitted envelope"},
    {StatusCode::InterlockBlocked, "interlock-blocked", "a configured interlock forbids the transition"},
    {StatusCode::ServiceObligationActive, "service-obligation-active", "a protected service obligation forbids the transition"},
    {StatusCode::DeviceNotPresent, "device-not-present", "the device is not present in the loop"},
    {StatusCode::DeviceFaulted, "device-faulted", "the device is latched in a faulted state"},
    {StatusCode::DeviceAbsent, "device-absent", "the device is registered but reported absent"},
    {StatusCode::ServiceWindowRequired, "service-window-required", "the action requires an open service window"},
    {StatusCode::InvalidStateTransition, "invalid-state-transition", "the loop or device state does not permit the transition"},
    {StatusCode::AlreadyInTargetState, "already-in-target-state", "the object already has the requested state"},
    {StatusCode::TargetUnchanged, "target-unchanged", "the requested setpoint equals the current target"},
    {StatusCode::LoopIsolated, "loop-isolated", "the loop is isolated and cannot accept the transition"},
    {StatusCode::LoopNotIsolated, "loop-not-isolated", "the loop is not isolated so the transition is refused"},
    {StatusCode::UnresolvedAttemptBlocks, "unresolved-attempt-blocks", "an open effect-bearing attempt forbids incompatible actuation"},
    {StatusCode::AttemptNotFound, "attempt-not-found", "no attempt with that identity exists"},
    {StatusCode::AttemptNotOpen, "attempt-not-open", "the attempt is already resolved"},
    {StatusCode::IdempotencyConflict, "idempotency-conflict", "the idempotency key was reused for a different request"},
    {StatusCode::AttemptLimitExceeded, "attempt-limit-exceeded", "the bound on retained attempts was exceeded"},
    {StatusCode::AdapterUnavailable, "adapter-unavailable", "the adapter refused or could not accept the call"},
    {StatusCode::AdapterRejected, "adapter-rejected", "the adapter rejected the actuation command"},
    {StatusCode::AdapterDeviceMissing, "adapter-device-missing", "the adapter reports the device as missing"},
    {StatusCode::AdapterGenerationMismatch, "adapter-generation-mismatch", "the adapter reports a different device generation"},
    {StatusCode::AdapterBusy, "adapter-busy", "the adapter is busy and the command was not attempted"},
    {StatusCode::AdapterInternalError, "adapter-internal-error", "the adapter reported an internal failure"},
    {StatusCode::EffectNotObserved, "effect-not-observed", "no post-command observation consistent with the intent was seen"},
    {StatusCode::EffectContradicted, "effect-contradicted", "post-command observation contradicts the intent"},
    {StatusCode::EffectPending, "effect-pending", "the effect has not yet been observed but remains possible"},
    {StatusCode::StoreMissing, "store-missing", "the durable store does not exist at the configured root"},
    {StatusCode::StoreCorrupt, "store-corrupt", "the durable store failed integrity validation"},
    {StatusCode::StoreLocked, "store-locked", "another process holds the exclusive store lock"},
    {StatusCode::StoreIoError, "store-io-error", "an input/output operation on the store failed"},
    {StatusCode::StoreVersionUnsupported, "store-version-unsupported", "the store format version is not supported"},
    {StatusCode::StoreBoundsExceeded, "store-bounds-exceeded", "the store exceeded a configured bound"},
    {StatusCode::StorePathInvalid, "store-path-invalid", "the store path failed canonicalisation or hardening checks"},
    {StatusCode::StoreJournalLagging, "store-journal-lagging", "the journal is behind the authoritative snapshot"},
    {StatusCode::StoreNotOpen, "store-not-open", "the store is not open"},
    {StatusCode::ShuttingDown, "shutting-down", "the runtime is shutting down and refuses new work"},
    {StatusCode::ResourceExhausted, "resource-exhausted", "a bounded runtime resource is exhausted"},
    {StatusCode::RecoveredEvidenceRequiresRefresh, "recovered-evidence-requires-refresh", "recovered evidence must be refreshed before it can authorise a transition"},
    {StatusCode::InternalError, "internal-error", "an internal invariant was violated"},
};

constexpr bool codes_are_ordered() {
    for (std::size_t i = 1; i < std::size(kCodes); ++i) {
        if (static_cast<std::uint16_t>(kCodes[i].code) <= static_cast<std::uint16_t>(kCodes[i - 1].code)) {
            return false;
        }
    }
    return true;
}

static_assert(codes_are_ordered(), "status code table must be strictly ordered by numeric value");
static_assert(std::size(kCodeNames) == std::size(kCodes), "status name table must match the code table");

constexpr std::size_t kCodeCount = std::size(kCodes);

constexpr std::size_t index_of(StatusCode code) noexcept {
    const auto value = static_cast<std::uint16_t>(code);
    for (std::size_t i = 0; i < kCodeCount; ++i) {
        if (static_cast<std::uint16_t>(kCodes[i].code) == value) {
            return i;
        }
    }
    return kCodeCount;
}

}  // namespace

std::string_view to_string(StatusCode code) noexcept {
    const std::size_t index = index_of(code);
    return index < kCodeCount ? kCodes[index].name : std::string_view{"unknown-status"};
}

bool status_code_defined(StatusCode code) noexcept { return index_of(code) < kCodeCount; }

std::string_view describe(StatusCode code) noexcept {
    const std::size_t index = index_of(code);
    return index < kCodeCount ? kCodes[index].description : std::string_view{"unrecognised status code"};
}

std::string Status::to_display_string() const {
    if (ok()) {
        return "ok";
    }
    std::string out;
    out.reserve(message_.size() + 32);
    out.append(to_string(code_));
    if (!message_.empty()) {
        out.append(": ");
        out.append(message_);
    }
    return out;
}

BadResultAccess::BadResultAccess(const Status& status)
    : std::logic_error("liquidcooling::Result holds no value: " + status.to_display_string()),
      status_(status) {}

}  // namespace liquidcooling
