# Persistence, publication and recovery

## Files

A store root contains exactly four files:

| File | Role |
| --- | --- |
| `lcc.snapshot` | the authoritative generation |
| `lcc.journal` | the audit trail published with that generation |
| `lcc.snapshot.prev` | the generation superseded by the last successful commit |
| `lcc.store.lock` | the exclusive cross-process lock |

Staging files are named `lcc.<role>.<pid>-<counter>.tmp` and are never
authoritative.

## Framing

Both data files share one framing:

```
header (64 bytes) | payload (record stream) | trailer (16 bytes)
```

The header carries an eight-byte magic that identifies the file's role, a 32-bit
format version, the header size, three reserved 32-bit fields that must be zero,
the commit sequence, the record count, the payload length, a CRC-32C over the
payload, a flags field that must be zero and a CRC-32C over the first 56 header
bytes. The trailer repeats the magic and the commit sequence so that a truncated
publication is detected even when the header survived.

A record is a 16-byte header (type, flags that must be zero, body length, body
CRC-32C, reserved field that must be zero) followed by its body.

## Publication and the commit point

```
1. encode the payload and re-decode it as a self-check
2. write lcc.snapshot.<id>.tmp, flush it, read it back and compare
3. write lcc.journal.<id>.tmp the same way
4. copy the current snapshot to lcc.snapshot.prev
5. atomically replace lcc.snapshot        <-- COMMIT POINT
6. atomically replace lcc.journal
```

A crash before step 5 leaves the previous generation authoritative; the staging
files are reclaimed on the next open. A crash between steps 5 and 6 leaves the
new snapshot authoritative with a lagging journal, which is reported through
`StoreDiagnostics::journal_lagging` rather than hidden. A journal whose commit
sequence is *ahead* of the snapshot can only come from tampering, and is refused
with `store-corrupt`.

Atomic replacement uses `MoveFileExW` with `REPLACE_EXISTING` and
`WRITE_THROUGH`, so the store directory always contains either the previous
complete generation or the new complete generation, never a mixture.

## Strict decoding

The decoder rejects, with a distinct code:

* a file shorter than its framing, or larger than the configured bound;
* a wrong magic or an unsupported format version or header size;
* a non-zero reserved field anywhere;
* a header, payload, record-body or trailer integrity failure;
* a size that disagrees with the declared payload length (truncation or trailing
  bytes);
* a trailer that disagrees with the header;
* an undefined record type or an undefined enumerator;
* a record body with trailing bytes;
* duplicate loop, device, attempt, obligation, service-window or revoked-token
  identities;
* duplicate idempotency keys;
* record counts that disagree with the store header;
* an identity counter that does not strictly exceed every identity in use, which
  would let a restart reuse an identity.

## Recovery

Dynamic observations are persisted but never restored as current evidence:
`mark_evidence_recovered` demotes every adapter-origin channel to recovered
origin, and usability then reports
`recovered-evidence-requires-refresh`.

When the authoritative snapshot fails validation the runtime refuses to open by
default. With `RecoveryPolicy::UsePreviousGeneration` it validates
`lcc.snapshot.prev` and republishes it as a **new** generation, so the commit
sequence never regresses and exactly one authoritative generation exists after
recovery.

## Path hardening

The store root is canonicalised before any file is touched:

* empty, over-long and NUL-containing paths are refused;
* `.` and `..` components are refused before resolution;
* components ending in a dot or a space are refused, because Win32 strips them
  and two spellings would then denote one directory with two lock identities;
* reserved device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1`-`COM9`,
  `LPT1`-`LPT9`, with or without an extension) are refused;
* the path is resolved to an absolute form, and every non-root component is
  validated again after resolution;
* a root that is a reparse point (symbolic link or junction) is refused, and so
  is a data file that turns out to be one when it is opened;
* relative roots are resolved against the process working directory; every later
  file operation uses the resolved absolute path, prefixed with the Win32
  extended-length marker so long paths work and trailing-dot handling cannot
  differ between processes.

File sharing is chosen so that correctness does not depend on timing: the lock
file is opened with no sharing at all, so a second writer cannot even open it,
and read-only inspection opens the data files sharing read, write and delete so
that a reader can never block the atomic replace that publishes a generation.
A reader therefore always observes a complete generation.

## Durable state

The snapshot holds the incarnation, epoch, revision, configuration and topology
generations, commit sequence, identity counters, the loop and device registry,
the last reading per loop, command attempts, idempotency records, protected
service obligations, service windows and revoked authority tokens. Bounds apply
to every collection and to the total file size; encoding refuses to exceed them
rather than truncating.
