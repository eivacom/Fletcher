<!-- SPDX-License-Identifier: LGPL-3.0-or-later
     Copyright (C) 2026 The Fletcher Authors
-->
# binding-abi-conformance

Does a C# publisher put the same bytes on the wire as a C++ one?

## The question this suite can ask, and the one it cannot

Every Fletcher binding calls **one** codec — the shim's `NanoarrowCodec`, reached
through `fl_codec_open` / `fl_rows_bind` / `fl_encode_row`. So "do C# and C++
encode the same bytes?" is not a question this suite could fail: there is only
one encoder, and a suite comparing it with itself would certify nothing.

The open question is one layer up, and it is real:

> Two **independent Arrow implementations** build the same logical batch. Do they
> hand that one codec the same thing?

Arrow C++ builds the fixtures here. `Apache.Arrow` rebuilds its own arrays from
the IPC Arrow C++ wrote, lays out its own buffers, and exports them across the C
Data Interface. Offsets, validity bitmaps, child ordering, buffer padding, the
empty-versus-null distinction — the two libraries agree about every one of them,
or the codec reads the difference straight onto the wire. That failure does not
crash. It is a subscriber decoding a publisher's row into the wrong values, which
is the most expensive thing that can go wrong at this boundary.

Correctness of the bytes themselves is not this suite's job. That is
`NanoarrowCodec.ByteIdenticalToArrowBridge`, in C++, against `arrow-bridge` — a
genuinely second implementation. This suite asks only whether the binding is a
faithful path to the codec that test already certified.

## The corpus is borrowed, not written

`src/emit_corpus.cpp` includes **`c-abi/tests/codec_corpus.hpp`** — the same five
fixtures the byte-identity test compares the two C++ encoders over, which
D-BIND-35 ruled the right shape for this job and D-BIND-40 ruled the right corpus
for this one. Writing a corpus here instead would have been quicker and would
have created exactly the drift a corpus exists to prevent: two sets of scenarios,
both passing, covering different things.

The corpus is **regenerated on every run** rather than committed. A committed
corpus is a golden that the thing it checks is free to drift away from.

## Running it

```bash
# 1. the components, into the local Conan cache
for c in core pubsub arrow-bridge fastdds-pubsub-provider xrcedds-pubsub-provider c-abi; do
  (cd "$c" && conan create . --build=missing)
done

# 2. the emitter
cd integration-tests/binding-abi-conformance
conan build . --build=missing

# 3. emit the corpus
./build/Release/emit_corpus <output-dir>

# 4. round-trip it from C#
cd dotnet/BindingAbiConformance
FLETCHER_CORPUS_DIR=<output-dir> dotnet test -p:FletcherNativeShim=<path-to-shim>
```

Per fixture the emitter writes `<name>.arrows` (the batch as an Arrow IPC stream,
what C# rebuilds from) and `<name>.rows.bin` (the same rows as the shim encoded
them, what C# must reproduce byte for byte), plus a `manifest.txt` the C# side
**discovers** its cases from — so a fixture added to `codec_corpus.hpp` is
exercised here without anyone remembering to add it.

Both halves **fail rather than skip** when the shim or the corpus is missing: a
suite that reads green because it ran over nothing is worse than a red one.
