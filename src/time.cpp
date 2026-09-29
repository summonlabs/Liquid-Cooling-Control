#include "liquidcooling/time.hpp"

#include <chrono>

namespace liquidcooling {

Result<TimestampNs> checked_add(TimestampNs base, Duration delta) {
    std::int64_t out = 0;
    if (detail::add_overflow_i64(base.nanos(), delta.value(), out)) {
        return Status{StatusCode::ValueOutOfRange, "timestamp arithmetic overflowed"};
    }
    return TimestampNs::from_nanos(out);
}

Duration elapsed(TimestampNs from, TimestampNs to) {
    if (to <= from) {
        return Duration::from_value(0);
    }
    std::int64_t out = 0;
    if (detail::sub_overflow_i64(to.nanos(), from.nanos(), out)) {
        return Duration::from_value(std::numeric_limits<std::int64_t>::max());
    }
    return Duration::from_value(out);
}

std::string to_display_string(TimestampNs value) { return std::to_string(value.nanos()) + " ns"; }

Clock::~Clock() = default;

TimestampNs SteadyClock::now() const {
    const auto point = std::chrono::steady_clock::now().time_since_epoch();
    return TimestampNs::from_nanos(std::chrono::duration_cast<std::chrono::nanoseconds>(point).count());
}

void ManualClock::advance(Duration delta) {
    const auto result = checked_add(current_, delta);
    if (result.ok()) {
        current_ = result.value();
    } else {
        current_ = TimestampNs::from_nanos(std::numeric_limits<std::int64_t>::max());
    }
}

}  // namespace liquidcooling
