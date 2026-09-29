# Architecture

## Core question

Given current authority, loop/device generation, interlocks, coolant evidence,
flow/pressure state, leak state and service obligations, which liquid-cooling
transition may be attempted safely, and how is the physical effect proven rather
than merely recorded as a command acknowledgement?

## Owned boundary

This runtime owns the vendor-neutral control model for liquid cooling:

* loop, CDU, pump and valve identities, plus the hardware generation of each
  device as reported by its adapter;
* device lifecycle and loop operating state;
* flow and pressure targets and the evidence that they were achieved;
* coolant state and quality **as typed evidence** at the control boundary;
* leak evidence and isolation authority;
* service and maintenance authority, including protected obligations;
* safe start / stop / open / close / setpoint / isolate transitions;
* command attempts, idempotency, acknowledgement, observation and verified
  effect;
* interlocks and protected service obligations;
* durable journaling, recovery and stale-authority fencing;
* the adapter abstraction for vendor-specific actuation.

## Explicit non-ownership

This runtime does **not** own, model or reimplement:

* cooling structural topology (it holds opaque loop and device identities);
* facility cooling-capacity accounting;
* zone-level thermal policy;
* facility-wide cooling-source failover orchestration;
* thermal emergency orchestration;
* vendor firmware control loops (these sit behind `ILoopAdapter`);
* chemistry laboratory interpretation beyond typed evidence;
* electrical power authority;
* facility placement.

External state owned elsewhere enters as typed, generation-stamped evidence. A
missing channel is never coerced into zero, permission, availability or success.

## Layers

```
        embedder / operations
                |
   +--------------------------+
   |  Runtime                 |  planning, fencing, attempts, interlocks
   |  (runtime.hpp)           |
   +--------------------------+
      |                |
      |                +--------------------+
      |                                     |
   +---------------------+        +-----------------------+
   |  DurableStore       |        |  ILoopAdapter         |
   |  (store.hpp)        |        |  (adapter.hpp)        |
   |  snapshot + journal |        |  vendor actuation     |
   +---------------------+        +-----------------------+
      |                                     |
   host file system                  vendor hardware / plant
```

Supporting value types are split by semantics rather than convenience:

| Header | Contents |
| --- | --- |
| `status.hpp` | `StatusCode`, `Status`, `Result<T>` |
| `units.hpp` | exact-integer quantities, checked arithmetic, physical domains |
| `ids.hpp` | strong identities, generations, epochs, revisions, fingerprints |
| `time.hpp` | monotonic timestamps, injected clock |
| `model.hpp` | device, loop, valve, pump, leak, coolant, service records |
| `evidence.hpp` | `Evidence<T>`, usability, `LoopReading` |
| `authority.hpp` | capabilities, principals, authority tokens |
| `policy.hpp` | limits, interlocks, resource bounds |
| `transition.hpp` | actions, requests, plans, fingerprints |
| `attempt.hpp` | attempt lifecycle, idempotency records |
| `adapter.hpp` | vendor-neutral actuation and observation boundary |
| `store.hpp` | durable format, publication, recovery |
| `runtime.hpp` | the control runtime |

## Exact quantities

Every physical quantity is an exact integer in a named unit, and the unit is part
of the C++ type:

| Type | Unit | Representation |
| --- | --- | --- |
| `FlowRate` | millilitres per minute | `int64` |
| `Pressure` | pascals (gauge) | `int64` |
| `Temperature` | milli-degrees Celsius | `int32` |
| `Conductivity` | microsiemens per centimetre | `int64` |
| `Acidity` | milli-pH (pH 7.4 is 7400) | `uint32` |
| `Duration` | nanoseconds | `int64` |

Arithmetic is only available through checked helpers that report overflow as a
status rather than wrapping. No authority or accounting decision compares
floating-point values.

## Distinct states are distinct

`Evidence<T>` carries `present`, `unsupported`, a value, an observation
sequence, a device generation, a timestamp and an origin. A default-constructed
channel is **absent**. Usability is one of `Usable`, `Absent`, `Stale`,
`Unsupported`, `Recovered` or `Future`; the evaluation order is fixed and
documented. Evidence loaded from the durable store is demoted to `Recovered`
origin and can never authorise a transition until a current adapter read
revalidates it.

## Transition lifecycle

```
request --> plan (pure validation, deterministic precedence)
              |
              +--> refusal (one primary StatusCode)
              |
              v
            apply
              |
              +-- idempotency resolution (before any validation)
              +-- re-validation against the current state binding
              +-- durable attempt record, status Issued     <-- commit
              +-- adapter dispatch (runtime lock released)
              +-- acknowledgement recorded, status Acknowledged  <-- commit
              +-- post-command observation
              +-- effect verdict: Verified / Contradicted / Pending
              +-- durable resolution + idempotency record   <-- commit
```

Three durable commits bracket one physical command, so a process death at any
point leaves a record that explains what was attempted and what remained
unproven.

## Examples, CLI and benchmark

* `examples/quickstart.cpp` - observe, circulate, raise the flow setpoint,
  isolate and enter service.
* `examples/leak_isolation.cpp` - leak precedence, isolation authority,
  unresolved-attempt fencing and explicit abandonment.
* `cli/lccctl.cpp` - `status`, `loops`, `attempts`, `journal`, `fsck` and
  eight synthetic scenarios.
* `bench/transition_bench.cpp` - completed durable transition latency.
* `downstream/main.cpp` - an independent out-of-tree consumer that links the
  installed package.
