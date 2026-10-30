<!-- SPDX-License-Identifier: LGPL-3.0-or-later
     Copyright (C) 2026 The Fletcher Authors

     Maintainers: every fenced code block here is checked when the package is packed
     (scripts/check_readme_snippets.py, run by ci.protoc's pack-nuget job; BIND-9b review B1). A
     block must equal the region of the same name in
     integration-tests/preview-bundle-consumer/dotnet/PreviewConsumer/PreviewConsumer.csproj, which
     the consumer test builds and runs; dotnet/README.package.md says how the markers work.
-->
# Eiva.Fletcher.Protoc

`protoc-gen-fletcher`, the Fletcher protoc plugin, for C# projects. It generates the C# that the
`Eiva.Fletcher` packages consume (model classes, Arrow schemas, topics, the native and gateway
publisher and subscriber pairs, the accessor) from your `.proto` files.

The package only locates the plugin. You write the protoc call in your own project, as C++ and
TypeScript projects do, and take protoc itself from your side, for example from
`Google.Protobuf.Tools`.

## What it gives you

| Property | Value |
|---|---|
| `FletcherProtocPlugin` | The plugin for this machine (linux-x64 or win-x64), for protoc's `--plugin=protoc-gen-fletcher=...` |
| `FletcherProtoInclude` | The folder holding `fletcher/options.proto`, for protoc's `-I` |

On Linux the package makes the plugin executable before your build steps run. On any other
platform `FletcherProtocPlugin` is empty and the build warns (`FLETCHERPROTOC001`).

## Platform requirements

The plugin loads the C++ runtime of the compiler it is built with, so the machine that builds
your project needs:

- **linux-x64:** glibc 2.34 and the GCC 13 C++ runtime (libstdc++ with `GLIBCXX_3.4.32`) or
  newer; for example Ubuntu 24.04, Debian 13 or RHEL 10. On Ubuntu 22.04, Debian 12 or RHEL 9 the
  plugin does not start, and the system reports `GLIBCXX_3.4.32' not found`.
- **win-x64:** the Visual C++ 2015-2022 redistributable (x64), 14.40 or newer; without it the
  plugin does not start, because `MSVCP140.dll` and `VCRUNTIME140.dll` are missing.

These are the floors of the build profiles, `Linux-gcc13-x86_64-Release` and
`Windows-msvc194-x86_64-Release`, the same as the `Eiva.Fletcher` packages' native shim.

## Generating C#

Reference the packages; `FletcherVersion` is the `Eiva.Fletcher` packages' version and
`FletcherProtocVersion` this package's, with the same `MAJOR.MINOR`. `Google.Protobuf.Tools`
brings `protoc`. A project that only generates code needs `Eiva.Fletcher.Model` and the package
its generated code uses (`Eiva.Fletcher` for the native pair, `Eiva.Fletcher.GatewayClient` for
the gateway pair).

<!-- snippet: csproj-references -->
```xml
<ItemGroup>
  <PackageReference Include="Eiva.Fletcher" Version="[$(FletcherVersion)]" />
  <PackageReference Include="Eiva.Fletcher.GatewayClient" Version="[$(FletcherVersion)]" />
  <PackageReference Include="Eiva.Fletcher.Model" Version="[$(FletcherVersion)]" />
  <!-- The plugin, and protoc with the well-known .proto files, used at build time only. -->
  <PackageReference Include="Eiva.Fletcher.Protoc" Version="[$(FletcherProtocVersion)]" PrivateAssets="all" />
  <PackageReference Include="Google.Protobuf.Tools" Version="[3.36.1]" GeneratePathProperty="true" PrivateAssets="all" />
</ItemGroup>
```

Then call protoc with the plugin on your `.proto` files (`fix.proto` here) before every compile.
Its output folder must exist, and on Linux protoc from a NuGet package must be made executable
(this package does that for the plugin).
`--fletcher_opt` chooses the output: `csharp` (the model, topics and the native pair),
`csharp_gateway`, `csharp_accessor`, and `csharp_model_only` to leave the native pair out.

<!-- snippet: csproj-generate -->
```xml
<PropertyGroup>
  <Protoc Condition="$([MSBuild]::IsOSPlatform('Windows'))">$(PkgGoogle_Protobuf_Tools)/tools/windows_x64/protoc.exe</Protoc>
  <Protoc Condition="!$([MSBuild]::IsOSPlatform('Windows'))">$(PkgGoogle_Protobuf_Tools)/tools/linux_x64/protoc</Protoc>
</PropertyGroup>

<Target Name="FletcherGenerate" BeforeTargets="BeforeCompile;CoreCompile">
  <PropertyGroup>
    <FletcherOut>$(IntermediateOutputPath)fletcher</FletcherOut>
  </PropertyGroup>
  <!-- A NuGet package does not keep the executable bit (Eiva.Fletcher.Protoc sets its own). -->
  <Exec Condition="!$([MSBuild]::IsOSPlatform('Windows'))" Command="chmod +x &quot;$(Protoc)&quot;" />
  <RemoveDir Directories="$(FletcherOut)" />
  <MakeDir Directories="$(FletcherOut)" />
  <Exec Command="&quot;$(Protoc)&quot; --plugin=protoc-gen-fletcher=&quot;$(FletcherProtocPlugin)&quot; --fletcher_out=&quot;$(FletcherOut)&quot; --fletcher_opt=csharp,csharp_gateway,csharp_accessor -I . -I &quot;$(FletcherProtoInclude)&quot; -I &quot;$(PkgGoogle_Protobuf_Tools)/tools&quot; fix.proto" />
  <ItemGroup>
    <Compile Include="$(FletcherOut)/*.cs" />
  </ItemGroup>
</Target>
```

Both blocks are the project the release's consumer test builds and runs on Linux and Windows; the
`Eiva.Fletcher` package README walks through the rest of it.

## Standard gRPC projects

In a project that compiles `.proto` files with `Grpc.Tools` (`<Protobuf>` items), the package also
lets those files import `fletcher/options.proto`: it adds `FletcherProtoInclude` to every
`<Protobuf>` item's `AdditionalImportDirs` and compiles `options.proto` itself, so the options can
be read from C# (`Eiva.Fletcher.Protobuf.OptionsExtensions`). Set
`FletcherCompileOptionsProto=false` when a project you reference already compiles it. Neither
`Grpc.Tools` nor `Google.Protobuf.Tools` is a dependency.

## Which protoc

The plugin is built with protobuf 3.21.12 and is tested with the protoc of
`Google.Protobuf.Tools` 3.36.1 and of `Grpc.Tools` 2.80.0. It supports proto2 and proto3, including
proto3 `optional`; it does not support editions (`edition = "2023"`), and protoc refuses such a
file.

## Versions

Generated code and the `Eiva.Fletcher` runtime packages are compatible when their
`MAJOR.MINOR` versions are equal.

Released versions are on nuget.org, and the GitHub Release `protoc-v<version>` of
[eivacom/Fletcher](https://github.com/eivacom/Fletcher) carries the same file. A prerelease
version (`-alpha`) is taken only when the reference names one, or with
`dotnet add package --prerelease`.

## Licence

LGPL-3.0-or-later (`LICENSE`, with the GPL text it incorporates in `COPYING`). The plugin links
protobuf and zlib statically and compiles in nanoarrow; their licences are in
`THIRD-PARTY-LICENSES.txt`.
