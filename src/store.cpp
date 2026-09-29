#include "liquidcooling/store.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

#include "codec.hpp"
#include "crc32c.hpp"
#include "platform.hpp"

namespace liquidcooling {
namespace {

namespace det = detail;

enum class RecordType : std::uint16_t {
    StoreHeader = 1,
    JournalHeader = 2,
    Loop = 10,
    LoopEvidence = 11,
    Attempt = 12,
    Idempotency = 13,
    Obligation = 14,
    ServiceWindow = 15,
    RevokedToken = 16,
    JournalEntry = 17,
};

constexpr std::size_t kChannelFlagsBytes = 1;

template <typename T, typename WriteValue>
void write_evidence(det::Writer& writer, const Evidence<T>& evidence, WriteValue write_value) {
    writer.boolean(evidence.present);
    writer.boolean(evidence.unsupported);
    writer.u8(static_cast<std::uint8_t>(evidence.origin));
    writer.u8(0);  // reserved
    write_value(writer, evidence.value);
    writer.u64(evidence.sequence.value());
    writer.u32(evidence.generation.value());
    writer.i64(evidence.observed_at.nanos());
    writer.text(evidence.source);
}

template <typename T, typename ReadValue>
void read_evidence(det::Reader& reader, Evidence<T>& evidence, ReadValue read_value) {
    evidence.present = reader.boolean();
    evidence.unsupported = reader.boolean();
    const std::uint8_t origin_raw = reader.u8();
    reader.reserved(kChannelFlagsBytes, "evidence flags");
    if (reader.failed()) {
        return;
    }
    if (evidence.present && evidence.unsupported) {
        reader.fail(StatusCode::StoreCorrupt, "evidence channel is both present and unsupported");
        return;
    }
    const auto origin = parse_evidence_origin(origin_raw);
    if (!origin.ok()) {
        reader.fail(StatusCode::StoreCorrupt, "evidence channel has an undefined origin");
        return;
    }
    evidence.origin = origin.value();
    read_value(reader, evidence.value);
    evidence.sequence = ObservationSequence::from_value(reader.u64());
    evidence.generation = DeviceGeneration::from_value(reader.u32());
    evidence.observed_at = TimestampNs::from_nanos(reader.i64());
    evidence.source = reader.text(kStoreMaxTextBytes);
}

void append_record(det::Writer& out, RecordType type, const std::vector<std::uint8_t>& body) {
    out.u16(static_cast<std::uint16_t>(type));
    out.u16(0);
    out.u32(static_cast<std::uint32_t>(body.size()));
    out.u32(det::crc32c(body.data(), body.size()));
    out.u32(0);
    out.raw(body.data(), body.size());
}

void append_record(det::Writer& out, RecordType type) {
    static const std::vector<std::uint8_t> kEmpty;
    append_record(out, type, kEmpty);
}

struct RawRecord final {
    RecordType type{RecordType::StoreHeader};
    std::span<const std::uint8_t> body{};
};

/// Reads the next record header, validating framing and the body checksum.
[[nodiscard]] bool next_record(det::Reader& reader, const StoreBounds& bounds, RawRecord& out) {
    if (reader.at_end()) {
        return false;
    }
    if (reader.remaining() < kStoreRecordHeaderBytes) {
        reader.fail(StatusCode::StoreCorrupt, "trailing bytes do not form a complete record header");
        return false;
    }
    const std::uint16_t type_raw = reader.u16();
    const std::uint16_t flags = reader.u16();
    const std::uint32_t length = reader.u32();
    const std::uint32_t declared_crc = reader.u32();
    const std::uint32_t reserved = reader.u32();
    if (reader.failed()) {
        return false;
    }
    if (flags != 0) {
        reader.fail(StatusCode::StoreCorrupt, "record flags must be zero");
        return false;
    }
    if (reserved != 0) {
        reader.fail(StatusCode::StoreCorrupt, "record reserved field must be zero");
        return false;
    }
    if (length > bounds.max_record_body_bytes) {
        reader.fail(StatusCode::StoreBoundsExceeded, "record body exceeds the configured maximum");
        return false;
    }
    const auto body = reader.raw(length);
    if (reader.failed()) {
        return false;
    }
    if (det::crc32c(body.data(), body.size()) != declared_crc) {
        reader.fail(StatusCode::StoreCorrupt, "record body failed its integrity check");
        return false;
    }
    switch (type_raw) {
        case 1:
            out.type = RecordType::StoreHeader;
            break;
        case 2:
            out.type = RecordType::JournalHeader;
            break;
        case 10:
            out.type = RecordType::Loop;
            break;
        case 11:
            out.type = RecordType::LoopEvidence;
            break;
        case 12:
            out.type = RecordType::Attempt;
            break;
        case 13:
            out.type = RecordType::Idempotency;
            break;
        case 14:
            out.type = RecordType::Obligation;
            break;
        case 15:
            out.type = RecordType::ServiceWindow;
            break;
        case 16:
            out.type = RecordType::RevokedToken;
            break;
        case 17:
            out.type = RecordType::JournalEntry;
            break;
        default:
            reader.fail(StatusCode::StoreCorrupt, "record type is not defined for this format version");
            return false;
    }
    out.body = body;
    return true;
}

[[nodiscard]] Result<void> check_unique(const std::vector<std::uint64_t>& values, std::string_view what) {
    std::vector<std::uint64_t> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i] == sorted[i - 1]) {
            return Status{StatusCode::StoreCorrupt, std::string("duplicate ") + std::string(what) + " in store file"};
        }
    }
    return ok_result();
}

void write_device(det::Writer& writer, const DeviceRecord& device) {
    writer.u64(device.id.value());
    writer.u8(static_cast<std::uint8_t>(device.kind));
    writer.u32(device.generation.value());
    writer.u8(static_cast<std::uint8_t>(device.lifecycle));
    writer.u8(static_cast<std::uint8_t>(device.valve_position));
    writer.u8(static_cast<std::uint8_t>(device.pump_state));
    writer.u32(device.state_generation.value());
    writer.u64(device.duty_minutes);
}

[[nodiscard]] DeviceRecord read_device(det::Reader& reader) {
    DeviceRecord device;
    device.id = DeviceId::from_value(reader.u64());
    device.kind = det::read_enum<DeviceKind>(reader, kDeviceKindNames, "device kind");
    device.generation = DeviceGeneration::from_value(reader.u32());
    device.lifecycle = det::read_enum<LifecycleState>(reader, kLifecycleStateNames, "lifecycle state");
    device.valve_position = det::read_enum<ValvePosition>(reader, kValvePositionNames, "valve position");
    device.pump_state = det::read_enum<PumpState>(reader, kPumpStateNames, "pump state");
    device.state_generation = DeviceGeneration::from_value(reader.u32());
    device.duty_minutes = reader.u64();
    return device;
}

void write_reading(det::Writer& writer, const LoopReading& reading) {
    writer.u64(reading.loop.value());
    writer.u64(reading.sequence.value());
    writer.u64(reading.evidence_generation.value());
    writer.u64(reading.switchover_generation.value());
    writer.i64(reading.observed_at.nanos());
    write_evidence<bool>(writer, reading.device_present, [](det::Writer& w, bool v) { w.boolean(v); });
    write_evidence<DeviceGeneration>(writer, reading.device_generation,
                                     [](det::Writer& w, DeviceGeneration v) { w.u32(v.value()); });
    write_evidence<ValvePosition>(writer, reading.valve_position,
                                  [](det::Writer& w, ValvePosition v) { w.u8(static_cast<std::uint8_t>(v)); });
    write_evidence<PumpState>(writer, reading.pump_state,
                              [](det::Writer& w, PumpState v) { w.u8(static_cast<std::uint8_t>(v)); });
    write_evidence<FlowRate>(writer, reading.flow, [](det::Writer& w, FlowRate v) { w.i64(v.value()); });
    write_evidence<Pressure>(writer, reading.supply_pressure,
                             [](det::Writer& w, Pressure v) { w.i64(v.value()); });
    write_evidence<Pressure>(writer, reading.return_pressure,
                             [](det::Writer& w, Pressure v) { w.i64(v.value()); });
    write_evidence<Temperature>(writer, reading.supply_temperature,
                                [](det::Writer& w, Temperature v) { w.i32(v.value()); });
    write_evidence<Temperature>(writer, reading.return_temperature,
                                [](det::Writer& w, Temperature v) { w.i32(v.value()); });
    write_evidence<Conductivity>(writer, reading.conductivity,
                                 [](det::Writer& w, Conductivity v) { w.i64(v.value()); });
    write_evidence<Acidity>(writer, reading.acidity, [](det::Writer& w, Acidity v) { w.u32(v.value()); });
    write_evidence<LeakState>(writer, reading.leak,
                              [](det::Writer& w, LeakState v) { w.u8(static_cast<std::uint8_t>(v)); });
    write_evidence<CoolantQuality>(writer, reading.coolant_quality,
                                   [](det::Writer& w, CoolantQuality v) { w.u8(static_cast<std::uint8_t>(v)); });
    write_evidence<std::uint64_t>(writer, reading.duty_minutes,
                                  [](det::Writer& w, std::uint64_t v) { w.u64(v); });
}

[[nodiscard]] LoopReading read_reading(det::Reader& reader) {
    LoopReading reading;
    reading.loop = LoopId::from_value(reader.u64());
    reading.sequence = ObservationSequence::from_value(reader.u64());
    reading.evidence_generation = EvidenceGeneration::from_value(reader.u64());
    reading.switchover_generation = SwitchoverGeneration::from_value(reader.u64());
    reading.observed_at = TimestampNs::from_nanos(reader.i64());
    read_evidence<bool>(reader, reading.device_present, [](det::Reader& r, bool& v) { v = r.boolean(); });
    read_evidence<DeviceGeneration>(reader, reading.device_generation,
                                    [](det::Reader& r, DeviceGeneration& v) {
                                        v = DeviceGeneration::from_value(r.u32());
                                    });
    read_evidence<ValvePosition>(reader, reading.valve_position, [](det::Reader& r, ValvePosition& v) {
        v = det::read_enum<ValvePosition>(r, kValvePositionNames, "valve position");
    });
    read_evidence<PumpState>(reader, reading.pump_state, [](det::Reader& r, PumpState& v) {
        v = det::read_enum<PumpState>(r, kPumpStateNames, "pump state");
    });
    read_evidence<FlowRate>(reader, reading.flow,
                            [](det::Reader& r, FlowRate& v) { v = FlowRate::from_value(r.i64()); });
    read_evidence<Pressure>(reader, reading.supply_pressure,
                            [](det::Reader& r, Pressure& v) { v = Pressure::from_value(r.i64()); });
    read_evidence<Pressure>(reader, reading.return_pressure,
                            [](det::Reader& r, Pressure& v) { v = Pressure::from_value(r.i64()); });
    read_evidence<Temperature>(reader, reading.supply_temperature,
                               [](det::Reader& r, Temperature& v) { v = Temperature::from_value(r.i32()); });
    read_evidence<Temperature>(reader, reading.return_temperature,
                               [](det::Reader& r, Temperature& v) { v = Temperature::from_value(r.i32()); });
    read_evidence<Conductivity>(reader, reading.conductivity,
                                [](det::Reader& r, Conductivity& v) { v = Conductivity::from_value(r.i64()); });
    read_evidence<Acidity>(reader, reading.acidity,
                           [](det::Reader& r, Acidity& v) { v = Acidity::from_value(r.u32()); });
    read_evidence<LeakState>(reader, reading.leak, [](det::Reader& r, LeakState& v) {
        v = det::read_enum<LeakState>(r, kLeakStateNames, "leak state");
    });
    read_evidence<CoolantQuality>(reader, reading.coolant_quality, [](det::Reader& r, CoolantQuality& v) {
        v = det::read_enum<CoolantQuality>(r, kCoolantQualityNames, "coolant quality");
    });
    read_evidence<std::uint64_t>(reader, reading.duty_minutes,
                                 [](det::Reader& r, std::uint64_t& v) { v = r.u64(); });
    return reading;
}

}  // namespace

std::uint32_t store_integrity_crc32c(const void* data, std::size_t size) noexcept {
    return det::crc32c(data, size);
}

Result<void> mark_evidence_recovered(DurableState& state) {
    for (auto& record : state.evidence) {
        LoopReading& reading = record.reading;
        auto demote = [](auto& channel) {
            if (channel.origin == EvidenceOrigin::Adapter) {
                channel.origin = EvidenceOrigin::RecoveredFromStore;
            }
        };
        demote(reading.device_present);
        demote(reading.device_generation);
        demote(reading.valve_position);
        demote(reading.pump_state);
        demote(reading.flow);
        demote(reading.supply_pressure);
        demote(reading.return_pressure);
        demote(reading.supply_temperature);
        demote(reading.return_temperature);
        demote(reading.conductivity);
        demote(reading.acidity);
        demote(reading.leak);
        demote(reading.coolant_quality);
        demote(reading.duty_minutes);
    }
    return ok_result();
}

Result<std::vector<std::uint8_t>> encode_snapshot_payload(const DurableState& state, const StoreBounds& bounds) {
    if (state.loops.size() > bounds.max_loops) {
        return Status{StatusCode::StoreBoundsExceeded, "loop count exceeds the configured maximum"};
    }
    if (state.attempts.size() > bounds.max_attempts) {
        return Status{StatusCode::StoreBoundsExceeded, "attempt count exceeds the configured maximum"};
    }
    if (state.idempotency.size() > bounds.max_idempotency_records) {
        return Status{StatusCode::StoreBoundsExceeded, "idempotency record count exceeds the configured maximum"};
    }
    if (state.obligations.size() > bounds.max_obligations) {
        return Status{StatusCode::StoreBoundsExceeded, "obligation count exceeds the configured maximum"};
    }
    if (state.service_windows.size() > bounds.max_service_windows) {
        return Status{StatusCode::StoreBoundsExceeded, "service window count exceeds the configured maximum"};
    }
    if (state.revoked_tokens.size() > bounds.max_revoked_tokens) {
        return Status{StatusCode::StoreBoundsExceeded, "revoked token count exceeds the configured maximum"};
    }

    det::Writer payload;
    {
        det::Writer body;
        body.u32(kStoreFormatVersion);
        body.u64(state.last_incarnation.value());
        body.u64(state.epoch.value());
        body.u64(state.revision.value());
        body.u64(state.config_generation.value());
        body.u64(state.topology_generation.value());
        body.u64(state.commit_sequence.value());
        body.u64(state.evidence_generation.value());
        body.u64(state.switchover_generation.value());
        body.i64(state.boot_time.nanos());
        body.u32(static_cast<std::uint32_t>(state.loops.size()));
        body.u32(static_cast<std::uint32_t>(state.evidence.size()));
        body.u32(static_cast<std::uint32_t>(state.attempts.size()));
        body.u32(static_cast<std::uint32_t>(state.idempotency.size()));
        body.u32(static_cast<std::uint32_t>(state.obligations.size()));
        body.u32(static_cast<std::uint32_t>(state.service_windows.size()));
        body.u32(static_cast<std::uint32_t>(state.revoked_tokens.size()));
        body.u32(0);
        body.u64(state.next_plan_id.value());
        body.u64(state.next_attempt_id.value());
        body.u64(state.next_command_id.value());
        body.u64(state.next_obligation_id.value());
        body.u64(state.next_service_window_id.value());
        body.u64(state.next_authority_token_id.value());
        body.u64(0);
        append_record(payload, RecordType::StoreHeader, body.bytes());
    }
    for (const auto& loop : state.loops) {
        if (loop.devices.size() > bounds.max_devices_per_loop) {
            return Status{StatusCode::StoreBoundsExceeded, "device count exceeds the configured maximum"};
        }
        det::Writer body;
        body.u64(loop.id.value());
        body.text(loop.name);
        body.u64(loop.topology_generation.value());
        body.u8(static_cast<std::uint8_t>(loop.operating));
        body.u8(static_cast<std::uint8_t>(loop.isolation));
        body.u8(static_cast<std::uint8_t>(loop.service_mode));
        body.u8(0);
        body.i64(loop.flow_target.value());
        body.i64(loop.pressure_target.value());
        body.u32(loop.flow_target_generation.value());
        body.u32(loop.pressure_target_generation.value());
        body.u32(static_cast<std::uint32_t>(loop.devices.size()));
        for (const auto& device : loop.devices) {
            write_device(body, device);
        }
        append_record(payload, RecordType::Loop, body.bytes());
    }
    for (const auto& record : state.evidence) {
        det::Writer body;
        write_reading(body, record.reading);
        append_record(payload, RecordType::LoopEvidence, body.bytes());
    }
    for (const auto& attempt : state.attempts) {
        det::Writer body;
        body.u64(attempt.id.value());
        body.u64(attempt.plan.value());
        body.u64(attempt.command.value());
        body.text(attempt.idempotency_key.str());
        body.u64(attempt.fingerprint.high);
        body.u64(attempt.fingerprint.low);
        body.u8(static_cast<std::uint8_t>(attempt.action));
        body.u8(static_cast<std::uint8_t>(attempt.effect));
        body.u64(attempt.loop.value());
        body.u64(attempt.target.value());
        body.u32(attempt.generation.value());
        body.i64(attempt.commanded_flow.value());
        body.i64(attempt.commanded_pressure.value());
        body.u64(attempt.epoch.value());
        body.u64(attempt.incarnation.value());
        body.u8(static_cast<std::uint8_t>(attempt.status));
        body.u64(attempt.revision_at_issue.value());
        body.u64(attempt.issued_at_commit.value());
        body.i64(attempt.issued_at.nanos());
        body.i64(attempt.updated_at.nanos());
        body.i64(attempt.observed_at.nanos());
        body.u64(attempt.ack_sequence.value());
        body.u64(attempt.effect_sequence.value());
        body.u32(attempt.ack_generation.value());
        body.u32(attempt.effect_generation.value());
        body.u16(static_cast<std::uint16_t>(attempt.adapter_code));
        body.u16(static_cast<std::uint16_t>(attempt.outcome));
        body.text(attempt.detail);
        append_record(payload, RecordType::Attempt, body.bytes());
    }
    for (const auto& record : state.idempotency) {
        det::Writer body;
        body.text(record.key.str());
        body.u64(record.fingerprint.high);
        body.u64(record.fingerprint.low);
        body.u64(record.plan.value());
        body.u64(record.attempt.value());
        body.u16(static_cast<std::uint16_t>(record.outcome));
        body.u64(record.committed_at.value());
        append_record(payload, RecordType::Idempotency, body.bytes());
    }
    for (const auto& obligation : state.obligations) {
        det::Writer body;
        body.u64(obligation.id.value());
        body.u64(obligation.target.value());
        body.u8(static_cast<std::uint8_t>(obligation.kind));
        body.boolean(obligation.active);
        body.u64(obligation.due_at_duty_minutes);
        body.i64(obligation.raised_at.nanos());
        body.text(obligation.note);
        append_record(payload, RecordType::Obligation, body.bytes());
    }
    for (const auto& window : state.service_windows) {
        det::Writer body;
        body.u64(window.id.value());
        body.u64(window.loop.value());
        body.u64(window.opened_by.value());
        body.i64(window.opened_at.nanos());
        body.i64(window.expires_at.nanos());
        body.text(window.note);
        append_record(payload, RecordType::ServiceWindow, body.bytes());
    }
    for (const auto& token : state.revoked_tokens) {
        det::Writer body;
        body.u64(token.value());
        append_record(payload, RecordType::RevokedToken, body.bytes());
    }
    if (payload.size() > bounds.max_file_bytes) {
        return Status{StatusCode::StoreBoundsExceeded, "snapshot payload exceeds the configured maximum size"};
    }
    return payload.bytes();
}

Result<DurableState> decode_snapshot_payload(std::span<const std::uint8_t> payload, const StoreBounds& bounds) {
    det::Reader reader(payload);
    DurableState state;
    bool saw_header = false;
    std::vector<std::uint64_t> loop_ids;
    std::vector<std::uint64_t> evidence_ids;
    std::vector<std::uint64_t> attempt_ids;
    std::vector<std::string> idempotency_keys;
    std::vector<std::uint64_t> obligation_ids;
    std::vector<std::uint64_t> window_ids;
    std::vector<std::uint64_t> revoked_ids;
    std::uint32_t declared_loops = 0;
    std::uint32_t declared_evidence = 0;
    std::uint32_t declared_attempts = 0;
    std::uint32_t declared_idempotency = 0;
    std::uint32_t declared_obligations = 0;
    std::uint32_t declared_windows = 0;
    std::uint32_t declared_revoked = 0;
    std::size_t records = 0;

    RawRecord record;
    while (next_record(reader, bounds, record)) {
        ++records;
        if (records > bounds.max_records) {
            return Status{StatusCode::StoreBoundsExceeded, "record count exceeds the configured maximum"};
        }
        det::Reader body(record.body);
        switch (record.type) {
            case RecordType::StoreHeader: {
                if (saw_header || records != 1) {
                    return Status{StatusCode::StoreCorrupt, "store header record must appear exactly once, first"};
                }
                saw_header = true;
                const std::uint32_t version = body.u32();
                if (version != kStoreFormatVersion) {
                    return Status{StatusCode::StoreVersionUnsupported, "snapshot payload version is not supported"};
                }
                state.last_incarnation = ControllerIncarnation::from_value(body.u64());
                state.epoch = ControlPlaneEpoch::from_value(body.u64());
                state.revision = StateRevision::from_value(body.u64());
                state.config_generation = ConfigGeneration::from_value(body.u64());
                state.topology_generation = TopologyGeneration::from_value(body.u64());
                state.commit_sequence = JournalCommitSequence::from_value(body.u64());
                state.evidence_generation = EvidenceGeneration::from_value(body.u64());
                state.switchover_generation = SwitchoverGeneration::from_value(body.u64());
                state.boot_time = TimestampNs::from_nanos(body.i64());
                declared_loops = body.u32();
                declared_evidence = body.u32();
                declared_attempts = body.u32();
                declared_idempotency = body.u32();
                declared_obligations = body.u32();
                declared_windows = body.u32();
                declared_revoked = body.u32();
                body.reserved(4, "store header flags");
                state.next_plan_id = PlanId::from_value(body.u64());
                state.next_attempt_id = AttemptId::from_value(body.u64());
                state.next_command_id = CommandId::from_value(body.u64());
                state.next_obligation_id = ObligationId::from_value(body.u64());
                state.next_service_window_id = ServiceWindowId::from_value(body.u64());
                state.next_authority_token_id = AuthorityTokenId::from_value(body.u64());
                body.reserved(8, "store header trailer");
                if (body.ok()) {
                    if (!state.next_plan_id.valid() || !state.next_attempt_id.valid() ||
                        !state.next_command_id.valid() || !state.next_obligation_id.valid() ||
                        !state.next_service_window_id.valid() || !state.next_authority_token_id.valid()) {
                        return Status{StatusCode::StoreCorrupt, "store header carries a zero identity counter"};
                    }
                }
                break;
            }
            case RecordType::Loop: {
                if (!saw_header) {
                    return Status{StatusCode::StoreCorrupt, "loop record precedes the store header"};
                }
                LoopRecord loop;
                loop.id = LoopId::from_value(body.u64());
                loop.name = body.text(kMaxNameLength);
                loop.topology_generation = TopologyGeneration::from_value(body.u64());
                loop.operating = det::read_enum<LoopOperatingState>(body, kLoopOperatingStateNames,
                                                                    "loop operating state");
                loop.isolation = det::read_enum<IsolationState>(body, kIsolationStateNames, "isolation state");
                loop.service_mode = det::read_enum<ServiceMode>(body, kServiceModeNames, "service mode");
                body.reserved(1, "loop flags");
                loop.flow_target = FlowRate::from_value(body.i64());
                loop.pressure_target = Pressure::from_value(body.i64());
                loop.flow_target_generation = DeviceGeneration::from_value(body.u32());
                loop.pressure_target_generation = DeviceGeneration::from_value(body.u32());
                const std::uint32_t device_count = body.u32();
                if (device_count > bounds.max_devices_per_loop) {
                    return Status{StatusCode::StoreBoundsExceeded, "device count exceeds the configured maximum"};
                }
                std::vector<std::uint64_t> device_ids;
                for (std::uint32_t i = 0; i < device_count && body.ok(); ++i) {
                    loop.devices.push_back(read_device(body));
                    device_ids.push_back(loop.devices.back().id.value());
                }
                if (body.ok()) {
                    const auto unique = check_unique(device_ids, "device id within a loop");
                    if (!unique.ok()) {
                        return unique.status();
                    }
                }
                loop_ids.push_back(loop.id.value());
                state.loops.push_back(std::move(loop));
                break;
            }
            case RecordType::LoopEvidence: {
                LoopEvidenceRecord entry;
                entry.reading = read_reading(body);
                entry.loop = entry.reading.loop;
                evidence_ids.push_back(entry.loop.value());
                state.evidence.push_back(std::move(entry));
                break;
            }
            case RecordType::Attempt: {
                CommandAttempt attempt;
                attempt.id = AttemptId::from_value(body.u64());
                attempt.plan = PlanId::from_value(body.u64());
                attempt.command = CommandId::from_value(body.u64());
                const std::string key_text = body.text(kMaxIdempotencyKeyLength);
                if (body.ok()) {
                    const auto key = IdempotencyKey::create(key_text);
                    if (!key.ok()) {
                        return Status{StatusCode::StoreCorrupt, "attempt carries an invalid idempotency key"};
                    }
                    attempt.idempotency_key = key.value();
                }
                attempt.fingerprint.high = body.u64();
                attempt.fingerprint.low = body.u64();
                attempt.action = det::read_enum<ActionKind>(body, kActionKindNames, "action kind");
                attempt.effect = det::read_enum<EffectClass>(body, kEffectClassNames, "effect class");
                attempt.loop = LoopId::from_value(body.u64());
                attempt.target = DeviceId::from_value(body.u64());
                attempt.generation = DeviceGeneration::from_value(body.u32());
                attempt.commanded_flow = FlowRate::from_value(body.i64());
                attempt.commanded_pressure = Pressure::from_value(body.i64());
                attempt.epoch = ControlPlaneEpoch::from_value(body.u64());
                attempt.incarnation = ControllerIncarnation::from_value(body.u64());
                attempt.status = det::read_enum<AttemptStatus>(body, kAttemptStatusNames, "attempt status");
                attempt.revision_at_issue = StateRevision::from_value(body.u64());
                attempt.issued_at_commit = JournalCommitSequence::from_value(body.u64());
                attempt.issued_at = TimestampNs::from_nanos(body.i64());
                attempt.updated_at = TimestampNs::from_nanos(body.i64());
                attempt.observed_at = TimestampNs::from_nanos(body.i64());
                attempt.ack_sequence = ObservationSequence::from_value(body.u64());
                attempt.effect_sequence = ObservationSequence::from_value(body.u64());
                attempt.ack_generation = DeviceGeneration::from_value(body.u32());
                attempt.effect_generation = DeviceGeneration::from_value(body.u32());
                attempt.adapter_code = static_cast<StatusCode>(body.u16());
                attempt.outcome = static_cast<StatusCode>(body.u16());
                attempt.detail = body.text(kMaxReasonLength);
                if (body.ok()) {
                    if (!status_code_defined(attempt.adapter_code) || !status_code_defined(attempt.outcome)) {
                        return Status{StatusCode::StoreCorrupt, "attempt carries an undefined status code"};
                    }
                    if (attempt.status == AttemptStatus::Unknown || attempt.action == ActionKind::Unknown) {
                        return Status{StatusCode::StoreCorrupt, "attempt carries an undefined enumerator"};
                    }
                }
                attempt_ids.push_back(attempt.id.value());
                state.attempts.push_back(std::move(attempt));
                break;
            }
            case RecordType::Idempotency: {
                IdempotencyRecord entry;
                const std::string key_text = body.text(kMaxIdempotencyKeyLength);
                if (body.ok()) {
                    const auto key = IdempotencyKey::create(key_text);
                    if (!key.ok()) {
                        return Status{StatusCode::StoreCorrupt, "idempotency record carries an invalid key"};
                    }
                    entry.key = key.value();
                }
                entry.fingerprint.high = body.u64();
                entry.fingerprint.low = body.u64();
                entry.plan = PlanId::from_value(body.u64());
                entry.attempt = AttemptId::from_value(body.u64());
                entry.outcome = static_cast<StatusCode>(body.u16());
                entry.committed_at = JournalCommitSequence::from_value(body.u64());
                if (body.ok()) {
                    if (!status_code_defined(entry.outcome)) {
                        return Status{StatusCode::StoreCorrupt, "idempotency record carries an undefined status code"};
                    }
                    idempotency_keys.push_back(entry.key.str());
                }
                state.idempotency.push_back(std::move(entry));
                break;
            }
            case RecordType::Obligation: {
                ServiceObligation obligation;
                obligation.id = ObligationId::from_value(body.u64());
                obligation.target = DeviceId::from_value(body.u64());
                obligation.kind = det::read_enum<ObligationKind>(body, kObligationKindNames, "obligation kind");
                obligation.active = body.boolean();
                obligation.due_at_duty_minutes = body.u64();
                obligation.raised_at = TimestampNs::from_nanos(body.i64());
                obligation.note = body.text(kMaxReasonLength);
                if (body.ok() && obligation.kind == ObligationKind::Unknown) {
                    return Status{StatusCode::StoreCorrupt, "obligation carries an undefined kind"};
                }
                obligation_ids.push_back(obligation.id.value());
                state.obligations.push_back(std::move(obligation));
                break;
            }
            case RecordType::ServiceWindow: {
                ServiceWindow window;
                window.id = ServiceWindowId::from_value(body.u64());
                window.loop = LoopId::from_value(body.u64());
                window.opened_by = PrincipalId::from_value(body.u64());
                window.opened_at = TimestampNs::from_nanos(body.i64());
                window.expires_at = TimestampNs::from_nanos(body.i64());
                window.note = body.text(kMaxReasonLength);
                window_ids.push_back(window.id.value());
                state.service_windows.push_back(std::move(window));
                break;
            }
            case RecordType::RevokedToken: {
                revoked_ids.push_back(body.u64());
                state.revoked_tokens.push_back(AuthorityTokenId::from_value(revoked_ids.back()));
                break;
            }
            case RecordType::JournalHeader:
            case RecordType::JournalEntry:
                return Status{StatusCode::StoreCorrupt, "journal record found in a snapshot payload"};
        }
        if (body.failed()) {
            return body.status();
        }
        if (!body.at_end()) {
            return Status{StatusCode::StoreCorrupt, "record body has trailing bytes"};
        }
    }
    if (reader.failed()) {
        return reader.status();
    }
    if (!saw_header) {
        return Status{StatusCode::StoreCorrupt, "snapshot payload contains no store header record"};
    }
    if (state.loops.size() != declared_loops || state.evidence.size() != declared_evidence ||
        state.attempts.size() != declared_attempts || state.idempotency.size() != declared_idempotency ||
        state.obligations.size() != declared_obligations || state.service_windows.size() != declared_windows ||
        state.revoked_tokens.size() != declared_revoked) {
        return Status{StatusCode::StoreCorrupt, "decoded record counts disagree with the store header"};
    }
    const auto checks = {check_unique(loop_ids, "loop id"), check_unique(evidence_ids, "loop evidence entry"),
                         check_unique(attempt_ids, "attempt id"), check_unique(obligation_ids, "obligation id"),
                         check_unique(window_ids, "service window id"),
                         check_unique(revoked_ids, "revoked token id")};
    for (const auto& check : checks) {
        if (!check.ok()) {
            return check.status();
        }
    }
    std::sort(idempotency_keys.begin(), idempotency_keys.end());
    for (std::size_t i = 1; i < idempotency_keys.size(); ++i) {
        if (idempotency_keys[i] == idempotency_keys[i - 1]) {
            return Status{StatusCode::StoreCorrupt, "duplicate idempotency key in store file"};
        }
    }
    // Identity counters must strictly exceed every identity already present,
    // otherwise a restart could hand out an identity that is still in use.
    std::uint64_t max_plan = 0;
    std::uint64_t max_command = 0;
    for (const auto& attempt : state.attempts) {
        max_plan = std::max(max_plan, attempt.plan.value());
        max_command = std::max(max_command, attempt.command.value());
    }
    for (const auto& idempotency : state.idempotency) {
        max_plan = std::max(max_plan, idempotency.plan.value());
    }
    auto max_of = [](const std::vector<std::uint64_t>& values) {
        std::uint64_t result = 0;
        for (const std::uint64_t value : values) {
            result = std::max(result, value);
        }
        return result;
    };
    if (state.next_plan_id.value() <= max_plan || state.next_attempt_id.value() <= max_of(attempt_ids) ||
        state.next_command_id.value() <= max_command ||
        state.next_obligation_id.value() <= max_of(obligation_ids) ||
        state.next_service_window_id.value() <= max_of(window_ids) ||
        state.next_authority_token_id.value() <= max_of(revoked_ids)) {
        return Status{StatusCode::StoreCorrupt, "an identity counter does not exceed the identities in use"};
    }
    return state;
}

Result<std::vector<std::uint8_t>> encode_journal_payload(const DurableState& state,
                                                         const std::vector<JournalEntry>& entries,
                                                         const StoreBounds& bounds) {
    if (entries.size() > bounds.max_journal_entries) {
        return Status{StatusCode::StoreBoundsExceeded, "journal entry count exceeds the configured maximum"};
    }
    det::Writer payload;
    {
        det::Writer body;
        body.u32(kStoreFormatVersion);
        body.u64(state.commit_sequence.value());
        body.u32(static_cast<std::uint32_t>(entries.size()));
        body.u32(0);
        append_record(payload, RecordType::JournalHeader, body.bytes());
    }
    for (const auto& entry : entries) {
        det::Writer body;
        body.u64(entry.commit.value());
        body.u64(entry.revision.value());
        body.i64(entry.timestamp.nanos());
        body.u8(static_cast<std::uint8_t>(entry.action));
        body.u16(static_cast<std::uint16_t>(entry.outcome));
        body.u8(static_cast<std::uint8_t>(entry.attempt_status));
        body.u64(entry.loop.value());
        body.u64(entry.target.value());
        body.u64(entry.attempt.value());
        body.u64(entry.fingerprint.high);
        body.u64(entry.fingerprint.low);
        body.text(entry.detail);
        append_record(payload, RecordType::JournalEntry, body.bytes());
    }
    if (payload.size() > bounds.max_file_bytes) {
        return Status{StatusCode::StoreBoundsExceeded, "journal payload exceeds the configured maximum size"};
    }
    return payload.bytes();
}

Result<std::vector<JournalEntry>> decode_journal_payload(std::span<const std::uint8_t> payload,
                                                         const StoreBounds& bounds,
                                                         JournalCommitSequence* sequence) {
    det::Reader reader(payload);
    std::vector<JournalEntry> entries;
    bool saw_header = false;
    std::uint32_t declared_entries = 0;
    std::size_t records = 0;
    JournalCommitSequence previous_commit{};

    RawRecord record;
    while (next_record(reader, bounds, record)) {
        ++records;
        if (records > bounds.max_records) {
            return Status{StatusCode::StoreBoundsExceeded, "journal record count exceeds the configured maximum"};
        }
        det::Reader body(record.body);
        if (record.type == RecordType::JournalHeader) {
            if (saw_header || records != 1) {
                return Status{StatusCode::StoreCorrupt, "journal header must appear exactly once, first"};
            }
            saw_header = true;
            const std::uint32_t version = body.u32();
            if (version != kStoreFormatVersion) {
                return Status{StatusCode::StoreVersionUnsupported, "journal payload version is not supported"};
            }
            if (sequence != nullptr) {
                *sequence = JournalCommitSequence::from_value(body.u64());
            } else {
                (void)body.u64();
            }
            declared_entries = body.u32();
            body.reserved(4, "journal header flags");
        } else if (record.type == RecordType::JournalEntry) {
            if (!saw_header) {
                return Status{StatusCode::StoreCorrupt, "journal entry precedes the journal header"};
            }
            JournalEntry entry;
            entry.commit = JournalCommitSequence::from_value(body.u64());
            entry.revision = StateRevision::from_value(body.u64());
            entry.timestamp = TimestampNs::from_nanos(body.i64());
            entry.action = det::read_enum<ActionKind>(body, kActionKindNames, "action kind");
            entry.outcome = static_cast<StatusCode>(body.u16());
            entry.attempt_status = det::read_enum<AttemptStatus>(body, kAttemptStatusNames, "attempt status");
            entry.loop = LoopId::from_value(body.u64());
            entry.target = DeviceId::from_value(body.u64());
            entry.attempt = AttemptId::from_value(body.u64());
            entry.fingerprint.high = body.u64();
            entry.fingerprint.low = body.u64();
            entry.detail = body.text(kMaxReasonLength);
            if (body.ok()) {
                if (!status_code_defined(entry.outcome)) {
                    return Status{StatusCode::StoreCorrupt, "journal entry carries an undefined status code"};
                }
                if (static_cast<std::size_t>(entry.action) >= kActionKindCount) {
                    return Status{StatusCode::StoreCorrupt, "journal entry carries an undefined action"};
                }
                if (!entries.empty() && !(previous_commit < entry.commit)) {
                    return Status{StatusCode::StoreCorrupt, "journal commit sequence is not strictly increasing"};
                }
                previous_commit = entry.commit;
            }
            entries.push_back(std::move(entry));
        } else {
            return Status{StatusCode::StoreCorrupt, "snapshot record found in a journal payload"};
        }
        if (body.failed()) {
            return body.status();
        }
        if (!body.at_end()) {
            return Status{StatusCode::StoreCorrupt, "journal record body has trailing bytes"};
        }
    }
    if (reader.failed()) {
        return reader.status();
    }
    if (!saw_header) {
        return Status{StatusCode::StoreCorrupt, "journal payload contains no journal header record"};
    }
    if (entries.size() != declared_entries) {
        return Status{StatusCode::StoreCorrupt, "journal entry count disagrees with the journal header"};
    }
    return entries;
}

}  // namespace liquidcooling
