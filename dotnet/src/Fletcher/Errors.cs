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

/// <summary>Turns an ABI status into the right managed exception, or nothing; one decoder, two callers.</summary>
internal static class Errors
{
    /// <summary>The message of a failure that carries none (the header allows it only when allocating one failed).</summary>
    internal const string NoMessage = "Fletcher failed and the shim could not allocate a message for it";

    /// <summary>Throw if <paramref name="status"/> is a failure; always release the message.</summary>
    /// <param name="status">The value the entry point returned.</param>
    /// <param name="err">The error it filled. Released here, whatever happens.</param>
    /// <param name="captured">
    /// A managed exception a callback recorded on its own side, if any.
    /// </param>
    /// <param name="sentence">
    /// One sentence the binding appends after the shim's verbatim message, if any; the number
    /// and the origin are unchanged. Used once: the edge build's refusal of a batched
    /// subscribe names the server package (d10 L3, D29).
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
    internal static void ThrowIfFailed(int status, ref FlError err, ExceptionDispatchInfo? captured = null, string? sentence = null)
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

            throw ToException(status, in err, sentence);
        }
        finally
        {
            NativeMethods.fl_error_dispose(ref err);
        }
    }

    /// <summary>
    /// The exception for a failure, built and not thrown; <paramref name="err"/> is only read, and
    /// DISPOSED BY NOBODY HERE. <see cref="ThrowIfFailed"/> throws it and disposes; the batch thunk
    /// copies a failed hand-over's borrowed <c>failure</c> with it, which the shim frees after the
    /// callback (d10 L8, d9 L2), so the handler gets what a throw would have carried.
    /// </summary>
    internal static FletcherException ToException(int status, in FlError err, string? sentence = null)
    {
        string message = sentence is null ? ReadMessage(in err) : ReadMessage(in err) + " " + sentence;
        FletcherStatus typed = (FletcherStatus)status;

        // The ORIGIN says which catch arm fired; the STATUS says what went wrong.
        // Malformed input is the pair of them: an InvalidArgument from the codec.
        // A codec-origin PayloadTooLarge is a row that does not fit the bound -
        // valid data, a limit the caller can raise - and a codec-origin Internal
        // is a defect; neither is the format exception (D-BIND-54).
        return (FletcherOrigin)err.Origin == FletcherOrigin.Codec && typed == FletcherStatus.InvalidArgument
            ? new FletcherFormatException(typed, message)
            : new FletcherException(typed, (FletcherOrigin)err.Origin, message);
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
            return NoMessage;
        }

        return Encoding.UTF8.GetString((byte*)err.Message, checked((int)err.MessageLen));
    }
}
