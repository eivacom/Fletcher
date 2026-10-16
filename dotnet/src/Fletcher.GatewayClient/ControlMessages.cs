// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The gateway's JSON control messages (text frames).
//
// gateway/src/ws_session.cpp is the authority, and it says more than the wire-format
// spec does: the spec omits the `unsubscribed` and `published` replies and the optional
// `schema` of `create_topic`. Requests carry an `action`; replies carry a `type`:
//
//   create_topic {topic, schema?}  -> topic_created
//   subscribe    {topic}           -> subscribed {subId, topic, schema?, schemaIpc?}
//   unsubscribe  {subId}           -> unsubscribed
//   list_topics                    -> topics_list {topics}
//   (a binary PUBLISH frame)       -> published
//   anything refused               -> error {message}   (no request id: D-BIND-110)
//
// A subscription id travels as a decimal STRING in both directions, because JavaScript
// numbers lose integers past 2^53. That is a wire contract, not a JavaScript detail, so
// the C# side writes and accepts a string and nothing else.
//
// Written with Utf8JsonWriter and read with JsonDocument: no reflection, so the package
// stays trimmable and NativeAOT-safe.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json;

using Apache.Arrow;
using Apache.Arrow.Ipc;
using Apache.Arrow.Types;

namespace Eiva.Fletcher.GatewayClient;

/// <summary>A reply the gateway sent in a text frame.</summary>
internal abstract record GatewayReply;

/// <summary><c>topic_created</c>.</summary>
internal sealed record TopicCreatedReply : GatewayReply;

/// <summary><c>subscribed</c>.</summary>
/// <param name="SubscriptionId">The id every MESSAGE frame for this subscription carries.</param>
/// <param name="Topic">The topic, as the request named it.</param>
/// <param name="Schema">
/// The topic's schema read from <c>schemaIpc</c>, or null when the gateway had none in hand
/// at the moment it subscribed (a late-joining Fast DDS subscription, a topic created
/// without one). A typed subscriber brings its own and does not depend on this.
/// </param>
/// <param name="SchemaJson">The <c>schema</c> descriptor as raw JSON, or null with <paramref name="Schema"/>.</param>
internal sealed record SubscribedReply(ulong SubscriptionId, string Topic, Schema? Schema, string? SchemaJson) : GatewayReply;

/// <summary><c>unsubscribed</c>.</summary>
internal sealed record UnsubscribedReply : GatewayReply;

/// <summary><c>published</c>: the acknowledgement of one PUBLISH frame (D-BIND-111).</summary>
internal sealed record PublishedReply : GatewayReply;

/// <summary><c>topics_list</c>.</summary>
internal sealed record TopicsListReply(IReadOnlyList<string> Topics) : GatewayReply;

/// <summary><c>error</c>: the request in flight was refused (D-BIND-110).</summary>
internal sealed record ErrorReply(string Message) : GatewayReply;

/// <summary>Builders for the requests and the parser for the replies.</summary>
internal static class ControlMessages
{
    /// <summary>The metadata key the generator writes each field's proto field number under.</summary>
    public static readonly string FieldNumberKey = "field_number";

    /// <summary><c>create_topic</c>, announcing <paramref name="schema"/> when one is given.</summary>
    /// <remarks>
    /// The gateway forwards an announced schema to later subscribers. Note what it accepts
    /// TODAY (gateway/src/schema_codec.cpp, BuildArrowSchemaFromJson): top-level scalar
    /// fields only; a struct, list or map field makes it reply <c>error</c>. The descriptor
    /// written here is nonetheless the whole one, the same shape the gateway itself sends
    /// in <c>subscribed</c>, so the request says what the topic is rather than what one
    /// gateway version can parse.
    /// </remarks>
    public static string BuildCreateTopic(string topic, Schema? schema = null)
    {
        ArgumentNullException.ThrowIfNull(topic);
        return Write(w =>
        {
            w.WriteString("action", "create_topic");
            w.WriteString("topic", topic);
            if (schema is not null)
            {
                w.WritePropertyName("schema");
                WriteSchema(w, schema);
            }
        });
    }

    /// <summary><c>subscribe</c>.</summary>
    public static string BuildSubscribe(string topic)
    {
        ArgumentNullException.ThrowIfNull(topic);
        return Write(w =>
        {
            w.WriteString("action", "subscribe");
            w.WriteString("topic", topic);
        });
    }

    /// <summary><c>unsubscribe</c>; the id goes out as a decimal string.</summary>
    public static string BuildUnsubscribe(ulong subscriptionId) => Write(w =>
    {
        w.WriteString("action", "unsubscribe");
        w.WriteString("subId", subscriptionId.ToString(CultureInfo.InvariantCulture));
    });

    /// <summary><c>list_topics</c>.</summary>
    public static string BuildListTopics() => Write(w => w.WriteString("action", "list_topics"));

    /// <summary>Parse a reply.</summary>
    /// <exception cref="InvalidDataException">
    /// The text is not JSON, has no <c>type</c>, names a type this client does not know,
    /// or lacks a member its type requires.
    /// </exception>
    public static GatewayReply ParseReply(string text)
    {
        ArgumentNullException.ThrowIfNull(text);
        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(text);
        }
        catch (JsonException e)
        {
            throw new InvalidDataException("Gateway reply is not JSON: " + e.Message, e);
        }

        using (document)
        {
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
            {
                throw new InvalidDataException("Gateway reply is not a JSON object");
            }

            string type = RequiredString(root, "type", "a reply");
            switch (type)
            {
                case "topic_created":
                    return new TopicCreatedReply();
                case "unsubscribed":
                    return new UnsubscribedReply();
                case "published":
                    return new PublishedReply();
                case "error":
                    return new ErrorReply(RequiredString(root, "message", "error"));
                case "topics_list":
                    {
                        if (!root.TryGetProperty("topics", out JsonElement topics) || topics.ValueKind != JsonValueKind.Array)
                        {
                            throw new InvalidDataException("Gateway reply topics_list has no 'topics' array");
                        }

                        var list = new List<string>(topics.GetArrayLength());
                        foreach (JsonElement topic in topics.EnumerateArray())
                        {
                            if (topic.ValueKind != JsonValueKind.String)
                            {
                                throw new InvalidDataException("Gateway reply topics_list has a non-string topic");
                            }

                            list.Add(topic.GetString()!);
                        }

                        return new TopicsListReply(list);
                    }

                case "subscribed":
                    {
                        string subId = RequiredString(root, "subId", "subscribed");
                        if (!ulong.TryParse(subId, NumberStyles.None, CultureInfo.InvariantCulture, out ulong id))
                        {
                            throw new InvalidDataException("Gateway reply subscribed has a subId that is not a decimal u64: '" + subId + "'");
                        }

                        string topic = RequiredString(root, "topic", "subscribed");
                        Schema? schema = null;
                        if (root.TryGetProperty("schemaIpc", out JsonElement ipc) && ipc.ValueKind == JsonValueKind.String)
                        {
                            schema = ReadSchemaIpc(ipc.GetString()!);
                        }

                        string? schemaJson = root.TryGetProperty("schema", out JsonElement json) ? json.GetRawText() : null;
                        return new SubscribedReply(id, topic, schema, schemaJson);
                    }

                default:
                    throw new InvalidDataException("Unknown gateway reply type: " + type);
            }
        }
    }

    /// <summary>Read the schema a <c>subscribed</c> reply carries as base64 Arrow IPC.</summary>
    /// <exception cref="InvalidDataException">The text is not base64, or not an IPC stream with a schema.</exception>
    public static Schema ReadSchemaIpc(string base64)
    {
        ArgumentNullException.ThrowIfNull(base64);
        byte[] bytes;
        try
        {
            bytes = Convert.FromBase64String(base64);
        }
        catch (FormatException e)
        {
            throw new InvalidDataException("schemaIpc is not base64: " + e.Message, e);
        }

        try
        {
            using var reader = new ArrowStreamReader(new MemoryStream(bytes));
            return reader.Schema ?? throw new InvalidDataException("schemaIpc holds no schema");
        }
        catch (Exception e) when (e is not InvalidDataException)
        {
            throw new InvalidDataException("schemaIpc is not an Arrow IPC schema: " + e.Message, e);
        }
    }

    /// <summary>The JSON schema descriptor of <paramref name="schema"/>, as a string.</summary>
    public static string SchemaToJson(Schema schema)
    {
        ArgumentNullException.ThrowIfNull(schema);
        var buffer = new MemoryStream();
        using (var w = new Utf8JsonWriter(buffer))
        {
            WriteSchema(w, schema);
        }

        return Encoding.UTF8.GetString(buffer.GetBuffer(), 0, (int)buffer.Length);
    }

    // {"fields":[...]}, field by field as gateway/src/schema_codec.cpp's FieldToJson writes
    // it: name, nullable, wireType, fieldNumber (when the field carries one), and the
    // nested members of a composite: fields / element (+ fixedSize) / mapKey + mapValue.
    private static void WriteSchema(Utf8JsonWriter w, Schema schema)
    {
        w.WriteStartObject();
        w.WriteStartArray("fields");
        foreach (Field field in schema.FieldsList)
        {
            WriteField(w, field);
        }

        w.WriteEndArray();
        w.WriteEndObject();
    }

    private static void WriteField(Utf8JsonWriter w, Field field)
    {
        IArrowType type = field.DataType is DictionaryType dictionary ? dictionary.ValueType : field.DataType;

        w.WriteStartObject();
        w.WriteString("name", field.Name ?? string.Empty);
        w.WriteBoolean("nullable", field.IsNullable);
        w.WriteNumber("wireType", (int)WireTypes.Of(field.DataType));
        if (field.HasMetadata
            && field.Metadata.TryGetValue(FieldNumberKey, out string? number)
            && int.TryParse(number, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out int fieldNumber))
        {
            w.WriteNumber("fieldNumber", fieldNumber);
        }

        switch (type)
        {
            case StructType st:
                w.WriteStartArray("fields");
                foreach (Field child in st.Fields)
                {
                    WriteField(w, child);
                }

                w.WriteEndArray();
                break;
            case MapType map:
                w.WritePropertyName("mapKey");
                WriteField(w, map.KeyField);
                w.WritePropertyName("mapValue");
                WriteField(w, map.ValueField);
                break;
            case ListType list:
                w.WritePropertyName("element");
                WriteField(w, list.ValueField);
                break;
            case LargeListType largeList:
                w.WritePropertyName("element");
                WriteField(w, largeList.ValueField);
                break;
            case FixedSizeListType fixedList:
                w.WritePropertyName("element");
                WriteField(w, fixedList.ValueField);
                w.WriteNumber("fixedSize", fixedList.ListSize);
                break;
        }

        w.WriteEndObject();
    }

    private static string Write(Action<Utf8JsonWriter> members)
    {
        var buffer = new MemoryStream();
        using (var w = new Utf8JsonWriter(buffer))
        {
            w.WriteStartObject();
            members(w);
            w.WriteEndObject();
        }

        return Encoding.UTF8.GetString(buffer.GetBuffer(), 0, (int)buffer.Length);
    }

    private static string RequiredString(JsonElement root, string name, string what)
    {
        if (!root.TryGetProperty(name, out JsonElement value) || value.ValueKind != JsonValueKind.String)
        {
            throw new InvalidDataException("Gateway reply " + what + " has no string '" + name + "'");
        }

        return value.GetString()!;
    }
}
