<!--
  Maintainers: every fenced code block in this file is checked when the packages are built
  (scripts/check_readme_snippets.py, run by ci.dotnet's pack job; D-BIND-144, D-BIND-147).
  The line above each block is a marker, an HTML comment holding "snippet: NAME" or
  "snippet: none" (the blocks below show the exact form):
    snippet: NAME   the block must equal the region NAME, which the consumer test marks with
                    <snippet name="NAME"> and </snippet> comment lines in
                    integration-tests/preview-bundle-consumer/dotnet/PreviewConsumer/
                    (fix.proto, PreviewConsumer.csproj, Program.cs), and which CI builds and
                    runs against these packages on Linux and Windows;
    snippet: none   the block is not test code (shell commands, say): a visible decision.
  A block with no marker fails the build, as does a region no block shows. The version and
  commit placeholders are filled in at pack time (dotnet/Directory.Build.targets).
-->
# Fletcher for .NET

Arrow-typed publish/subscribe for .NET over Fletcher's one wire format, with the codec running
natively. Version `@VERSION@`, built from commit `@COMMIT@` of
[eivacom/Fletcher](https://github.com/eivacom/Fletcher). The four packages share one version and
are used together at the same version.

| Package | What it is | Native assets |
|---|---|---|
| `Eiva.Fletcher` | Publishers, subscribers, the codec and the Arrow tier, over a provider chosen by name (`inprocess`, `fastdds`, `xrce`) | through `Eiva.Fletcher.Interop` |
| `Eiva.Fletcher.Interop` | The P/Invoke surface and the native shim for `linux-x64` and `win-x64`; referenced by `Eiva.Fletcher`, not used directly | **yes**, `runtimes/{rid}/native/` |
| `Eiva.Fletcher.GatewayClient` | A managed WebSocket client for the Fletcher gateway | none |
| `Eiva.Fletcher.Model` | The types generated C# refers to: lossless `Timestamp` and `Duration`, `ListView` and `MapView` | none |

An application that publishes and subscribes natively references `Eiva.Fletcher`; one that talks
to a gateway references `Eiva.Fletcher.GatewayClient`. Both reference `Eiva.Fletcher.Model` for
their generated code. The C# for your messages comes from Fletcher's `protoc` plugin,
`protoc-gen-fletcher`, which the `Eiva.Fletcher.Protoc` package brings (`--fletcher_opt=csharp` for the model and the native publisher and
subscriber, `csharp_gateway` for the gateway pair, `csharp_accessor` for the Arrow view, and
`csharp_model_only` to leave the native pair out).

Every package targets .NET 8 and .NET 10.

## From a `.proto` to publish and subscribe

Every snippet below is taken from a test that compiles and runs it against these packages on
Linux and Windows, so it works as shown.

**1. Describe the data.** A message is a row; a `service` method that takes a `stream` of it and
returns `google.protobuf.Empty` names a topic, here `preview.consumer/FixFeed/Report`, and the
generated classes are named after it (`FixFeed_Report...`). The message must be declared in the
same file, and a method that streams its replies is skipped. A `google.protobuf.Timestamp` becomes a lossless
`Eiva.Fletcher.Model.Timestamp`, a `repeated` field a list, a `map` a list of key/value pairs.

<!-- snippet: proto -->
```proto
syntax = "proto3";

package preview.consumer;

import "google/protobuf/empty.proto";
import "google/protobuf/timestamp.proto";

message Position {
  double lat = 1;
  double lon = 2;
}

message Fix {
  int32 id = 1;
  string label = 2;
  Position at = 3;
  google.protobuf.Timestamp when = 4;
  repeated double readings = 5;
  map<string, int32> tags = 6;
}

// One topic per method that takes a stream: preview.consumer/FixFeed/Report carries Fix rows.
service FixFeed {
  rpc Report(stream Fix) returns (google.protobuf.Empty);
}
```

**2. Reference the packages** in your project. `FletcherVersion` is the version of these
packages, `@VERSION@`. `Eiva.Fletcher.Protoc` brings the code generator, `protoc-gen-fletcher`;
`FletcherProtocVersion` is its version, which has the same `MAJOR.MINOR` as these packages.
`Google.Protobuf.Tools` brings `protoc`.

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

**3. Generate the C# on every build.** `Eiva.Fletcher.Protoc` sets `FletcherProtocPlugin`, the
path to `protoc-gen-fletcher` for this machine, and `FletcherProtoInclude`, the folder holding its
`fletcher/options.proto`. The options choose what
is generated: `csharp` the model and the native publisher and subscriber, `csharp_gateway` the
gateway publisher and subscriber, `csharp_accessor` the Arrow accessor and view; add
`csharp_model_only` to leave the native pair out of an application that only talks to a gateway.

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

**4. Publish and subscribe rows natively** through `Eiva.Fletcher`. A provider is chosen by name:
`inprocess` within one process, `fastdds` across processes and machines (set
`ProviderConfig.DomainId` to keep applications apart), `xrce` through a Micro XRCE-DDS agent.
The handler is called with each row and its attachments.

<!-- snippet: native-rows -->
```csharp
using PubSubProviderHandle provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());

using var subscriber = new FixFeed_ReportSubscriber(provider);
using Subscription subscription = subscriber.Subscribe((fix, attachments) => received.Add(fix));

using var publisher = new FixFeed_ReportPublisher(provider);
publisher.Publish(fixes);
```

**5. Or receive them as Arrow batches**, for volume: `SubscriberArrow` collects rows into a
window, flushed at `MaxRows` or after `Timeout` (8000 rows or one minute by default), and the
generated accessor reads the batch, as model objects (`ToRow`) or in place (`At`). An Arrow
subscriber reads rows with the topic's schema, which comes from its publisher. Over `fastdds` the
schema arrives with the data, so the order does not matter. Over the default `inprocess` provider,
a subscription made before the topic is declared stays without a schema, and every row on it is
dropped and counted, so declare the publisher first. The handler owns the batch, and its calls are
not serialised: a window flushed at `MaxRows` runs on the delivery thread, one flushed by `Timeout`
on a timer thread, and the last one on the thread that unsubscribes, so two may run at once and
complete out of order.

<!-- snippet: native-batch -->
```csharp
using PubSubProviderHandle provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());

// The publisher declares the topic and its schema, which an Arrow subscriber reads rows with.
using var publisher = new FixFeed_ReportPublisher(provider);

using var subscriber = new SubscriberArrow(provider);
SubscribeResult result = subscriber.SubscribeBatched(FixFeed_ReportPublisher.Topic, (batch, attachments, status) =>
{
    using (batch)
    {
        if (FixAccessor.TryMake(batch, out FixAccessor? rows, out string? error))
        {
            for (int i = 0; i < rows.Length; ++i)
            {
                received.Add(rows.ToRow(i)); // or rows.At(i).Label, read in place
            }
        }
    }
}, new BatchOptions { MaxRows = fixes.Length, Timeout = TimeSpan.FromSeconds(10) });
using Subscription subscription = result.Subscription;
using SchemaArrival schema = result.Schema;

publisher.Publish(fixes);
```

**6. Through a Fletcher gateway** with `Eiva.Fletcher.GatewayClient`, which loads no native code:
the generated gateway pair borrows a connected `FletcherClient`, and the publisher declares the
topic on the gateway with the message's schema.

<!-- snippet: gateway-rows -->
```csharp
await using var publisherClient = new FletcherClient(new FletcherClientOptions { Uri = uri });
await using var subscriberClient = new FletcherClient(new FletcherClientOptions { Uri = uri });
await publisherClient.ConnectAsync();
await subscriberClient.ConnectAsync();

FixFeed_ReportGatewayPublisher publisher = await FixFeed_ReportGatewayPublisher.CreateAsync(publisherClient);
var subscriber = new FixFeed_ReportGatewaySubscriber(subscriberClient);
ulong subscription = await subscriber.SubscribeAsync((fix, envelope) => received.Add(fix));

await publisher.PublishAsync(fixes);
```

Batches from a gateway are cut the same way, on the client, with the same defaults. Here the
handler is synchronous and never runs concurrently with itself, and its windows arrive in order.

<!-- snippet: gateway-batch -->
```csharp
await subscriberClient.SubscribeBatchedAsync(FixFeed_ReportGatewayPublisher.TopicKey, Fix.Schema, (batch, envelopes, status) =>
{
    using (batch)
    {
        if (FixAccessor.TryMake(batch, out FixAccessor? rows, out string? error))
        {
            for (int i = 0; i < rows.Length; ++i)
            {
                received.Add(rows.ToRow(i));
            }
        }
    }
}, new GatewayBatchOptions { MaxRows = fixes.Length, Timeout = TimeSpan.FromSeconds(10) });
```

A row handler runs on the provider's or the client's delivery thread, one call at a time. Over
`inprocess` that is the publishing thread, before `Publish` returns, which is why step 4 can collect
into a plain list; over `fastdds` and through a gateway it is another thread, so hand rows to a queue
or a collection that is safe to use from it.

## The native shim (`Eiva.Fletcher`, `Eiva.Fletcher.Interop`)

`Eiva.Fletcher.Interop` carries one native file per platform, the shim `fletcher-c-abi`. It is
self-contained: Fletcher's core, pub/sub and providers, Fast DDS, Micro XRCE-DDS and nanoarrow are
linked into it. `Eiva.Fletcher.GatewayClient` and `Eiva.Fletcher.Model` load no native code, and
nothing in this section applies to them.

### Platform requirements

- **linux-x64:** glibc 2.38 and the GCC 13 C++ runtime (`libstdc++` with `GLIBCXX_3.4.32`) or
  newer: Ubuntu 24.04, Debian 13, RHEL 10 or newer. The shim loads only `libstdc++`, `libgcc_s`,
  `libm` and `libc`.
- **win-x64:** the latest Visual C++ 2015-2022 redistributable (x64), 14.40 or newer. Otherwise
  the shim loads only DLLs every Windows has.

When the shim is loaded, `Eiva.Fletcher.Interop` checks that it speaks the binding ABI this
package was built against, and refuses one that does not, with a message saying so. It does not
check the package version or the commit, so use a shim built from the commit this package names.

### Replacing the shim (LGPL-3.0)

Fletcher is licensed under the LGPL-3.0-or-later (`LICENSE` in this package, with `COPYING`, the
GNU GPL-3.0 text it incorporates). The shim is a shared library that the .NET assembly loads at run
time, so you may replace it with one you build from the corresponding source, commit `@COMMIT@`.
With [Conan 2](https://conan.io) and a C++ toolchain, from the repository's root, where `<profile>`
is `Linux-gcc13-x86_64-Release` or `Windows-msvc194-x86_64-Release`:

<!-- snippet: none -->
```bash
git clone https://github.com/eivacom/Fletcher.git
cd Fletcher
git checkout @COMMIT@
for component in core pubsub fastdds-pubsub-provider xrcedds-pubsub-provider c-abi; do
  conan create $component --build=missing -pr:a=.conan-profiles/<profile>
done
conan install --requires="fletcher-c-abi/[*, include_prerelease]" --deployer=direct_deploy --deployer-folder=shim -pr:a=.conan-profiles/<profile>
```

The shim is then `shim/direct_deploy/fletcher-c-abi/lib/libfletcher-c-abi.so` (Linux) or
`shim/direct_deploy/fletcher-c-abi/bin/fletcher-c-abi.dll` (Windows). Put it in place of the file
of the same name under `runtimes/<rid>/native/` in this package, or of the copy beside your
published application.

**NativeAOT.** The packages are trimming- and NativeAOT-compatible (a NativeAOT consumer is
published and run in CI on Linux; Windows NativeAOT is not yet exercised), and the shim stays a
separate shared library beside a NativeAOT application. Linking the shim statically into the application is
not supported, and an application that did so would form a different combined work under the
LGPL, with the obligations that brings.

## Third-party licences

`THIRD-PARTY-LICENSES.txt` in this package lists the third-party libraries linked into the shim,
with their licence texts.
