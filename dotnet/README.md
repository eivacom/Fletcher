# Fletcher for .NET

**Trying the bindings before they are released:** see [PREVIEW-TESTERS.md](PREVIEW-TESTERS.md).

Fletcher's C# binding: five NuGet packages, built from one solution at one version. The native
half is [`c-abi/`](../c-abi/README.md), the shim these packages call; the C# for a user's own
messages comes from the code generator, `protoc-gen-fletcher`, which the `Eiva.Fletcher.Protoc`
package brings (built in [`protoc/`](../protoc/README.md), not here).

| Package | What it is | Native assets | Depends on |
|---|---|---|---|
| `Eiva.Fletcher` | Publishers and subscribers over a provider chosen by name (`inprocess`, `fastdds`, `xrce`): `ProviderRegistry`, `Publisher`, `Subscriber`, `SubscriberArrow`, `SchemaArrival`; the codec (`FletcherCodec`, `BoundRows`); attachments; `FletcherException` | through `Interop` | `Interop`, `Apache.Arrow` |
| `Eiva.Fletcher.Interop` | The P/Invoke surface over the shim, and the shim itself for `linux-x64` and `win-x64`; it has no public types and is used only by `Eiva.Fletcher` | **yes**, `runtimes/{rid}/native/` | — |
| `Eiva.Fletcher.Server` | The server build of the shim (`fletcher-c-abi-server`, with Arrow C++ and the batcher) for `linux-x64` and `win-x64`, and nothing else: no assembly (D10C-5); added beside `Eiva.Fletcher` by an app that wants it | **yes**, `runtimes/{rid}/native/` | `Interop` |
| `Eiva.Fletcher.GatewayClient` | `FletcherClient`, a managed WebSocket client for the Fletcher gateway, with its own managed codec | none | `Apache.Arrow` |
| `Eiva.Fletcher.Model` | What generated C# refers to: the lossless `Timestamp` and `Duration`, the `ListView` and `MapView` windows and the element readers of the generated Arrow view and accessor | none | `Apache.Arrow` |

Each package's namespace is its own name (`Eiva.Fletcher`, `Eiva.Fletcher.Interop`,
`Eiva.Fletcher.GatewayClient`, `Eiva.Fletcher.Model`), and its assembly is that name without
`Eiva.` (`Fletcher.dll`, …).

Four managed packages, not six (D-BIND-14′, amended by D-BIND-74): with `Apache.Arrow` a base
dependency there is no dependency-free tier left for a `Core`/`Arrow`/`PubSub` split to protect. Only
`Interop` and `Server` carry native assets, one shim each (d10 L15, BC-17), which isolates the RID
matrix, the packed-size ceilings (12 MB for `Interop`, D-BIND-157; the server's own, d10 Q7) and the
LGPL relinking route to one artifact per shim. `GatewayClient` loads no native code: it
is the managed port of `@eiva/fletcher-gateway-client` and the one documented exception to "there
is exactly one codec", held to the C++ goldens by both test projects (through
`tests/Shared/CoverageFixture.cs`) and to the native codec, byte for byte, by
`ManagedCodecAgreesWithNativeTests` for the shapes no golden covers. `Model` is a package of its
own because generated code is compiled into the consumer's assembly, so a type emitted with it
would be defined once per assembly and collide in an application that references two; and it
takes `Apache.Arrow` only, so a gateway or WASM application can use the generated model without
native assets (D-BIND-72). User-defined messages are not in it.

All five target **`net8.0;net10.0`**. The three that use Arrow pin `Apache.Arrow` **exactly** (`[23.0.0]`; `Interop` and `Server` take no Arrow package): that is
the version `tests/Fletcher.Tests/ArrowCDataInterfaceTests.cs` proved carries every Arrow type the
wire-format mapping produces across the Arrow C Data Interface (16 types, `struct`,
`list<struct>`, `map<utf8,int32>`, a timezone-carrying timestamp and a duration among them, and
schema and field metadata; risk N-9), and a bump re-runs that proof.

## Getting the packages

Released versions are on [nuget.org](https://www.nuget.org/packages/Eiva.Fletcher): the five
packages above, pushed by `cd.dotnet` when a `dotnet-v*` tag is cut, and the code generator
`Eiva.Fletcher.Protoc`, pushed by `cd.protoc` from a `protoc-v*` tag (D-BIND-176). The generator's
version has the packages' `MAJOR.MINOR` and a `PATCH` of its own. Every version so far is a
prerelease (`-alpha`), which NuGet takes only when asked: reference a prerelease version in the
`PackageReference`, or pass `--prerelease` to `dotnet add package`. The GitHub Release of each tag
carries the same files; the `dotnet-v*` Release also carries `fletcher-shim-<rid>.conan.lock`, the
lockfile the package README's relinking route rebuilds the native shim with (D-BIND-173).

## Layout

```
dotnet/
  Fletcher.slnx              XML solution (it can carry a licence header; .sln cannot)
  Directory.Build.props      the single <VersionPrefix> for all five packages
  Directory.Build.targets    the AOT toolchain packs, pinned with global.json (D-BIND-81), and the
                             package README packed into all five (README.package.md, D-BIND-142)
  README.package.md          that README: what each package is, a walkthrough whose code CI runs,
                             the platform floors and the relinking route
  global.json                the SDK band, pinned to the devcontainer's
  .editorconfig              what `dotnet format --verify-no-changes` enforces
  src/…                      the five packages
  tests/Fletcher.Tests/      Eiva.Fletcher's and Interop's tests, against the real shim
  tests/Fletcher.Model.Tests/  Model's tests; references Model ALONE, which shows it is Arrow-only
  tests/Fletcher.GatewayClient.Tests/  the gateway client's tests; no shim, no gateway
  tests/Fletcher.CopyOracle.Tests/  the copy oracle over C#'s real publish, against the PROBE shim (BIND-5b)
  tests/Fletcher.AotSmoke/   a NativeAOT consumer of the generated pair, published and run by CI (D-BIND-81)
  tests/Shared/              the protoc-coverage rows in C#, shared by both codecs' tests
  benchmarks/Fletcher.Benchmarks/  the per-row publish benchmark (risk B-2; not in the solution,
                             not run by CI)
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
(c-abi built with `with_probe_shim`, see `c-abi/README.md`), and
`-p:FletcherServerShim=<fletcher-c-abi-server>` (c-abi built with `variant=server`) for
`Fletcher.Tests`' loader-layout rows, which run a test host from staged folders with
either shim or both, and `-p:FletcherHandoverFaultShim=<fletcher-c-abi-handover-fault>` for
`Fletcher.Handover.Tests`: d9's test-only fault shim, built only by c-abi's test build of the server
variant, `(cd c-abi && conan build . -o "&:variant=server" -o "&:run_tests=True" -c tools.build:skip_test=True)`,
under `c-abi/build`. A project whose shim is missing FAILS with the loader's diagnostic
rather than skipping. CI runs `Fletcher.Tests` a second time with the server shim copied
beside it, under its own name, so the whole suite also runs on the server build.

The SDK version is pinned twice on purpose — `global.json` here and
`DOTNET_SDK_VERSION` in `.devcontainer/Dockerfile` — so the container, CI and a
developer's host cannot silently diverge. Bump them together. CI restores with
`--locked-mode`, so a dependency change must arrive with its updated
`packages.lock.json`; those files are committed.

The test projects target `net8.0;net10.0`, as the libraries do, and roll forward to the
installed runtime, so the net8.0-compiled assemblies are executed, not merely compiled.
`Fletcher.Model.Tests` and `Fletcher.GatewayClient.Tests` load no shim.

The suites that need a real transport, a gateway or generated code live under
[`integration-tests/`](../integration-tests/) and are deliberately not in `Fletcher.slnx`, so
`dotnet test Fletcher.slnx` stays a unit run:

| Suite | What it proves |
|---|---|
| [`protoc-dotnet`](../integration-tests/protoc-dotnet/README.md) | the generated model and accessor, run from a project's own build, compile against `Apache.Arrow` and `Eiva.Fletcher.Model` alone, match C++'s schemas and round-trip; the accessor capstone's C# arm |
| [`pubsub-transport-conformance`](../integration-tests/pubsub-transport-conformance/README.md) | one body of C# publishes and subscribes over `inprocess`, `fastdds` and `xrce`, chosen by name, with no per-transport code |
| [`binding-abi-conformance`](../integration-tests/binding-abi-conformance/README.md) | `Apache.Arrow` hands the codec what Arrow C++ hands it, so a C# publisher puts C++'s bytes on the wire |
| [`gateway-dotnet-end-to-end`](../integration-tests/gateway-dotnet-end-to-end/README.md) | `FletcherClient` against the real gateway, over `inprocess` and `fastdds` |
| [`gateway-dotnet-crosslang`](../integration-tests/gateway-dotnet-crosslang/README.md) | the cross-language matrix's C# rows, both directions, every field compared |
| [`preview-bundle-consumer`](../integration-tests/preview-bundle-consumer/README.md) | an application built from the packed packages alone, the package README's code among it |

The cross-language matrix as a whole, with every lane, is in the root README's
[Languages](../README.md#languages) section.

## NativeAOT

*The facts are D-BIND-81's; the package README (`README.package.md`) carries the short form.*

The four packages with an assembly are marked `IsAotCompatible` and raise no trim or AOT warnings
(`Eiva.Fletcher.GatewayClient` since BIND-8d). Whether an application uses NativeAOT is the
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
`win-x64`); the Linux shim needs **glibc 2.34 and `GLIBCXX_3.4.32` or newer** and only the
system's `libstdc++`, `libm`, `libgcc_s` and `libc`; and the shim must come from the
same commit as the managed packages: its binding ABI version is checked at load (exact
minor before 1.0), nothing finer. Proven by `tests/Fletcher.AotSmoke`, published and run in CI
on Linux and on Windows (`linux-x64` and `win-x64`, .NET 8 and .NET 10).

## One shim per process

A process holds exactly one copy of the native shim (D-BIND-17). Fletcher's re-entrancy guard is
per copy, so a second copy in one process would serve a re-entrant call that must be refused,
silently. The shim therefore links every Fletcher component it uses, and when it loads it looks for
another module exporting its marker (`fl_single_copy_marker`); if it finds one, every call that can
fail is refused, with a message naming both modules. For an application this means no second
shim loaded beside the one the packages bring (the edge shim from `Eiva.Fletcher.Interop`, or the
server shim from `Eiva.Fletcher.Server`), for example by a plugin that carries its own. The case it cannot detect, a C++ host that links Fletcher statically and also
hosts .NET, is ruled out rather than checked: the consumers are .NET processes hosting this binding
only.

**Which shim loads (D10C-4).** The loader looks only in the app's own folders: beside
`Eiva.Fletcher.Interop.dll`, then beside the application, each flat and under
`runtimes/<os>-<arch>/native/` (the folder named from the operating system and the process's
architecture, so a distro-built .NET still finds `linux-x64`). If a server shim
(`fletcher-c-abi-server`) is in any of them, it is loaded; otherwise the edge shim
(`fletcher-c-abi`). The first file present is loaded, once; if the operating system refuses it,
the app stops with an exception naming the file and the reason, and no other copy is tried. The
operating system's library path (PATH, LD_LIBRARY_PATH) is not searched, so an app that found
the shim only through it now stops at start-up, listing the folders searched; so does a
single-file app published with `IncludeNativeLibrariesForSelfExtract=true`, whose shim is
extracted elsewhere, and so does a host that loads the package straight from the NuGet cache
(.NET Interactive's `#r "nuget:"`, dotnet-script, F# Interactive), where the package's
`runtimes/` folder is not beside the loaded assembly. None of these was documented or tested.
A file that is present but will not load is not skipped: the message names it, says which
build it is, and the remedy is to remove it or replace it. The choice is made once per
`AssemblyLoadContext`, and a refused load stays refused there; it reaches the app as a
`TypeInitializationException` whose inner `DllNotFoundException` names the file and the reason.

A transport is chosen by its selector string, and C# never implements one (D-BIND-24): the shim
registers the three built-in providers itself, and managed code can neither register a provider nor
load one by path.

## Licences

The packages are `LGPL-3.0-or-later`. Each one packs `LICENSE`, `COPYING` (the GPL-3.0 text the
LGPL-3.0 incorporates, D-BIND-152) and `THIRD-PARTY-LICENSES.txt`, which lists the libraries linked
into the shim with their licence texts and is derived from the shim's Conan graph; the pack takes
them from `-p:FletcherLicenceDir=<dir>`, which CI's pack job passes (`Directory.Build.props`). The
package README states the relinking route: how to rebuild the shim from the commit the package
names, with the Conan lockfile attached to the release, and put it in place of the shipped one
(D-BIND-140, D-BIND-173).

The four managed packages (`Eiva.Fletcher`, `.Interop`, `.Model`, `.GatewayClient`) also pack
`LICENSE-EXCEPTION`, the Fletcher Linking Exception v1.1 (D-BIND-175), approved by EIVA A/S's legal
counsel (ruling 2026-11-17); `Eiva.Fletcher.Protoc` packs it too, and the `dotnet-v*` and
`protoc-v*` Releases attach it. It is an additional permission under GPL-3.0 section 7: an
application built with the managed assemblies, unmodified as a Release ships them, may be conveyed
under terms of its author's choice, NativeAOT, single-file and trimmed builds included, without
LGPL sections 4(d) and 4(e) for those assemblies but with 4(a) to 4(c); code generated from
`options.proto` and the generator's output are the consumer's to license. A modified assembly and
every native library, both shims included, stay under the LGPL alone, so the relinking route above
still applies to the shims; `Eiva.Fletcher.Server`, which carries only the server shim, does not
pack the exception, and `check_dotnet_packages.sh` refuses it there and requires it in the four.
It takes effect with the first Release that carries it (its section 9). Whether LGPL sections 4(b)
and 4(c) can be met for single-file, mobile or embedded builds is unresolved, and the exception
does not relax them. EIVA A/S's copy in the repository and its Releases is the authoritative text;
the earlier public draft (v1.0) was never in force. `PackageLicenseExpression` stays
`LGPL-3.0-or-later`: NuGet accepts only SPDX identifiers, and the exception only adds permissions.
