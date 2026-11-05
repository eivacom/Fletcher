# Fletcher C# preview @VERSION@

A **preview** build of the Fletcher C# bindings, made from branch `feature/csharp-bindings`
(pull request #129) at commit `@COMMIT@`. It is not a release: nothing here is published to a
feed, and the round has not merged. Every push to the branch makes a new bundle, kept for 30 days.

## What is in it

| Folder | Contents |
|---|---|
| `nuget/` | `Eiva.Fletcher`, `Eiva.Fletcher.Interop` (the native shim for linux-x64 and win-x64), `Eiva.Fletcher.Model`, `Eiva.Fletcher.GatewayClient`, version `@VERSION@`, with their symbol packages; and `Eiva.Fletcher.Protoc`, the code generator for C# projects, at the same version; and `fletcher-shim-linux-x64.conan.lock`, `fletcher-shim-win-x64.conan.lock`, the Conan lockfiles the packages' relinking route uses |
| `gateway/` | The Fletcher gateway built from the same commit: `gateway-linux.tar.gz`, `gateway-windows.zip`. Use this gateway with `Eiva.Fletcher.GatewayClient`; the released 0.5 gateway does not accept the schema the client announces |
| `protoc/` | The code generator outside NuGet, for C++ or a script: `protoc-gen-fletcher-linux-x64`, `protoc-gen-fletcher-windows-x64.exe`, and `fletcher/options.proto` |

Each package's README (`README.md` inside the `.nupkg`, or shown by your IDE's package manager)
walks from a `.proto` file to publishing and subscribing, natively and through the gateway, with
code that CI compiles and runs against these very packages. This file covers what is particular
to the preview.

## Using the packages

Unpack the bundle, then point NuGet at the `nuget/` folder, for example in a `nuget.config`
beside your solution:

```xml
<configuration>
  <packageSources>
    <add key="fletcher-preview" value="path/to/bundle/nuget" />
  </packageSources>
</configuration>
```

and reference the exact version: `dotnet add package Eiva.Fletcher --version @VERSION@`, and
`Eiva.Fletcher.Model`, which generated code always needs (and `Eiva.Fletcher.GatewayClient` to use
a gateway).

## Running the gateway

Unpack `gateway/gateway-linux.tar.gz` or `gateway/gateway-windows.zip` and run it from that
folder, for example `./gateway --port 9090 --provider fastdds` on Linux (after
`chmod +x gateway`: a downloaded zip keeps no executable bit) or
`gateway.exe --port 9090 --provider fastdds` on Windows; `--provider inprocess` keeps it to one
machine. Connect `Eiva.Fletcher.GatewayClient` to `ws://<host>:9090`.

## Generating C# code

In a C# project, reference `Eiva.Fletcher.Protoc` (version `@VERSION@`, `PrivateAssets="all"`): it sets
`FletcherProtocPlugin` to the plugin for your machine and `FletcherProtoInclude` to the folder of
`fletcher/options.proto`, and the packages' README shows the protoc call that uses them. In a
project that compiles `.proto` files with `Grpc.Tools`, it also lets them import
`fletcher/options.proto`. Outside NuGet, use the plugin in `protoc/`.

You need `protoc` itself (protobuf 3.21 or newer, for example from a protobuf release or the
`Google.Protobuf.Tools` package); the plugin in `protoc/` is what adds Fletcher's output:

```
protoc --plugin=protoc-gen-fletcher=path/to/protoc-gen-fletcher-linux-x64 \
       -I path/to/bundle/protoc -I path/to/well-known-protos -I your/protos \
       --fletcher_out=out --fletcher_opt=csharp,csharp_gateway your.proto
```

`path/to/well-known-protos` is the folder holding `google/protobuf/timestamp.proto` and the
other well-known types; a protobuf release has it beside `protoc`, `Google.Protobuf.Tools` in its
`tools/` folder. On Linux, make the plugin executable first
(`chmod +x protoc-gen-fletcher-linux-x64`). Options: `csharp` (the model, topics and the native
pair), `csharp_gateway` (the gateway pair), `csharp_accessor` (the Arrow view and accessor), and
`csharp_model_only` to leave the native pair out. The plugin also writes C++ headers into
`--fletcher_out`; a C# project compiles only the `.cs` files.

## Platform requirements

The floors are those of Fletcher's two Conan build profiles, `Linux-gcc13-x86_64-Release` and
`Windows-msvc194-x86_64-Release`:

- **linux-x64:** glibc 2.34 and the GCC 13 C++ runtime (`GLIBCXX_3.4.32`) or newer for the
  packages, so Ubuntu 24.04, Debian 13, RHEL 10 or newer. The bundled gateway needs glibc 2.38
  and the same C++ runtime, which every one of those distributions has.
- **win-x64:** the latest Visual C++ 2015-2022 redistributable (x64), 14.40 or newer.
- .NET 8 or .NET 10.

## Licence

Fletcher is LGPL-3.0-or-later (`LICENSE`, with `COPYING`, the GNU GPL-3.0 text it incorporates).
The native shim is a shared library that the .NET assembly loads at run time, so you may replace
it with one built from the source of commit `@COMMIT@`; the package README gives the commands and
says where the result goes. An application that links the shim statically into a NativeAOT
binary forms a different combined work, with the obligations that brings. The third-party
libraries in each binary are listed, with their licence texts, in the `THIRD-PARTY-LICENSES.txt`
inside each package, each gateway archive and `protoc/`. Those notices are titled with the
components' own versions (`0.5.0-alpha`): until the merge the preview is built from the 0.5
components and only the packages are numbered `0.6.0-pr.129.*`.

## When 0.6.0-alpha is released

These builds are numbered `0.6.0-pr.129.<run>` (with `.<attempt>` after it when a run was
re-run), which NuGet ranks above the coming `0.6.0-alpha`. When `0.6.0-alpha` is released, delete
the bundle and the `fletcher-preview` package source, clear the NuGet cache
(`dotnet nuget locals all --clear`), and reference `0.6.0-alpha`.
