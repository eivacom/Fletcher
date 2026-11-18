// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// N-1, enforced mechanically: no exception may cross in either direction.
//
// A managed exception escaping an `[UnmanagedCallersOnly]` method is not an
// exception - it is a FAIL-FAST. The runtime cannot unwind through the C frame
// that called in, so it does not try: the process dies, with no catch site
// anywhere able to see it. That makes an unwrapped thunk a defect of a kind
// tests do not normally reach, because reaching it ends the test run.
//
// The development plan asks for "a Roslyn analyzer or a reflection test that
// every such method's body is wrapped". This is the reflection test, and it is
// written HERE, at the slice that introduces the round's first thunk, because a
// reflection test over an empty set passes while asserting nothing - which is
// exactly why 3b deliberately left it unwritten.
//
// WHAT IT PROVES, precisely: that each thunk's OUTERMOST exception clause is a
// catch-all for `Exception`. WHAT IT DOES NOT: that nothing runs before the try
// begins, or that the catch itself cannot throw. Those are read in review.
//
// The behaviour is asserted separately in WriteWindowTests. Worth knowing why
// both exist: an escaping exception was checked empirically here, by narrowing
// the thunk's catch, and on Windows it did NOT fail-fast - it unwound through
// the shim's C++ frames and reached the managed caller, so the behavioural test
// still passed. That is a property of this platform's unwinder and of the flags
// the shim happens to be built with, not a contract; on a toolchain whose
// `catch (...)` sees it, the ORIGINAL exception is replaced by an FL_INTERNAL
// and the caller's own type is lost. Which is exactly why the rule is enforced
// structurally rather than by observing what happens to work.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;

using Eiva.Fletcher.Interop;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class ThunkDisciplineTests
{
    /// <summary>The assemblies that may legally contain a thunk.</summary>
    private static IEnumerable<Assembly> BindingAssemblies =>
    [
        typeof(FletcherCodec).Assembly,
        typeof(NativeMethods).Assembly,
    ];

    /// <summary>Every callback native can enter catches everything.</summary>
    [Fact]
    public void EveryUnmanagedCallersOnlyMethodIsWrapped()
    {
        List<MethodInfo> thunks = [.. BindingAssemblies.SelectMany(Thunks)];

        // The vacuity guard, and the reason this file did not exist one slice
        // ago: with no thunks in the tree, every assertion below is over an empty
        // sequence and passes.
        Assert.NotEmpty(thunks);

        foreach (MethodInfo thunk in thunks)
        {
            MethodBody? body = thunk.GetMethodBody();
            Assert.True(body is not null, $"{Describe(thunk)} has no readable body");

            IList<ExceptionHandlingClause> clauses = body!.ExceptionHandlingClauses;
            Assert.True(clauses.Count > 0, $"{Describe(thunk)} has no exception handling at all");

            // THE OUTERMOST clause, not merely SOME clause. The first version of
            // this test asked whether any clause caught `Exception`, and a
            // deliberate break proved it toothless: narrowing the thunk's own
            // catch to one exception type left the catch-all that guards the
            // FAILURE-RECORDING path, nested inside the handler, and the row went
            // on passing. The outermost clause is the one that decides whether
            // anything can leave.
            ExceptionHandlingClause outermost = clauses
                .OrderBy(clause => clause.TryOffset)
                .ThenByDescending(clause => clause.TryLength)
                .First();

            Assert.True(
                outermost.Flags == ExceptionHandlingClauseOptions.Clause &&
                (outermost.CatchType == typeof(Exception) || outermost.CatchType == typeof(object)),
                $"{Describe(thunk)} is entered from native code and its outermost handler is not a " +
                "catch-all. What an escaping exception does there is decided by the platform's " +
                "unwinder, not by this code: on Windows it travelled through the shim's C++ frames " +
                "and reached the managed caller; elsewhere it is a fail-fast or a swallowed " +
                "original. None of the three is a contract.");
        }
    }

    /// <summary>A thunk is static, non-generic, and returns a status or nothing.</summary>
    /// <remarks>
    /// The first two the compiler enforces, so asserting them is cheap
    /// documentation rather than a guard. The third is not enforced anywhere: a
    /// thunk that returned a managed type would be a marshalling stub on a path
    /// whose whole purpose is not having one, and the ABI's two callback
    /// signatures (<c>fl_grow_fn</c>, <c>fl_writer_fn</c>) return an
    /// <c>fl_status</c> and a <c>size_t</c> respectively.
    /// </remarks>
    [Fact]
    public void EveryThunkIsStaticAndBlittable()
    {
        List<MethodInfo> thunks = [.. BindingAssemblies.SelectMany(Thunks)];
        Assert.NotEmpty(thunks);

        foreach (MethodInfo thunk in thunks)
        {
            Assert.True(thunk.IsStatic, $"{Describe(thunk)} is not static");
            Assert.False(thunk.IsGenericMethod, $"{Describe(thunk)} is generic");

            Assert.True(
                IsBlittable(thunk.ReturnType),
                $"{Describe(thunk)} returns {thunk.ReturnType}, which is not a C type");

            foreach (ParameterInfo parameter in thunk.GetParameters())
            {
                Assert.True(
                    IsBlittable(parameter.ParameterType),
                    $"{Describe(thunk)} takes {parameter.ParameterType} for '{parameter.Name}', " +
                    "which is not a C type");
            }
        }
    }

    private static IEnumerable<MethodInfo> Thunks(Assembly assembly) =>
        assembly
            .GetTypes()
            .SelectMany(type => type.GetMethods(
                BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic |
                BindingFlags.DeclaredOnly))
            .Where(method =>
                method.GetCustomAttribute<UnmanagedCallersOnlyAttribute>() is not null);

    /// <summary>A screen for "this looks like a C type", not a proof of one.</summary>
    /// <remarks>
    /// The compiler already refuses a genuinely non-blittable signature on an
    /// <c>[UnmanagedCallersOnly]</c> method, so this is not the guard — it catches
    /// the readable cases (a <c>bool</c>, a <c>char</c>, a generic value type)
    /// early and states the shape a reader should expect.
    /// </remarks>
    private static bool IsBlittable(Type type) =>
        type == typeof(void) ||
        type.IsPointer ||
        (type.IsPrimitive && type != typeof(bool) && type != typeof(char)) ||
        (type.IsValueType && !type.IsGenericType);

    private static string Describe(MethodInfo method) =>
        $"{method.DeclaringType?.FullName}.{method.Name}";
}
