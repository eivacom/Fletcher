<!-- SPDX-License-Identifier: LGPL-3.0-or-later
     Copyright (C) 2026 The Fletcher Authors -->

# fletcher-copy-probe — the copy oracle's instrument

**Test-only. Never released.** This package holds the copy-accounting oracle's
instrument so that two builds can score with **one** `Judge()` (D-BIND-62):

| Consumer | What it scores |
|---|---|
| [`integration-tests/pubsub-conformance`](../pubsub-conformance/README.md) (`CopyAccounting`) | every provider at the seam, and the codec behind the shim through `fl_encode_row` |
| `c-abi`'s `fletcher-c-abi-probe`, built only under `with_probe_shim` | a binding's REAL publish path — C#'s `Publisher.Publish`, through `Eiva.Fletcher.Interop` and the shipped entry points |

It is a package of its own because neither consumer can reach the other's tree:
CI builds c-abi with `conan create`, which sees only `c-abi/`.

## What is in it

- **`ledger.hpp`**: `CopyLedger`, `CopyVerdict` and the one pure `Judge()`.
  Moved here from pubsub-conformance unchanged. The argument for them (what
  counts as a copy, why provenance and not counting, premises P2 and P5) is in
  that harness's README, "The `CopyAccounting` suite".
- **`seam_probe_provider.hpp`**: `SeamProbeProvider` and its modes. There are
  three controls: `kZeroCopy`, `kStaging` and `kGrowable`. The fourth mode,
  `kTracing`, is new and is what the probe shim registers as `probe`.
- **`tracing_window.hpp`**: the source-tracing window and D-BIND-61's scoring
  rule, `ScoreProducedFromSource`.

## Why a tracing window (D-BIND-61)

`encode_copies` asks whether the producer wrote the row straight into the window.
The ledger learns that from the producer, which reports where it composed the
row. A binding's fused publish has no such producer: the codec encodes field by
field inside the shim. Without this window, a correct publish and one that staged
the row in managed memory score identically.

What tells them apart is where the window's payload bytes came from. The codec
writes a binary field with `Append(data, len)`, and `data` points into the
caller's Arrow buffer. `Append` reaches the virtual `AppendSlow(src, len)` only
when the window is full. So this window is kept exactly full between writes, over
a fixed slot that never moves, and every append arrives with its source.

- `encode_copies == 0` only if the payload arrived in one recorded `Append` from
  the caller's own buffer.
- A staged row enters through `AppendInPlace`, which is sourceless.
- An export that copied its buffers appends from a second address.

**Which way the blind spots fail.** `AppendZeros` and a span lent by
`AppendInPlace` are the same call on this side of the window, so both are
recorded as sourceless. That can only ever read as a copy. Bytes that arrive
without passing a slow path fault the trace, and a faulted trace is never
scored. This is the guard for a future inline append that skips the capacity
check. The package's tests simulate that case by subclassing the window.

**The bound.** Only variable-length fields carry provenance. Scalars are read by
value and appended from a stack temporary. So the legs that use this window
publish a one-binary-field row, and the claim is for payload bytes.

## Building

```bash
conan create ../../core   --build=missing -pr:a=../../.conan-profiles/<profile>
conan create ../../pubsub --build=missing -pr:a=../../.conan-profiles/<profile>
conan create .            --build=missing -pr:a=../../.conan-profiles/<profile> -o "&:run_tests=True"
```

`ci.c-abi.yml` creates it with its tests on both platforms. `ci.dotnet.yml` and
the pubsub-conformance lane create it as a dependency.
