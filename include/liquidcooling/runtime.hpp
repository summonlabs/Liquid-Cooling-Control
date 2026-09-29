#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "liquidcooling/adapter.hpp"
#include "liquidcooling/attempt.hpp"
#include "liquidcooling/authority.hpp"
#include "liquidcooling/policy.hpp"
#include "liquidcooling/store.hpp"
#include "liquidcooling/transition.hpp"

namespace liquidcooling {

/// Construction parameters for a runtime attached to one durable store.
struct RuntimeConfig final {
    /// Directory that holds the authoritative store files.
    std::string store_root{};
    /// Create the store directory and an empty generation when absent.
    bool create_store_if_missing{true};
    /// What to do when the authoritative snapshot fails validation.
    RecoveryPolicy recovery{RecoveryPolicy::RefuseOnCorruption};
    /// The configuration in force.  Must pass validate().
    Policy policy{};
    /// Generation of the loop/device topology described by initial_loops.
    TopologyGeneration topology_generation{};
    /// Vendor-neutral actuation boundary.  Must not be null.
    std::shared_ptr<ILoopAdapter> adapter{};
    /// Monotonic time source.  Defaults to a steady clock when null.
    std::shared_ptr<Clock> clock{};
    StoreBounds store_bounds{};
    /// Loop registry used only when a fresh store is created.
    std::vector<LoopRecord> initial_loops{};
    /// Protected service obligations used only when a fresh store is created.
    std::vector<ServiceObligation> initial_obligations{};
    /// Explicitly adopt initial_loops over a persisted registry.  Requires
    /// topology_generation to be strictly newer than the persisted one, and
    /// bumps the control-plane epoch.
    bool accept_topology_change{false};
};

/// Observed effect of one observation pass.
struct ObservationOutcome final {
    LoopId loop{};
    ObservationSequence sequence{};
    DeviceGeneration generation{};
    TimestampNs observed_at{};
    bool accepted{false};
    StatusCode code{StatusCode::Ok};
    std::vector<Contradiction> contradictions{};
    bool leak_indeterminate{false};
    bool leak_confirmed{false};
    StateRevision revision{};
};

/// Live view of one loop.
struct LoopStatus final {
    LoopRecord loop{};
    LoopReading evidence{};
    std::vector<CommandAttempt> open_attempts{};
    std::size_t active_obligations{0};
    bool service_window_open{false};
    StateRevision revision{};
    ControlPlaneEpoch epoch{};
    ControllerIncarnation incarnation{};
    ConfigGeneration config_generation{};
    bool evidence_usable{false};
    EvidenceUsability evidence_usability{EvidenceUsability::Absent};
    bool leak_indeterminate{false};
    bool leak_confirmed{false};
};

/// Live view of the whole runtime.
struct RuntimeStatus final {
    ControllerIncarnation incarnation{};
    ControlPlaneEpoch epoch{};
    StateRevision revision{};
    ConfigGeneration config_generation{};
    TopologyGeneration topology_generation{};
    std::size_t loop_count{0};
    std::size_t open_attempts{0};
    std::size_t retained_attempts{0};
    std::size_t obligations{0};
    std::size_t service_windows{0};
    std::size_t issued_authority{0};
    std::size_t idempotency_records{0};
    bool shutting_down{false};
    StoreDiagnostics store{};
};

/// The liquid-cooling control runtime.
///
/// Authority model: every mutation is planned against an explicit state binding
/// (epoch, incarnation, configuration generation, topology generation, device
/// generation, state revision) and executed only while that binding still holds.
/// A command acknowledgement is recorded but never treated as proof of effect.
///
/// Concurrency model: one mutex guards all mutable state.  Adapter calls are
/// always made with that mutex released; durable commits are always made with it
/// held.  The store never calls back into the runtime, so no lock is ever held
/// across a callback.
class Runtime final {
public:
    [[nodiscard]] static Result<std::unique_ptr<Runtime>> open(const RuntimeConfig& config);

    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    /// Stops accepting work, flushes the store and releases the exclusive lock.
    /// Safe to call repeatedly.
    [[nodiscard]] Result<void> close();

    [[nodiscard]] Result<RuntimeStatus> status() const;
    [[nodiscard]] Result<LoopStatus> loop_status(LoopId loop) const;
    [[nodiscard]] Result<std::vector<LoopRecord>> loops() const;
    [[nodiscard]] Result<std::vector<CommandAttempt>> attempts(LoopId loop) const;
    [[nodiscard]] Result<std::vector<JournalEntry>> journal() const;

    /// Refreshes every evidence channel for one loop from the adapter.
    [[nodiscard]] Result<ObservationOutcome> observe(LoopId loop);

    /// Mints a scoped authority token for a configured principal.
    [[nodiscard]] Result<AuthorityToken> issue_authority(const AuthorityRequest& request);

    /// Revokes a previously issued token.
    [[nodiscard]] Result<void> revoke_authority(AuthorityTokenId token);

    /// Validates a request against current state without mutating anything.
    [[nodiscard]] Result<Plan> plan(const TransitionRequest& request) const;

    /// Plans and executes a request.  A repeated idempotency key with the same
    /// intent replays the stored outcome without actuating again.
    [[nodiscard]] Result<ExecutionRecord> execute(const TransitionRequest& request);

    /// Executes a previously produced plan, re-validating its binding.
    [[nodiscard]] Result<ExecutionRecord> apply(const Plan& plan);

    /// Re-reads the adapter and re-evaluates the effect of an attempt.
    [[nodiscard]] Result<VerificationRecord> verify(AttemptId attempt);

    /// Abandons an open attempt under explicit service authority.
    [[nodiscard]] Result<void> abandon(AttemptId attempt, const AuthorityToken& authority, std::string reason);

    /// Opens a bounded service window that admits obligated exposure-increasing
    /// work on one loop.  Requires service authority.
    [[nodiscard]] Result<ServiceWindow> open_service_window(LoopId loop, const AuthorityToken& authority,
                                                            Duration duration, std::string note);

    /// Closes a service window early.  Requires service authority.
    [[nodiscard]] Result<void> close_service_window(ServiceWindowId window, const AuthorityToken& authority);

    /// Service windows currently open on a loop.
    [[nodiscard]] Result<std::vector<ServiceWindow>> service_windows(LoopId loop) const;

    /// Publishes the current state without other mutation, proving durability.
    [[nodiscard]] Result<void> flush();

private:
    Runtime() = default;

    struct PlanOutcome final {
        Plan plan{};
    };

    [[nodiscard]] Result<PlanOutcome> plan_locked(const TransitionRequest& request, TimestampNs now) const;
    [[nodiscard]] Result<void> check_authority_locked(const AuthorityToken& token, LoopId loop,
                                                      RequiredCapability capability, TimestampNs now) const;
    [[nodiscard]] CommandAttempt* find_attempt_locked(AttemptId id);
    [[nodiscard]] const CommandAttempt* find_attempt_locked(AttemptId id) const;
    [[nodiscard]] const LoopRecord* find_loop_locked(LoopId id) const;
    [[nodiscard]] LoopRecord* find_loop_locked(LoopId id);
    [[nodiscard]] const LoopReading* find_reading_locked(LoopId id) const;
    [[nodiscard]] LoopReading* find_reading_locked(LoopId id);
    [[nodiscard]] EvidenceUsability channel_usability(const Evidence<LeakState>& channel, TimestampNs now) const;
    [[nodiscard]] bool service_window_open_locked(LoopId loop, TimestampNs now) const;
    [[nodiscard]] DeviceId loop_focus_device_locked(const LoopRecord& loop) const;

    [[nodiscard]] Result<VerificationRecord> verify_locked(std::unique_lock<std::mutex>& lock,
                                                           AttemptId attempt, TimestampNs now);

    /// Resolves an idempotency key against durable state when it is present.
    [[nodiscard]] std::optional<Result<ExecutionRecord>> try_replay_locked(
        const TransitionRequest& request, const RequestFingerprint& fingerprint) const;

    [[nodiscard]] Result<void> persist_locked(DurableState candidate, const JournalEntry* entry);
    [[nodiscard]] Result<LoopReading> read_device_unlocked(std::unique_lock<std::mutex>& lock,
                                                           const ObservationRequest& request);

    mutable std::mutex mutex_{};
    DurableState state_{};
    Policy policy_{};
    std::shared_ptr<ILoopAdapter> adapter_{};
    std::shared_ptr<Clock> clock_{};
    std::unique_ptr<DurableStore> store_{};
    std::vector<IssuedAuthority> issued_{};
    std::vector<JournalEntry> journal_{};
    StoreDiagnostics final_store_diagnostics_{};
    std::uint64_t token_nonce_{0};
    bool shutting_down_{false};
    bool closed_{false};
};

}  // namespace liquidcooling
