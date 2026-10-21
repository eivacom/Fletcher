# Integration test: a consumer of the preview bundle

What a tester does with `fletcher-preview-0.6.0-pr.129.<run>` (BIND-9a-2, D-BIND-139), using the
bundle and nothing else built from this repository. It is the forcing test for BIND-9a's packages,
gateway and plugin working together as shipped, on Linux and Windows.

## What it covers

[`run.sh`](run.sh), given an unpacked bundle:

- **Restore from the bundle alone.** A `nuget.config` with package source mapping takes every
  `Eiva.Fletcher*` package from the bundle's `nuget/` and from no other source, into an empty
  package cache; `Apache.Arrow` and `Google.Protobuf.Tools` (for `protoc`) come from nuget.org.
- **Generate with the bundled plugin.** [`PreviewConsumer.csproj`](dotnet/PreviewConsumer/PreviewConsumer.csproj)
  runs `protoc` with the bundle's `protoc-gen-fletcher` over
  [`fix.proto`](dotnet/PreviewConsumer/fix.proto) (`csharp,csharp_gateway,csharp_accessor`: the
  model, the native pair, the gateway pair and the Arrow accessor), with the bundle's `protoc/` on
  the include path.
- **Publish and subscribe natively** through `Eiva.Fletcher` over `inprocess`, row by row and as one
  Arrow batch read through the generated accessor; then over `fastdds`, in this one process on a
  domain of its own (D-BIND-151; `FLETCHER_PREVIEW_DDS_DOMAIN`, 157 by default), after a probe
  row shows discovery is done; and check that the shim that answered is the one
  `Eiva.Fletcher.Interop` put under `runtimes/<rid>/native/`.
- **Reach the bundled gateway** through `Eiva.Fletcher.GatewayClient`: the bundle's gateway is
  started over `inprocess`, and the generated gateway pair carries the same rows across it, row by
  row and as one batch.

Each route must deliver exactly the rows sent, field for field (a nested message, a Timestamp, a
repeated field, a map), and nothing more: a route that receives through a queue waits briefly
after the last row, and an extra or duplicate delivery fails it. The program's exit code is the
verdict. It runs once per target framework the packages ship, .NET 8 and .NET 10, and fails if
the net8.0 build is not running on .NET 8 (set `FLETCHER_PREVIEW_ALLOW_ROLL_FORWARD=1` to rehearse
on a machine with only a newer runtime).

## Snippets: the package README's code is this test's code

Every fenced code block in [`dotnet/README.package.md`](../../dotnet/README.package.md), the
README all four packages carry, is checked by
[`scripts/check_readme_snippets.py`](../../scripts/check_readme_snippets.py), which `ci.dotnet`'s
pack job runs before packing (D-BIND-144, D-BIND-147):

- **Here**, a region is marked by two comment lines in `fix.proto`, `PreviewConsumer.csproj` or
  `Program.cs`: one containing `<snippet name="NAME">` (`// <snippet name="native-rows">`, or
  `<!-- <snippet name="csproj-generate"> -->` in the project) and one containing `</snippet>`.
- **In the README**, the line above a code block is `<!-- snippet: NAME -->`, and the block must
  equal the region `NAME` with its common indentation removed. A block that is not test code
  (shell commands) is marked `<!-- snippet: none -->` instead.
- The check fails when a block and its region differ, when a region is not shown or a shown
  snippet has no region, when a code block has no marker, and when there is no snippet at all.

To add one: write the code in `Program.cs` (it then compiles and runs on every push), put the two
marker lines around it, and paste the region, unindented, into the README under its marker.

## How it runs

```bash
bash integration-tests/preview-bundle-consumer/run.sh <unpacked bundle> [<work folder>]
```

The project is copied into the work folder (a temporary folder by default) so that nothing of the
repository's build applies to it. Linux, or Git Bash on Windows; it needs the .NET 8 and .NET 10
runtimes and network access to nuget.org.

CI: the `preview-consumer` job in [`ci.pr.yml`](../../.github/workflows/ci.pr.yml), after
`preview-bundle`, on plain `ubuntu-24.04` and `windows-2022` runners (not the devcontainer), with
only this folder checked out. It runs on pushes to `feature/csharp-bindings`, where the bundle is
built, and goes away with the bundle when the round merges (BIND-9b).
