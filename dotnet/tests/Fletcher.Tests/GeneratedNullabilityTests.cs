// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// D-BIND-86 (BIND-6 review Q4): G-5 asks that the generated C#'s nullability match the Arrow
// schema's, or a consumer reading the annotations is told something the columns do not say. The
// ruling scopes it to FIELDS: each property of a generated class is nullable exactly when the
// matching top-level field of the class's own Schema is. CHILDREN (list items, map keys and map
// values) follow the C++ row class's storage instead, which cannot hold a null
// (`std::vector<int32_t>`), so every child in C# is non-null, whatever nanoarrow's default child
// nullability in the schema says.
//
// Every generated class compiled into this assembly is checked (the coverage golden, the pair and
// names goldens, timestamp.proto's model), found by reflection, so a golden added later is
// covered without being listed here. A property is paired with its field by declaration order,
// which the generator keeps equal to the schema's; the counts must agree first.
//
// One field is a named exception, ruled with D-BIND-86: an `optional` flatten-wrapper list
// (`optional StructListWrapper optional_flattened_struct_list`). Its field is nullable, but C++
// stores it as `std::vector<Leaf>` (measured 2026-10-14), so C# keeps a non-null `List<Leaf>` and
// null reads as empty. The review accepted that as debt (G-Q4). The exception must still be met,
// so it cannot outlive the shape unnoticed; any other mismatch fails.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class GeneratedNullabilityTests
{
    // Class.Property names whose field is nullable while the property is not (G-Q4, see above).
    private static readonly HashSet<string> NullableFieldNonNullProperty = new(StringComparer.Ordinal)
    {
        "CompositeCoverage.OptionalFlattenedStructList",
        // BIND-7b: the same shape in the accessor golden, there so the view's read of a null row
        // (an empty window) is tested.
        "Track.MaybePoints",
    };

    private static IReadOnlyList<Type> GeneratedClasses() =>
        typeof(GeneratedNullabilityTests).Assembly.GetTypes()
            .Where(t => t.IsClass && t.Namespace is not null
                && t.Namespace.StartsWith("Fletcher.Gen", StringComparison.Ordinal)
                && t.GetProperty("Schema", BindingFlags.Public | BindingFlags.Static)?.PropertyType
                    == typeof(Apache.Arrow.Schema)
                && t.GetMethod("FromArrow", BindingFlags.Public | BindingFlags.Static) is not null)
            .OrderBy(t => t.FullName, StringComparer.Ordinal)
            .ToList();

    private static bool IsNullable(Type type, NullabilityInfo info) =>
        type.IsValueType ? Nullable.GetUnderlyingType(type) is not null : info.ReadState == NullabilityState.Nullable;

    // Every child of a List<T> or KeyValuePair<K,V>, at any depth, with a path for the message.
    private static IEnumerable<(string Path, Type Type, NullabilityInfo Info)> Children(string path, NullabilityInfo info)
    {
        Type t = info.Type;
        if (!t.IsGenericType)
        {
            yield break;
        }
        Type def = t.GetGenericTypeDefinition();
        if (def != typeof(List<>) && def != typeof(KeyValuePair<,>))
        {
            yield break;
        }
        string[] names = def == typeof(List<>) ? new[] { "item" } : new[] { "key", "value" };
        for (int i = 0; i < names.Length; i++)
        {
            NullabilityInfo child = info.GenericTypeArguments[i];
            string childPath = path + "." + names[i];
            // A KeyValuePair is the map entry, not a child of its own: only its key and value count.
            if (!(child.Type.IsGenericType && child.Type.GetGenericTypeDefinition() == typeof(KeyValuePair<,>)))
            {
                yield return (childPath, child.Type, child);
            }
            foreach (var grand in Children(childPath, child))
            {
                yield return grand;
            }
        }
    }

    [Fact]
    public void EachPropertyIsNullableExactlyWhenItsFieldIs()
    {
        var context = new NullabilityInfoContext();
        var problems = new List<string>();
        int nullable = 0, nonNullable = 0;
        var met = new HashSet<string>(StringComparer.Ordinal);
        IReadOnlyList<Type> classes = GeneratedClasses();
        foreach (Type cls in classes)
        {
            var schema = (Apache.Arrow.Schema)cls.GetProperty("Schema", BindingFlags.Public | BindingFlags.Static)!.GetValue(null)!;
            PropertyInfo[] props = cls.GetProperties(BindingFlags.Public | BindingFlags.Instance | BindingFlags.DeclaredOnly)
                .OrderBy(p => p.MetadataToken).ToArray();
            if (props.Length != schema.FieldsList.Count)
            {
                problems.Add($"{cls.Name}: {props.Length} properties, {schema.FieldsList.Count} fields");
                continue;
            }
            for (int i = 0; i < props.Length; i++)
            {
                Apache.Arrow.Field field = schema.FieldsList[i];
                NullabilityInfo info = context.Create(props[i]);
                bool propNullable = IsNullable(props[i].PropertyType, info);
                string name = cls.Name + "." + props[i].Name;
                if (NullableFieldNonNullProperty.Contains(name))
                {
                    if (field.IsNullable && !propNullable)
                    {
                        met.Add(name);
                    }
                }
                else if (propNullable != field.IsNullable)
                {
                    problems.Add($"{name}: property nullable={propNullable}, field '{field.Name}' nullable={field.IsNullable}");
                }
                if (field.IsNullable)
                {
                    nullable++;
                }
                else
                {
                    nonNullable++;
                }
            }
        }
        Assert.Empty(problems);
        Assert.Equal(NullableFieldNonNullProperty.OrderBy(n => n, StringComparer.Ordinal), met.OrderBy(n => n, StringComparer.Ordinal));
        // Not vacuous: the goldens hold many classes and fields of both kinds.
        Assert.True(classes.Count >= 20, $"only {classes.Count} generated classes found");
        Assert.True(nullable >= 10 && nonNullable >= 10, $"nullable={nullable} nonNullable={nonNullable}");
    }

    [Fact]
    public void EveryChildIsNonNull()
    {
        var context = new NullabilityInfoContext();
        var problems = new List<string>();
        int children = 0;
        foreach (Type cls in GeneratedClasses())
        {
            foreach (PropertyInfo p in cls.GetProperties(BindingFlags.Public | BindingFlags.Instance | BindingFlags.DeclaredOnly))
            {
                foreach (var (path, type, info) in Children(cls.Name + "." + p.Name, context.Create(p)))
                {
                    children++;
                    if (IsNullable(type, info))
                    {
                        problems.Add($"{path} is nullable");
                    }
                }
            }
        }
        Assert.Empty(problems);
        Assert.True(children >= 20, $"only {children} children found");
    }
}
