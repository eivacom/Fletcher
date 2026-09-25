// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// A topic's schema, shared, and the one place two lifetimes meet.
//
// ── THE MOST EXPENSIVE MISTAKE AT THIS BOUNDARY, and why this type exists ───
// `fl_schema` carries an owner handle plus a BORROWED `ArrowSchema*`. Running
// the Arrow C Data Interface's own `release` on that pointer destroys the schema
// for every other holder - including the provider still delivering on it - and
// the failure surfaces later, elsewhere, in something that looks unrelated. The
// header calls it the single most expensive mistake available here, and it is
// the ordinary thing to do with an `ArrowSchema*`, which is exactly why this
// type wraps one rather than handing it out.
//
// So there are two releases and they are not interchangeable:
//
//   fl_schema_release   the HANDLE. Drops one reference to the shared schema.
//   copy.release        the COPY. An independent schema the caller owns.
//
// `ToArrowSchema` is the bridge, and it needs a deep copy from the shim because
// Apache.Arrow's importer TAKES OWNERSHIP of what it is given - handing it the
// shared pointer would run exactly the release this file exists to prevent.
// D-BIND-46 added `fl_schema_copy` for it; before that, this method could not be
// written at all.
//
// ── A NULL schema is an ANSWER, not a failure ───────────────────────────────
// A schema-less transport reports kOk with a null schema (seam §7 clause 1),
// meaning "this transport carries no schemas; the caller brings its own". That
// is the one kOk on which there is nothing to release, and confusing it with a
// failure is the divergence §7 exists to prevent: on a schema-carrying transport
// the two demand opposite handling, and guessing wrong decodes into the wrong
// slot silently rather than crashing.
using System;

using Apache.Arrow;
using Apache.Arrow.C;

using Eiva.Fletcher.Interop;

namespace Eiva.Fletcher;

/// <summary>A reference to a topic's schema, shared with the transport.</summary>
public sealed unsafe class SchemaHandle : IDisposable
{
    private FlSchema _schema;
    private readonly bool _owned;
    private bool _disposed;

    /// <summary>A handle that OWNS its reference and releases it on dispose.</summary>
    internal SchemaHandle(FlSchema schema) : this(schema, owned: true)
    {
    }

    /// <summary>A handle that may or may not own its reference.</summary>
    /// <remarks>
    /// A DELIVERY'S SCHEMA IS BORROWED, and that is why this distinction exists.
    /// The shim holds the reference for the duration of the callback and drops it
    /// afterwards; a handler that called <see cref="Dispose"/> on what it was
    /// handed would drop a reference it never took, and the schema would die under
    /// the provider still delivering on it. So a borrowed handle's Dispose is a
    /// NO-OP rather than a bug waiting to happen - the handler is user code
    /// running on a transport thread, and the cost of being wrong there is not a
    /// managed exception.
    ///
    /// <see cref="Retain"/> always hands back an OWNED handle, which is how a
    /// handler keeps a schema past its frame.
    /// </remarks>
    internal SchemaHandle(FlSchema schema, bool owned)
    {
        _schema = schema;
        _owned = owned;

        // A borrowed handle took no reference, so its finaliser would have
        // nothing to drop - and running one anyway would drop the SHIM's.
        if (!owned)
        {
            GC.SuppressFinalize(this);
        }
    }

    /// <summary>Whether this is the schema-less transport's answer.</summary>
    /// <remarks>
    /// True means the transport carries no schemas at all and the caller supplies
    /// its own - NOT that a schema failed to arrive, and NOT that one is still
    /// pending. Those are separate outcomes on <see cref="SchemaArrival"/>.
    /// </remarks>
    public bool IsNull => _schema.Schema == 0;

    /// <summary>Take another reference to the same shared schema.</summary>
    /// <remarks>
    /// What a delivery handler calls to keep the schema past its frame: everything
    /// a handler receives is borrowed for the duration of the call. The returned
    /// handle is independent - disposing one does not disturb the other.
    /// </remarks>
    public SchemaHandle Retain()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        NativeMethods.fl_schema_retain(in _schema);
        return new SchemaHandle(_schema);
    }

    /// <summary>Import an independent Arrow schema of your own.</summary>
    /// <exception cref="InvalidOperationException">This is the schema-less answer.</exception>
    /// <remarks>
    /// The shim deep-copies first, and the copy is what the importer consumes. The
    /// returned <see cref="Schema"/> is ordinary managed Arrow with no tie to this
    /// handle: disposing this afterwards, or before, changes nothing about it.
    /// </remarks>
    public Schema ToArrowSchema()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        if (IsNull)
        {
            throw new InvalidOperationException(
                "this transport carries no schemas, so there is no schema to import; " +
                "check IsNull before calling, and supply your own schema instead");
        }

        CArrowSchema* copy = CArrowSchema.Create();
        try
        {
            FlError err = default;
            int status = NativeMethods.fl_schema_copy(in _schema, (nint)copy, ref err);
            Errors.ThrowIfFailed(status, ref err);

            // ImportSchema TAKES OWNERSHIP of the structure, which is precisely why
            // it is handed the copy and never `_schema.Schema`.
            return CArrowSchemaImporter.ImportSchema(copy);
        }
        catch
        {
            // The import did not happen, so the copy is still ours to free. On the
            // success path ImportSchema owns it and freeing here would be a double
            // release - which is why this is a catch and not a finally.
            CArrowSchema.Free(copy);
            throw;
        }
    }

    /// <summary>Drop this reference to the shared schema.</summary>
    /// <remarks>
    /// Never the Arrow release callback. Safe to call more than once, a no-op on
    /// the schema-less answer (which has no owner to count), and a no-op on a
    /// handle a delivery BORROWED to a handler.
    /// </remarks>
    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;

        // A borrowed handle took no reference, so it has none to drop. See the
        // constructor: this is the delivery case, and silently doing nothing is
        // the only safe answer there.
        if (_owned)
        {
            NativeMethods.fl_schema_release(in _schema);
        }

        GC.SuppressFinalize(this);
    }

    /// <summary>The safety net for an OWNED handle nobody disposed.</summary>
    /// <remarks>
    /// <para>
    /// Why a finaliser here, when this type is not a <c>SafeHandle</c> (D-BIND-55):
    /// <c>fl_schema</c> is TWO pointers, an owner and a borrowed schema, and a
    /// <c>SafeHandle</c> wraps one; and a borrowed handle must never release. But
    /// every other native handle in this binding IS a <c>SafeHandle</c>, and is
    /// finalised, so without this an undisposed <see cref="Retain"/> was the one
    /// native leak the tier allowed.
    /// </para>
    /// <para>
    /// Safe on the finaliser thread because the header makes it so:
    /// <c>fl_schema_release</c> never fails, never re-enters the seam and is safe
    /// from any thread. It is still the fallback - <see cref="Dispose"/> is the
    /// contract, and the finaliser only bounds the cost of forgetting it.
    /// </para>
    /// </remarks>
    ~SchemaHandle()
    {
        if (_disposed || !_owned)
        {
            return;
        }

        NativeMethods.fl_schema_release(in _schema);
        ReleasedByFinalizerForTest?.Invoke(_schema.Owner);
    }

    /// <summary>Called with the owner of each reference the FINALISER released.</summary>
    /// <remarks>
    /// Observes the branch rather than a count, because a count of finalisations is
    /// process-wide and xUnit runs classes in parallel. Null in production.
    /// </remarks>
    internal static Action<nint>? ReleasedByFinalizerForTest { get; set; }

    /// <summary>The native owner this handle refers to, for the test above.</summary>
    internal nint OwnerForTest => _schema.Owner;
}
