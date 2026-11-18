// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// A borrowed window over one row of a list column (BIND-7b, D-BIND-101): what a generated
// view's list property returns, as C++'s accessor returns a span. It holds the column's element
// reader and a range, so reading it allocates nothing; ToList copies it out when a List is
// wanted.
using System;
using System.Collections;
using System.Collections.Generic;
using System.ComponentModel;

using Apache.Arrow;

namespace Eiva.Fletcher.Model;

/// <summary>The elements of one list in an Arrow list column, read in place.</summary>
/// <typeparam name="T">The value an element reads as: a null element reads as its documented default, see <see cref="IsNull"/>.</typeparam>
/// <remarks>
/// Borrows the accessor that produced it, and is valid only while the accessor's source batch or
/// array is not disposed (D-BIND-103). <c>foreach</c> over it does not allocate; through
/// <see cref="IEnumerable{T}"/> it is boxed, as any struct is.
/// </remarks>
public readonly struct ListView<T> : IReadOnlyList<T>
{
    private readonly IArrowElements<T>? _elements;
    private readonly int _start;
    private readonly int _count;

    /// <summary>A window of <paramref name="count"/> elements of <paramref name="elements"/>, from <paramref name="start"/>.</summary>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public ListView(IArrowElements<T> elements, int start, int count)
    {
        ArgumentNullException.ThrowIfNull(elements);
        ArgumentOutOfRangeException.ThrowIfNegative(start);
        ArgumentOutOfRangeException.ThrowIfNegative(count);
        _elements = elements;
        _start = start;
        _count = count;
    }

    /// <summary>The list at <paramref name="row"/> of <paramref name="list"/>, its elements read through <paramref name="elements"/>.</summary>
    /// <remarks>The offsets are absolute in the list's values child, a sliced list included.</remarks>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public static ListView<T> Of(ListArray list, int row, IArrowElements<T> elements)
    {
        ArgumentNullException.ThrowIfNull(list);
        ReadOnlySpan<int> offsets = list.ValueOffsets;
        return new ListView<T>(elements, offsets[row], offsets[row + 1] - offsets[row]);
    }

    /// <summary>The number of elements.</summary>
    public int Count => _count;

    /// <summary>The element at <paramref name="index"/>.</summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="index"/> is outside the list.</exception>
    public T this[int index]
    {
        get
        {
            Check(index);
            return _elements!.Get(_start + index);
        }
    }

    /// <summary>Whether the element at <paramref name="index"/> is null.</summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="index"/> is outside the list.</exception>
    public bool IsNull(int index)
    {
        Check(index);
        return _elements!.IsNull(_start + index);
    }

    /// <summary>A copy of the elements.</summary>
    public List<T> ToList()
    {
        var list = new List<T>(_count);
        for (int i = 0; i < _count; i++)
        {
            list.Add(_elements!.Get(_start + i));
        }
        return list;
    }

    /// <summary>An enumerator that does not allocate.</summary>
    public Enumerator GetEnumerator() => new(this);

    IEnumerator<T> IEnumerable<T>.GetEnumerator() => GetEnumerator();

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

    private void Check(int index)
    {
        if ((uint)index >= (uint)_count)
        {
            throw new ArgumentOutOfRangeException(nameof(index), index, null);
        }
    }

    /// <summary>Enumerates a <see cref="ListView{T}"/>.</summary>
    public struct Enumerator : IEnumerator<T>
    {
        private readonly ListView<T> _list;
        private int _index;

        internal Enumerator(ListView<T> list)
        {
            _list = list;
            _index = -1;
        }

        /// <inheritdoc/>
        public readonly T Current => _list._elements!.Get(_list._start + _index);

        readonly object? IEnumerator.Current => Current;

        /// <inheritdoc/>
        public bool MoveNext() => ++_index < _list._count;

        /// <inheritdoc/>
        public void Reset() => _index = -1;

        /// <inheritdoc/>
        public readonly void Dispose()
        {
        }
    }
}
