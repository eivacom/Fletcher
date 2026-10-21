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
  Arrow batch read through the generated accessor, and check that the shim that answered is the
  one `Eiva.Fletcher.Interop` put under `runtimes/<rid>/native/`.
- **Reach the bundled gateway** through `Eiva.Fletcher.GatewayClient`: the bundle's gateway is
  started over `inprocess`, and the generated gateway pair carries the same rows across it, row by
  row and as one batch.

**The package README's code** (BIND-9a-4): every code block in
[`dotnet/README.package.md`](../../dotnet/README.package.md) is a region of `fix.proto`,
`PreviewConsumer.csproj` or `Program.cs` between `<snippet name="...">` markers, and
[`scripts/check_readme_snippets.py`](../../scripts/check_readme_snippets.py), run by `ci.dotnet`'s
pack job, fails when the two differ or one is missing; so the walkthrough every package carries is
code this test compiles and runs.

Each route must deliver exactly the rows sent, field for field (a nested message, a Timestamp, a
repeated field, a map); the program's exit code is the verdict. It runs once per target framework
the packages ship, .NET 8 and .NET 10.

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
