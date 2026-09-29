# Authority, generations and fencing

## Identities that are not interchangeable

| Concept | Type | Meaning |
| --- | --- | --- |
| control-plane epoch | `ControlPlaneEpoch` | monotonic across controller restarts; survives in the store |
| controller incarnation | `ControllerIncarnation` | one running controller attached to one store |
| device generation | `DeviceGeneration` | hardware/software generation of one device |
| configuration generation | `ConfigGeneration` | the policy in force |
| topology generation | `TopologyGeneration` | the loop/device registry in force |
| evidence generation | `EvidenceGeneration` | the adapter's evidence set generation |
| switchover generation | `SwitchoverGeneration` | adapter replacement behind a loop |
| state revision | `StateRevision` | advances on every durable mutation |
| journal commit sequence | `JournalCommitSequence` | the authoritative generation counter |
| observation sequence | `ObservationSequence` | monotonic per adapter session |
| attempt / plan / command identity | `AttemptId`, `PlanId`, `CommandId` | separate, never reused |

Each is a distinct C++ type built on a tagged scalar whose zero value means
"never assigned", so an epoch can never be passed where a generation is expected.

## Capabilities are sets, not a ladder

`AuthorityCapabilities` has four independent flags: `observe`, `isolate`,
`production` and `service`. The shipped presets are:

| Preset | observe | isolate | production | service |
| --- | --- | --- | --- | --- |
| auditor | yes | no | no | no |
| isolation only | yes | yes | no | no |
| production operator | yes | yes | yes | no |
| service engineer | yes | yes | no | yes |
| full | yes | yes | yes | yes |

Two properties follow directly:

* **Isolation is broader than production.** Hazard-reducing actions
  (`stop-pump`, `close-valve`, `isolate-loop`, lowering a setpoint) require only
  the isolation capability, so they remain available to a responder who cannot
  add load or flow.
* **Service never inherits production.** A service engineer holds service and
  isolation rights and *never* production rights, so entering service mode cannot
  silently grant plant actuation.

## Authority tokens

An `AuthorityToken` binds a principal's capabilities to one loop and to the
state it was issued against: epoch, incarnation, configuration generation,
topology generation, state revision, issue time, expiry and a nonce. Tokens are
minted only by `Runtime::issue_authority` for a configured, enabled principal,
and are tracked in a bounded issued-set.

Validation order during planning is fixed:

1. token present;
2. epoch equals the current epoch;
3. incarnation equals the running incarnation;
4. configuration generation is current;
5. topology generation matches the loop;
6. token scope covers the requested loop;
7. token was issued by this incarnation and is not revoked;
8. lease has not expired;
9. capabilities cover the capability the action requires.

A token from an earlier incarnation is therefore refused with
`incarnation-mismatch`; one from an earlier epoch with `epoch-mismatch`; a
forged or evicted one with `authority-revoked`. When the issued-set bound is
reached the token closest to expiry is evicted deterministically (ties broken by
the lower identity), which turns the evicted token into a revoked one.

## Deterministic validation precedence

The same invalid request always yields the same primary machine-readable code.
Precedence is:

| Stage | Checks | Example codes |
| --- | --- | --- |
| 1 shape | action defined, required and forbidden fields, domains, bounded strings | `unsupported-action`, `missing-required-field`, `value-out-of-range` |
| 2 identity | loop, device, obligation, attempt exist; device kind suits the action | `unknown-object`, `wrong-object-kind` |
| 3 generation | expected device generation versus the observed one | `stale-generation`, `future-generation` |
| 4 authority | the nine token checks above | `epoch-mismatch`, `authority-insufficient` |
| 5 revision | expected state revision versus the current one | `stale-revision`, `future-revision` |
| 6 service | service mode conflicts, protected obligations, service windows | `service-mode-conflict`, `service-obligation-active` |
| 7 presence | lifecycle state and reported absence | `device-absent`, `device-faulted` |
| 8 attempts | unresolved effect-bearing attempts, open-attempt bound | `unresolved-attempt-blocks`, `attempt-limit-exceeded` |
| 9 leak | leak interlocks | `leak-state-indeterminate`, `leak-confirmed`, `interlock-blocked` |
| 10 coolant | coolant quality interlock | `coolant-quality-not-nominal` |
| 11 envelope | measured and commanded quantities, evidence freshness | `pressure-out-of-range`, `evidence-stale` |
| 12 transition | lifecycle legality | `invalid-state-transition`, `already-in-target-state` |

Stage 1 also requires the idempotency key, which is why a request without one is
refused before any state is consulted.

## Idempotency

A request fingerprint is a 128-bit hash over the canonical encoding of the
*semantic intent*: action, loop, target, expected device generation, flow target
and its presence flag, pressure target and its presence flag, obligation,
attempt and expected revision. The authority token, the idempotency key and the
free-form reason are deliberately excluded, so the same intent retried with a
fresh lease replays instead of conflicting.

* same key, same fingerprint -> the stored outcome is replayed and the adapter is
  not called again;
* same key, different fingerprint -> `idempotency-conflict`.

Idempotency is resolved **before** state validation, in both `execute` and
`apply`. That is what makes a lost-response retry work: by the time the caller
retries, the revision and device generation have already moved on, so a retry
that had to satisfy validation first could never replay. Planning the same stale
request on its own is still refused with `stale-revision` -- replay is a property
of execution, not of validation.

## Fencing rules

* **Stale device generation never receives actuation.** Planning compares the
  expected device generation with the generation the runtime last observed. A
  device whose generation has never been observed cannot be actuated at all.
* **A generation change fences in-flight attempts.** An attempt whose observation
  arrives on a different generation is left unresolved with
  `evidence-generation-mismatch` and continues to fence incompatible actuation.
* **Discrete device state is bound to a generation.** A valve position or pump
  state only satisfies an "already in the target state" check when it was
  observed on the current device generation. After a verified loop-scoped
  isolation every other device's state is unbound until it is re-read.
* **Recovered evidence is never current evidence.** On load, every
  adapter-origin channel is demoted to recovered origin and fails usability.
* **Restart fences authority.** Each open bumps the controller incarnation;
  tokens from the previous incarnation are refused, revocations are cleared
  because they are meaningful only within one incarnation, and observation
  sequencing is re-baselined because the adapter session restarted too.

## Unresolved attempts

An attempt is *open* while its physical effect is unproven: `planned`, `issued`,
`acknowledged` or `unresolved`. An open attempt on a loop fences any further
action on that loop that is not:

* strictly hazard-reducing (so isolation and shutdown always remain available),
* an exact retry of the same intent, or
* a service action that resolves the situation.

An attempt is resolved only by a current post-command observation bound to the
same device generation (`effect-verified`), by a definite adapter refusal
(`failed`), by a proven contradiction after the effect deadline
(`contradicted`), or by explicit abandonment under service authority
(`abandoned`). A command acknowledgement alone is never a resolution.
