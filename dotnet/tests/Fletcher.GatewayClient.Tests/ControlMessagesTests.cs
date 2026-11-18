// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-8a: the JSON control messages and schemaIpc (Bucket 2).
//
// gateway-client-ts/test/ws-protocol.test.ts's text cases, ported: four request builders,
// seven reply parses. Then gateway/tests/test_schema_codec.cpp from the other side of the
// wire, as D-BIND-128 re-mapped it: since D-BIND-112 the client announces a schema as
// schemaIpc only, so what it owes that file is that its schemaIpc reads back as the same
// declaration, at every depth. (8a's JSON descriptor and its wire-type table, which the
// client no longer sends, went with D-BIND-128.)
//
// Last, the generator's own schemas (protoc/tests/golden/*.ipc): each opens the managed
// codec (as SchemaIpcTests opens the native one over them) and each reads back from base64
// as a subscribed reply carries it, unchanged.
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
        Assert.Null(reply.SchemaError);
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

    [Fact]
    public void AnUnreadableSchemaIpcIsAbsentWithItsReason()
    {
        // D-BIND-124: a subscribed reply whose schema this client cannot read is not a broken
        // reply; the schema is absent and the reason kept, so a subscriber that brought its own
        // schema is unaffected and one that did not is told why.
        var reply = Assert.IsType<SubscribedReply>(
            ControlMessages.ParseReply("{\"type\":\"subscribed\",\"subId\":\"1\",\"topic\":\"a\",\"schemaIpc\":\"%%%\"}"));

        Assert.Null(reply.Schema);
        Assert.Contains("not base64", reply.SchemaError, StringComparison.Ordinal);
    }

    // ── gateway/tests/test_schema_codec.cpp, from the client's side (D-BIND-128) ──
    //
    // What the client owes the gateway's schema handling since D-BIND-112 is one thing: the
    // schemaIpc it writes reads back as the same declaration. The C++ goldens are the native
    // declarations' own bytes; read by the client and written again, each must be the same
    // schema at every depth: names, types with their parameters, nullability, and every
    // metadata entry, the schema's and each field's. That the C++ reader then gives back the
    // native declaration's exact bytes is integration-tests/gateway-dotnet-crosslang's, live.
    // The rest of test_schema_codec tests the gateway's own C++ (the JSON parser, the conflict
    // measurement, base64, FieldToJson) and has no managed counterpart; Bucket 2 records it.

    [Theory]
    [MemberData(nameof(IpcGoldens))]
    public void BuildCreateTopicAnnouncesTheSchemaAsSchemaIpcAndNotAsJson(string golden)
    {
        Schema schema = ReadGolden(golden);

        using JsonDocument json = JsonDocument.Parse(ControlMessages.BuildCreateTopic("t", schema));

        Assert.False(json.RootElement.TryGetProperty("schema", out _));
        Schema back = ControlMessages.ReadSchemaIpc(json.RootElement.GetProperty("schemaIpc").GetString()!);
        AssertSameSchema(schema, back, golden);
    }

    [Fact]
    public void TheSchemaComparisonSeesNestedMetadataAndTimestampUnits()
    {
        // The comparison the golden cases rest on, held to the differences it must see: a
        // nested field's metadata, a Timestamp's unit and its zone.
        Schema Make(TimeUnit unit, string zone, string nestedNumber) => new Schema.Builder()
            .Field(new Field("at", new StructType(new List<Field>
            {
                new("lat", DoubleType.Default, false, new Dictionary<string, string> { ["field_number"] = nestedNumber }),
            }), false))
            .Field(new Field("when", new TimestampType(unit, zone), false))
            .Build();

        AssertSameSchema(Make(TimeUnit.Nanosecond, "UTC", "1"), Make(TimeUnit.Nanosecond, "UTC", "1"), "same");
        Assert.ThrowsAny<Exception>(() => AssertSameSchema(Make(TimeUnit.Nanosecond, "UTC", "1"), Make(TimeUnit.Microsecond, "UTC", "1"), "unit"));
        Assert.ThrowsAny<Exception>(() => AssertSameSchema(Make(TimeUnit.Nanosecond, "UTC", "1"), Make(TimeUnit.Nanosecond, "+01:00", "1"), "zone"));
        Assert.ThrowsAny<Exception>(() => AssertSameSchema(Make(TimeUnit.Nanosecond, "UTC", "1"), Make(TimeUnit.Nanosecond, "UTC", "2"), "nested metadata"));
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

        Assert.NotNull(reply.Schema);
        Assert.Null(reply.SchemaError);
        AssertSameSchema(ReadGolden(golden), reply.Schema!, golden);
    }

    // Every field at every depth: name, nullability, type with its parameters, metadata.
    private static void AssertSameSchema(Schema expected, Schema actual, string what)
    {
        AssertSameMetadata(expected.Metadata, actual.Metadata, what + ": the schema");
        AssertSameFields(expected.FieldsList, actual.FieldsList, what);
    }

    private static void AssertSameFields(IReadOnlyList<Field> expected, IReadOnlyList<Field> actual, string path)
    {
        Assert.True(expected.Count == actual.Count, path + ": " + expected.Count + " fields, got " + actual.Count);
        for (int i = 0; i < expected.Count; ++i)
        {
            AssertSameField(expected[i], actual[i], path + "." + expected[i].Name);
        }
    }

    private static void AssertSameField(Field expected, Field actual, string path)
    {
        Assert.True(expected.Name == actual.Name, path + ": named '" + actual.Name + "'");
        Assert.True(expected.IsNullable == actual.IsNullable, path + ": nullable " + actual.IsNullable);
        AssertSameMetadata(expected.Metadata, actual.Metadata, path);
        AssertSameType(expected.DataType, actual.DataType, path);
    }

    private static void AssertSameType(IArrowType expected, IArrowType actual, string path)
    {
        Assert.True(expected.TypeId == actual.TypeId, path + ": " + expected.TypeId + ", got " + actual.TypeId);
        switch (expected)
        {
            case TimestampType ts:
                Assert.True(ts.Unit == ((TimestampType)actual).Unit, path + ": unit " + ((TimestampType)actual).Unit);
                Assert.True(ts.Timezone == ((TimestampType)actual).Timezone, path + ": zone '" + ((TimestampType)actual).Timezone + "'");
                break;
            case DurationType d:
                Assert.True(d.Unit == ((DurationType)actual).Unit, path + ": unit " + ((DurationType)actual).Unit);
                break;
            case DictionaryType dict:
                AssertSameType(dict.IndexType, ((DictionaryType)actual).IndexType, path + "<index>");
                AssertSameType(dict.ValueType, ((DictionaryType)actual).ValueType, path + "<value>");
                break;
            case FixedSizeListType fixedList:
                Assert.True(fixedList.ListSize == ((FixedSizeListType)actual).ListSize, path + ": size");
                AssertSameField(fixedList.ValueField, ((FixedSizeListType)actual).ValueField, path + "[]");
                break;
            case NestedType nested:
                AssertSameFields(nested.Fields, ((NestedType)actual).Fields, path);
                break;
        }
    }

    private static void AssertSameMetadata(IReadOnlyDictionary<string, string>? expected, IReadOnlyDictionary<string, string>? actual, string path)
    {
        var e = expected ?? new Dictionary<string, string>();
        var a = actual ?? new Dictionary<string, string>();
        Assert.True(e.Count == a.Count, path + ": " + e.Count + " metadata entries, got " + a.Count);
        foreach (KeyValuePair<string, string> entry in e)
        {
            Assert.True(a.TryGetValue(entry.Key, out string? value) && value == entry.Value,
                path + ": metadata '" + entry.Key + "' = '" + entry.Value + "', got '" + value + "'");
        }
    }

    private static string GoldenDirectory => Path.Combine(AppContext.BaseDirectory, "golden");

    private static Schema ReadGolden(string name)
    {
        using var reader = new ArrowStreamReader(File.OpenRead(Path.Combine(GoldenDirectory, name)));
        return reader.Schema;
    }
}
