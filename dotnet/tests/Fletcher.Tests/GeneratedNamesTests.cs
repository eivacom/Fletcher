// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-6 review B2: the plugin's C# for protoc/tests/golden/csharp_names.proto, compiled into this
// assembly. Most of the proof is that it compiles at all: each message there has a name that a
// bare class name in a code expression would bind to instead of the class (a property of another
// type, a local `row`, FromArrow's parameter `array`, the pair's own `Topic`), and before the fix
// each was CS0120, CS0176 or CS1061. These cases add that every shape still converts, and that the
// pair for a message called Topic publishes and delivers over the shim.
using System;
using System.Collections.Generic;
using System.Linq;

using Apache.Arrow;
using Apache.Arrow.Types;

using Fletcher.Gen.Golden.Names;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class GeneratedNamesTests : IDisposable
{
    private readonly PubSubProviderHandle _provider;

    public GeneratedNamesTests()
    {
        _provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
    }

    public void Dispose() => _provider.Dispose();

    private static T RoundTrip<T>(T sent, Func<IEnumerable<T>, RecordBatch> toArrow, Func<StructArray, int, T> fromArrow)
    {
        using RecordBatch batch = toArrow(new[] { sent });
        return fromArrow(new StructArray(new StructType(batch.Schema.FieldsList), batch.Length, batch.Arrays, ArrowBuffer.Empty, 0), 0);
    }

    [Fact]
    public void AFieldNamedAfterAMessageOfAnotherShapeConverts()
    {
        RouteList list = RoundTrip(
            new RouteList { Waypoint = { new Waypoint { Lat = 1 }, new Waypoint { Lat = 2 } } },
            RouteList.ToArrow, RouteList.FromArrow);
        Assert.Equal(new[] { 1.0, 2.0 }, list.Waypoint.Select(w => w.Lat));

        RouteMap map = RoundTrip(
            new RouteMap { Waypoint = { new("a", new Waypoint { Lat = 3 }) } },
            RouteMap.ToArrow, RouteMap.FromArrow);
        Assert.Equal(("a", 3.0), (map.Waypoint.Single().Key, map.Waypoint.Single().Value.Lat));

        RouteMixed mixed = RoundTrip(
            new RouteMixed { Start = new Waypoint { Lat = 4 }, Waypoint = 7 },
            RouteMixed.ToArrow, RouteMixed.FromArrow);
        Assert.Equal((4.0, 7), (mixed.Start.Lat, mixed.Waypoint));

        Assert.Null(RoundTrip(new RouteOptional(), RouteOptional.ToArrow, RouteOptional.FromArrow).Waypoint);
        Assert.Equal(5.0, RoundTrip(
            new RouteOptional { Waypoint = new Waypoint { Lat = 5 } },
            RouteOptional.ToArrow, RouteOptional.FromArrow).Waypoint!.Lat);

        // The control, which compiled before the fix too.
        Assert.Equal(6.0, RoundTrip(
            new RouteSingle { Waypoint = new Waypoint { Lat = 6 } },
            RouteSingle.ToArrow, RouteSingle.FromArrow).Waypoint.Lat);
    }

    [Fact]
    public void MessagesNamedLikeGeneratedLocalsConvert()
    {
        UsesLocals back = RoundTrip(
            new UsesLocals { R = new row { A = 8 }, Items = { new array { B = 9 }, new array { B = 10 } } },
            UsesLocals.ToArrow, UsesLocals.FromArrow);

        Assert.Equal(8, back.R.A);
        Assert.Equal(new[] { 9, 10 }, back.Items.Select(i => i.B));
    }

    [Fact]
    public void ALowerCaseTypeNameIsWrittenVerbatimAndKeepsItsName()
    {
        // `message class` is a C# keyword and `enum level` a lower-case name: the plugin writes
        // them `@class` and `@level`, which compile with every warning an error (no CS8981), and
        // the compiled names carry no `@`: they are the .proto's names.
        UsesKeywords back = RoundTrip(
            new UsesKeywords { K = new @class { C = 1 }, Lv = level.High, Ks = { new @class { C = 2 } } },
            UsesKeywords.ToArrow, UsesKeywords.FromArrow);

        Assert.Equal((1, level.High, 2), (back.K.C, back.Lv, back.Ks.Single().C));
        Assert.Equal("class", typeof(@class).Name);
        Assert.Equal("level", typeof(level).Name);
        Assert.Equal("row", typeof(row).Name);
    }

    [Fact]
    public void AMessageNamedAfterAGeneratedMemberIsSuffixedAndWorks()
    {
        // D-BIND-84: `message Schema` is class Schema_ (and its field `schema` the property
        // Schema__), `message FromArrow` class FromArrow_; both convert, and the pair whose input
        // is Schema publishes and delivers.
        UsesOwnNames back = RoundTrip(
            new UsesOwnNames { S = new Schema_ { Schema__ = 3 }, Fs = { new FromArrow_ { X = 4 } } },
            UsesOwnNames.ToArrow, UsesOwnNames.FromArrow);
        Assert.Equal((3, 4), (back.S.Schema__, back.Fs.Single().X));

        using var subscriber = new Feed_DescribeSubscriber(_provider);
        var seen = new List<int>();
        using Subscription subscription = subscriber.Subscribe((received, attachments) => seen.Add(received.Schema__));
        using var publisher = new Feed_DescribePublisher(_provider);
        publisher.Publish(new Schema_ { Schema__ = 5 });
        Assert.Equal(new[] { 5 }, seen);
        Assert.Same(Schema_.Schema, Feed_DescribePublisher.Schema);
    }

    [Fact]
    public void ThePairForAMessageNamedTopicPublishesAndDelivers()
    {
        using var subscriber = new Feed_SendSubscriber(_provider);
        var seen = new List<int>();
        using Subscription subscription = subscriber.Subscribe((received, attachments) => seen.Add(received.A));
        using var publisher = new Feed_SendPublisher(_provider);

        publisher.Publish(new Topic { A = 11 });
        publisher.Publish(new[] { new Topic { A = 12 }, new Topic { A = 13 } });

        Assert.Equal(new[] { 11, 12, 13 }, seen);
        Assert.Same(Topic.Schema, Feed_SendPublisher.Schema);
    }
}
