#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace liquidcooling {

/// Machine-readable outcome code.
///
/// The numeric value of every enumerator is part of the public contract and is
/// stable across releases.  Codes are grouped by the validation stage that can
/// produce them so that a caller can reason about precedence.
enum class StatusCode : std::uint16_t {
    Ok = 0,

    // 100-199: request shape and input validity.
    InvalidArgument = 100,
    MalformedRequest = 101,
    StringTooLong = 102,
    ValueOutOfRange = 103,
    UnsupportedAction = 104,
    InvalidEnumValue = 105,
    MissingRequiredField = 106,
    DuplicateIdentity = 107,
    TooManyObjects = 108,
    InvalidEncoding = 109,

    // 200-299: identity, generation and revision binding.
    UnknownObject = 200,
    WrongObjectKind = 201,
    StaleGeneration = 202,
    FutureGeneration = 203,
    StaleRevision = 204,
    FutureRevision = 205,
    EpochMismatch = 206,
    IncarnationMismatch = 207,
    ConfigGenerationMismatch = 208,
    TopologyGenerationMismatch = 209,
    EvidenceGenerationMismatch = 210,
    SupersededAuthority = 211,

    // 300-399: authority.
    AuthorityMissing = 300,
    AuthorityExpired = 301,
    AuthorityRevoked = 302,
    AuthorityScopeMismatch = 303,
    AuthorityInsufficient = 304,
    ServiceModeConflict = 305,

    // 400-499: evidence.
    EvidenceAbsent = 400,
    EvidenceStale = 401,
    EvidenceUnsupported = 402,
    EvidenceConflicting = 403,
    EvidenceFuture = 404,
    LeakStateIndeterminate = 405,
    LeakConfirmed = 406,
    CoolantQualityNotNominal = 407,
    FlowOutOfRange = 408,
    PressureOutOfRange = 409,
    TemperatureOutOfRange = 410,
    ConductivityOutOfRange = 411,
    PhOutOfRange = 412,

    // 500-599: interlocks, presence and service obligations.
    InterlockBlocked = 500,
    ServiceObligationActive = 501,
    DeviceNotPresent = 502,
    DeviceFaulted = 503,
    DeviceAbsent = 504,
    ServiceWindowRequired = 505,

    // 600-699: lifecycle and state transitions.
    InvalidStateTransition = 600,
    AlreadyInTargetState = 601,
    TargetUnchanged = 602,
    LoopIsolated = 603,
    LoopNotIsolated = 604,

    // 700-799: attempts and idempotency.
    UnresolvedAttemptBlocks = 700,
    AttemptNotFound = 701,
    AttemptNotOpen = 702,
    IdempotencyConflict = 703,
    AttemptLimitExceeded = 704,

    // 800-899: adapter outcomes.
    AdapterUnavailable = 800,
    AdapterRejected = 801,
    AdapterDeviceMissing = 802,
    AdapterGenerationMismatch = 803,
    AdapterBusy = 804,
    AdapterInternalError = 805,

    // 900-999: effect verification.
    EffectNotObserved = 900,
    EffectContradicted = 901,
    EffectPending = 902,

    // 1000-1099: persistence.
    StoreMissing = 1000,
    StoreCorrupt = 1001,
    StoreLocked = 1002,
    StoreIoError = 1003,
    StoreVersionUnsupported = 1004,
    StoreBoundsExceeded = 1005,
    StorePathInvalid = 1006,
    StoreJournalLagging = 1007,
    StoreNotOpen = 1008,

    // 1100-1199: runtime and resource bounds.
    ShuttingDown = 1100,
    ResourceExhausted = 1101,
    RecoveredEvidenceRequiresRefresh = 1102,
    InternalError = 1103,
};

/// Stable numeric value of a status code.
[[nodiscard]] constexpr std::uint16_t code_value(StatusCode code) noexcept {
    return static_cast<std::uint16_t>(code);
}

/// Stable, lower-case, machine-friendly textual form of a status code.
[[nodiscard]] std::string_view to_string(StatusCode code) noexcept;

/// Human-readable explanation for a status code, independent of any instance.
[[nodiscard]] std::string_view describe(StatusCode code) noexcept;

[[nodiscard]] constexpr bool is_ok(StatusCode code) noexcept { return code == StatusCode::Ok; }

/// True when the numeric value corresponds to a code defined by this build.
/// Used to reject impossible enumerators decoded from durable state.
[[nodiscard]] bool status_code_defined(StatusCode code) noexcept;

/// A status code plus a human-readable explanation.
///
/// The code is the machine contract; the message is diagnostic only and must
/// never be parsed by callers.
class Status final {
public:
    Status() noexcept = default;
    explicit Status(StatusCode code) noexcept : code_(code) {}
    Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

    [[nodiscard]] StatusCode code() const noexcept { return code_; }
    [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::Ok; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] std::string_view code_name() const noexcept { return to_string(code_); }

    /// "ok" or "<code-name>: <message>".
    [[nodiscard]] std::string to_display_string() const;

    [[nodiscard]] bool operator==(const Status& other) const noexcept { return code_ == other.code_; }
    [[nodiscard]] bool operator!=(const Status& other) const noexcept { return !(*this == other); }

private:
    StatusCode code_{StatusCode::Ok};
    std::string message_{};
};

/// Thrown by Result accessors when the result does not hold a value.
class BadResultAccess final : public std::logic_error {
public:
    explicit BadResultAccess(const Status& status);
    [[nodiscard]] const Status& status() const noexcept { return status_; }

private:
    Status status_;
};

/// Outcome of an operation: either a value or a Status.
template <typename T>
class [[nodiscard]] Result final {
public:
    Result(T value) : value_(std::move(value)) {}                     // NOLINT(google-explicit-constructor)
    Result(Status status) : status_(std::move(status)) {}             // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
    [[nodiscard]] const Status& status() const noexcept { return status_; }
    [[nodiscard]] StatusCode code() const noexcept { return status_.code(); }

    [[nodiscard]] const T& value() const& {
        require_value();
        return *value_;
    }
    [[nodiscard]] T& value() & {
        require_value();
        return *value_;
    }
    [[nodiscard]] T&& value() && {
        require_value();
        return std::move(*value_);
    }
    [[nodiscard]] const T* operator->() const { return &value(); }
    [[nodiscard]] T* operator->() { return &value(); }
    [[nodiscard]] const T& operator*() const& { return value(); }
    [[nodiscard]] T& operator*() & { return value(); }

    [[nodiscard]] T value_or(T fallback) const {
        return value_.has_value() ? *value_ : std::move(fallback);
    }

private:
    void require_value() const {
        if (!value_.has_value()) {
            throw BadResultAccess(status_);
        }
    }

    std::optional<T> value_{};
    Status status_{};
};

template <>
class [[nodiscard]] Result<void> final {
public:
    Result() = default;
    Result(Status status) : status_(std::move(status)) {}             // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
    [[nodiscard]] const Status& status() const noexcept { return status_; }
    [[nodiscard]] StatusCode code() const noexcept { return status_.code(); }

private:
    Status status_{};
};

[[nodiscard]] inline Result<void> ok_result() noexcept { return Result<void>{}; }

}  // namespace liquidcooling
