// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-8a: the JSON control messages, the schema descriptor and schemaIpc (Bucket 2).
//
// gateway-client-ts/test/ws-protocol.test.ts's text cases, ported: four request builders,
// seven reply parses. Then gateway/tests/test_schema_codec.cpp from the other side of the
// wire: that file tests the GATEWAY's parser of a create_topic schema, and what a client
// owes it is a descriptor that parser accepts. So each of its cases becomes a property of
// the descriptor C# writes (the accepted shape for its two building cases, "never the
// shape it refuses" for its seven refusals, and its one documented gap: a nested field,
// which C# describes in full and today's gateway refuses). Its eleventh case, the
// wire-type round trip, is WireTypeTests.
//
// Last, the generator's own schemas (protoc/tests/golden/*.ipc): each opens the managed
// codec (as SchemaIpcTests opens the native one over them), each reads back from base64
// as a subscribed reply carries it, and each describes itself with the field numbers its
// metadata holds (D-BIND-72 point 2).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;

using Apache.Arrow;
using Apache.Arrow.Ipc;
using Apache.Arrow.Types;

using Xunit;

namespace Eiva.Fletcher.GatewayClient.Tests;

public sealed class ControlMessagesTests
{
    // ── gateway-client-ts test/ws-protocol.test.ts: requests ──

    [Fact]
    public void BuildCreateTopicCarriesActionAndTopic()
    {
        using JsonDocument json = JsonDocument.Parse(ControlMessages.BuildCreateTopic("demo/telemetry"));

        Assert.Equal("create_topic", json.RootElement.GetProperty("action").GetString());
        Assert.Equal("demo/telemetry", json.RootElement.GetProperty("topic").GetString());
        Assert.False(json.RootElement.TryGetProperty("schema", out _));
    }

    [Fact]
    public void BuildSubscribeCarriesActionAndTopic()
    {
        using JsonDocument json = JsonDocument.Parse(ControlMessages.BuildSubscribe("test/topic"));

        Assert.Equal("subscribe", json.RootElement.GetProperty("action").GetString());
        Assert.Equal("test/topic", json.RootElement.GetProperty("topic").GetString());
    }

    [Fact]
    public void BuildUnsubscribeWritesTheIdAsADecimalString()
    {
        // 2^53 + 1: the first integer a JavaScript number cannot hold.
        using JsonDocument json = JsonDocument.Parse(ControlMessages.BuildUnsubscribe(9_007_199_254_740_993ul));

        Assert.Equal("unsubscribe", json.RootElement.GetProperty("action").GetString());
        Assert.Equal(JsonValueKind.String, json.RootElement.GetProperty("subId").ValueKind);
        Assert.Equal("9007199254740993", json.RootElement.GetProperty("subId").GetString());
    }

    [Fact]
    public void BuildListTopicsCarriesTheActionOnly()
    {
        using JsonDocument json = JsonDocument.Parse(ControlMessages.BuildListTopics());

        Assert.Equal(new[] { "action" }, json.RootElement.EnumerateObject().Select(p => p.Name));
        Assert.Equal("list_topics", json.RootElement.GetProperty("action").GetString());
    }

    // ── gateway-client-ts test/ws-protocol.test.ts: replies ──

    [Fact]
    public void ParsesTopicCreated() => Assert.IsType<TopicCreatedReply>(ControlMessages.ParseReply("{\"type\":\"topic_created\"}"));

    [Fact]
    public void ParsesSubscribedWithAnIdBeyondJavaScriptsIntegers()
    {
        var reply = Assert.IsType<SubscribedReply>(
            ControlMessages.ParseReply("{\"type\":\"subscribed\",\"subId\":\"9007199254740993\",\"topic\":\"a/b\"}"));

        Assert.Equal(9_007_199_254_740_993ul, reply.SubscriptionId);
        Assert.Equal("a/b", reply.Topic);
        Assert.Null(reply.Schema);
        Assert.Null(reply.SchemaJson);
    }

    [Fact]
    public void ParsesUnsubscribed() => Assert.IsType<UnsubscribedReply>(ControlMessages.ParseReply("{\"type\":\"unsubscribed\"}"));

    [Fact]
    public void ParsesPublished() => Assert.IsType<PublishedReply>(ControlMessages.ParseReply("{\"type\":\"published\"}"));

    [Fact]
    public void ParsesTopicsList()
    {
        var reply = Assert.IsType<TopicsListReply>(ControlMessages.ParseReply("{\"type\":\"topics_list\",\"topics\":[\"x\",\"y\"]}"));

        Assert.Equal(new[] { "x", "y" }, reply.Topics);
    }

    [Fact]
    public void ParsesError()
    {
        var reply = Assert.IsType<ErrorReply>(ControlMessages.ParseReply("{\"type\":\"error\",\"message\":\"bad\"}"));

        Assert.Equal("bad", reply.Message);
    }

    [Fact]
    public void ThrowsOnAnUnknownType()
    {
        InvalidDataException e = Assert.Throws<InvalidDataException>(() => ControlMessages.ParseReply("{\"type\":\"unknown\"}"));
        Assert.Contains("Unknown gateway reply type", e.Message, StringComparison.Ordinal);
    }

    // ── C# additions: what a reply may not be ──

    public static TheoryData<string> MalformedReplies => new()
    {
        "not json",
        "[]",
        "{}",
        "{\"type\":7}",
        "{\"type\":\"error\"}",
        "{\"type\":\"subscribed\",\"subId\":42,\"topic\":\"a\"}",
        "{\"type\":\"subscribed\",\"subId\":\"-1\",\"topic\":\"a\"}",
        "{\"type\":\"subscribed\",\"subId\":\"18446744073709551616\",\"topic\":\"a\"}",
        "{\"type\":\"subscribed\",\"subId\":\"1\"}",
        "{\"type\":\"subscribed\",\"subId\":\"1\",\"topic\":\"a\",\"schemaIpc\":\"%%%\"}",
        "{\"type\":\"topics_list\"}",
        "{\"type\":\"topics_list\",\"topics\":[1]}",
    };

    [Theory]
    [MemberData(nameof(MalformedReplies))]
    public void AMalformedReplyIsRefusedAsInvalidData(string text)
    {
        // A numeric subId is refused even though TypeScript's BigInt() would take it: the
        // gateway always sends a string, and a number past 2^53 would already be wrong.
        Assert.Throws<InvalidDataException>(() => ControlMessages.ParseReply(text));
    }

    // ── gateway/tests/test_schema_codec.cpp, from the client's side ──

    [Fact]
    public void BuildsStructWithScalarFields()
    {
        Schema schema = new Schema.Builder()
            .Field(f => f.Name("id").DataType(Int64Type.Default))
            .Field(f => f.Name("value").DataType(DoubleType.Default))
            .Field(f => f.Name("label").DataType(StringType.Default))
            .Build();

        using JsonDocument json = JsonDocument.Parse(ControlMessages.BuildCreateTopic("t", schema));
        JsonElement[] fields = json.RootElement.GetProperty("schema").GetProperty("fields").EnumerateArray().ToArray();

        Assert.Equal(new[] { "id", "value", "label" }, fields.Select(f => f.GetProperty("name").GetString()));
        Assert.Equal(new[] { 0x05, 0x0B, 0x0C }, fields.Select(f => f.GetProperty("wireType").GetInt32()));
    }

    [Fact]
    public void BuildsEmptyStruct()
    {
        using JsonDocument json = JsonDocument.Parse(ControlMessages.SchemaToJson(new Schema.Builder().Build()));

        Assert.Equal(0, json.RootElement.GetProperty("fields").GetArrayLength());
    }

    [Theory]
    [MemberData(nameof(IpcGoldens))]
    public void NeverWritesADescriptorTheGatewaysParserRefusesForItsShape(string golden)
    {
        // test_schema_codec's seven malformed-descriptor refusals (no fields array, fields
        // not an array, a field without a string name or an integer wireType, an unknown
        // wireType): the descriptor C# writes has none of them, at any depth.
        using JsonDocument json = JsonDocument.Parse(ControlMessages.SchemaToJson(ReadGolden(golden)));

        Assert.Equal(JsonValueKind.Array, json.RootElement.GetProperty("fields").ValueKind);
        foreach (JsonElement field in json.RootElement.GetProperty("fields").EnumerateArray())
        {
            AssertWellFormed(field);
        }
    }

    [Fact]
    public void ANestedFieldIsDescribedInFullThoughTodaysGatewayRefusesIt()
    {
        // test_schema_codec ThrowsOnUnsupportedWireType: the gateway's create_topic parser
        // takes top-level SCALARS only and refuses 0x20 (struct) and the other composites.
        // C# still describes the topic as it is - the same shape the gateway itself sends
        // in a subscribed reply - and BIND-8b decides what CreateTopicAsync announces.
        var point = new StructType(new List<Field> { new("x", DoubleType.Default, false), new("y", DoubleType.Default, false) });
        Schema schema = new Schema.Builder()
            .Field(f => f.Name("at").DataType(point))
            .Field(f => f.Name("tags").DataType(new MapType(StringType.Default, Int32Type.Default)))
            .Field(f => f.Name("grid").DataType(new FixedSizeListType(new Field("item", FloatType.Default, false), 9)))
            .Build();

        using JsonDocument json = JsonDocument.Parse(ControlMessages.SchemaToJson(schema));
        JsonElement[] fields = json.RootElement.GetProperty("fields").EnumerateArray().ToArray();

        Assert.Equal(0x20, fields[0].GetProperty("wireType").GetInt32());
        Assert.Equal(new[] { "x", "y" }, fields[0].GetProperty("fields").EnumerateArray().Select(f => f.GetProperty("name").GetString()));
        Assert.Equal(0x24, fields[1].GetProperty("wireType").GetInt32());
        Assert.Equal(0x0C, fields[1].GetProperty("mapKey").GetProperty("wireType").GetInt32());
        Assert.Equal(0x04, fields[1].GetProperty("mapValue").GetProperty("wireType").GetInt32());
        Assert.Equal(0x23, fields[2].GetProperty("wireType").GetInt32());
        Assert.Equal(9, fields[2].GetProperty("fixedSize").GetInt32());
        Assert.Equal(0x0A, fields[2].GetProperty("element").GetProperty("wireType").GetInt32());
    }

    [Fact]
    public void ADictionaryFieldIsDescribedByItsValueType()
    {
        Schema schema = new Schema.Builder()
            .Field(f => f.Name("heading").DataType(new DictionaryType(Int8Type.Default, StringType.Default, false)))
            .Build();

        using JsonDocument json = JsonDocument.Parse(ControlMessages.SchemaToJson(schema));

        Assert.Equal(0x0C, json.RootElement.GetProperty("fields")[0].GetProperty("wireType").GetInt32());
    }

    // ── The generator's schemas ──

    public static TheoryData<string> IpcGoldens
    {
        get
        {
            var data = new TheoryData<string>();
            foreach (string path in Directory.GetFiles(GoldenDirectory, "*.ipc"))
            {
                data.Add(Path.GetFileName(path));
            }

            return data;
        }
    }

    [Theory]
    [MemberData(nameof(IpcGoldens))]
    public void EveryGeneratedSchemaOpensTheManagedCodec(string golden)
    {
        Schema schema = ReadGolden(golden);

        var codec = new GatewayRowCodec(schema);

        Assert.Equal(schema.FieldsList.Count, codec.Schema.FieldsList.Count);
    }

    [Theory]
    [MemberData(nameof(IpcGoldens))]
    public void ASubscribedReplyCarriesTheSchemaAsBase64Ipc(string golden)
    {
        byte[] ipc = File.ReadAllBytes(Path.Combine(GoldenDirectory, golden));
        string text = "{\"type\":\"subscribed\",\"subId\":\"7\",\"topic\":\"t\",\"schemaIpc\":\"" + Convert.ToBase64String(ipc) +
            "\",\"schema\":{\"fields\":[]}}";

        var reply = Assert.IsType<SubscribedReply>(ControlMessages.ParseReply(text));

        Schema expected = ReadGolden(golden);
        Assert.NotNull(reply.Schema);
        Assert.Equal(expected.FieldsList.Select(f => (f.Name, f.DataType.TypeId)), reply.Schema!.FieldsList.Select(f => (f.Name, f.DataType.TypeId)));
        Assert.Equal("{\"fields\":[]}", reply.SchemaJson);
    }

    [Theory]
    [MemberData(nameof(IpcGoldens))]
    public void TheDescriptorCarriesEachFieldsNumberFromItsMetadata(string golden)
    {
        Schema schema = ReadGolden(golden);
        using JsonDocument json = JsonDocument.Parse(ControlMessages.SchemaToJson(schema));
        JsonElement[] fields = json.RootElement.GetProperty("fields").EnumerateArray().ToArray();

        for (int i = 0; i < fields.Length; ++i)
        {
            Field field = schema.FieldsList[i];
            Assert.True(field.HasMetadata && field.Metadata.ContainsKey(ControlMessages.FieldNumberKey),
                golden + ": the generator wrote no field_number for '" + field.Name + "'");
            Assert.Equal(int.Parse(field.Metadata[ControlMessages.FieldNumberKey], System.Globalization.CultureInfo.InvariantCulture),
                fields[i].GetProperty("fieldNumber").GetInt32());
        }
    }

    private static void AssertWellFormed(JsonElement field)
    {
        Assert.Equal(JsonValueKind.String, field.GetProperty("name").ValueKind);
        Assert.Equal(JsonValueKind.Number, field.GetProperty("wireType").ValueKind);
        Assert.NotEqual(0, field.GetProperty("wireType").GetInt32());
        foreach (string nested in new[] { "element", "mapKey", "mapValue" })
        {
            if (field.TryGetProperty(nested, out JsonElement child))
            {
                AssertWellFormed(child);
            }
        }

        if (field.TryGetProperty("fields", out JsonElement children))
        {
            foreach (JsonElement child in children.EnumerateArray())
            {
                AssertWellFormed(child);
            }
        }
    }

    private static string GoldenDirectory => Path.Combine(AppContext.BaseDirectory, "golden");

    private static Schema ReadGolden(string name)
    {
        using var reader = new ArrowStreamReader(File.OpenRead(Path.Combine(GoldenDirectory, name)));
        return reader.Schema;
    }
}
