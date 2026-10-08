# Fletcher for .NET

Four NuGet packages, one solution, one version — the managed half of round
BIND. The C half is [`c-abi/`](../c-abi/README.md), the shim these packages call.

| Package | Native assets? | Depends on |
|---|---|---|
| `Eiva.Fletcher.Interop` | **yes** — `runtimes/{rid}/native/` (`win-x64`, `linux-x64` at first release) | — |
| `Eiva.Fletcher` | no | `Interop`, `Apache.Arrow` |
| `Eiva.Fletcher.GatewayClient` | **no** | `Apache.Arrow` |
| `Eiva.Fletcher.Model` | **no** | `Apache.Arrow` |

Four, not six (D-BIND-14′, amended by D-BIND-74): with `Apache.Arrow` a base dependency there is no
dependency-free tier left for a `Core`/`Arrow`/`PubSub` split to protect.
Namespaces inside `Eiva.Fletcher` keep the C++ component names
(`Eiva.Fletcher.PubSub`, `Eiva.Fletcher.Arrow`, …), so a later split needs no
renames. `Interop` is the only package carrying native assets, which is what
isolates the RID matrix, the packed-size budget and the LGPL question to one
artifact. `GatewayClient` has no native dependency at all — it is the managed
port of `@eiva/fletcher-gateway-client` and the one documented exception to
"there is exactly one codec". `Model` holds exactly the lossless `Timestamp` and
`Duration` that generated C# refers to. It is a package of its own because
generated code is compiled into the consumer's assembly, so a type emitted with
it would be defined once per assembly and collide in an app that references two;
and it takes `Apache.Arrow` only, so a gateway or WASM app can use the generated
model without native assets (D-BIND-72). User-defined messages are not in it.

## Layout

```
dotnet/
  Fletcher.slnx              XML solution (it can carry a licence header; .sln cannot)
  Directory.Build.props      the single <VersionPrefix> for all four packages
  Directory.Build.targets    the AOT toolchain packs, pinned with global.json (D-BIND-81)
  global.json                the SDK band, pinned to the devcontainer's
  .editorconfig              what `dotnet format --verify-no-changes` enforces
  src/…                      the four packages
  tests/Fletcher.Tests/      unit tests
  tests/Fletcher.Model.Tests/  Model's tests; references Model ALONE, which shows it is Arrow-only
  tests/Fletcher.CopyOracle.Tests/  the copy oracle over C#'s real publish, against the PROBE shim (BIND-5b)
  tests/Fletcher.AotSmoke/   a NativeAOT consumer of the generated pair, published and run by CI (D-BIND-81)
```

## Building

```bash
dotnet restore Fletcher.slnx
dotnet build Fletcher.slnx -c Release
dotnet test Fletcher.slnx -c Release
```

The tests load native shims, which a source build has to stage: pass
`-p:FletcherNativeShim=<fletcher-c-abi>` for `Fletcher.Tests` and
`-p:FletcherProbeShim=<fletcher-c-abi-probe>` for `Fletcher.CopyOracle.Tests`
(c-abi built with `with_probe_shim`, see `c-abi/README.md`). A project whose
shim is missing FAILS with the loader's diagnostic rather than skipping.

The SDK version is pinned twice on purpose — `global.json` here and
`DOTNET_SDK_VERSION` in `.devcontainer/Dockerfile` — so the container, CI and a
developer's host cannot silently diverge. Bump them together. CI restores with
`--locked-mode`, so a dependency change must arrive with its updated
`packages.lock.json`; those files are committed.

Libraries target **`net8.0;net10.0`**. The test project targets both as well and
rolls forward to the installed runtime, so the net8.0-compiled assemblies are
executed, not merely compiled.

## NativeAOT

*A draft for the package README (BIND-9); the facts are D-BIND-81's.*

`Eiva.Fletcher`, `Eiva.Fletcher.Interop` and `Eiva.Fletcher.Model` are marked
`IsAotCompatible` and raise no trim or AOT warnings; `Eiva.Fletcher.GatewayClient`
makes no such claim yet. Whether an application uses NativeAOT is the
**application's** choice, not the packages': set `<PublishAot>true</PublishAot>` in
the app's project, or don't, and the packages behave the same either way. Generated
C# is compiled into the app and follows the app's settings.

What an application can control:

- **Keep all of a package's code** if trimming ever removes something it needs:
  `<TrimmerRootAssembly Include="Fletcher" />` (likewise `Fletcher.Interop`,
  `Fletcher.Model`; these are the assembly names). NativeAOT honours it too.
- **Trim only assemblies marked trimmable** with `<TrimMode>partial</TrimMode>`; the
  default, `full`, trims everything.

What it cannot:

- **AOT for some assemblies only.** NativeAOT compiles the whole app, with no JIT fallback.
- **Fold the native shim into the executable.** `fletcher-c-abi` stays a separate
  shared library beside the app (`libfletcher-c-abi.so` / `fletcher-c-abi.dll`), with
  or without AOT. Linking it statically is not supported, and would change the LGPL
  obligations (risk N-8).

Deployment facts that apply with or without AOT: one build per RID (`linux-x64`,
`win-x64`); the Linux shim needs **glibc 2.38 or newer** and only the system's
`libstdc++`, `libm`, `libgcc_s` and `libc`; and the shim must come from the same
release as the managed packages, because its ABI version is checked at load
(exact minor before 1.0). Proven by `tests/Fletcher.AotSmoke` on Linux in CI;
Windows NativeAOT has not yet been exercised.

## Status — round BIND

At **BIND-0** the three packages were **empty**, and that was the deliverable: the
lanes (`ci.dotnet.yml`, `ci.c-abi.yml`) restored, built, tested and packed on both
platforms before there was any real code to get wrong. Seam §12.4's lesson is the
reason — PR #126's first lane run found seven defects that local green could not,
three of them Linux-only.

What was *not* empty was the one question BIND-0 had to answer before the design
could stand: **risk N-9**, whether `Apache.Arrow` can carry every type in the
mapping across the Arrow C Data Interface. `tests/Fletcher.Tests/
ArrowCDataInterfaceTests.cs` round-trips one array per Arrow type the wire-format
mapping produces — including `struct`, `list<int32>`, `list<struct>`,
`map<utf8,int32>`, a timezone-carrying timestamp and a duration — plus schema and
field metadata. All 17 types pass on `Apache.Arrow` **23.0.0**, which is why the
version is pinned exactly rather than floated: bumping it re-runs that
verification.

**Since then (2026-09-25).** **BIND-3** built `Eiva.Fletcher.Interop` and the
codec/Arrow tier (`FletcherCodec`, `BoundRows`), and **BIND-4** the pub/sub tier
(`ProviderRegistry`, `Publisher`, `Subscriber`, `SchemaArrival`), all over the
native shim in [`../c-abi`](../c-abi/README.md) at ABI 0.7. A transport is chosen
by selector string and C# never implements one (D-BIND-24).
`Eiva.Fletcher.GatewayClient` is still empty. The suites that need a real
transport live under `integration-tests/` and are deliberately not in
`Fletcher.slnx`, so `dotnet test Fletcher.slnx` stays a unit run.

Next: **BIND-5** `SubscriberArrow`, **BIND-6** generated C# rows, **BIND-8** the
gateway client, **BIND-9** the packaging and the publish pipeline. Item by item:
[`plans/BIND-progress-log.md`](../plans/BIND-progress-log.md).
