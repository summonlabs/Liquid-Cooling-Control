# Validation

## Evidence classes

* **REAL** - actual software, process, file system and package behaviour
  exercised on the host described below.
* **SYNTHETIC** - the plant, sensors and actuator behind `ILoopAdapter`.
* **UNSUPPORTED** - proof that could not be obtained in this environment.

No live data centre, chiller plant, CDU, CRAH/CRAC, pump, valve, BMS or DCIM was
available. Nothing in this repository claims hardware validation. The synthetic
plant exercises control semantics only.

## Environment

| Item | Value |
| --- | --- |
| operating system | Windows 10.0.26200 |
| toolset | MSVC 19.44.35207 (Visual Studio Build Tools 2022 17.14) |
| build system | CMake 4.3.2, Ninja 1.13.2 |
| warning policy | `/W4 /permissive- /utf-8 /Zc:__cplusplus /WX` on every first-party target |
| configurations | Release and Debug; Debug additionally under AddressSanitizer |

## Test groups

The suite is one binary, `lcc_tests`, with cases selected by group. Groups are
registered with CTest as `lcc.<group>`. No test, and no CTest test, has a timeout:
a hanging case is a defect to be diagnosed.

| Group | What it proves |
| --- | --- |
| `units` | checked arithmetic at every boundary, quantity domains, identifier and string validation, fingerprint stability and sensitivity, status table completeness, `Result` access rules |
| `model` | enum decoding rejects undefined values, evidence defaults are absent rather than zero, capability sets are not a privilege ladder, action descriptor consistency, policy validation, direction resolution |
| `determinism` | the same invalid request yields the same primary code over 200 repetitions, precedence is stable across field orderings, snapshot payload encoding is byte-for-byte stable, fingerprints are distinct, scripted scenarios reproduce exactly |
| `transitions` | every validation stage, interlocks, leak precedence, service mode, obligations, lockout tags, service windows, capability enforcement, generation and revision fencing, state legality, open failure modes |
| `attempts` | acknowledgement is not proof, delayed effect, contradiction, fencing and the reduction carve-out, idempotent replay without re-actuation, key reuse conflicts, abandonment, plan identity consumption, plan re-validation, the open-attempt bound |
| `adapter` | adapter unavailability, device-level refusals, generation-mismatch acks, stale and replayed sequences, future timestamps, unsupported channels, leak appearance, starved flow, contradictory position |
| `store` | payload round trip for every record kind, structural and semantic decoder rejection, framing corruption, torn publication, previous-generation recovery, staging reclamation, journal bounding, read-only inspection, repeated reopen |
| `bounds` | loop, device, obligation, token, revocation, idempotency, string and file-size bounds, and deterministic token eviction |
| `concurrency` | the reentrancy probe, concurrent readers and writers, repeated open and close, destruction without close, concurrent open admitting exactly one writer |
| `multiprocess` | real child processes holding the lock, five-process races, and a live reader observing only complete generations |
| `crash` | real process death during commits, cumulative generations, staging residue from a dead writer, restart after death mid-actuation, repeated death and restart cycles |
| `adversarial` | hostile adapters that throw or lie, empty readings, path traversal and ambiguity, reserved names, reparse points, lifetime independence, file and lock leakage, shutdown under concurrent readers |

## Crash-consistency proof

A dedicated child process commits generations forever while maintaining the
invariant

```
state.revision == state.commit_sequence == state.topology_generation
```

The parent kills it with `TerminateProcess` at seven different delays so that
death lands at different points inside the commit sequence, then reopens the
store from a fresh process. After every kill the parent asserts that the store
either contains no authoritative generation at all (the writer died before its
first publication, which is permitted) or decodes to a state that satisfies the
invariant. A state assembled from two publications would break the invariant and
fail.

`crash.restart_after_death_during_actuation_does_not_reactuate` is the headline
fencing proof: a child observes a loop, issues a command whose effect never
appears, records the open attempt and dies while holding the store lock. The
parent reopens with a brand new adapter and asserts that

* the unresolved attempt survived with the same identity, action and target;
* the fresh adapter has performed **zero** actuations -- nothing was replayed;
* recovered evidence is not usable and the loop reads as leak-indeterminate;
* a start command is refused with `unresolved-attempt-blocks` and still performs
  no actuation;
* a hazard-reducing close is admitted and is the only actuation performed;
* explicit abandonment under service authority clears the fence, after which the
  loop is workable again.

## Independent reference models

* The synthetic plant is an independent physical model: the runtime never sees
  the plant's internal state and can only act through the adapter interface.
* The crash-invariant check compares the decoded store against the arithmetic
  identity the writer maintained, not against the runtime's own bookkeeping.
* Fingerprints and canonical encodings are tested for stability and sensitivity
  rather than against themselves.
* The downstream consumer exercises the installed package rather than the build
  tree.

## Benchmark methodology and results

`lcc_bench <store> 200 1500` on the environment above, Release, MSVC 19.44:

| Metric | Run 1 | Run 2 |
| --- | --- | --- |
| measured completed transitions | 1500 | 1500 |
| durable commits | 5103 | 5103 |
| mean | 27.37 ms/op | 29.96 ms/op |
| p50 | 23.50 ms/op | 24.76 ms/op |
| p95 | 44.05 ms/op | 53.39 ms/op |
| p99 | 103.20 ms/op | 123.39 ms/op |
| min / max | 17.45 / 200.78 ms | 15.66 / 401.37 ms |
| throughput | 36.5 transitions/s | 33.4 transitions/s |

The measured operation is a completed transition: plan, durable attempt record,
adapter dispatch, post-command observation, effect verification and the durable
resolution with its audit journal -- three durable commits per transition, each
including a snapshot sync, a journal sync and the superseded-generation copy.
Submission latency is not measured. The plant is SYNTHETIC; the runtime, the
store and the file system work are REAL. The p99 tail is dominated by host file
system flush latency, which is why two runs of the same workload differ.

No before/after claim is made: no earlier revision was benchmarked under a
comparable alternating methodology.

## Defects found and repaired during hardening

Each of these was found by a test or by the benchmark, reproduced, fixed at the
root cause and then re-validated by the complete surface:

1. **Identity counters were not consumed for plans.** The snapshot self-check
   rejected its own output because `next_plan_id` never advanced past the plan
   identity already recorded on an attempt.
2. **A completed attempt produced no idempotency record.** Verification resolved
   the attempt in its own commit, after which the caller's commit skipped the
   record, so a lost-response retry would have actuated a second time.
3. **Replay was unreachable.** Idempotency was resolved after state validation,
   so a retry -- whose revision had necessarily moved on -- could never replay.
   Resolution now happens before validation in both `execute` and `apply`.
4. **`close()` did not release the store lock.** The lock lived until the
   runtime was destroyed, so a store could not be reopened immediately after a
   clean close and a removed directory could not be deleted.
5. **Device generation was enforced for loop-scoped commands.** A loop-scoped
   command carries no device generation, so every isolation and setpoint
   verification failed with a spurious generation mismatch.
6. **Verified setpoints were never recorded.** The loop's flow and pressure
   targets were never updated from a verified effect, so every later setpoint
   change resolved as an increase.
7. **Post-command observations were not absorbed.** Verification judged the
   effect but left the device's recorded position stale, so the next plan could
   reject or accept a transition on outdated state.
8. **Path roots were validated as components.** On a drive-qualified path the
   `C:` designator itself was validated, which rejected every absolute path.
9. **Relative store roots broke every later file operation.** The stored display
   path was the caller's relative spelling while every file operation prefixes
   the Win32 extended-length marker, which requires an absolute path.
10. **A read-only inspector could block publication.** Windows requires the
    rename target to permit deletion, so a reader that opened without sharing
    delete made the atomic replace fail. Readers now share read, write and
    delete, and always observe a complete generation.
11. **Transient access denial on atomic replace.** A bounded retry on
    `ERROR_ACCESS_DENIED`, `ERROR_SHARING_VIOLATION` and
    `ERROR_LOCK_VIOLATION` makes publication reliable without weakening
    atomicity.
12. **Acknowledged-but-unproven effects were closed as failed.** A generation or
    internal adapter error does not prove the absence of a physical effect, so
    those attempts now stay unresolved and keep fencing actuation.
13. **Future-stamped observations could prove effect.** Verification now rejects
    a timestamp ahead of the runtime clock.
14. **Restart could not re-baseline observation sequencing.** A restarted
    controller restarts its adapter session, so the recorded sequence is
    re-baselined on open and a higher adapter switchover generation re-baselines
    it mid-session.
15. **A partially accepted observation pass reported success.** `observe` now
    fails when nothing was accepted instead of returning an empty success.
16. **Loop-scoped isolation left other devices' states stale.** A verified
    isolation unbinds every other device's discrete state from the current
    generation, so a stale position can never satisfy an "already in the target
    state" check.
17. **Revocation was not idempotent.** Repeating a revocation consumed record
    budget and could fail with `resource-exhausted`.
18. **Redundant durable commit.** The idempotency record is written in the same
    commit that resolves the attempt, cutting one of four commits per transition.
19. **Storage bounds could not hold the policy's retention bounds.** The runtime
    now refuses to open unless every store bound accommodates the corresponding
    policy bound, instead of failing later at encode time.
20. **Test harness use-after-scope.** AddressSanitizer caught the check macros
    binding a reference into a temporary; the macros now materialise operands by
    value and compare scalars through an opaque helper.

## Package and downstream proof

`cmake --install` places the static library, the public headers and a namespaced
CMake package (`LiquidCooling::lcc`) with config and version files. The
independent project under `downstream/` configures against the installed prefix
with `find_package(LiquidCoolingControl 1.0 REQUIRED CONFIG)`, links the exported
target, runs a real loop lifecycle and reopens the durable store.

## What is not proven

* Behaviour on real cooling hardware, or against a real vendor adapter.
* POSIX builds: the platform layer contains a POSIX implementation, but only the
  Windows path was compiled and exercised here.
* AddressSanitizer coverage of the POSIX path.
