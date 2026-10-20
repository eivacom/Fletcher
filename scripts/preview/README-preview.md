# Fletcher C# preview @VERSION@

A **preview** build of the Fletcher C# bindings, made from branch `feature/csharp-bindings`
(pull request #129) at commit `@COMMIT@`. It is not a release: nothing here is published to a
feed, and the round has not merged. Every push to the branch makes a new bundle, kept for 30 days.

## What is in it

| Folder | Contents |
|---|---|
| `nuget/` | `Eiva.Fletcher`, `Eiva.Fletcher.Interop` (the native shim for linux-x64 and win-x64), `Eiva.Fletcher.Model`, `Eiva.Fletcher.GatewayClient`, version `@VERSION@`, with their symbol packages |
| `gateway/` | The Fletcher gateway built from the same commit: `gateway-linux.tar.gz`, `gateway-windows.zip`. Use this gateway with `Eiva.Fletcher.GatewayClient`; the released 0.5 gateway does not accept the schema the client announces |
| `protoc/` | The code generator: `protoc-gen-fletcher-linux-x64`, `protoc-gen-fletcher-windows-x64.exe`, and `fletcher/options.proto` |

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

and reference the exact version: `dotnet add package Eiva.Fletcher --version @VERSION@` (and
`Eiva.Fletcher.GatewayClient`, `Eiva.Fletcher.Model` as needed).

## Running the gateway

Unpack `gateway/gateway-linux.tar.gz` or `gateway/gateway-windows.zip` and run, for example,
`gateway --port 9090 --provider fastdds` (or `--provider inprocess` for one machine); connect
`Eiva.Fletcher.GatewayClient` to `ws://<host>:9090`.

## Generating C# code

You need `protoc` itself (protobuf 3.21 or newer, for example from a protobuf release or the
`Google.Protobuf.Tools` package); the plugin in `protoc/` is what adds Fletcher's output:

```
protoc --plugin=protoc-gen-fletcher=path/to/protoc-gen-fletcher-linux-x64 \
       -I path/to/bundle/protoc -I your/protos \
       --fletcher_out=out --fletcher_opt=csharp,csharp_gateway your.proto
```

On Linux, make the plugin executable first (`chmod +x protoc-gen-fletcher-linux-x64`): a
downloaded zip does not keep the executable bit. Options: `csharp` (model, topics, native pair),
`csharp_accessor` (Arrow view and accessor), `csharp_gateway` (gateway pair).

## Platform requirements

- **linux-x64:** glibc 2.38 and the GCC 13 C++ runtime (`GLIBCXX_3.4.32`) or newer, so Ubuntu
  24.04, Debian 13, RHEL 10 or newer.
- **win-x64:** the Microsoft Visual C++ 2015-2022 redistributable (x64).
- .NET 8 or .NET 10.

## Licence

Fletcher is LGPL-3.0-or-later (`LICENSE`). The native shim is a shared library that the .NET
assembly loads at run time: you may replace `runtimes/<rid>/native/` in `Eiva.Fletcher.Interop`
with a shim built from the source of commit `@COMMIT@`
(https://github.com/eivacom/Fletcher/tree/@COMMIT@, `c-abi/`, `conan create`). An application
that links the shim statically into a NativeAOT binary forms a different combined work, with the
obligations that brings. The third-party libraries in each binary are listed, with their licence
texts, in the `THIRD-PARTY-LICENSES.txt` inside each package, each gateway archive and `protoc/`.

## When 0.6.0-alpha is released

These builds are numbered `0.6.0-pr.129.<run>`, which NuGet ranks above the coming
`0.6.0-alpha`. When `0.6.0-alpha` is released, delete the bundle and the `fletcher-preview`
package source, and clear the NuGet cache (`dotnet nuget locals all --clear`).
