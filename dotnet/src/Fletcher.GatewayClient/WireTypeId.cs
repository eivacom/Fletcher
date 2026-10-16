// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The wire-type byte a JSON schema descriptor carries in its `wireType` member.
//
// The table is the gateway's, not this file's: gateway/src/schema_codec.cpp's
// NanoarrowTypeToWireType writes these numbers into the `schema` of a `subscribed`
// reply, and its inverse reads them from the `schema` of a `create_topic`. TypeScript's
// copy is gateway-client-ts/src/wire-types.ts. WireTypeTests holds all three to the
// same numbers.
using Apache.Arrow.Types;

namespace Eiva.Fletcher.GatewayClient;

/// <summary>The wire-type identifiers of the gateway's JSON schema descriptor.</summary>
internal enum WireTypeId : byte
{
    Unknown = 0x00,

    Bool = 0x01,
    Int8 = 0x02,
    Int16 = 0x03,
    Int32 = 0x04,
    Int64 = 0x05,
    UInt8 = 0x06,
    UInt16 = 0x07,
    UInt32 = 0x08,
    UInt64 = 0x09,
    Float32 = 0x0a,
    Float64 = 0x0b,
    String = 0x0c,
    Binary = 0x0d,
    Date32 = 0x0e,
    Date64 = 0x0f,
    TimestampNano = 0x10,
    Time32 = 0x11,
    Time64 = 0x12,
    DurationNano = 0x13,
    FixedSizeBinary = 0x14,
    HalfFloat = 0x15,
    Decimal128 = 0x16,
    Decimal256 = 0x17,
    LargeString = 0x18,
    LargeBinary = 0x19,
    StringView = 0x1a,
    BinaryView = 0x1b,
    IntervalMonths = 0x1c,
    IntervalDayTime = 0x1d,
    IntervalMonthDayNano = 0x1e,

    Struct = 0x20,
    List = 0x21,
    LargeList = 0x22,
    FixedSizeList = 0x23,
    Map = 0x24,
    SparseUnion = 0x25,
    DenseUnion = 0x26,
}

/// <summary>The Arrow type to wire-type mapping.</summary>
internal static class WireTypes
{
    /// <summary>
    /// The wire type of <paramref name="type"/>, as the gateway's table gives it, or
    /// <see cref="WireTypeId.Unknown"/> for a type the table has no number for.
    /// </summary>
    /// <remarks>
    /// One departure, and it is the wire's own rule: a dictionary is described by its
    /// VALUE type, because the wire carries values and never indices (spec
    /// "Dictionary Types"). The gateway's table would say 0, a number its own
    /// <c>create_topic</c> parser refuses.
    /// </remarks>
    public static WireTypeId Of(IArrowType type)
    {
        switch (type.TypeId)
        {
            case ArrowTypeId.Boolean: return WireTypeId.Bool;
            case ArrowTypeId.Int8: return WireTypeId.Int8;
            case ArrowTypeId.Int16: return WireTypeId.Int16;
            case ArrowTypeId.Int32: return WireTypeId.Int32;
            case ArrowTypeId.Int64: return WireTypeId.Int64;
            case ArrowTypeId.UInt8: return WireTypeId.UInt8;
            case ArrowTypeId.UInt16: return WireTypeId.UInt16;
            case ArrowTypeId.UInt32: return WireTypeId.UInt32;
            case ArrowTypeId.UInt64: return WireTypeId.UInt64;
            case ArrowTypeId.Float: return WireTypeId.Float32;
            case ArrowTypeId.Double: return WireTypeId.Float64;
            case ArrowTypeId.String: return WireTypeId.String;
            case ArrowTypeId.Binary: return WireTypeId.Binary;
            case ArrowTypeId.Date32: return WireTypeId.Date32;
            case ArrowTypeId.Date64: return WireTypeId.Date64;
            case ArrowTypeId.Timestamp: return WireTypeId.TimestampNano;
            case ArrowTypeId.Time32: return WireTypeId.Time32;
            case ArrowTypeId.Time64: return WireTypeId.Time64;
            case ArrowTypeId.Duration: return WireTypeId.DurationNano;
            case ArrowTypeId.FixedSizedBinary: return WireTypeId.FixedSizeBinary;
            case ArrowTypeId.HalfFloat: return WireTypeId.HalfFloat;
            case ArrowTypeId.Decimal128: return WireTypeId.Decimal128;
            case ArrowTypeId.Decimal256: return WireTypeId.Decimal256;
            case ArrowTypeId.LargeString: return WireTypeId.LargeString;
            case ArrowTypeId.LargeBinary: return WireTypeId.LargeBinary;
            case ArrowTypeId.StringView: return WireTypeId.StringView;
            case ArrowTypeId.BinaryView: return WireTypeId.BinaryView;
            case ArrowTypeId.Interval:
                return ((IntervalType)type).Unit switch
                {
                    IntervalUnit.YearMonth => WireTypeId.IntervalMonths,
                    IntervalUnit.DayTime => WireTypeId.IntervalDayTime,
                    IntervalUnit.MonthDayNanosecond => WireTypeId.IntervalMonthDayNano,
                    _ => WireTypeId.Unknown,
                };
            case ArrowTypeId.Struct: return WireTypeId.Struct;
            case ArrowTypeId.List: return WireTypeId.List;
            case ArrowTypeId.LargeList: return WireTypeId.LargeList;
            case ArrowTypeId.FixedSizeList: return WireTypeId.FixedSizeList;
            case ArrowTypeId.Map: return WireTypeId.Map;
            case ArrowTypeId.Union:
                return ((UnionType)type).Mode == UnionMode.Sparse ? WireTypeId.SparseUnion : WireTypeId.DenseUnion;
            case ArrowTypeId.Dictionary: return Of(((DictionaryType)type).ValueType);
            default: return WireTypeId.Unknown;
        }
    }
}
