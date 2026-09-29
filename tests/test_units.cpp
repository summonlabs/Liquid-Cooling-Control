#include <limits>
#include <string>

#include "liquidcooling/liquidcooling.hpp"
#include "testing.hpp"

using namespace liquidcooling;

namespace {

constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kI64Min = std::numeric_limits<std::int64_t>::min();

}  // namespace

TEST(units, checked_add_boundaries) {
    CHECK(checked_add(FlowRate::from_value(1), FlowRate::from_value(2)).value() == FlowRate::from_value(3));
    CHECK(checked_add(FlowRate::from_value(kI64Max), FlowRate::from_value(1)).code() ==
          StatusCode::ValueOutOfRange);
    CHECK(checked_add(FlowRate::from_value(kI64Min), FlowRate::from_value(-1)).code() ==
          StatusCode::ValueOutOfRange);
    CHECK(checked_add(FlowRate::from_value(kI64Max), FlowRate::from_value(0)).ok());
    CHECK(checked_add(FlowRate::from_value(kI64Min), FlowRate::from_value(0)).ok());
    CHECK(checked_add(FlowRate::from_value(kI64Max - 1), FlowRate::from_value(1)).value().value() == kI64Max);
    CHECK(checked_add(FlowRate::from_value(kI64Min + 1), FlowRate::from_value(-1)).value().value() == kI64Min);
}

TEST(units, checked_sub_boundaries) {
    CHECK(checked_sub(FlowRate::from_value(5), FlowRate::from_value(9)).value() == FlowRate::from_value(-4));
    CHECK(checked_sub(FlowRate::from_value(kI64Min), FlowRate::from_value(1)).code() ==
          StatusCode::ValueOutOfRange);
    CHECK(checked_sub(FlowRate::from_value(kI64Max), FlowRate::from_value(-1)).code() ==
          StatusCode::ValueOutOfRange);
    CHECK(checked_sub(FlowRate::from_value(kI64Min), FlowRate::from_value(kI64Min)).value().value() == 0);
}

TEST(units, checked_scale_boundaries) {
    CHECK(checked_scale(FlowRate::from_value(7), 6).value() == FlowRate::from_value(42));
    CHECK(checked_scale(FlowRate::from_value(kI64Max), 2).code() == StatusCode::ValueOutOfRange);
    CHECK(checked_scale(FlowRate::from_value(kI64Min), -1).code() == StatusCode::ValueOutOfRange);
    CHECK(checked_scale(FlowRate::from_value(kI64Min), 1).ok());
    CHECK(checked_scale(FlowRate::from_value(kI64Min + 1), -1).value().value() == kI64Max);
    CHECK(checked_scale(FlowRate::from_value(1), 0).value().value() == 0);
    // Narrowing into a 32-bit representation must be checked, not truncated.
    CHECK(checked_scale(Temperature::from_value(1000), 10).value().value() == 10'000);
    CHECK(checked_scale(Temperature::from_value(2'000'000'000), 2).code() == StatusCode::ValueOutOfRange);
    // Unsigned representations must reject negative results.
    CHECK(checked_scale(Acidity::from_value(10), -1).code() == StatusCode::ValueOutOfRange);
}

TEST(units, absolute_difference) {
    CHECK(checked_abs_diff(FlowRate::from_value(3), FlowRate::from_value(10)).value().value() == 7);
    CHECK(checked_abs_diff(FlowRate::from_value(10), FlowRate::from_value(3)).value().value() == 7);
    // The true difference between the two extremes is 2^64-2 and does not fit;
    // it must be reported rather than wrapped.
    CHECK(checked_abs_diff(FlowRate::from_value(kI64Min + 1), FlowRate::from_value(kI64Max)).code() ==
          StatusCode::ValueOutOfRange);
    CHECK(checked_abs_diff(FlowRate::from_value(kI64Max), FlowRate::from_value(kI64Min)).code() ==
          StatusCode::ValueOutOfRange);
    CHECK_EQ(checked_abs_diff(FlowRate::from_value(kI64Min + 1), FlowRate::from_value(0)).value().value(), kI64Max);
}

TEST(units, physical_domain_validation) {
    CHECK(validate_domain(limits::flow_min).ok());
    CHECK(validate_domain(limits::flow_max).ok());
    CHECK(validate_domain(FlowRate::from_value(-1)).code() == StatusCode::ValueOutOfRange);
    CHECK(validate_domain(FlowRate::from_value(limits::flow_max.value() + 1)).code() ==
          StatusCode::ValueOutOfRange);
    CHECK(validate_domain(limits::pressure_min).ok());
    CHECK(validate_domain(limits::pressure_max).ok());
    CHECK(validate_domain(Pressure::from_value(limits::pressure_max.value() + 1)).code() ==
          StatusCode::ValueOutOfRange);
    CHECK(validate_domain(limits::temperature_min).ok());
    CHECK(validate_domain(Temperature::from_value(limits::temperature_max.value() + 1)).code() ==
          StatusCode::ValueOutOfRange);
    CHECK(validate_domain(Acidity::from_value(14'000)).ok());
    CHECK(validate_domain(Acidity::from_value(14'001)).code() == StatusCode::ValueOutOfRange);
    CHECK(validate_domain(Duration::from_value(0)).ok());
    CHECK(validate_domain(Duration::from_value(-1)).code() == StatusCode::ValueOutOfRange);
}

TEST(units, quantity_types_are_not_interchangeable) {
    // Compilation of this case is the assertion: FlowRate and Pressure are
    // distinct types and cannot be assigned to one another.
    const FlowRate flow = FlowRate::from_value(1000);
    const Pressure pressure = Pressure::from_value(1000);
    CHECK(flow.value() == pressure.value());
    CHECK(flow == FlowRate::from_value(1000));
    CHECK(flow < FlowRate::from_value(1001));
}

TEST(units, display_rendering) {
    CHECK_EQ(to_display_string(FlowRate::from_value(150'000)), std::string("150.000 L/min"));
    CHECK_EQ(to_display_string(FlowRate::from_value(-1'500)), std::string("-1.500 L/min"));
    CHECK_EQ(to_display_string(Pressure::from_value(350'000)), std::string("350.000 kPa"));
    CHECK_EQ(to_display_string(Temperature::from_value(18'000)), std::string("18.000 C"));
    CHECK_EQ(to_display_string(Acidity::from_value(7'400)), std::string("7.400 pH"));
}

TEST(units, timestamp_and_duration_arithmetic) {
    const TimestampNs base = TimestampNs::from_nanos(1'000);
    const auto later = checked_add(base, Duration::from_value(500));
    REQUIRE(later.ok());
    CHECK_EQ(later.value().nanos(), 1500);
    CHECK_EQ(elapsed(base, later.value()).value(), 500);
    CHECK_EQ(elapsed(later.value(), base).value(), 0);
    CHECK(checked_add(TimestampNs::from_nanos(kI64Max), Duration::from_value(1)).code() ==
          StatusCode::ValueOutOfRange);

    ManualClock clock(TimestampNs::from_nanos(10));
    CHECK_EQ(clock.now().nanos(), 10);
    clock.advance(Duration::from_value(5));
    CHECK_EQ(clock.now().nanos(), 15);
    clock.set(TimestampNs::from_nanos(0));
    CHECK_EQ(clock.now().nanos(), 0);
}

TEST(units, scalar_identity_semantics) {
    const LoopId unset;
    CHECK(!unset.valid());
    CHECK_EQ(unset.value(), 0u);
    const LoopId first = LoopId::from_value(1);
    CHECK(first.valid());
    CHECK_EQ(first.next().value(), 2u);
    CHECK(LoopId::invalid() == unset);
    CHECK(first != unset);
    // Distinct scalar types cannot be compared or assigned: this line would not
    // compile if the tag were dropped.
    const DeviceGeneration generation = DeviceGeneration::from_value(3);
    CHECK_EQ(generation.value(), 3u);
}

TEST(units, idempotency_key_validation) {
    CHECK(IdempotencyKey::create("abc-123_XYZ").ok());
    CHECK(IdempotencyKey::create("").code() == StatusCode::MissingRequiredField);
    CHECK(IdempotencyKey::create(std::string(kMaxIdempotencyKeyLength + 1, 'a')).code() ==
          StatusCode::StringTooLong);
    CHECK(IdempotencyKey::create("with space").code() == StatusCode::InvalidEncoding);
    CHECK(IdempotencyKey::create("with\ttab").code() == StatusCode::InvalidEncoding);
    CHECK(IdempotencyKey::create("with,comma").code() == StatusCode::InvalidEncoding);
    CHECK(IdempotencyKey::create(std::string("a\0b", 3)).code() == StatusCode::InvalidEncoding);
    const auto exact = IdempotencyKey::create(std::string(kMaxIdempotencyKeyLength, 'a'));
    CHECK(exact.ok());
    CHECK_EQ(exact.value().str().size(), kMaxIdempotencyKeyLength);
}

TEST(units, name_and_reason_validation) {
    CHECK(validate_name("loop-a_1", "loop").ok());
    CHECK(validate_name("", "loop").code() == StatusCode::MissingRequiredField);
    CHECK(validate_name("bad name", "loop").code() == StatusCode::InvalidEncoding);
    CHECK(validate_name(std::string(kMaxNameLength + 1, 'a'), "loop").code() == StatusCode::StringTooLong);
    CHECK(validate_reason("servicing pump A", "reason").ok());
    CHECK(validate_reason("line\nbreak", "reason").code() == StatusCode::InvalidEncoding);
    CHECK(validate_reason(std::string(kMaxReasonLength + 1, 'a'), "reason").code() == StatusCode::StringTooLong);
}

TEST(units, fingerprint_is_stable_and_sensitive) {
    TransitionRequest request;
    request.action = ActionKind::StartPump;
    request.loop = LoopId::from_value(1);
    request.target = DeviceId::from_value(2);
    request.expected_device_generation = DeviceGeneration::from_value(3);
    request.expected_revision = StateRevision::from_value(4);

    const RequestFingerprint first = fingerprint_request(request);
    const RequestFingerprint second = fingerprint_request(request);
    CHECK(first == second);
    CHECK_EQ(first.to_hex().size(), 16u + 16u);

    TransitionRequest other = request;
    other.expected_revision = StateRevision::from_value(5);
    CHECK(!(fingerprint_request(other) == first));

    // The authority token, the idempotency key and the reason are not part of
    // the semantic intent, so they must not change the fingerprint.
    TransitionRequest decorated = request;
    decorated.idempotency_key = IdempotencyKey::create("different-key").value();
    decorated.reason = "a completely different reason";
    decorated.authority.id = AuthorityTokenId::from_value(99);
    decorated.authority.nonce = 12345;
    CHECK(fingerprint_request(decorated) == first);

    // A trailing zero byte must not collide with a shorter encoding.
    const std::uint8_t a[] = {1, 2, 3};
    const std::uint8_t b[] = {1, 2, 3, 0};
    CHECK(!(fingerprint_bytes(a, 3) == fingerprint_bytes(b, 4)));
}

TEST(units, status_code_table_is_complete_and_ordered) {
    const StatusCode samples[] = {StatusCode::Ok,
                                  StatusCode::InvalidArgument,
                                  StatusCode::StaleGeneration,
                                  StatusCode::AuthorityInsufficient,
                                  StatusCode::LeakStateIndeterminate,
                                  StatusCode::UnresolvedAttemptBlocks,
                                  StatusCode::StoreCorrupt,
                                  StatusCode::InternalError};
    for (const StatusCode code : samples) {
        CHECK(status_code_defined(code));
        CHECK(!to_string(code).empty());
        CHECK(!describe(code).empty());
        CHECK(to_string(code) != "unknown-status");
        CHECK(describe(code) != "unrecognised status code");
    }
    CHECK(!status_code_defined(static_cast<StatusCode>(9999)));
    CHECK(!status_code_defined(static_cast<StatusCode>(99)));
    CHECK_EQ(code_value(StatusCode::Ok), 0u);
    CHECK_EQ(code_value(StatusCode::StoreCorrupt), 1001u);
    CHECK(status_code_defined(static_cast<StatusCode>(1103)));

    Status status{StatusCode::LeakConfirmed, "leak on loop 1"};
    CHECK_EQ(status.to_display_string(), std::string("leak-confirmed: leak on loop 1"));
    CHECK_EQ(Status{}.to_display_string(), std::string("ok"));
    CHECK(Status{}.ok());
}

TEST(units, result_access_is_checked) {
    const Result<int> good{7};
    CHECK(good.ok());
    CHECK_EQ(good.value(), 7);
    CHECK_EQ(good.value_or(0), 7);

    const Result<int> bad{Status{StatusCode::InvalidArgument, "nope"}};
    CHECK(!bad.ok());
    CHECK_EQ(bad.code(), StatusCode::InvalidArgument);
    CHECK_EQ(bad.value_or(42), 42);
    bool threw = false;
    try {
        (void)bad.value();
    } catch (const BadResultAccess& error) {
        threw = true;
        CHECK(error.status().code() == StatusCode::InvalidArgument);
    }
    CHECK(threw);

    const Result<void> void_ok = ok_result();
    CHECK(void_ok.ok());
    const Result<void> void_bad{Status{StatusCode::ShuttingDown, "closing"}};
    CHECK(!void_bad.ok());
}
