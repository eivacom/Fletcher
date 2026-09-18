# Fletcher for .NET

Three NuGet packages, one solution, one version — the managed half of round
BIND. The C half is [`c-abi/`](../c-abi/README.md), the shim these packages call.

| Package | Native assets? | Depends on |
|---|---|---|
| `Eiva.Fletcher.Interop` | **yes** — `runtimes/{rid}/native/` (`win-x64`, `linux-x64` at first release) | — |
| `Eiva.Fletcher` | no | `Interop`, `Apache.Arrow` |
| `Eiva.Fletcher.GatewayClient` | **no** | `Apache.Arrow` |

Three, not six (D-BIND-14′): with `Apache.Arrow` a base dependency there is no
dependency-free tier left for a `Core`/`Arrow`/`PubSub` split to protect.
Namespaces inside `Eiva.Fletcher` keep the C++ component names
(`Eiva.Fletcher.PubSub`, `Eiva.Fletcher.Arrow`, …), so a later split needs no
renames. `Interop` is the only package carrying native assets, which is what
isolates the RID matrix, the packed-size budget and the LGPL question to one
artifact. `GatewayClient` has no native dependency at all — it is the managed
port of `@eiva/fletcher-gateway-client` and the one documented exception to
"there is exactly one codec".

## Layout

```
dotnet/
  Fletcher.slnx              XML solution (it can carry a licence header; .sln cannot)
  Directory.Build.props      the single <VersionPrefix> for all three packages
  global.json                the SDK band, pinned to the devcontainer's
  .editorconfig              what `dotnet format --verify-no-changes` enforces
  src/…                      the three packages
  tests/Fletcher.Tests/      unit tests
```

## Building

```bash
dotnet restore Fletcher.slnx
dotnet build Fletcher.slnx -c Release
dotnet test Fletcher.slnx -c Release
```

The SDK version is pinned twice on purpose — `global.json` here and
`DOTNET_SDK_VERSION` in `.devcontainer/Dockerfile` — so the container, CI and a
developer's host cannot silently diverge. Bump them together. CI restores with
`--locked-mode`, so a dependency change must arrive with its updated
`packages.lock.json`; those files are committed.

Libraries target **`net8.0;net10.0`**. The test project targets both as well and
rolls forward to the installed runtime, so the net8.0-compiled assemblies are
executed, not merely compiled.

## Status — round BIND

At **BIND-0** the three packages are **empty** and that is the deliverable: the
lanes (`ci.dotnet.yml`, `ci.c-abi.yml`) restore, build, test and pack on both
platforms before there is any real code to get wrong. Seam §12.4's lesson is the
reason — PR #126's first lane run found seven defects that local green could not,
three of them Linux-only.

What is *not* empty is the one question BIND-0 had to answer before the design
could stand: **risk N-9**, whether `Apache.Arrow` can carry every type in the
mapping across the Arrow C Data Interface. `tests/Fletcher.Tests/
ArrowCDataInterfaceTests.cs` round-trips one array per Arrow type the wire-format
mapping produces — including `struct`, `list<int32>`, `list<struct>`,
`map<utf8,int32>`, a timezone-carrying timestamp and a duration — plus schema and
field metadata. All 17 types pass on `Apache.Arrow` **23.0.0**, which is why the
version is pinned exactly rather than floated: bumping it re-runs that
verification.

Then: **BIND-3** brings `Interop` and the codec/Arrow tier, **BIND-4** pub/sub,
**BIND-5** `SubscriberArrow`, **BIND-8** the gateway client, **BIND-9** the
packaging and the publish pipeline.
