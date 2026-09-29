# Concurrency and ownership audit

## Model

The runtime has exactly one mutex, `Runtime::mutex_`, guarding all mutable
state: the working `DurableState`, the issued-authority set, the journal cache,
the shutdown flag and the identity counters. There is no second lock, no
recursive lock and no lock hierarchy to get wrong.

Two rules are absolute:

1. **No lock is held across an adapter callback.** Every call into
   `ILoopAdapter` is made after releasing `mutex_` and before re-acquiring it.
2. **Durable commits are made with the lock held.** `DurableStore` never calls
   back into the runtime, so this cannot re-enter mutable state.

Rule 2 means persistence costs serialise behind the mutex, which is intended:
single-writer authority for durable mutation.

## How rule 1 is implemented

Adapter calls happen in exactly three places, each of which releases the lock for
the duration of the call and restores it afterwards, converting any exception the
adapter throws into `adapter-internal-error`:

| Site | Purpose |
| --- | --- |
| `Runtime::read_device_unlocked` | one device read during verification |
| `Runtime::apply` | the command dispatch |
| `Runtime::observe` | one read per registered device |

Because the lock is released, another thread can mutate state while a call is in
flight. On re-acquisition the runtime re-reads the attempt record by identity and
applies the result with a guard: if the attempt was resolved concurrently, the
stored outcome is reported instead of overwriting it, and the caller is told the
outcome came from elsewhere.

## Why this is proven rather than assumed

`lcctest::ReentrancyProbeAdapter` wraps the synthetic adapter. Inside every
`actuate` and `read` callback it starts a thread that calls
`Runtime::status()` -- which needs `mutex_` -- and joins it before returning.

* If the runtime held `mutex_` across the callback, the probe thread could never
  acquire it, the join would never complete and the test would hang.
* The test terminates, so no lock is held across the adapter boundary.

There is no timeout anywhere in the suite: a hang is treated as a defect to be
diagnosed, not masked.

## Audited call paths

| Path | Lock acquisition | Notes |
| --- | --- | --- |
| `plan` / `plan_locked` | one `lock_guard`, never nested | pure reads of `state_`, `policy_` and the clock |
| `execute` | `lock_guard` for the replay probe, then `plan` + `apply` | the probe releases before planning |
| `apply` | one `unique_lock` | released only around the adapter calls |
| `verify` / `verify_locked` | one `unique_lock` | released only around the device read |
| `observe` | one `unique_lock` | released around each device read; reads are sequential |
| `abandon`, `issue_authority`, `revoke_authority`, `open_service_window`, `close_service_window`, `flush` | one `lock_guard` | no adapter calls |
| `status`, `loop_status`, `loops`, `attempts`, `journal`, `service_windows` | one `lock_guard` | pure reads |
| `close` | one `lock_guard` | verifies the store, caches diagnostics, drops the store to release the OS lock |
| `~Runtime` | calls `close` | idempotent |

Checked explicitly during the audit:

* no read-lock-then-write-acquire on the same lock;
* no write lock held while calling code that reads or writes the same lock;
* no mutex re-entry through a callback (`std::mutex` is not recursive, and the
  probe adapter proves it is never needed);
* no event emission or user callback under the lock;
* no joining or waiting on workers while holding state they need -- the runtime
  starts no workers of its own;
* no cancellation or shutdown lock inversion: `close` takes the lock once,
  performs no blocking waits and publishes no results afterwards;
* no nested resource acquisition, because there is only one lock;
* no persistence or adapter callback that can re-enter mutable state;
* no stale asynchronous completion: there is no asynchronous path, and every
  post-callback update re-reads the record it intends to modify.

## Shutdown

`close` sets `shutting_down_`, which makes every subsequent public call return
`shutting-down` except the read-only queries and repeated `close` calls. It then
verifies the on-disk snapshot against the open generation, caches the store
diagnostics so `status` keeps working, and destroys the store, which releases the
exclusive cross-process lock. Repeated `close` is a no-op.

## Cross-process exclusion

The store lock is a real operating-system lock: the lock file is opened with no
sharing at all on Windows, so a second writer cannot open it and receives
`store-locked`. The identity of the lock is the resolved absolute path of the
store root, so the same logical store cannot be locked twice through different
spellings. Because the lock lives inside the store directory and the operating
system enforces exclusivity on the file object itself, two processes that reach
the same physical directory through different textual paths still contend
correctly.

The tests exercise this with real child processes: one holds the store while the
parent is refused; five processes race and exactly one becomes the writer; and a
reader sampling a live writer always observes a complete generation.
