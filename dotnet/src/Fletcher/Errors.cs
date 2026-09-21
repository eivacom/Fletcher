// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// THE throw site. One function, called by every managed wrapper, so the taxonomy
// is readable in one place instead of inferred from forty call sites (D-BIND-19
// rule 2, managed direction).
using System;
using System.Runtime.ExceptionServices;
using System.Text;

using Eiva.Fletcher.Interop;

namespace Eiva.Fletcher;

/// <summary>Turns an ABI status into the right managed exception, or nothing.</summary>
internal static class Errors
{
    /// <summary>Throw if <paramref name="status"/> is a failure; always release the message.</summary>
    /// <param name="status">The value the entry point returned.</param>
    /// <param name="err">The error it filled. Released here, whatever happens.</param>
    /// <param name="captured">
    /// A managed exception a callback recorded on its own side, if any.
    /// </param>
    /// <remarks>
    /// Three rules meet here.
    ///
    /// RULE 3 — a managed exception that caused a native failure is rethrown AS
    /// ITSELF, with its own stack, not wrapped in a FletcherException. A user's
    /// <c>InvalidDataException</c> thrown from their own row writer comes back as
    /// that. Native still saw a real failure and logged it, so the C++ side stays
    /// consistent; what changes is that the managed caller's contract is not
    /// rewritten by having crossed a boundary. <paramref name="captured"/> is
    /// checked FIRST for that reason.
    ///
    /// RULE 1 — the number and the message always cross, and the message is used
    /// verbatim. It is UTF-8 and NOT NUL-terminated, so the length is
    /// authoritative; decoding by length rather than by scanning is also what
    /// makes an embedded escape sequence survive.
    ///
    /// The DISPOSE IS A FINALLY, unconditionally. The message is heap-allocated by
    /// the shim on every failure, and a throw site that released it only on the
    /// paths it remembered would leak on the others.
    /// </remarks>
    internal static void ThrowIfFailed(int status, ref FlError err, ExceptionDispatchInfo? captured = null)
    {
        if (status == (int)FletcherStatus.Ok)
        {
            return;
        }

        try
        {
            // Rule 3, before anything else: the original exception outranks the
            // status the shim synthesised from it.
            captured?.Throw();

            string message = ReadMessage(in err);
            FletcherStatus typed = (FletcherStatus)status;

            throw (FletcherOrigin)err.Origin == FletcherOrigin.Codec
                ? new FletcherFormatException(typed, message)
                : new FletcherException(typed, (FletcherOrigin)err.Origin, message);
        }
        finally
        {
            NativeMethods.fl_error_dispose(ref err);
        }
    }

    /// <summary>The shim's message, decoded by LENGTH.</summary>
    private static unsafe string ReadMessage(in FlError err)
    {
        if (err.Message == 0 || err.MessageLen == 0)
        {
            // The header allows a failure to carry no message only when
            // allocating one failed, and says the number is worth more than
            // nothing. Say which case this is rather than throwing an empty
            // string at the reader.
            return "Fletcher failed and the shim could not allocate a message for it";
        }

        return Encoding.UTF8.GetString((byte*)err.Message, checked((int)err.MessageLen));
    }
}
