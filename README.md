# Liquid Cooling Control

Vendor-neutral control runtime for liquid cooling, repository 29 of the Data
Center Control Plane (DCCP) program.

**Core question.** Given current authority, loop/device generation, interlocks,
coolant evidence, flow/pressure state, leak state and service obligations, which
liquid-cooling transition may be attempted safely -- and how do we prove the
physical effect rather than merely record a command acknowledgement?

Version 1.0.0. C++20, CMake, dependency-light, Windows/MSVC first with a
portable core.

## What this is

A production library that owns the control model for liquid-cooling loops:

* a durable, single-writer control state with an explicitly versioned,
  integrity-checked on-disk format;
* planning that refuses unsafe or stale transitions with one deterministic
  machine-readable code;
* a command-attempt lifecycle in which an acknowledgement is never proof of
  effect, and a verified effect requires a current post-command observation
  bound to the same device generation;
* interlocks for leak evidence, coolant quality, pressure and temperature
  envelopes, protected service obligations and service mode;
* stale-authority and stale-generation fencing across restarts;
* a vendor-neutral adapter boundary so vendor actuation can be added without
  changing the control model.

## Owned boundary

This repository owns the vendor-neutral control model: loop/CDU/pump/valve
identities and hardware generations; device lifecycle and loop operating state;
flow and pressure targets and their evidence; coolant state and quality **as
typed evidence**; leak evidence and isolation authority; service and maintenance
authority; safe start/stop/open/close/setpoint/isolation transitions; command
attempts, idempotency, acknowledgement, observation and verified effect;
interlocks and protected service obligations; durable journaling, recovery and
stale-authority fencing; and the adapter abstraction for vendor-specific
actuation.

## Explicitly not owned

Cooling structural topology; facility cooling-capacity accounting; zone-level
thermal policy; facility-wide cooling-source failover orchestration; thermal
emergency orchestration; vendor firmware control loops; chemistry laboratory
interpretation beyond typed evidence; electrical power authority; facility
placement.

Those concerns live in ASI, DFI, other DCCP repositories, vendor firmware or a
BMS/DCIM/PLC. State owned elsewhere arrives here as typed, generation-stamped
evidence or as an opaque reference. A missing channel is never coerced into
zero, permission, availability or success.

## Safety semantics

* **Leak evidence is safety critical.** Absent, stale, unsupported or suspected
  leak evidence blocks every action that could increase exposure. Isolation,
  shutdown and setpoint reduction remain available.
* **Isolation is broader than production.** Hazard-reducing actions require only
  the isolation capability; a responder with isolation rights can reduce hazard
  but cannot add load or flow.
* **Service never inherits production.** The service capability never implies the
  production capability, so entering service mode cannot silently grant plant
  actuation.
* **An acknowledgement is not proof.** `acknowledged` means the adapter accepted
  the command. `effect-verified` requires a later observation, from the same
  device generation, published after the acknowledgement.
* **Unresolved effects fence actuation.** An open effect-bearing attempt blocks
  any incompatible action on its loop until it is verified, contradicted, failed
  or explicitly abandoned under service authority.
* **Recovered state is not fresh evidence.** Persisted observations are demoted
  to recovered origin on load and cannot authorise a transition until a current
  read revalidates them.

## Quick start

```cpp
#include <liquidcooling/liquidcooling.hpp>
using namespace liquidcooling;

Policy policy = Policy::defaults(ConfigGeneration::from_value(1));
policy.principals.principals.push_back(
    PrincipalRecord{PrincipalId::from_value(1), "operator",
                    AuthorityCapabilities::production_operator(), true});

LoopRecord loop;                       // loop, CDU, pump and valve registry
loop.id = LoopId::from_value(1);
loop.name = "loop-a";
loop.isolation = IsolationState::Open;
loop.devices.push_back(DeviceRecord{DeviceId::from_value(1), DeviceKind::Cdu,
                                    DeviceGeneration::from_value(1), LifecycleState::Present});

RuntimeConfig config;
config.store_root = "C:/var/lib/lcc/loop-a";
config.policy = policy;
config.topology_generation = TopologyGeneration::from_value(1);
config.adapter = my_vendor_adapter;    // implements ILoopAdapter
config.clock = my_clock;               // optional; defaults to a steady clock
config.initial_loops.push_back(loop);

auto runtime = Runtime::open(config);
if (!runtime.ok()) { /* runtime.status() carries the machine-readable code */ }

// Evidence must be current before an exposure-increasing action is admitted.
(void)runtime.value()->observe(LoopId::from_value(1));

auto token = runtime.value()->issue_authority(
    AuthorityRequest{PrincipalId::from_value(1), LoopId::from_value(1),
                     Duration::from_value(600'000'000'000)});

TransitionRequest request;
request.action = ActionKind::StartPump;
request.loop = LoopId::from_value(1);
request.target = DeviceId::from_value(2);
request.expected_device_generation = DeviceGeneration::from_value(1);
request.expected_revision = runtime.value()->status().value().revision;
request.authority = token.value();
request.idempotency_key = IdempotencyKey::create("start-pump-1").value();
request.reason = "raise flow for job 42";

auto execution = runtime.value()->execute(request);
if (execution.ok()) {
    // execution.value().outcome is the machine contract;
    // execution.value().attempt.status says whether effect was proven.
}
```

Two runnable examples exercise the real library paths:

```
lcc_example_quickstart       <store-dir>
lcc_example_leak_isolation   <store-dir>
```

## Architecture

| Header | Contents |
| --- | --- |
| `status.hpp` | stable `StatusCode` values, `Status`, `Result<T>` |
| `units.hpp` | exact-integer quantities, checked arithmetic, physical domains |
| `ids.hpp` | strong identities, generations, epochs, revisions, fingerprints |
| `time.hpp` | monotonic timestamps and an injectable clock |
| `model.hpp` | device, loop, valve, pump, leak, coolant and service records |
| `evidence.hpp` | `Evidence<T>`, usability evaluation, `LoopReading` |
| `authority.hpp` | capability sets, principal registry, authority tokens |
| `policy.hpp` | operating envelope, named interlocks, resource bounds |
| `transition.hpp` | the closed action set, requests, plans, fingerprints |
| `attempt.hpp` | attempt lifecycle and idempotency records |
| `adapter.hpp` | the vendor-neutral actuation and observation boundary |
| `store.hpp` | durable format, publication, recovery and read-only inspection |
| `runtime.hpp` | the control runtime |
| `synthetic_adapter.hpp` | a deterministic **simulated** loop/plant adapter |

Quantities are exact integers in named units (`FlowRate` in mL/min, `Pressure`
in Pa, `Temperature` in milli-degrees Celsius, `Conductivity` in uS/cm,
`Acidity` in milli-pH, `Duration` in nanoseconds), and the unit is part of the
C++ type. Arithmetic is only reachable through checked helpers that report
overflow instead of wrapping. No authority decision compares floating-point
values.

## Authority and generation model

Every mutation is planned against an explicit binding: controller incarnation,
control-plane epoch, configuration generation, topology generation, device
generation, state revision, plan identity and idempotency key. Refusal is
deterministic -- the same invalid request always yields the same primary code --
and the precedence is documented in `docs/AUTHORITY.md`.

Key rules:

* a device whose generation has never been observed cannot be actuated;
* a device generation change fences in-flight attempts;
* a valve position or pump state only counts as current when it was observed on
  the current device generation;
* authority tokens are scoped to one loop and one incarnation, and are refused
  after a restart;
* a request fingerprint covers the semantic intent only, so a lost-response
  retry replays while a different intent reusing the key conflicts.

## Persistence and recovery

The store root holds `lcc.snapshot` (authoritative), `lcc.journal` (the audit
trail published with it), `lcc.snapshot.prev` (the superseded generation) and
`lcc.store.lock`. Files are framed with magic, format version, lengths, CRC-32C
integrity checks and a trailer, and decoding rejects truncation, trailing bytes,
non-zero reserved fields, undefined enumerators and duplicate identities.

Commit point: after the payload is written to a staging file, flushed, read back
and verified, the snapshot is atomically replaced. A crash before that point
leaves the previous generation authoritative; a crash between the snapshot and
journal replacements leaves a lagging journal, which is reported rather than
hidden. See `docs/PERSISTENCE.md`.

## Concurrency model

One mutex guards all mutable state. No lock is ever held across an adapter
callback; durable commits are always made under the lock. A test adapter
re-enters the runtime from inside every callback on a second thread, so
termination of that test is itself the proof that the boundary is clean. See
`docs/CONCURRENCY.md`.

The store lock is a real operating-system lock, so several processes cannot write
one store; concurrent readers never block publication and always observe a
complete generation.

## CLI

`lccctl` inspects a store without taking the write lock and can drive synthetic
scenarios through the real runtime:

```
lccctl status   --store <dir>
lccctl loops    --store <dir>
lccctl attempts --store <dir> [--loop N]
lccctl journal  --store <dir>
lccctl fsck     --store <dir>
lccctl sim quickstart|leak-isolation|ack-without-effect|delayed-effect|
             contradiction|stale-sequence|adapter-failure|pressure-excursion
             --store <dir>
```

## Benchmark

`lcc_bench <store-dir> [warmup] [iterations]` measures a *completed* durable
transition: plan, durable attempt record, adapter dispatch, post-command
observation, effect verification and the final durable commit of the resolved
attempt with its audit journal -- including the file syncs. Submission latency is
not measured. The plant is SYNTHETIC; the runtime, the durable store and the file
system work are REAL. The benchmark prints the workload, warm-up and iteration
counts, mean/p50/p95/p99 latency, throughput and the store location.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Options (all default `ON` except ASan):

| Option | Effect |
| --- | --- |
| `LCC_BUILD_TESTS` | build `lcc_tests` and register the CTest groups |
| `LCC_BUILD_CLI` | build `lccctl` |
| `LCC_BUILD_EXAMPLES` | build the two examples |
| `LCC_BUILD_BENCHMARKS` | build `lcc_bench` |
| `LCC_ENABLE_ASAN` | build with AddressSanitizer |
| `LCC_WARNINGS_AS_ERRORS` | treat first-party warnings as errors |

First-party targets are compiled with `/W4 /permissive- /utf-8
/Zc:__cplusplus /WX` on MSVC and with an equivalent strict set elsewhere. No
warning is globally suppressed.

## Install and consume

```
cmake --install build --prefix <prefix>
```

This installs the static library, the public headers and a namespaced CMake
package. An independent project consumes it:

```
cmake -S downstream -B build-downstream -DCMAKE_PREFIX_PATH=<prefix>
cmake --build build-downstream
./build-downstream/downstream_consumer <store-dir>
```

`downstream/` is a separate CMake project that uses
`find_package(LiquidCoolingControl 1.0 REQUIRED CONFIG)`, links
`LiquidCooling::lcc`, runs a real loop lifecycle against a real store and reopens
it, proving the installed package rather than the build tree is usable.

## Validation actually performed

Everything below was executed on Windows 10.0.26200 with MSVC 19.44.35207, CMake
4.3.2 and Ninja 1.13.2, in Release and Debug, with the Debug build additionally
run under AddressSanitizer.

* 12 CTest groups, all passing, with no timeouts anywhere in the suite.
* Real multi-process tests: an external writer excludes others, five processes
  race and exactly one wins, and a reader sampling a live writer always observes
  a complete generation.
* Real process-death tests: `TerminateProcess` at seven different points inside
  the commit sequence, plus restart after death mid-actuation proving that no
  unsafe re-actuation occurs and that the unresolved attempt still fences.
* Adversarial tests: hostile adapters that throw or lie, empty readings, path
  traversal and ambiguity, reserved device names, reparse points, lifetime
  independence and resource-leak checks.
* Install, package-config and independent downstream consumption.
* Benchmark: three durable commits per completed transition, mean 27-30 ms per
  completed transition on this host (see `docs/VALIDATION.md` for the full
  methodology and both runs).

### Real vs synthetic vs unsupported

* **REAL** -- all software, process, file system, package and multi-process
  behaviour described above.
* **SYNTHETIC** -- the plant, sensors and actuators behind the adapter. The
  synthetic adapter is a simulation and is never evidence of behaviour on real
  cooling hardware.
* **UNSUPPORTED** -- live hardware validation, vendor adapter interoperability,
  and POSIX execution (the POSIX platform path exists but was not compiled or
  exercised here).

## Limitations

* No real cooling hardware, CDU, pump, valve, BMS or DCIM was available; every
  physical effect in this repository is simulated.
* Only the Windows platform path is validated. The POSIX implementation in the
  platform layer is provided but unverified in this environment.
* The runtime deliberately does **not** actuate on its own. A confirmed leak
  blocks exposure-increasing work and remains isolatable under isolation
  authority, but the isolation itself must be requested by an authorised
  principal; autonomous safety actuation is an orchestration policy that belongs
  above this layer.
* Verifying a flow or pressure setpoint requires the loop to be circulating;
  otherwise the attempt stays unresolved, which is intended but means an
  operator must either run the loop or explicitly abandon the attempt.
* Topology changes are explicit administrative actions
  (`RuntimeConfig::accept_topology_change` with a strictly newer topology
  generation); the runtime refuses silent drift.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
