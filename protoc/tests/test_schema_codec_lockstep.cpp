// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// PR #125 review finding 4 — the schema/codec field-drop lockstep.
//
// Two separate hand-written walks answer the same question, "does this field
// become a column?":
//
//   * IsSchemaRepresentable(), inside BuildFlattenedFieldList -> schema + TS
//   * ProjectIrToFieldMapping() returning nullopt, via GatherFields -> the rest
//
// They agree today, and a source comment asserts they are "kept in exact
// structural lockstep" — but that was a MANUAL invariant, the exact class of
// drift round GIR eliminated everywhere else. It matters because the decode
// visitor's IsNull index is a position in the GatherFields list while the
// schema's child count and order come from BuildFlattenedFieldList: if one ever
// drops a shape the other keeps, every field after the divergence decodes from
// the wrong positional slot, silently.
//
// This test makes the invariant asserted rather than asserted-about. It covers
// every drop reason the classifier can currently produce, so adding a NodeKind
// (or enabling FIXED_SIZE_LIST in only one switch) turns it red.

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cpp_backend_schema_visitor.hpp"
#include "generator_internal.hpp"
#include "ir.hpp"
#include "type_mapper.hpp"

using namespace fletcher;
using namespace google::protobuf;
namespace cb = fletcher::cpp_backend;

namespace {

FieldDescriptorProto* AddField(
    DescriptorProto* msg, const char* name, int number, FieldDescriptorProto::Type type,
    FieldDescriptorProto::Label label = FieldDescriptorProto::LABEL_OPTIONAL) {
    auto* f = msg->add_field();
    f->set_name(name);
    f->set_number(number);
    f->set_type(type);
    f->set_label(label);
    return f;
}

void SetMessageFlatten(DescriptorProto* msg) {
    auto* opts = msg->mutable_options();
    opts->GetReflection()->MutableUnknownFields(opts)->AddVarint(50000, 1);
}

// A corpus deliberately mixing KEPT and DROPPED shapes in one message, and
// interleaving them, so a divergence shows up as both a count mismatch and an
// order mismatch.
const FileDescriptor* BuildCorpus(DescriptorPool* pool) {
    FileDescriptorProto fdp;
    fdp.set_name("lockstep.proto");
    fdp.set_syntax("proto3");
    fdp.set_package("lockstep");

    auto* inner = fdp.add_message_type();
    inner->set_name("Inner");
    AddField(inner, "v", 1, FieldDescriptorProto::TYPE_STRING);

    // Struct-leaf flatten wrapper: `repeated W1` -> List<List<Struct>> (depth 2).
    auto* w1 = fdp.add_message_type();
    w1->set_name("W1");
    SetMessageFlatten(w1);
    AddField(w1, "v", 1, FieldDescriptorProto::TYPE_MESSAGE, FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".lockstep.Inner");

    // Scalar-leaf flatten wrappers: `repeated S1` is depth 3, `repeated S2` is
    // depth 4 — both representable; the depth limit lives in the view emitters.
    auto* s0 = fdp.add_message_type();
    s0->set_name("S0");
    SetMessageFlatten(s0);
    AddField(s0, "v", 1, FieldDescriptorProto::TYPE_INT32, FieldDescriptorProto::LABEL_REPEATED);
    auto* s1 = fdp.add_message_type();
    s1->set_name("S1");
    SetMessageFlatten(s1);
    AddField(s1, "v", 1, FieldDescriptorProto::TYPE_MESSAGE, FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".lockstep.S0");
    auto* s2 = fdp.add_message_type();
    s2->set_name("S2");
    SetMessageFlatten(s2);
    AddField(s2, "v", 1, FieldDescriptorProto::TYPE_MESSAGE, FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".lockstep.S1");

    // Self-referential flatten wrapper -> Unsupported (recursive).
    auto* cyc = fdp.add_message_type();
    cyc->set_name("Cyclic");
    SetMessageFlatten(cyc);
    AddField(cyc, "v", 1, FieldDescriptorProto::TYPE_MESSAGE, FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".lockstep.Cyclic");

    auto* host = fdp.add_message_type();
    host->set_name("Host");
    AddField(host, "k_scalar", 1, FieldDescriptorProto::TYPE_INT64);
    // Dropped: real oneof.
    auto* oo = host->add_oneof_decl();
    oo->set_name("choice");
    AddField(host, "d_oneof_a", 2, FieldDescriptorProto::TYPE_INT32)->set_oneof_index(0);
    AddField(host, "d_oneof_b", 3, FieldDescriptorProto::TYPE_STRING)->set_oneof_index(0);
    AddField(host, "k_struct", 4, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".lockstep.Inner");
    AddField(host, "k_rep_scalar", 5, FieldDescriptorProto::TYPE_STRING,
             FieldDescriptorProto::LABEL_REPEATED);
    AddField(host, "k_rep_struct", 6, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".lockstep.Inner");
    // Kept: struct-leaf nested list, depth 2.
    AddField(host, "k_nested_struct", 7, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".lockstep.W1");
    // Kept: scalar-leaf nested list, depth 3 (the bound's upper edge).
    AddField(host, "k_nested_scalar", 8, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".lockstep.S1");
    // KEPT, and load-bearing: a depth-4 scalar-leaf nested list. It is beyond what
    // the Arrow view layer can render (guarded there with a static_assert) but is
    // fully representable in the schema, storage and edge codec — and
    // transitive_gate.proto depends on that. Both walks must keep it.
    AddField(host, "k_too_deep_but_representable", 9, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".lockstep.S2");
    // Dropped: recursive flatten wrapper.
    AddField(host, "d_cyclic", 10, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".lockstep.Cyclic");
    // Kept, and LAST: its slot is what a divergence upstream would shift.
    AddField(host, "k_tail", 11, FieldDescriptorProto::TYPE_BOOL);

    return pool->BuildFile(fdp);
}

}  // namespace

TEST(SchemaCodecLockstep, BothWalksKeepAndDropExactlyTheSameFields) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildCorpus(&pool);
    ASSERT_NE(file, nullptr);

    for (int m = 0; m < file->message_type_count(); ++m) {
        const Descriptor* msg = file->message_type(m);
        if (msg->options().map_entry()) continue;

        const std::vector<cb::SchemaFieldRecord> schema_fields = cb::BuildFlattenedFieldList(msg);
        std::string skipped;
        const std::vector<FieldInfo> codec_fields = GatherFields(msg, &skipped);

        ASSERT_EQ(schema_fields.size(), codec_fields.size())
            << msg->full_name() << ": schema child count and codec field count diverged — "
            << "IsSchemaRepresentable and ProjectIrToFieldMapping no longer agree";

        for (size_t i = 0; i < schema_fields.size(); ++i) {
            EXPECT_EQ(schema_fields[i].name, codec_fields[i].name)
                << msg->full_name() << " slot " << i << ": positional drift";
            EXPECT_EQ(schema_fields[i].field_id, codec_fields[i].field_id)
                << msg->full_name() << " slot " << i << ": field_id drift";
        }
    }
}

// Pin the corpus itself. If a future change made one of the dropped shapes
// representable (or a kept one unrepresentable), the lockstep test above would
// still pass while silently covering less. This asserts the corpus really does
// exercise both outcomes.
TEST(SchemaCodecLockstep, CorpusActuallyExercisesBothKeptAndDroppedShapes) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildCorpus(&pool);
    ASSERT_NE(file, nullptr);
    const Descriptor* host = file->FindMessageTypeByName("Host");
    ASSERT_NE(host, nullptr);

    int kept = 0;
    int dropped = 0;
    for (int i = 0; i < host->field_count(); ++i) {
        const FieldDescriptor* fd = host->field(i);
        const bool representable = ProjectIrToFieldMapping(ir::BuildFieldIr(fd), file).has_value();
        if (fd->name().rfind("k_", 0) == 0) {
            EXPECT_TRUE(representable) << fd->name() << " was expected to be kept";
            ++kept;
        } else {
            EXPECT_FALSE(representable) << fd->name() << " was expected to be dropped";
            ++dropped;
        }
    }
    EXPECT_GE(kept, 7);
    EXPECT_GE(dropped, 3);
}
