// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// `SubscriptionRequest::check` on the in-process provider: a checked subscription, the shared
// `internal::RunSchemaCheck` helper, and the provider-specific wiring in
// `in_process_provider.cpp` (`TopicState::pending_check`, `CreateTopic`'s rejection branch,
// `Unsubscribe`'s reset).

#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.h>

#include <chrono>
#include <fletcher/core/internal/delivery_frame.hpp>
#include <fletcher/core/internal/status_name.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/in_process_provider.hpp>
#include <fletcher/pubsub/internal/segments.hpp>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fletcher;

namespace {

// Re-anchored to the seam's taxonomy (spec §5.1), same pattern as
// test_publisher_subscriber.cpp and test_segments.cpp: assert the NUMBER, not which std:: type
// happened to be thrown.
::testing::AssertionResult RefusedWith(PubSubStatus want, const std::function<void()>& call) {
    try {
        call();
    } catch (const PubSubError& e) {
        if (e.status() == want) return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure()
               << "refused with " << internal::PubSubStatusName(e.status()) << ", wanted "
               << internal::PubSubStatusName(want) << " (" << e.what() << ")";
    } catch (const std::exception& e) {
        return ::testing::AssertionFailure()
               << "refused with an untyped exception, which is what the taxonomy exists to "
                  "replace: "
               << e.what();
    }
    return ::testing::AssertionFailure() << "the call was not refused at all";
}

/// Build a nanoarrow schema: struct{ x: int32 }.
OwnedSchema TestSchema() {
    OwnedSchema schema;
    ArrowSchemaInit(schema.get());
    ArrowSchemaSetTypeStruct(schema.get(), 1);
    ArrowSchemaSetName(schema->children[0], "x");
    ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32);
    return schema;
}

// The in-process provider decodes nothing, so any non-empty payload stands for a row.
void EncodeRow(WriteBuffer& buf) { buf.AppendByte(0x01); }

const std::vector<std::string> kTopic = {"checked", "topic"};

InProcessPubSubProvider MakeCarriedProvider() {
    ProviderConfig cfg;
    cfg.document = "schema_carriage=carried";
    return InProcessPubSubProvider(cfg);
}

PubSubProvider::SubscribeCallback CountingCallback(int& count) {
    return [&count](const uint8_t*, size_t, const SharedSchema&, const Attachments&) { ++count; };
}

}  // namespace

// The in-process provider's own refusal order, PINNED rather than merely demonstrated: rows (b)
// and (c) carry every LATER problem too, so a reorder reddens the row whose own check moved, not
// just "some check fired first". The in-process provider honours `check`, so there is no refusal
// row for it.
TEST(InProcessChecked, OverrideRefusesInOrder) {
    InProcessPubSubProvider p;
    PubSubProvider::SchemaCheck accept = [](const SharedSchema&) { return true; };
    PubSubProvider::SubscribeCallback noop = [](const uint8_t*, size_t, const SharedSchema&,
                                                const Attachments&) {};

    // (a) the re-entrancy door, pushed by hand rather than through a real delivery.
    {
        internal::DeliveryScope frame(static_cast<const PubSubProvider*>(&p));
        EXPECT_TRUE(RefusedWith(PubSubStatus::kReentrantCall,
                                [&] { (void)p.Subscribe(kTopic, {noop, "", accept}); }));
    }

    // (b) an empty segment list wins even against a request that ALSO has an empty callback and a
    // profile: segments are checked before either.
    EXPECT_TRUE(RefusedWith(PubSubStatus::kInvalidArgument, [&] {
        (void)p.Subscribe({}, {PubSubProvider::SubscribeCallback{}, "x", accept});
    }));

    // (c) valid segments, an empty callback AND a profile: the callback is checked before the
    // profile.
    EXPECT_TRUE(RefusedWith(PubSubStatus::kInvalidArgument, [&] {
        (void)p.Subscribe(kTopic, {PubSubProvider::SubscribeCallback{}, "x", accept});
    }));

    // (d) valid callback, a profile: this provider knows no profiles.
    EXPECT_TRUE(RefusedWith(PubSubStatus::kNotSupported,
                            [&] { (void)p.Subscribe(kTopic, {noop, "x", accept}); }));

    // (e) CreateTopic's own order: a declaration carrying both a bound and a profile is refused
    // for the BOUND, named in the message — the profile check never runs.
    try {
        p.CreateTopic(kTopic, {TestSchema(), "x", 1024});
        FAIL() << "expected kNotSupported";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kNotSupported);
        EXPECT_NE(std::string(e.what()).find("bound"), std::string::npos)
            << "the message must name the bound, not the profile: " << e.what();
    }
}

// A rejected Subscribe carrying a check, against an already-known schema, must leave the slot
// exactly as it found it: a plain Subscribe on the same topic keeps delivering.
TEST(InProcessChecked, CarriedKnownSchemaFalseThrowsAndLeavesTheLiveSubscription) {
    InProcessPubSubProvider provider = MakeCarriedProvider();
    provider.CreateTopic(kTopic, {TestSchema()});

    int plain_calls = 0;
    (void)provider.Subscribe(kTopic, {CountingCallback(plain_calls)});

    int check_calls = 0;
    EXPECT_TRUE(RefusedWith(PubSubStatus::kSchemaConflict, [&] {
        (void)provider.Subscribe(
            kTopic,
            {.callback = [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {},
             .check =
                 [&](const SharedSchema&) {
                     ++check_calls;
                     return false;
                 }});
    }));
    EXPECT_EQ(check_calls, 1);

    provider.Publish(kTopic, EncodeRow);
    EXPECT_EQ(plain_calls, 1);
}

// The schema is already known when Subscribe is called: the check runs synchronously, once, and
// a `true` answer is indistinguishable from an empty check from there.
TEST(InProcessChecked, CarriedKnownSchemaTrueDeliversAndChecksOnce) {
    InProcessPubSubProvider provider = MakeCarriedProvider();
    provider.CreateTopic(kTopic, {TestSchema()});

    int check_calls = 0;
    int delivered = 0;
    SubscriptionResult result = provider.Subscribe(
        kTopic, {.callback = CountingCallback(delivered), .check = [&](const SharedSchema&) {
                     ++check_calls;
                     return true;
                 }});

    SharedSchema sch;
    EXPECT_EQ(result.schema.Wait(std::chrono::milliseconds(0), &sch), PubSubStatus::kOk);
    EXPECT_TRUE(sch);
    EXPECT_EQ(check_calls, 1);

    provider.Publish(kTopic, EncodeRow);
    EXPECT_EQ(delivered, 1);
    EXPECT_EQ(check_calls, 1);
}

// kAsDeclared with a topic already declared: the non-null schema IS checked, synchronously, the
// same way kCarried's known-schema branch is.
TEST(InProcessChecked, AsDeclaredNonNullIsChecked) {
    InProcessPubSubProvider provider;
    provider.CreateTopic(kTopic, {TestSchema()});

    EXPECT_TRUE(RefusedWith(PubSubStatus::kSchemaConflict, [&] {
        (void)provider.Subscribe(
            kTopic,
            {.callback = [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {},
             .check = [](const SharedSchema&) { return false; }});
    }));

    int delivered = 0;
    SubscriptionResult result =
        provider.Subscribe(kTopic, {.callback = CountingCallback(delivered),
                                    .check = [](const SharedSchema&) { return true; }});

    SharedSchema sch;
    EXPECT_EQ(result.schema.Wait(std::chrono::milliseconds(0), &sch), PubSubStatus::kOk);
    EXPECT_TRUE(sch);

    provider.Publish(kTopic, EncodeRow);
    EXPECT_EQ(delivered, 1);
}

// A throwing check is absorbed and counts as `false`, in both the synchronous and the
// later-declaration cases.
TEST(InProcessChecked, AThrowingCheckIsARefusal) {
    InProcessPubSubProvider provider = MakeCarriedProvider();

    // Synchronous case: the schema is already known.
    provider.CreateTopic(kTopic, {TestSchema()});
    EXPECT_TRUE(RefusedWith(PubSubStatus::kSchemaConflict, [&] {
        (void)provider.Subscribe(
            kTopic,
            {.callback = [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {},
             .check = [](const SharedSchema&) -> bool { throw std::runtime_error("boom"); }});
    }));

    // Later-declaration case, on a different topic so the two subscriptions do not interact.
    const std::vector<std::string> other_topic = {"checked", "topic", "later"};
    int delivered = 0;
    SubscriptionResult result = provider.Subscribe(
        other_topic,
        {.callback = CountingCallback(delivered),
         .check = [](const SharedSchema&) -> bool { throw std::runtime_error("boom"); }});
    provider.CreateTopic(other_topic, {TestSchema()});

    SharedSchema sch;
    EXPECT_EQ(result.schema.Wait(std::chrono::seconds(1), &sch), PubSubStatus::kSchemaConflict);

    provider.Publish(other_topic, EncodeRow);
    EXPECT_EQ(delivered, 0);
}

// Unsubscribe clears a pending check the same way it clears the resolver: the declaration that
// follows never runs it, and a fresh plain Subscribe afterwards sees an ordinary, unchecked
// subscription.
TEST(InProcessChecked, UnsubscribeClearsAPendingCheck) {
    InProcessPubSubProvider provider = MakeCarriedProvider();

    int check_calls = 0;
    (void)provider.Subscribe(
        kTopic, {.callback = [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {},
                 .check =
                     [&](const SharedSchema&) {
                         ++check_calls;
                         return true;
                     }});

    provider.Unsubscribe(kTopic);
    provider.CreateTopic(kTopic, {TestSchema()});
    EXPECT_EQ(check_calls, 0);

    int delivered = 0;
    SubscriptionResult result = provider.Subscribe(kTopic, {CountingCallback(delivered)});
    SharedSchema sch;
    EXPECT_EQ(result.schema.Wait(std::chrono::milliseconds(0), &sch), PubSubStatus::kOk);
    EXPECT_TRUE(sch);

    provider.Publish(kTopic, EncodeRow);
    EXPECT_EQ(delivered, 1);
}
