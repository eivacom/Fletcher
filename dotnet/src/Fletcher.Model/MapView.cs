// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// A borrowed window over one row of a map column (BIND-7b, D-BIND-101), as C++'s accessor
// returns a map span: the entries in the order the column holds them, keys and values read in
// place. A proto map has no order and the column may repeat a key; the window shows the entries
// as they are, as the model's List<KeyValuePair> does.
using System;
using System.Collections;
using System.Collections.Generic;

using Apache.Arrow;

namespace Eiva.Fletcher.Model;

/// <summary>The entries of one map in an Arrow map column, read in place.</summary>
/// <typeparam name="TKey">The key's C# type. Arrow map keys are never null.</typeparam>
/// <typeparam name="TValue">The value an entry's value reads as: a null value reads as its documented default, see <see cref="ValueIsNull"/>.</typeparam>
/// <remarks>Borrows the accessor that produced it. <c>foreach</c> over it does not allocate.</remarks>
public readonly struct MapView<TKey, TValue> : IReadOnlyList<KeyValuePair<TKey, TValue>>
{
    private readonly IArrowElements<TKey>? _keys;
    private readonly IArrowElements<TValue>? _values;
    private readonly int _start;
    private readonly int _count;

    /// <summary>A window of <paramref name="count"/> entries from <paramref name="start"/>.</summary>
    public MapView(IArrowElements<TKey> keys, IArrowElements<TValue> values, int start, int count)
    {
        ArgumentNullException.ThrowIfNull(keys);
        ArgumentNullException.ThrowIfNull(values);
        ArgumentOutOfRangeException.ThrowIfNegative(start);
        ArgumentOutOfRangeException.ThrowIfNegative(count);
        _keys = keys;
        _values = values;
        _start = start;
        _count = count;
    }

    /// <summary>The map at <paramref name="row"/> of <paramref name="map"/>.</summary>
    public static MapView<TKey, TValue> Of(MapArray map, int row, IArrowElements<TKey> keys, IArrowElements<TValue> values)
    {
        ArgumentNullException.ThrowIfNull(map);
        ReadOnlySpan<int> offsets = map.ValueOffsets;
        return new MapView<TKey, TValue>(keys, values, offsets[row], offsets[row + 1] - offsets[row]);
    }

    /// <summary>The number of entries.</summary>
    public int Count => _count;

    /// <summary>The entry at <paramref name="index"/>.</summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="index"/> is outside the map.</exception>
    public KeyValuePair<TKey, TValue> this[int index] => new(Key(index), Value(index));

    /// <summary>The key of the entry at <paramref name="index"/>.</summary>
    public TKey Key(int index)
    {
        Check(index);
        return _keys!.Get(_start + index);
    }

    /// <summary>The value of the entry at <paramref name="index"/>.</summary>
    public TValue Value(int index)
    {
        Check(index);
        return _values!.Get(_start + index);
    }

    /// <summary>Whether the value of the entry at <paramref name="index"/> is null.</summary>
    public bool ValueIsNull(int index)
    {
        Check(index);
        return _values!.IsNull(_start + index);
    }

    /// <summary>A copy of the entries, in the column's order.</summary>
    public List<KeyValuePair<TKey, TValue>> ToList()
    {
        var list = new List<KeyValuePair<TKey, TValue>>(_count);
        for (int i = 0; i < _count; i++)
        {
            list.Add(new(_keys!.Get(_start + i), _values!.Get(_start + i)));
        }
        return list;
    }

    /// <summary>An enumerator that does not allocate.</summary>
    public Enumerator GetEnumerator() => new(this);

    IEnumerator<KeyValuePair<TKey, TValue>> IEnumerable<KeyValuePair<TKey, TValue>>.GetEnumerator() => GetEnumerator();

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

    private void Check(int index)
    {
        if ((uint)index >= (uint)_count)
        {
            throw new ArgumentOutOfRangeException(nameof(index), index, null);
        }
    }

    /// <summary>Enumerates a <see cref="MapView{TKey, TValue}"/>.</summary>
    public struct Enumerator : IEnumerator<KeyValuePair<TKey, TValue>>
    {
        private readonly MapView<TKey, TValue> _map;
        private int _index;

        internal Enumerator(MapView<TKey, TValue> map)
        {
            _map = map;
            _index = -1;
        }

        /// <inheritdoc/>
        public readonly KeyValuePair<TKey, TValue> Current =>
            new(_map._keys!.Get(_map._start + _index), _map._values!.Get(_map._start + _index));

        readonly object IEnumerator.Current => Current;

        /// <inheritdoc/>
        public bool MoveNext() => ++_index < _map._count;

        /// <inheritdoc/>
        public void Reset() => _index = -1;

        /// <inheritdoc/>
        public readonly void Dispose()
        {
        }
    }
}
