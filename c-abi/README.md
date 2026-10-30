# fletcher-c-abi — the binding ABI shim

One pure C surface over Fletcher, shipped as **one shared library per RID**. It
is what `Eiva.Fletcher.Interop` carries in `runtimes/<rid>/native/`, and what a
second binding (Rust, or a Node.js add-on, on the root README's Roadmap) would
call. Nothing above this boundary links a Fletcher C++ library or sees a C++ type.

```
c-abi/
  include/fletcher/abi/binding.h   the ABI: pure C99, versioned, append-only within a major
  src/binding_version.c            fl_binding_abi_version()
  src/binding.cpp                  every other entry point: registry, publisher, subscriber,
                                   arrival, attachments, blobs, schemas, the codec surface
  src/nanoarrow_codec.*            the Arrow-array codec behind fl_codec_* / fl_rows_*
  src/containment.*                the one site that turns a C++ exception into fl_status
  src/single_copy.*                the one-copy-per-process marker (D-BIND-17)
  src/builtins.cpp                 registers the three built-in providers into the shim
  tests/                           the shim's own tests (gtest + the C99 self-containment probe)
  benchmarks/                      BIND-4d-v's per-row publish benchmark (not built by CI)
  test_package/                    a C program that consumes the package as a binding would
```

## Where this sits

**Above the seam.** Fletcher is the *callee*; an application calls in. That is
the mirror of the **driver ABI** (`fletcher/abi/driver.h`, round PDA-ABI), which
sits *below* the seam with Fletcher as the caller. The two headers are derived
from the same seam specification (`docs/pubsub-interface-spec.md`) and share no
declaration, no include and no definition — deliberately, on both sides (seam
§1, PDA-ABI decision 2, D-BIND-2′). They will resemble each other; that is a
consequence of one seam, not a coupling, and neither may be changed "to match"
the other.

## What the shim contains

The eProsima and Micro XRCE-DDS chains are linked **statically**, and the three
built-in providers — `inprocess`, `fastdds`, `xrce` — are **registered by the
shim itself** (D-BIND-16). A C# application therefore selects a real transport
with a configuration string and deploys one native file per RID: no loader, no
driver binaries beside it, no `conanrun`. Selecting a provider by **path**
answers `kNotSupported` until PDA-ABI fills the resolver seat, and managed code
can neither register a provider nor install a path resolver (D-BIND-24).

Symbol visibility is hidden by default, and on Linux `--exclude-libs ALL` also
localises everything that arrived from a static archive, so there the export
table is exactly what `binding.h` declares. **On Windows it is not**, and that is
a measured BIND-0 finding rather than an oversight: ConanCenter's `fast-dds`
static build is compiled with `EPROSIMA_USER_DLL_EXPORT`, so its objects carry
303003 `/EXPORT` directives that the linker honours when it pulls them into a
DLL — 3531 exported names, with or without a module-definition file (MSVC merges
the two). Nothing resolves incorrectly because of it (Windows binds imports
per-module and P/Invoke finds `fl_binding_abi_version` by name), but the export
table is bloat the packed-size budget pays for, and removing it means changing
how the dependency is built. Accepted (D-BIND-157): `Eiva.Fletcher.Interop` has a
12 MB packed-size ceiling, and the export surface is revisited only if it is hit.

## Building

```bash
conan create ../core            --build=missing -pr:a=../.conan-profiles/<profile>
conan create ../pubsub          --build=missing -pr:a=../.conan-profiles/<profile>
conan create ../fastdds-pubsub-provider  --build=missing -pr:a=../.conan-profiles/<profile>
conan create ../xrcedds-pubsub-provider  --build=missing -pr:a=../.conan-profiles/<profile>
conan create .                  --build=missing -pr:a=../.conan-profiles/<profile>
```

No Conan remote hosts the `fletcher-*` packages, so the dependencies have to be
in the local cache first. That builds the shim alone, which is what the NuGet
packages carry and what their README's relinking route builds (rehearsed as
written, BIND-9a review B3). **With the tests** (`-o "&:run_tests=True"`) the
recipe also needs `fletcher-arrow-bridge`, for the C ABI encoder's oracle, so
`conan create ../arrow-bridge` goes before it; `ci.c-abi.yml` does that, and
builds the probe shim too (below).

### The probe shim (`with_probe_shim`, test-only)

```bash
conan create ../integration-tests/copy-probe --build=missing -pr:a=../.conan-profiles/<profile>
conan create . --build=missing -pr:a=../.conan-profiles/<profile> -o "&:run_tests=True" -o "&:with_probe_shim=True"
```

This builds `fletcher-c-abi-probe` beside the shim. It uses the same object
library and entry points, but its builtins also register the copy oracle's
`SeamProbeProvider` as `probe`. Two test-only entry points go with it,
`fl_test_probe_loan` and `fl_test_probe_score`, declared in
`probe/include/fletcher/abi/test/probe.h` and never in `binding.h`.

A binding selects `probe` by name and publishes through its real path. The copy
oracle's own `Judge()` then scores what the probe saw (D-BIND-58, D-BIND-61,
D-BIND-62). The shipped shim still registers exactly three providers
(`TheShippedShimHasNoProbe`). The option stays in the package ID, so a package
built with it cannot pass for the release one. Its suite is `c_abi_probe_tests`.

## The surface

[`binding.h`](include/fletcher/abi/binding.h) is **ABI 0.7: 47 entry points**, every one
implemented, in the header's sections: the version handshake; status and `fl_error`; strings,
topics and the single-copy marker; blobs and schemas as shared owner handles; the schema arrival
with its five outcomes; attachments (a builder, and a sealed set read by position); the write
window a caller lends the encoder; the provider, publisher and subscriber, with the schema watch
(`fl_subscriber_subscribe_schema`, which answers `FL_NOT_SUPPORTED` on a transport with no schema
channel: of the built-ins, all but `fastdds`); and the codec. Every declaration says which part of the seam specification it comes from, and restates
the rule rather than pointing at it, so a binding author never has to read the C++ tree. Status
numbers are the seam's own and are never renumbered.

**Versioning.** `fl_binding_abi_version()` answers for the shim actually loaded, packed as
`(major << 16) | minor`. Within a major the header is append-only, and from 1.0 a declaration is
deprecated for a whole major before it goes. **Before 1.0 there is no compatibility guarantee**:
any declaration may change between 0.x minors, so a binding refuses a shim whose minor differs from
the one it was built against (C# does), and every change bumps the version and is recorded,
never silent.

## Ownership

The header states it once and uses three words precisely:

- **BORROWED**: valid for the call only; the callee may read it and must not keep it. A pointer the
  header does not qualify is borrowed.
- **OWNED**: the caller received a reference and releases it through the function named for it
  (`fl_provider_destroy`, `fl_publisher_destroy`, `fl_subscriber_destroy`, `fl_codec_close`,
  `fl_rows_unbind`, `fl_blob_release`, `fl_schema_release`, `fl_schema_arrival_dispose`,
  `fl_attachments_dispose`, `fl_string_list_dispose`, `fl_error_dispose`). Releasing twice, or
  releasing what was never received, is undefined.
- **OUT**: written only on success; on failure the callee leaves it untouched.

The rules a binding has to build its types around:

- **Errors.** Every fallible call takes an `fl_error*`, zero-initialised. On failure the caller owns
  its `message` (bytes with a length, so a zero byte cannot truncate it) and releases it with
  `fl_error_dispose`, which is why every binding's throw site is a `finally`. An error a binding's
  callback fills is the other way round: the callback keeps its bytes alive until it returns and
  the shim copies them.
- **Blobs and schemas are shared.** A blob is bytes plus an opaque owner, retained and released
  through `fl_blob_retain` / `fl_blob_release`; its bytes are immutable once they cross. A schema is
  an Arrow C Data Interface structure the shim owns and **shares**: a binding never calls its Arrow
  `release` callback, which would destroy it for the provider still delivering on it. A binding
  that wants an Arrow schema of its own takes `fl_schema_copy` and releases that copy the Arrow
  way. `fl_schema_release` for the handle, `out->release` for the copy, never the other way round.
- **Arrow input is borrowed, never consumed.** A schema passed in is deep-copied; an array passed to
  `fl_rows_bind` is read in place, its `release` never called, and kept alive by the caller until
  `fl_rows_unbind`. That is what lets one export serve many publishes.
- **Deliveries.** A delivery runs on the transport's own thread, and every pointer it is handed
  (`data`, `schema`, `atts`) is borrowed for the call: a handler that keeps the bytes copies them,
  one that keeps the schema or an attachment's blob retains it. A handler must not throw across the
  boundary, and re-entering the seam on the same provider and thread is refused with
  `FL_REENTRANT_CALL`.
- **Lifetimes and quiescence.** A provider outlives every publisher and subscriber built on it.
  `fl_subscriber_unsubscribe` outside a delivery blocks until that subscription's running delivery
  has finished, after which the binding may free its context; from inside a delivery it is served
  without waiting, and the context is freed only after a second cancel from outside (D-BIND-50).
  Destroying a subscriber from inside one of its own deliveries terminates the process, so a
  binding refuses that in its own code first (D-BIND-18).
- **Attachments are read, never sorted.** A set is enumerated by position
  (`fl_attachments_size`, `_key_at`, `_value_at`) in ascending unsigned-byte order of the key, which
  is the wire order; a key with a zero byte is refused.

## The codec surface

Values cross as **Arrow arrays**, never as per-field C arguments, and **no encode entry point
returns bytes**, so a row is never written anywhere but the transport's window (D-BIND-1a,
D-BIND-23). Three steps, each with its own cost and lifetime:

1. **Open**, once per schema: `fl_codec_open` validates the Arrow schema against the wire mapping,
   refuses a type the mapping does not carry by naming the field, and precomputes the field plan.
   An `fl_codec` is immutable, so any number of threads share one.
2. **Bind**, once per batch: `fl_rows_bind` checks that the struct array matches the codec and
   validates every buffer once, not per row.
3. **Publish or encode** a row: `fl_publisher_publish_row` (or `_rows` for a range in one
   crossing) runs the encoder inside the seam's publish, straight into the provider's window, after
   checking that the topic was declared on this publisher and that the rows encode its wire layout;
   `fl_encode_row` writes into a window the caller supplies (`fl_write_window`, with a grow
   callback), for a write-ahead log, a relay or a test.

Decoding is the mirror: `fl_decode_rows` turns `count` rows of bytes into a fresh struct array the
caller imports and owns, and `fl_decode_rows_framed` (ABI 0.7) takes each row's end offset, so a
row that does not decode, or does not end exactly at its boundary, is skipped and reported rather
than misaligning its neighbours. Both touch no provider and are callable from inside a delivery.
`fl_publisher_publish_raw` publishes bytes a binding's own writer puts into the window, unchecked.

The codec is `src/nanoarrow_codec.*`. `NanoarrowCodec.ByteIdenticalToArrowBridge` holds it to
`arrow-bridge`'s Arrow C++ codec, byte for byte, over the shared corpus in
`tests/codec_corpus.hpp`, and its messages name a nested field as that codec does (`readings[].x`,
`tags{}.value.x`).

## Consuming it from another binding

C# is the worked example, in [`dotnet/src/Fletcher.Interop`](../dotnet/src/Fletcher.Interop/) and
[`dotnet/src/Fletcher`](../dotnet/src/Fletcher/); a Rust crate or a Node.js add-on does the same
things in its own idiom:

1. **Load one shim per process.** Resolve `fletcher-c-abi` (`libfletcher-c-abi.so`,
   `fletcher-c-abi.dll`) from where the package puts it for the RID (C#: `NativeLoader`, a
   `DllImportResolver`), and never ship a second copy beside another binding's: the shim refuses
   to work when it finds another module exporting `fl_single_copy_marker`, naming both
   (D-BIND-17).
2. **Handshake before anything else.** Call `fl_binding_abi_version()` and refuse a shim whose major,
   or before 1.0 whose minor, differs from the `FL_BINDING_ABI_VERSION` the binding was built
   against, with a message saying which two.
3. **Bring your own Arrow.** The header only forward-declares `ArrowSchema` and `ArrowArray`; the
   binding uses its Arrow library's C Data Interface (C#: `Apache.Arrow.C`), exports a batch for
   `fl_rows_bind` and imports what `fl_decode_rows` and `fl_schema_copy` produce.
   [`binding-abi-conformance`](../integration-tests/binding-abi-conformance/README.md) is the lane
   that proves an Arrow implementation hands the codec what Arrow C++ does.
4. **Make the ownership rules types.** One owning wrapper per handle (C#: a `SafeHandle` each), an
   error-to-exception site that disposes in a `finally`, a bound batch whose disposal unbinds and
   then releases (`BoundRows`), and a delivery thunk that catches everything, counts in-flight
   deliveries and refuses a destroy from inside one.
5. **Select transports by name.** `fl_provider_create` takes a selector and a configuration
   (`max_payload_bytes`, `domain_id` and a document in the transport's own syntax). The shim
   registers `inprocess`, `fastdds` and `xrce` itself; a binding can neither register a provider
   nor install a path resolver (D-BIND-24), and a path selector answers `FL_NOT_SUPPORTED`.

[`test_package/`](test_package/) is the smallest consumer: a C program that links the package,
calls `fl_binding_abi_version()` and compares it with the header's.
