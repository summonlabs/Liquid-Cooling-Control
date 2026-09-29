#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "liquidcooling/attempt.hpp"
#include "liquidcooling/evidence.hpp"
#include "liquidcooling/ids.hpp"
#include "liquidcooling/status.hpp"

namespace liquidcooling {

/// Failure vocabulary of a vendor adapter, kept separate from the runtime's own
/// status vocabulary so that vendor-specific failures cannot be mistaken for
/// control-plane decisions.
enum class AdapterErrorCode : std::uint8_t {
    None = 0,
    Unavailable = 1,
    DeviceMissing = 2,
    GenerationMismatch = 3,
    Rejected = 4,
    Busy = 5,
    Internal = 6,
    Unsupported = 7,
};

inline constexpr std::size_t kAdapterErrorCodeCount = 8;
inline constexpr std::array<std::string_view, kAdapterErrorCodeCount> kAdapterErrorCodeNames = {
    "none", "unavailable", "device-missing", "generation-mismatch", "rejected", "busy", "internal", "unsupported"};

[[nodiscard]] inline std::string_view to_string(AdapterErrorCode value) noexcept {
    return detail::enum_name(value, kAdapterErrorCodeNames);
}
[[nodiscard]] inline Result<AdapterErrorCode> parse_adapter_error_code(std::uint8_t value) {
    return detail::enum_parse<AdapterErrorCode>(value, kAdapterErrorCodeNames, "adapter error code");
}

/// Maps an adapter error onto the runtime status that callers observe.
[[nodiscard]] StatusCode to_status(AdapterErrorCode error) noexcept;

/// Static description of an adapter and the generation of the evidence it can
/// currently produce.
struct AdapterDescriptor final {
    std::string vendor{};
    std::string model{};
    std::uint32_t protocol_version{0};
    EvidenceGeneration evidence_generation{};
    SwitchoverGeneration switchover_generation{};
};

/// A physical command handed to a vendor adapter.
struct ActuationCommand final {
    CommandId command{};
    AttemptId attempt{};
    PlanId plan{};
    ActionKind action{ActionKind::Unknown};
    LoopId loop{};
    DeviceId target{};
    DeviceGeneration generation{};
    FlowRate flow_target{};
    Pressure pressure_target{};
    ControlPlaneEpoch epoch{};
    StateRevision revision{};
    TimestampNs issued_at{};
};

/// The adapter's acknowledgement.  An acknowledgement never proves effect.
struct CommandAck final {
    CommandId command{};
    AttemptId attempt{};
    DeviceGeneration generation{};
    AdapterErrorCode error{AdapterErrorCode::None};
    ObservationSequence ack_sequence{};
    TimestampNs acked_at{};
    std::string detail{};
};

/// A request for a full evidence set.
struct ObservationRequest final {
    LoopId loop{};
    /// Optional device focus; invalid means a loop-wide read.
    DeviceId target{};
    DeviceGeneration generation{};
};

/// Vendor-neutral actuation and observation boundary.
///
/// Implementations must be callable from any thread and must never call back
/// into the runtime: the runtime makes every adapter call with its own state
/// lock released.
class ILoopAdapter {
public:
    ILoopAdapter() = default;
    ILoopAdapter(const ILoopAdapter&) = delete;
    ILoopAdapter& operator=(const ILoopAdapter&) = delete;
    virtual ~ILoopAdapter();

    [[nodiscard]] virtual AdapterDescriptor describe() const = 0;
    [[nodiscard]] virtual Result<CommandAck> actuate(const ActuationCommand& command) = 0;
    [[nodiscard]] virtual Result<LoopReading> read(const ObservationRequest& request) = 0;
};

}  // namespace liquidcooling
