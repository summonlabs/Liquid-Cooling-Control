#pragma once

#include <cstdint>

#include "liquidcooling/status.hpp"
#include "liquidcooling/units.hpp"

namespace liquidcooling {

/// A point on a monotonic timeline, in nanoseconds.
///
/// The origin is unspecified and may differ between processes; timestamps are
/// only ever compared within one opened store.  Wall-clock time is deliberately
/// not used for authority decisions because it can move backwards.
class TimestampNs final {
public:
    constexpr TimestampNs() noexcept = default;

    [[nodiscard]] static constexpr TimestampNs from_nanos(std::int64_t value) noexcept {
        TimestampNs t;
        t.value_ = value;
        return t;
    }

    [[nodiscard]] constexpr std::int64_t nanos() const noexcept { return value_; }

    [[nodiscard]] constexpr bool operator==(const TimestampNs&) const noexcept = default;
    [[nodiscard]] constexpr auto operator<=>(const TimestampNs&) const noexcept = default;

private:
    std::int64_t value_{0};
};

/// Adds a duration to a timestamp with overflow checking.
[[nodiscard]] Result<TimestampNs> checked_add(TimestampNs base, Duration delta);

/// Subtracts two timestamps, saturating at zero when b is later than a.
[[nodiscard]] Duration elapsed(TimestampNs from, TimestampNs to);

[[nodiscard]] std::string to_display_string(TimestampNs value);

/// Source of monotonic time.  Injected so that lease and freshness behaviour is
/// deterministic under test.
class Clock {
public:
    Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    virtual ~Clock();

    [[nodiscard]] virtual TimestampNs now() const = 0;
};

/// Monotonic clock backed by the standard steady clock.
class SteadyClock final : public Clock {
public:
    [[nodiscard]] TimestampNs now() const override;
};

/// Deterministic clock advanced explicitly by the caller.
class ManualClock final : public Clock {
public:
    ManualClock() = default;
    explicit ManualClock(TimestampNs start) : current_(start) {}

    [[nodiscard]] TimestampNs now() const override { return current_; }

    void advance(Duration delta);
    void set(TimestampNs value) { current_ = value; }

private:
    TimestampNs current_{};
};

}  // namespace liquidcooling
