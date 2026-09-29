#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <string>

#include "liquidcooling/status.hpp"

namespace liquidcooling {

namespace detail {

[[nodiscard]] constexpr bool add_overflow_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
        return true;
    }
    if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
        return true;
    }
    out = a + b;
    return false;
}

[[nodiscard]] constexpr bool sub_overflow_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if (b > 0 && a < std::numeric_limits<std::int64_t>::min() + b) {
        return true;
    }
    if (b < 0 && a > std::numeric_limits<std::int64_t>::max() + b) {
        return true;
    }
    out = a - b;
    return false;
}

[[nodiscard]] constexpr bool mul_overflow_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if (a == 0 || b == 0) {
        out = 0;
        return false;
    }
    if (a == -1) {
        if (b == std::numeric_limits<std::int64_t>::min()) {
            return true;
        }
        out = -b;
        return false;
    }
    if (b == -1) {
        if (a == std::numeric_limits<std::int64_t>::min()) {
            return true;
        }
        out = -a;
        return false;
    }
    if (a > 0) {
        if (b > 0) {
            if (a > std::numeric_limits<std::int64_t>::max() / b) {
                return true;
            }
        } else {
            if (a > std::numeric_limits<std::int64_t>::min() / b) {
                return true;
            }
        }
    } else {
        if (b > 0) {
            if (a < std::numeric_limits<std::int64_t>::min() / b) {
                return true;
            }
        } else {
            if (a < std::numeric_limits<std::int64_t>::max() / b) {
                return true;
            }
        }
    }
    out = a * b;
    return false;
}

}  // namespace detail

/// A physical quantity: an exact integer in a named unit.
///
/// The tag type makes quantities in different units distinct C++ types, so a
/// flow rate can never be substituted for a pressure.  Arithmetic is only
/// available through checked helpers; the raw operators are deliberately not
/// provided because silent wraparound is unacceptable at an authority boundary.
template <typename Tag, typename Rep>
class Quantity final {
public:
    using rep = Rep;
    using tag = Tag;

    constexpr Quantity() noexcept = default;

    /// Constructs a quantity from a raw integer in the tag's unit.
    [[nodiscard]] static constexpr Quantity from_value(Rep value) noexcept {
        Quantity q;
        q.value_ = value;
        return q;
    }

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }

    [[nodiscard]] constexpr bool operator==(const Quantity&) const noexcept = default;
    [[nodiscard]] constexpr auto operator<=>(const Quantity&) const noexcept = default;

private:
    Rep value_{};
};

struct MillilitresPerMinuteTag;
struct PascalTag;
struct MilliCelsiusTag;
struct MicroSiemensPerCentimetreTag;
struct MilliPhTag;
struct NanosecondTag;

/// Volumetric coolant flow, in millilitres per minute.
using FlowRate = Quantity<MillilitresPerMinuteTag, std::int64_t>;
/// Gauge pressure, in pascals.
using Pressure = Quantity<PascalTag, std::int64_t>;
/// Temperature, in milli-degrees Celsius.
using Temperature = Quantity<MilliCelsiusTag, std::int32_t>;
/// Electrolytic conductivity, in microsiemens per centimetre.
using Conductivity = Quantity<MicroSiemensPerCentimetreTag, std::int64_t>;
/// Acidity, in milli-pH units (pH 7.4 is 7400).
using Acidity = Quantity<MilliPhTag, std::uint32_t>;
/// A duration, in nanoseconds.
using Duration = Quantity<NanosecondTag, std::int64_t>;

namespace limits {

// Physical domains.  Values outside these domains are structurally invalid and
// are rejected before any policy comparison runs.
inline constexpr FlowRate flow_min = FlowRate::from_value(0);
inline constexpr FlowRate flow_max = FlowRate::from_value(10'000'000);          // 10 000 L/min
inline constexpr Pressure pressure_min = Pressure::from_value(-100'000);        // -1 bar gauge
inline constexpr Pressure pressure_max = Pressure::from_value(5'000'000);       // 50 bar gauge
inline constexpr Temperature temperature_min = Temperature::from_value(-50'000);  // -50 C
inline constexpr Temperature temperature_max = Temperature::from_value(200'000);  // 200 C
inline constexpr Conductivity conductivity_min = Conductivity::from_value(0);
inline constexpr Conductivity conductivity_max = Conductivity::from_value(1'000'000);
inline constexpr Acidity acidity_min = Acidity::from_value(0);
inline constexpr Acidity acidity_max = Acidity::from_value(14'000);
inline constexpr Duration duration_min = Duration::from_value(0);
inline constexpr Duration duration_max = Duration::from_value(86'400'000'000'000);  // 24 h

}  // namespace limits

/// A closed interval over one quantity type.
template <typename Q>
struct QuantityRange final {
    Q minimum{};
    Q maximum{};

    [[nodiscard]] constexpr bool contains(Q value) const noexcept {
        return !(value < minimum) && !(maximum < value);
    }
    [[nodiscard]] constexpr bool valid() const noexcept { return !(maximum < minimum); }
};

template <typename Tag, typename Rep>
[[nodiscard]] Result<Quantity<Tag, Rep>> checked_add(Quantity<Tag, Rep> a, Quantity<Tag, Rep> b) {
    Rep out{};
    if (detail::add_overflow_i64(static_cast<std::int64_t>(a.value()), static_cast<std::int64_t>(b.value()), out)) {
        return Status{StatusCode::ValueOutOfRange, "quantity addition overflowed"};
    }
    return Quantity<Tag, Rep>::from_value(static_cast<Rep>(out));
}

template <typename Tag, typename Rep>
[[nodiscard]] Result<Quantity<Tag, Rep>> checked_sub(Quantity<Tag, Rep> a, Quantity<Tag, Rep> b) {
    Rep out{};
    if (detail::sub_overflow_i64(static_cast<std::int64_t>(a.value()), static_cast<std::int64_t>(b.value()), out)) {
        return Status{StatusCode::ValueOutOfRange, "quantity subtraction overflowed"};
    }
    return Quantity<Tag, Rep>::from_value(static_cast<Rep>(out));
}

template <typename Tag, typename Rep>
[[nodiscard]] Result<Quantity<Tag, Rep>> checked_scale(Quantity<Tag, Rep> a, std::int64_t factor) {
    std::int64_t out = 0;
    if (detail::mul_overflow_i64(static_cast<std::int64_t>(a.value()), factor, out)) {
        return Status{StatusCode::ValueOutOfRange, "quantity scaling overflowed"};
    }
    if constexpr (std::numeric_limits<Rep>::is_signed) {
        if (out < static_cast<std::int64_t>(std::numeric_limits<Rep>::min()) ||
            out > static_cast<std::int64_t>(std::numeric_limits<Rep>::max())) {
            return Status{StatusCode::ValueOutOfRange, "scaled quantity does not fit its representation"};
        }
    } else {
        if (out < 0 || static_cast<std::uint64_t>(out) > static_cast<std::uint64_t>(std::numeric_limits<Rep>::max())) {
            return Status{StatusCode::ValueOutOfRange, "scaled quantity does not fit its representation"};
        }
    }
    return Quantity<Tag, Rep>::from_value(static_cast<Rep>(out));
}

/// Absolute difference between two quantities, computed without overflow.
template <typename Tag, typename Rep>
[[nodiscard]] Result<Quantity<Tag, Rep>> checked_abs_diff(Quantity<Tag, Rep> a, Quantity<Tag, Rep> b) {
    if (b < a) {
        return checked_sub(a, b);
    }
    return checked_sub(b, a);
}

[[nodiscard]] Result<void> validate_domain(FlowRate value);
[[nodiscard]] Result<void> validate_domain(Pressure value);
[[nodiscard]] Result<void> validate_domain(Temperature value);
[[nodiscard]] Result<void> validate_domain(Conductivity value);
[[nodiscard]] Result<void> validate_domain(Acidity value);
[[nodiscard]] Result<void> validate_domain(Duration value);

/// Renders a quantity with its unit suffix, for diagnostics only.
[[nodiscard]] std::string to_display_string(FlowRate value);
[[nodiscard]] std::string to_display_string(Pressure value);
[[nodiscard]] std::string to_display_string(Temperature value);
[[nodiscard]] std::string to_display_string(Conductivity value);
[[nodiscard]] std::string to_display_string(Acidity value);
[[nodiscard]] std::string to_display_string(Duration value);

}  // namespace liquidcooling
