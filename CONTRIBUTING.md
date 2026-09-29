# Contributing to Liquid Cooling Control

Thank you for your interest in Liquid Cooling Control. This document describes how to
propose changes, what is expected of a contribution, and how to report problems.

## License of contributions

Contributions are accepted under the Apache License, Version 2.0. Licensing is
inbound = outbound: anything you submit is licensed to the project under the same
terms that the project distributes under. There is **no Contributor License
Agreement (CLA) to sign**, and no copyright assignment is required.

By submitting a contribution (a pull request, a patch, or any other form of change)
you confirm that you have the right to license that work under the Apache License,
Version 2.0, and that you do so.

## Building

Liquid Cooling Control requires a C++20 compiler and CMake 3.21 or newer.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Configure with the documented options rather than editing the build files:

| Option | Default | Purpose |
| --- | --- | --- |
| `LCC_BUILD_TESTS` | `ON` | Build the test suite |
| `LCC_BUILD_CLI` | `ON` | Build the `lccctl` administration and inspection CLI |
| `LCC_BUILD_EXAMPLES` | `ON` | Build the examples |
| `LCC_BUILD_BENCHMARKS` | `ON` | Build the benchmark |
| `LCC_ENABLE_ASAN` | `OFF` | Build with AddressSanitizer |
| `LCC_WARNINGS_AS_ERRORS` | `ON` | Treat first-party compiler warnings as errors |

Warnings-as-errors is the default and is the configuration a change is judged in.
Leave it enabled; a pull request that only builds with `LCC_WARNINGS_AS_ERRORS=OFF`
is not ready.

## Testing

Build and run the full test suite before opening a pull request:

```
cmake -S . -B build -DLCC_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
```

The suite must pass in full. For changes that touch concurrency, storage, or memory
handling, also run a sanitizer configuration:

```
cmake -S . -B build-asan -DLCC_ENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

## Code quality expectations

- No new compiler warnings. The tree is warning-clean under the project's warning
  policy, and it stays that way.
- No TODOs, stubs, or placeholder behaviour in submitted code. If something is
  intentionally out of scope, say so in the pull request instead of leaving a marker
  in the source.
- Tests accompany behaviour changes. A change in observable behaviour without a
  test that exercises it will be asked for one.
- Document public API changes. New or changed headers, signatures, and semantics
  need matching documentation edits.
- Document durable format changes. Anything that alters an on-disk or wire format
  (framing, encoding, field layout, versioning) must be described, along with the
  compatibility consequences for existing data.
- Keep changes focused. Unrelated reformatting or refactoring makes review harder
  and should be sent separately.

## Commit messages

Write concise, imperative, public-facing commit messages: a short subject line in
the imperative mood ("Add pump curve validation", not "Added" or "Adding"), and a
body only when the reasoning is not obvious from the subject.

Do not add co-author trailers of any kind, and do not add attribution to tools,
assistants, or generators. The author of a commit is the person who submits it.

## Reporting a bug

Open an issue with a minimal reproduction. A useful report contains:

- What you expected to happen and what happened instead.
- The smallest program or command sequence that shows the problem.
- The exact build configuration: compiler and version, platform, CMake options, and
  project version.
- Any relevant output, including the full error text or sanitizer report.

Please keep reports reproducible and free of unrelated material.
