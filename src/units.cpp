#include "liquidcooling/units.hpp"

namespace liquidcooling {
namespace {

[[nodiscard]] std::string scaled_display(std::int64_t value, std::int64_t scale, const char* unit) {
    const bool negative = value < 0;
    const std::uint64_t magnitude = negative ? (~static_cast<std::uint64_t>(value) + 1u)
                                             : static_cast<std::uint64_t>(value);
    const auto divisor = static_cast<std::uint64_t>(scale);
    std::string out;
    if (negative) {
        out.push_back('-');
    }
    out.append(std::to_string(magnitude / divisor));
    out.push_back('.');
    const std::uint64_t fraction = magnitude % divisor;
    std::string digits = std::to_string(fraction);
    for (std::size_t pad = digits.size(); pad < 3; ++pad) {
        out.push_back('0');
    }
    out.append(digits);
    out.push_back(' ');
    out.append(unit);
    return out;
}

}  // namespace

Result<void> validate_domain(FlowRate value) {
    if (value < limits::flow_min || limits::flow_max < value) {
        return Status{StatusCode::ValueOutOfRange, "flow rate outside the physical domain"};
    }
    return ok_result();
}

Result<void> validate_domain(Pressure value) {
    if (value < limits::pressure_min || limits::pressure_max < value) {
        return Status{StatusCode::ValueOutOfRange, "pressure outside the physical domain"};
    }
    return ok_result();
}

Result<void> validate_domain(Temperature value) {
    if (value < limits::temperature_min || limits::temperature_max < value) {
        return Status{StatusCode::ValueOutOfRange, "temperature outside the physical domain"};
    }
    return ok_result();
}

Result<void> validate_domain(Conductivity value) {
    if (value < limits::conductivity_min || limits::conductivity_max < value) {
        return Status{StatusCode::ValueOutOfRange, "conductivity outside the physical domain"};
    }
    return ok_result();
}

Result<void> validate_domain(Acidity value) {
    if (value < limits::acidity_min || limits::acidity_max < value) {
        return Status{StatusCode::ValueOutOfRange, "acidity outside the physical domain"};
    }
    return ok_result();
}

Result<void> validate_domain(Duration value) {
    if (value < limits::duration_min || limits::duration_max < value) {
        return Status{StatusCode::ValueOutOfRange, "duration outside the permitted domain"};
    }
    return ok_result();
}

std::string to_display_string(FlowRate value) { return scaled_display(value.value(), 1000, "L/min"); }
std::string to_display_string(Pressure value) { return scaled_display(value.value(), 1000, "kPa"); }
std::string to_display_string(Temperature value) {
    return scaled_display(static_cast<std::int64_t>(value.value()), 1000, "C");
}
std::string to_display_string(Conductivity value) { return scaled_display(value.value(), 1000, "mS/cm"); }
std::string to_display_string(Acidity value) {
    return scaled_display(static_cast<std::int64_t>(value.value()), 1000, "pH");
}
std::string to_display_string(Duration value) { return scaled_display(value.value(), 1000000, "ms"); }

}  // namespace liquidcooling
