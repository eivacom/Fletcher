# fletcher-c-abi — the binding ABI shim

One pure C surface over Fletcher, shipped as **one shared library per RID**. It
is what `Eiva.Fletcher.Interop` carries in `runtimes/<rid>/native/`, and what
the Rust binding will call next round. Nothing above this boundary links a
Fletcher C++ library or sees a C++ type.

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
how the dependency is built. Revisited at BIND-9.

## Building

```bash
conan create ../core            --build=missing -pr:a=../.conan-profiles/<profile>
conan create ../pubsub          --build=missing -pr:a=../.conan-profiles/<profile>
conan create ../fastdds-pubsub-provider  --build=missing -pr:a=../.conan-profiles/<profile>
conan create ../xrcedds-pubsub-provider  --build=missing -pr:a=../.conan-profiles/<profile>
conan create .                  --build=missing -pr:a=../.conan-profiles/<profile> -o "&:run_tests=True"
```

No Conan remote hosts the `fletcher-*` packages, so the dependencies have to be
in the local cache first; `ci.c-abi.yml` does exactly the above.

## Status — round BIND

**BIND-0** brought the component up: the two lanes run on both platforms, and
the built-in providers are linked from the kickoff so the
static-link-into-a-shared-library question and the per-RID size budget are
answered by a real artifact rather than by a proxy.

**BIND-1 has now specified the whole header** — the status taxonomy (the seam's
own numbers), the caller-owned `fl_error` with its `origin`, the provider
registry, publisher and subscriber handles, the schema arrival and its five
outcomes, blobs and schemas as owner handles, attachments in published order,
the write window and its one-call writer, the delivery callback, the three-step
codec surface, and the single-copy marker. It is reviewed **as a specification**:
what is expensive to get wrong here is the ownership wording, not the syntax.

**Every declaration is now implemented: 44 entry points, ABI 0.5.** BIND-2 built
the codec surface and the publisher chain, BIND-3 the interop tier above it, and
BIND-4 the rest — the subscriber half, the arrival, attachments and blobs, and the
three entry points the header turned out to be missing (`fl_blob_create`,
`fl_schema_retain`, `fl_schema_copy`: D-BIND-42, 43, 46), each an ABI minor bump.
The schema-watch pair (`fl_subscriber_subscribe_schema` / `_unsubscribe_schema`)
answered `FL_NOT_SUPPORTED` per D-BIND-29 until the seam grew it (#128); since
**D-BIND-52** it forwards to the seam (ABI 0.4 → 0.5), and `FL_NOT_SUPPORTED` now
means only that the transport has no schema channel — of the built-ins, only
`fastdds` has one. The C# surface does not expose it yet. (This paragraph said "only
`fl_binding_abi_version()` is implemented" until the BIND-4 review found it stale
since BIND-2c.)
