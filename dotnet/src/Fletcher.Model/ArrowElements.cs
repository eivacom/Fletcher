// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The elements a generated view's collection windows read (BIND-7b, D-BIND-101).
//
// A generated accessor casts every column once in TryMake. For a list or a map it also builds,
// once, the reader of the column's element child: one of the sealed classes below, or for a
// struct element the inner message's accessor through ViewElements. A window (ListView,
// MapView) then holds that reader and a range, so reading an element is one interface call on a
// typed array and allocates nothing. These types live here, not beside the generated code,
// because generated code is compiled into each consumer's assembly (see the project file).
//
// They are for generated code (D-BIND-107): public because that code compiles in the consumer's
// assembly, hidden from IntelliSense, and free to change together with the plugin within a
// MAJOR.MINOR. ListView and MapView are the API a consumer uses.
using System;
using System.ComponentModel;
using System.Runtime.CompilerServices;

using Apache.Arrow;
using Apache.Arrow.Types;

namespace Eiva.Fletcher.Model;

/// <summary>Reads the elements of one Arrow child array by absolute index.</summary>
/// <typeparam name="T">The value an element reads as.</typeparam>
/// <remarks>
/// Implemented once per element kind in this package and used by generated accessors; an
/// element that is null reads as <see cref="Get"/>'s documented default, with
/// <see cref="IsNull"/> telling it apart (D-BIND-101).
/// </remarks>
[EditorBrowsable(EditorBrowsableState.Never)]
public interface IArrowElements<T>
{
    /// <summary>The element at <paramref name="index"/> of the child array.</summary>
    public T Get(int index);

    /// <summary>Whether the element at <paramref name="index"/> is null.</summary>
    public bool IsNull(int index);
}

/// <summary>The rows of a generated accessor, which a list of structs reads its elements from.</summary>
/// <typeparam name="TView">The accessor's view struct.</typeparam>
[EditorBrowsable(EditorBrowsableState.Never)]
public interface IArrowRows<TView>
{
    /// <summary>Whether row <paramref name="row"/> is null.</summary>
    public bool IsNull(int row);

    /// <summary>Row <paramref name="row"/>, read in place.</summary>
    public TView At(int row);
}

/// <summary>Numeric elements: a null element reads as zero.</summary>
/// <typeparam name="T">The element's C# type, as its typed array declares it.</typeparam>
[EditorBrowsable(EditorBrowsableState.Never)]
public sealed class PrimitiveElements<T> : IArrowElements<T>
    where T : struct, IEquatable<T>
{
    private readonly PrimitiveArray<T> _array;

    /// <summary>Reads the elements of <paramref name="array"/>.</summary>
    public PrimitiveElements(PrimitiveArray<T> array)
    {
        ArgumentNullException.ThrowIfNull(array);
        _array = array;
    }

    /// <inheritdoc/>
    public T Get(int index) => _array.GetValue(index).GetValueOrDefault();

    /// <inheritdoc/>
    public bool IsNull(int index) => _array.IsNull(index);
}

/// <summary>Boolean elements: a null element reads as false.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public sealed class BooleanElements : IArrowElements<bool>
{
    private readonly BooleanArray _array;

    /// <summary>Reads the elements of <paramref name="array"/>.</summary>
    public BooleanElements(BooleanArray array)
    {
        ArgumentNullException.ThrowIfNull(array);
        _array = array;
    }

    /// <inheritdoc/>
    public bool Get(int index) => _array.GetValue(index).GetValueOrDefault();

    /// <inheritdoc/>
    public bool IsNull(int index) => _array.IsNull(index);
}

/// <summary>UTF-8 elements, decoded on every read: a null element reads as the empty string.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public sealed class StringElements : IArrowElements<string>
{
    private readonly StringArray _array;

    /// <summary>Reads the elements of <paramref name="array"/>.</summary>
    public StringElements(StringArray array)
    {
        ArgumentNullException.ThrowIfNull(array);
        _array = array;
    }

    /// <inheritdoc/>
    public string Get(int index) => _array.GetString(index) ?? "";

    /// <inheritdoc/>
    public bool IsNull(int index) => _array.IsNull(index);
}

/// <summary>Bytes elements, copied on every read as the model's <c>byte[]</c>: a null element reads as empty.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public sealed class BinaryElements : IArrowElements<byte[]>
{
    private readonly BinaryArray _array;

    /// <summary>Reads the elements of <paramref name="array"/>.</summary>
    public BinaryElements(BinaryArray array)
    {
        ArgumentNullException.ThrowIfNull(array);
        _array = array;
    }

    /// <inheritdoc/>
    public byte[] Get(int index) => _array.IsNull(index) ? System.Array.Empty<byte>() : _array.GetBytes(index).ToArray();

    /// <inheritdoc/>
    public bool IsNull(int index) => _array.IsNull(index);
}

/// <summary>Enum elements, stored as int32: a null element reads as the enum's zero value.</summary>
/// <typeparam name="TEnum">A generated enum, whose underlying type is <see cref="int"/>.</typeparam>
[EditorBrowsable(EditorBrowsableState.Never)]
public sealed class EnumElements<TEnum> : IArrowElements<TEnum>
    where TEnum : struct, Enum
{
    private readonly Int32Array _array;

    /// <summary>Reads the elements of <paramref name="array"/>.</summary>
    /// <exception cref="ArgumentException"><typeparamref name="TEnum"/> is not int-backed.</exception>
    public EnumElements(Int32Array array)
    {
        ArgumentNullException.ThrowIfNull(array);
        if (Enum.GetUnderlyingType(typeof(TEnum)) != typeof(int))
        {
            throw new ArgumentException($"{typeof(TEnum).Name} is not an int-backed enum.", nameof(TEnum));
        }
        _array = array;
    }

    /// <inheritdoc/>
    public TEnum Get(int index)
    {
        int value = _array.GetValue(index).GetValueOrDefault();
        return Unsafe.As<int, TEnum>(ref value);
    }

    /// <inheritdoc/>
    public bool IsNull(int index) => _array.IsNull(index);
}

/// <summary>Struct elements, read through the inner message's accessor: a null element reads as null (D-BIND-101).</summary>
/// <typeparam name="TView">The inner message's view struct.</typeparam>
[EditorBrowsable(EditorBrowsableState.Never)]
public sealed class ViewElements<TView> : IArrowElements<TView?>
    where TView : struct
{
    private readonly IArrowRows<TView> _rows;

    /// <summary>Reads the elements through <paramref name="rows"/>, an accessor over the element child.</summary>
    public ViewElements(IArrowRows<TView> rows)
    {
        ArgumentNullException.ThrowIfNull(rows);
        _rows = rows;
    }

    /// <inheritdoc/>
    public TView? Get(int index) => _rows.IsNull(index) ? null : _rows.At(index);

    /// <inheritdoc/>
    public bool IsNull(int index) => _rows.IsNull(index);
}

/// <summary>List elements of a list of lists: each reads as a window; a null inner list as an empty one.</summary>
/// <typeparam name="T">The inner lists' element.</typeparam>
[EditorBrowsable(EditorBrowsableState.Never)]
public sealed class ListElements<T> : IArrowElements<ListView<T>>
{
    private readonly ListArray _array;
    private readonly IArrowElements<T> _inner;

    /// <summary>Reads the inner lists of <paramref name="array"/>, their elements through <paramref name="inner"/>.</summary>
    public ListElements(ListArray array, IArrowElements<T> inner)
    {
        ArgumentNullException.ThrowIfNull(array);
        ArgumentNullException.ThrowIfNull(inner);
        _array = array;
        _inner = inner;
    }

    /// <inheritdoc/>
    /// <remarks>A null inner list reads as an empty window whatever its offsets span (D-BIND-101).</remarks>
    public ListView<T> Get(int index) => _array.IsNull(index) ? default : ListView<T>.Of(_array, index, _inner);

    /// <inheritdoc/>
    public bool IsNull(int index) => _array.IsNull(index);
}

/// <summary>Timestamp elements, in their column's unit and zone.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public sealed class TimestampElements : IArrowElements<Timestamp>
{
    private readonly TimestampArray _array;
    private readonly TimestampType _type;

    /// <summary>Reads the elements of <paramref name="array"/>.</summary>
    public TimestampElements(TimestampArray array)
    {
        ArgumentNullException.ThrowIfNull(array);
        _array = array;
        _type = (TimestampType)array.Data.DataType;
    }

    /// <inheritdoc/>
    public Timestamp Get(int index) => new(_array.GetValue(index).GetValueOrDefault(), _type.Unit, _type.Timezone);

    /// <inheritdoc/>
    public bool IsNull(int index) => _array.IsNull(index);
}

/// <summary>Duration elements, in their column's unit.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public sealed class DurationElements : IArrowElements<Duration>
{
    private readonly DurationArray _array;
    private readonly DurationType _type;

    /// <summary>Reads the elements of <paramref name="array"/>.</summary>
    public DurationElements(DurationArray array)
    {
        ArgumentNullException.ThrowIfNull(array);
        _array = array;
        _type = (DurationType)array.Data.DataType;
    }

    /// <inheritdoc/>
    public Duration Get(int index) => new(_array.GetValue(index).GetValueOrDefault(), _type.Unit);

    /// <inheritdoc/>
    public bool IsNull(int index) => _array.IsNull(index);
}
