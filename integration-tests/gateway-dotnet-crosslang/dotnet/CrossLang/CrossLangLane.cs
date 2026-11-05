// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The lane's processes, started once for its tests (D-BIND-116): the gateway over Fast DDS on an
// isolated domain, and the peers, each its own process: the C++ native pair, the C# native pair
// (twice since BIND-9b-4, D-BIND-161: the second only receives NativeToNative), and the
// TypeScript gateway client. The C# gateway client under test is THIS process.
//
// Fletcher's built-in data profile is VOLATILE (qos_defaults.cpp): a reader that matches a
// writer after a row was written never sees it, and across processes Fast DDS discovery takes
// a moment the test cannot observe. So the gateway and the native peers load one document, the
// built-in text with both <durability> lines changed to TRANSIENT_LOCAL, as gateway-fastdds-ts
// does (fastdds-pubsub-provider/README.md, "The published starting point"): rows written before
// a match still reach the reader. The gateway takes it as --provider-config, the peers as their
// provider's document.
//
// Binaries: GATEWAY_BIN and CPP_PEER_BIN, or what `conan build .` left under ../../build;
// NATIVE_PEER_DLL, or ../NativePeer's build output (the ProjectReference builds it first); the
// TypeScript peer runs from ../../ts with its node_modules (`npm ci` there first).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading.Tasks;

using Xunit;

namespace Eiva.Fletcher.CrossLang;

public sealed class CrossLangLane : IAsyncLifetime
{
    public const string DurableDocument = """
        <?xml version="1.0" encoding="UTF-8"?>
        <dds xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">
          <profiles>
            <participant profile_name="fletcher_participant">
              <rtps><name>FletcherParticipant</name></rtps>
            </participant>
            <data_writer profile_name="default_writer" is_default_profile="true">
              <qos>
                <durability><kind>TRANSIENT_LOCAL</kind></durability>
                <reliability>
                  <kind>RELIABLE</kind>
                </reliability>
              </qos>
              <topic>
                <historyQos><kind>KEEP_ALL</kind></historyQos>
                <resourceLimitsQos>
                  <max_samples>100</max_samples>
                  <max_instances>1</max_instances>
                  <max_samples_per_instance>100</max_samples_per_instance>
                  <allocated_samples>100</allocated_samples>
                </resourceLimitsQos>
              </topic>
              <times>
                <heartbeat_period>
                  <sec>0</sec>
                  <nanosec>20000000</nanosec>
                </heartbeat_period>
              </times>
            </data_writer>
            <data_reader profile_name="default_reader" is_default_profile="true">
              <qos>
                <durability><kind>TRANSIENT_LOCAL</kind></durability>
                <reliability><kind>RELIABLE</kind></reliability>
              </qos>
              <topic>
                <historyQos><kind>KEEP_ALL</kind></historyQos>
                <resourceLimitsQos>
                  <max_samples>100</max_samples>
                  <max_instances>1</max_instances>
                  <max_samples_per_instance>100</max_samples_per_instance>
                  <allocated_samples>100</allocated_samples>
                </resourceLimitsQos>
              </topic>
            </data_reader>
          </profiles>
        </dds>
        """;

    private string? _documentDir;

    public int Port { get; } = 19196;

    public string DomainId { get; } = Environment.GetEnvironmentVariable("DDS_DOMAIN_ID") ?? "153";

    public Uri Uri => new($"ws://127.0.0.1:{Port}");

    public PeerProcess Gateway { get; private set; } = null!;

    public PeerProcess Cpp { get; private set; } = null!;

    public PeerProcess Native { get; private set; } = null!;

    public PeerProcess Native2 { get; private set; } = null!;

    public PeerProcess Ts { get; private set; } = null!;

    public async Task InitializeAsync()
    {
        _documentDir = Directory.CreateTempSubdirectory("gateway-dotnet-crosslang-").FullName;
        string document = Path.Combine(_documentDir, "durable.xml");
        await File.WriteAllTextAsync(document, DurableDocument);

        Gateway = await PeerProcess.StartAsync(
            "gateway",
            FindBuilt("GATEWAY_BIN", "gateway"),
            ["--port", $"{Port}", "--bind-address", "127.0.0.1", "--provider", "fastdds", "--domain-id", DomainId, "--provider-config", document]);
        Cpp = await PeerProcess.StartAsync(
            "C++ peer",
            FindBuilt("CPP_PEER_BIN", "crosslang_cpp_peer"),
            ["--domain-id", DomainId, "--document", document]);

        string nativePeer = Environment.GetEnvironmentVariable("NATIVE_PEER_DLL") is { Length: > 0 } fromEnv
            ? fromEnv
            : typeof(CrossLangLane).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>()
                .Single(a => a.Key == "NativePeerDll").Value!;
        string dotnet = Environment.GetEnvironmentVariable("DOTNET_HOST_PATH") is { Length: > 0 } host ? host : "dotnet";
        Native = await PeerProcess.StartAsync(
            "C# native peer",
            dotnet,
            [nativePeer, "--domain-id", DomainId, "--document", document, "--role", "primary"]);
        // BIND-9b-4 (D-BIND-161): C# native to C# native across processes needs a second one.
        Native2 = await PeerProcess.StartAsync(
            "second C# native peer",
            dotnet,
            [nativePeer, "--domain-id", DomainId, "--document", document, "--role", "secondary"]);

        string tsDir = Path.Combine(LaneDir(), "ts");
        Ts = await PeerProcess.StartAsync(
            "TypeScript peer",
            "node",
            [Path.Combine(tsDir, "node_modules", "tsx", "dist", "cli.mjs"), Path.Combine("src", "peer.ts"), "--url", Uri.ToString()],
            tsDir);
    }

    public async Task DisposeAsync()
    {
        foreach (PeerProcess? peer in new[] { Ts, Native2, Native, Cpp, Gateway })
        {
            if (peer is not null)
            {
                await peer.DisposeAsync();
            }
        }

        if (_documentDir is not null)
        {
            Directory.Delete(_documentDir, recursive: true);
        }
    }

    // integration-tests/gateway-dotnet-crosslang, from bin/<config>/<tfm>/ under dotnet/CrossLang.
    private static string LaneDir() =>
        Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", ".."));

    private static string FindBuilt(string envVar, string name)
    {
        if (Environment.GetEnvironmentVariable(envVar) is { Length: > 0 } fromEnv)
        {
            return fromEnv;
        }

        string file = OperatingSystem.IsWindows() ? name + ".exe" : name;
        string build = Path.Combine(LaneDir(), "build");
        string? found = Directory.Exists(build)
            ? Directory.EnumerateFiles(build, file, SearchOption.AllDirectories).FirstOrDefault()
            : null;
        return found ?? throw new FileNotFoundException(
            $"{file} not found under {build}: run `conan build .` in integration-tests/gateway-dotnet-crosslang, or set {envVar}");
    }
}
