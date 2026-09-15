# fletcher-c-abi — the binding ABI shim

One pure C surface over Fletcher, shipped as **one shared library per RID**. It
is what `Eiva.Fletcher.Interop` carries in `runtimes/<rid>/native/`, and what
the Rust binding will call next round. Nothing above this boundary links a
Fletcher C++ library or sees a C++ type.

```
c-abi/
  include/fletcher/abi/binding.h   the ABI: pure C99, versioned, append-only within a major
  src/binding_version.c            fl_binding_abi_version()
  src/builtins.cpp                 registers the three built-in providers into the shim
  tests/                           the shim's own tests (gtest + the C99 self-containment probe)
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

At **BIND-0** the exported surface is `fl_binding_abi_version()` and nothing
else. That is the item's point: the two new lanes run on both platforms before
there is any real code to run them on (seam §12.4 — the first lane run of
PR #126 found seven defects local green could not, three of them Linux-only).
The built-in providers are nonetheless linked from the kickoff, so the
static-link-into-a-shared-library question and the per-RID size budget are
answered by a real artifact rather than by a proxy.

**BIND-1** specifies the rest of the header — status codes, the caller-owned
`fl_error`, the registry, publisher and subscriber handles, `SchemaArrival`,
blobs, attachments, the write window and writer, the delivery callback, the
three-step codec surface and the single-copy marker — and is reviewed as a
specification. **BIND-2** implements the nanoarrow codec behind it.
