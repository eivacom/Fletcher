// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The managed end of `fl_write_window`, and the round's first thunk.
//
// ── What the native side promises, and what this has to promise back ────────
// `binding.h` states four normative properties for anyone writing into a window,
// and one of them is the whole reason this class is not three lines: the encoder
// uses RANDOM ACCESS, NOT A STREAM. Fletcher back-patches length prefixes and
// null bitfields at offsets below `pos`, so a refill must carry every byte below
// the cursor across VERBATIM. A refill that "grew" by handing back a fresh empty
// buffer would still encode successfully and produce a corrupt row.
//
// ── Why it does not simply grow the caller's IBufferWriter ──────────────────
// The obvious implementation asks the writer for a bigger `Memory<byte>` on each
// refill and keeps writing into it. That works for every implementation in the
// BCL, because they preserve bytes written but not yet advanced - and it is not
// something `IBufferWriter<byte>` PROMISES. A third-party writer that returned a
// fresh buffer would satisfy its own contract and corrupt every row long enough
// to need a second window, silently, because the bytes still parse as a
// well-formed encoding of different values.
//
// So growth is this class's own business: the first window is the writer's
// memory, and the first refill moves into a pooled buffer that this class copies
// into itself. A row that fits the writer's first span - the common case - is
// written straight into it and costs nothing. A row that does not costs one copy
// per refill, on a path the header already describes as the "bytes in hand"
// route rather than the zero-copy one (that is `Publish`, and it has no buffer
// at all).
//
// ── The thunk rule (N-1) ───────────────────────────────────────────────────
// An exception escaping an `[UnmanagedCallersOnly]` method is a FAIL-FAST, not
// an exception: the runtime cannot unwind through a C frame. So the thunk below
// catches everything, records the exception on this side, and returns a status.
// `Errors.ThrowIfFailed` then rethrows the original with its own stack, which is
// D-BIND-19 rule 3 - a managed caller's contract is not rewritten by having
// crossed a boundary.
using System;
using System.Buffers;
using System.Runtime.CompilerServices;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;
using System.Text;

using Eiva.Fletcher.Interop;

namespace Eiva.Fletcher;

/// <summary>A growable <c>fl_write_window</c> over an <see cref="IBufferWriter{T}"/>.</summary>
/// <remarks>
/// One instance per encode call, which is what makes the codec and the bound
/// batch usable from several threads at once: none of the mutable state of an
/// encode lives on either of them.
/// </remarks>
internal sealed unsafe class GrowingWindow : IDisposable
{
    /// <summary>What the first window asks the caller's writer for.</summary>
    /// <remarks>
    /// A hint, not a requirement: a writer may hand back more or less, and a row
    /// that does not fit refills into this class's own buffer. Sized so that the
    /// rows a message-shaped schema produces normally fit on the first try, which
    /// is the case that costs no copy at all.
    ///
    /// Internal rather than private so the tests can size a fixture ABOVE it and
    /// know a refill happened, instead of restating the number and hoping the two
    /// stay equal — a duplicated threshold is a refill test that quietly stops
    /// refilling the day this changes.
    /// </remarks>
    internal const int FirstWindowHint = 512;

    private readonly IBufferWriter<byte> _output;
    private readonly Exception? _refillFault;
    private GCHandle _self;

    private MemoryHandle _writerPin;
    private bool _writerPinned;

    private byte[]? _pooled;
    private GCHandle _pooledPin;

    private GCHandle _messagePin;
    private int _written;

    /// <summary>Build a window over <paramref name="output"/>.</summary>
    /// <param name="output">Where the encoded row ends up.</param>
    /// <param name="refillFault">
    /// Thrown by the next refill instead of growing. The only way to reach the
    /// thunk's catch: once growth stopped going through the caller's writer,
    /// nothing on this path can fail short of the allocator itself, and a
    /// containment clause that is never executed is a claim rather than a
    /// property. Set by the tests only; always null in use.
    /// </param>
    internal GrowingWindow(IBufferWriter<byte> output, Exception? refillFault = null)
    {
        _output = output;
        _refillFault = refillFault;

        // The context the thunk gets back. Not pinned - it is an opaque token
        // that native never dereferences, only hands back (N-6).
        _self = GCHandle.Alloc(this);
    }

    /// <summary>The exception a refill captured, if one did.</summary>
    internal ExceptionDispatchInfo? Captured { get; private set; }

    /// <summary>Encode one row through this window, returning the ABI status.</summary>
    /// <remarks>
    /// Nothing is handed to the caller's writer here. A failed encode must leave
    /// the writer exactly as it found it, so the advance is <see cref="Commit"/>,
    /// called only after the status has been checked.
    /// </remarks>
    internal int Encode(BoundRowsHandle rows, int row, ref FlError err)
    {
        Memory<byte> first = _output.GetMemory(FirstWindowHint);
        _writerPin = first.Pin();
        _writerPinned = true;

        FlWriteWindow window = new()
        {
            Ctx = GCHandle.ToIntPtr(_self),
            Data = (nint)_writerPin.Pointer,
            Capacity = (nuint)first.Length,
            Pos = 0,
            Grow = (nint)(delegate* unmanaged[Cdecl]<FlWriteWindow*, nuint, FlError*, int>)&GrowThunk,
        };

        int status = NativeMethods.fl_encode_row(rows, row, ref window, ref err);
        if (status == (int)FletcherStatus.Ok)
        {
            _written = checked((int)window.Pos);
        }

        return status;
    }

    /// <summary>Hand the encoded bytes to the caller's writer. Success only.</summary>
    internal void Commit()
    {
        if (_pooled is null)
        {
            // The row fit the writer's own memory and was written into it: the
            // only thing left is to say how much of it is real.
            _output.Advance(_written);
            return;
        }

        // It did not fit, so the bytes live in this class's buffer. One copy, on
        // the path that asked for bytes in hand.
        _output.Write(_pooled.AsSpan(0, _written));
    }

    /// <inheritdoc/>
    public void Dispose()
    {
        ReleaseWindow();

        if (_messagePin.IsAllocated)
        {
            _messagePin.Free();
        }

        if (_self.IsAllocated)
        {
            _self.Free();
        }
    }

    /// <summary>The refill hook native calls. Nothing may escape it.</summary>
    /// <remarks>
    /// Static, blittable in and out, and cdecl to match <c>fl_grow_fn</c>. The
    /// window is the same structure this class handed down, so its <c>ctx</c> is
    /// the token that leads back to the instance.
    /// </remarks>
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int GrowThunk(FlWriteWindow* window, nuint minBytes, FlError* err)
    {
        GrowingWindow? state = null;
        try
        {
            state = (GrowingWindow?)GCHandle.FromIntPtr(window->Ctx).Target;
            state!.Grow(window, minBytes);
            return (int)FletcherStatus.Ok;
        }
        catch (Exception exception)
        {
            try
            {
                state?.Capture(exception, err);
            }
            catch (Exception)
            {
                // Recording the failure failed too - out of memory, most likely.
                // The number still crosses, and the shim's own text covers a
                // refusal that carried no message. What must NOT happen here is a
                // second exception leaving this frame.
            }

            return (int)FletcherStatus.Internal;
        }
    }

    /// <summary>Move to a buffer with room for <paramref name="minBytes"/> more.</summary>
    private void Grow(FlWriteWindow* window, nuint minBytes)
    {
        if (_refillFault is not null)
        {
            throw _refillFault;
        }

        int pos = checked((int)window->Pos);
        int capacity = checked((int)window->Capacity);

        // Bounds by SUBTRACTION, never addition - the window's own rule, and the
        // reason an absurd `min_bytes` cannot wrap this into a small allocation.
        if (minBytes > (nuint)(Array.MaxLength - pos))
        {
            throw new OutOfMemoryException(
                $"the row needs {minBytes} more bytes at offset {pos}, which is past the largest " +
                "buffer this runtime can allocate");
        }

        int wanted = Math.Max(
            pos + (int)minBytes,
            capacity <= Array.MaxLength / 2 ? capacity * 2 : Array.MaxLength);

        byte[] grown = ArrayPool<byte>.Shared.Rent(wanted);
        GCHandle grownPin = GCHandle.Alloc(grown, GCHandleType.Pinned);

        // THE BYTES BELOW THE CURSOR CROSS VERBATIM. Everything Fletcher
        // back-patches lives down there; a refill that lost them would produce a
        // row that still parses, as different values.
        new ReadOnlySpan<byte>((void*)window->Data, pos).CopyTo(grown);

        ReleaseWindow();
        _pooled = grown;
        _pooledPin = grownPin;

        window->Data = grownPin.AddrOfPinnedObject();
        window->Capacity = (nuint)grown.Length;

        // `pos` is left alone, as the contract requires: grow reports where the
        // room is, never what has been written.
    }

    /// <summary>Record a refill's failure for the throw site, and for native.</summary>
    /// <remarks>
    /// The message is BORROWED to the shim (D-BIND-32): it copies the bytes and
    /// frees nothing. It does so immediately after this returns, so the pin is
    /// held until this window is disposed rather than released here — a buffer
    /// that the collector may move between the return and the copy is the same
    /// defect as one that was freed.
    /// </remarks>
    private void Capture(Exception exception, FlError* err)
    {
        Captured = ExceptionDispatchInfo.Capture(exception);

        byte[] text = Encoding.UTF8.GetBytes(
            $"the managed grow callback threw {exception.GetType().FullName}: {exception.Message}");

        if (_messagePin.IsAllocated)
        {
            _messagePin.Free();
        }

        _messagePin = GCHandle.Alloc(text, GCHandleType.Pinned);

        err->Status = (int)FletcherStatus.Internal;
        err->Message = _messagePin.AddrOfPinnedObject();
        err->MessageLen = (nuint)text.Length;
    }

    /// <summary>Let go of whatever the window is currently pointing at.</summary>
    private void ReleaseWindow()
    {
        if (_pooledPin.IsAllocated)
        {
            _pooledPin.Free();
        }

        if (_pooled is not null)
        {
            ArrayPool<byte>.Shared.Return(_pooled);
            _pooled = null;
        }

        if (_writerPinned)
        {
            _writerPin.Dispose();
            _writerPinned = false;
        }
    }
}
