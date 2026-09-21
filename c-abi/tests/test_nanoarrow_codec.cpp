// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-2's forcing tests. Two properties, one file:
//
//   ENCODE (BIND-2a) — the nanoarrow codec writes the bytes `arrow-bridge`'s
//   codec writes. Not similar bytes — the same bytes.
//
//   DECODE (BIND-2b) — decode is encode's inverse, in both directions: the
//   values come back as the values that went in (Arrow equality), and the bytes
//   come back as the bytes that went in (re-encode identity). Neither alone is
//   enough. Arrow equality alone would pass a decoder that produced the right
//   values through a wrong-but-compensating framing; re-encode identity alone
//   would pass a decoder that swapped two same-typed columns, because swapping
//   them back on the way out restores the bytes exactly.
//
// ── Why this is the whole safety argument ───────────────────────────────────
// This round puts a SECOND encoder of the positional wire format into the tree.
// One is unavoidable: the Arrow C++ one cannot sit behind a C ABI (it returns
// the row, which is the copy, and takes a vector of shared_ptr<Scalar>, which
// cannot cross P/Invoke). Two encoders is a standing invitation to drift, and
// drift here is not a crash — it is a subscriber decoding a publisher's bytes
// into the wrong fields, silently, on a production bus.
//
// So the two are compared byte for byte over a corpus of schemas, on every CI
// run. A single differing byte fails, and the failure names the fixture and the
// row.
//
// ── Why this test links arrow-bridge and the component does not ─────────────
// `fletcher-c-abi` must never link `arrow-bridge`: that dependency would pull
// Arrow C++ into the shipped per-RID native asset, which is most of the reason
// the codec was written on nanoarrow in the first place. The TEST links it,
// because the oracle has to be the real thing rather than a transcription of it.
// A test-only dependency is not a shipped one, and CI's packed-size check
// measures the packaged shim rather than this binary.
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/core/status.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/nanoarrow_codec.hpp"
#include "codec_corpus.hpp"

namespace {

using fletcher::abi::BoundRows;
using fletcher::abi::NanoarrowCodec;

using fletcher::abi::corpus::Composites;
using fletcher::abi::corpus::Corpus;
using fletcher::abi::corpus::Fixture;
using fletcher::abi::corpus::Nested;
using fletcher::abi::corpus::Scalars;

/// The oracle: `arrow-bridge`'s codec, over the same row.
std::vector<uint8_t> OracleEncode(const arrow::RecordBatch& batch, int64_t row) {
    fletcher::Codec codec(batch.schema());
    fletcher::ArrowRow values;
    values.reserve(static_cast<size_t>(batch.num_columns()));
    for (int c = 0; c < batch.num_columns(); ++c) {
        auto scalar = batch.column(c)->GetScalar(row);
        EXPECT_TRUE(scalar.ok()) << scalar.status().ToString();
        values.push_back(scalar.ValueOrDie());
    }
    return codec.EncodeRow(values);
}

/// The subject: the nanoarrow codec, over the batch exported across the C Data
/// Interface — which is exactly how a binding hands it over.
std::vector<uint8_t> SubjectEncode(const arrow::RecordBatch& batch, int64_t row) {
    ArrowSchema c_schema = {};
    ArrowArray c_array = {};
    const arrow::Status exported = arrow::ExportRecordBatch(batch, &c_array, &c_schema);
    EXPECT_TRUE(exported.ok()) << exported.ToString();

    std::vector<uint8_t> bytes;
    {
        NanoarrowCodec codec(c_schema);
        BoundRows rows(codec, c_array);
        fletcher::VectorWriteBuffer buffer;
        codec.EncodeRow(rows, row, buffer);
        bytes = buffer.Finish();
    }

    // The borrow rule, standing guard: the codec never consumed the export, so
    // both structures are still ours to release. Had BoundRows called the
    // array's release callback, this would be a double free.
    if (c_array.release != nullptr) c_array.release(&c_array);
    if (c_schema.release != nullptr) c_schema.release(&c_schema);
    return bytes;
}

std::string Hex(const std::vector<uint8_t>& bytes) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0F]);
    }
    return out;
}

TEST(NanoarrowCodec, ByteIdenticalToArrowBridge) {
    const std::vector<Fixture> corpus = Corpus();
    ASSERT_FALSE(corpus.empty()) << "the corpus is empty, so this row proves nothing";

    int compared = 0;
    for (const Fixture& fixture : corpus) {
        // A fixture whose builders failed would have zero rows, and the loop
        // below would then compare nothing and pass. The vacuity is the thing to
        // guard: a green row that ran no comparison looks exactly like a green
        // row that ran every one.
        ASSERT_GT(fixture.batch->num_rows(), 0) << "fixture '" << fixture.name << "' built no rows";

        for (int64_t row = 0; row < fixture.batch->num_rows(); ++row) {
            const std::vector<uint8_t> expected = OracleEncode(*fixture.batch, row);
            const std::vector<uint8_t> actual = SubjectEncode(*fixture.batch, row);

            ASSERT_FALSE(expected.empty())
                << "fixture '" << fixture.name << "', row " << row
                << ": the ORACLE produced no bytes, so equality here would mean nothing";
            ++compared;

            // Compared as hex so a failure prints the bytes rather than a
            // container's address, and the first differing nibble is findable.
            ASSERT_EQ(Hex(expected), Hex(actual))
                << "fixture '" << fixture.name << "', row " << row
                << ": the nanoarrow codec and arrow-bridge's codec disagree about the wire "
                   "format. One wire format means these two are never allowed to differ by a "
                   "byte - a difference here is a subscriber decoding a publisher's bytes into "
                   "the wrong fields, silently.";
        }
    }

    // The corpus is the coverage claim, so its size is stated rather than
    // implied: five fixtures of three rows each.
    EXPECT_EQ(compared, 15) << "the corpus changed size; update this count deliberately";
}

/// The borrow rule, as its own row rather than as a side effect of the one above.
///
/// `fl_rows_bind` promises the array is borrowed and never consumed: that is what
/// lets ONE export serve N publishes. If the codec ever called the release
/// callback, the pointer would be nulled and this row says so directly.
TEST(NanoarrowCodec, BindBorrowsTheArrayAndNeverConsumesIt) {
    const Fixture fixture = Scalars();

    ArrowSchema c_schema = {};
    ArrowArray c_array = {};
    const arrow::Status exported = arrow::ExportRecordBatch(*fixture.batch, &c_array, &c_schema);
    ASSERT_TRUE(exported.ok()) << exported.ToString();

    {
        NanoarrowCodec codec(c_schema);
        BoundRows rows(codec, c_array);
        fletcher::VectorWriteBuffer buffer;
        for (int64_t row = 0; row < fixture.batch->num_rows(); ++row) {
            codec.EncodeRow(rows, row, buffer);
        }
        EXPECT_NE(c_array.release, nullptr)
            << "the array was released while still bound - one export must serve N publishes";
    }

    EXPECT_NE(c_array.release, nullptr)
        << "unbinding released the caller's array; the ABI promises the caller releases it";
    EXPECT_NE(c_schema.release, nullptr) << "the schema export was consumed; it is deep-copied";

    c_array.release(&c_array);
    c_schema.release(&c_schema);
}

/// Refusals name the field. A schema the wire format cannot carry is refused at
/// OPEN — before a single row is bound — and the message says which field, because
/// "unions are not supported" without a field name costs its reader an afternoon
/// on a wide schema.
///
/// The subject used to be a dictionary. It is a UNION now, because D-BIND-39 made
/// dictionaries supported, and a refusal test still pointing at behaviour that is
/// now a feature would lock in the very defect it was written to prevent. A union
/// is the honest replacement: the proto mapping produces none (`oneof` is in the
/// wire spec's own §"Unsupported Types"), and the format's union framing exists
/// for the Arrow-native tier, which is not this codec's.
TEST(NanoarrowCodec, RefusesAnUnsupportedTypeNamingTheField) {
    auto union_type = arrow::dense_union({arrow::field("n", arrow::int32())});
    auto schema =
        arrow::schema({arrow::field("id", arrow::int32()), arrow::field("category", union_type)});

    ArrowSchema c_schema = {};
    ASSERT_TRUE(arrow::ExportSchema(*schema, &c_schema).ok());

    try {
        NanoarrowCodec codec(c_schema);
        ADD_FAILURE() << "a union column was accepted; the format's union framing belongs to the "
                         "Arrow-native tier";
    } catch (const fletcher::PubSubError& e) {
        EXPECT_EQ(e.status(), fletcher::PubSubStatus::kInvalidArgument);
        EXPECT_NE(std::string(e.what()).find("category"), std::string::npos)
            << "the refusal did not name the offending field: " << e.what();
    }

    c_schema.release(&c_schema);
}

// ---------------------------------------------------------------------------
// BIND-2b — decode
// ---------------------------------------------------------------------------

/// Every row of a batch, encoded back to back into one buffer.
///
/// This is the shape `fl_decode_rows` is handed: N rows, no framing between
/// them. The format is self-delimiting, so "where does row 2 begin" has exactly
/// one answer and the decoder has to find it the same way the encoder placed it.
std::vector<uint8_t> EncodeAllRows(const arrow::RecordBatch& batch) {
    ArrowSchema c_schema = {};
    ArrowArray c_array = {};
    const arrow::Status exported = arrow::ExportRecordBatch(batch, &c_array, &c_schema);
    EXPECT_TRUE(exported.ok()) << exported.ToString();

    std::vector<uint8_t> bytes;
    {
        NanoarrowCodec codec(c_schema);
        BoundRows rows(codec, c_array);
        fletcher::VectorWriteBuffer buffer;
        for (int64_t row = 0; row < batch.num_rows(); ++row) {
            codec.EncodeRow(rows, row, buffer);
        }
        bytes = buffer.Finish();
    }

    if (c_array.release != nullptr) c_array.release(&c_array);
    if (c_schema.release != nullptr) c_schema.release(&c_schema);
    return bytes;
}

/// Decode is the inverse of encode, proven twice over per fixture.
///
/// The round trip runs through the C Data Interface at both ends, because that
/// is the only way a binding ever touches this codec: the batch is exported,
/// encoded, decoded into a FRESH array the test owns, re-encoded from that
/// array, and imported back into Arrow C++ for a value comparison.
TEST(NanoarrowCodec, DecodeIsTheInverseOfEncode) {
    const std::vector<Fixture> corpus = Corpus();
    ASSERT_FALSE(corpus.empty()) << "the corpus is empty, so this row proves nothing";

    int compared = 0;
    for (const Fixture& fixture : corpus) {
        ASSERT_GT(fixture.batch->num_rows(), 0) << "fixture '" << fixture.name << "' built no rows";

        const std::vector<uint8_t> encoded = EncodeAllRows(*fixture.batch);
        ASSERT_FALSE(encoded.empty())
            << "fixture '" << fixture.name
            << "' encoded to nothing, so decoding it back would prove nothing";

        ArrowSchema c_schema = {};
        ASSERT_TRUE(arrow::ExportSchema(*fixture.batch->schema(), &c_schema).ok());

        NanoarrowCodec codec(c_schema);

        ArrowArray decoded = {};
        codec.DecodeRows(encoded.data(), encoded.size(), fixture.batch->num_rows(), &decoded);
        ASSERT_NE(decoded.release, nullptr)
            << "fixture '" << fixture.name << "': decode produced no array to own";
        ASSERT_EQ(decoded.length, fixture.batch->num_rows())
            << "fixture '" << fixture.name << "': decode produced the wrong number of rows";

        // (1) The BYTES come back. Re-encoding what decode produced must
        // reproduce the buffer decode was given, byte for byte.
        std::vector<uint8_t> reencoded;
        {
            BoundRows rows(codec, decoded);
            fletcher::VectorWriteBuffer buffer;
            for (int64_t row = 0; row < decoded.length; ++row) {
                codec.EncodeRow(rows, row, buffer);
            }
            reencoded = buffer.Finish();
        }
        EXPECT_EQ(Hex(encoded), Hex(reencoded))
            << "fixture '" << fixture.name
            << "': re-encoding the decoded rows did not reproduce the wire bytes, so decode and "
               "encode disagree about the format";

        // (2) The VALUES come back. `ImportRecordBatch` consumes both structures,
        // which is also the ownership claim being tested: the array decode handed
        // over is a complete, self-owning export and Arrow can take it.
        auto imported = arrow::ImportRecordBatch(&decoded, &c_schema);
        ASSERT_TRUE(imported.ok())
            << "fixture '" << fixture.name
            << "': the decoded array was not importable: " << imported.status().ToString();
        const std::shared_ptr<arrow::RecordBatch> actual = imported.ValueOrDie();
        EXPECT_TRUE(actual->Equals(*fixture.batch))
            << "fixture '" << fixture.name << "': the values did not survive the round trip.\n"
            << "expected:\n"
            << fixture.batch->ToString() << "\nactual:\n"
            << actual->ToString();
        ++compared;
    }

    EXPECT_EQ(compared, 5) << "the corpus changed size; update this count deliberately";
}

/// Decode refuses malformed bytes THROUGH `PositionalReader`, never around it.
///
/// This is the whole of BIND-2's "malformed-input parity with HARD-1..7"
/// acceptance, and it is stated as a property rather than as a list. The HARD
/// rounds hardened one reader; the binding inherits that hardening only for as
/// long as nothing in the decoder reads a length, a count or a bitfield by hand.
/// So: mutate the buffer everywhere, and require every refusal to carry the
/// READER's prefix. A decoder that grew its own bounds check would announce
/// itself here as a message that does not start with "PositionalReader:".
///
/// The sweep is a four-byte 0xFF window walked across a valid encoding, which is
/// the cheapest way to hit every count, every length prefix and every bitfield in
/// the corpus's most structural fixture without naming a single byte offset.
TEST(NanoarrowCodec, DecodeRefusalsComeFromTheReader) {
    const Fixture fixture = Composites();
    const std::vector<uint8_t> valid = EncodeAllRows(*fixture.batch);
    ASSERT_GT(valid.size(), 4U) << "the fixture encoded to too little to mutate meaningfully";

    ArrowSchema c_schema = {};
    ASSERT_TRUE(arrow::ExportSchema(*fixture.batch->schema(), &c_schema).ok());
    NanoarrowCodec codec(c_schema);

    int refused = 0;
    for (size_t i = 0; i + 4 <= valid.size(); ++i) {
        std::vector<uint8_t> corrupt = valid;
        for (size_t b = 0; b < 4; ++b) corrupt[i + b] = 0xFF;

        ArrowArray decoded = {};
        try {
            codec.DecodeRows(corrupt.data(), corrupt.size(), fixture.batch->num_rows(), &decoded);
            // A mutation the format happens to accept is fine - the bytes are
            // still a well-formed encoding of different values. Release and move
            // on; the guard below insists the sweep found SOME refusals.
            if (decoded.release != nullptr) decoded.release(&decoded);
        } catch (const std::invalid_argument& e) {
            ++refused;
            EXPECT_EQ(std::string(e.what()).rfind("PositionalReader:", 0), 0U)
                << "a refusal at mutation offset " << i
                << " did not come from the reader, so the decoder is checking bounds of its own "
                   "and the HARD-1..7 hardening no longer covers the binding: "
                << e.what();
            EXPECT_EQ(decoded.release, nullptr)
                << "a failed decode left something in the caller's ArrowArray";
        } catch (const fletcher::PubSubError& e) {
            ADD_FAILURE() << "malformed input at offset " << i
                          << " produced a codec-level refusal rather than a reader one. Malformed "
                             "BYTES are the reader's to refuse; a PubSubError here means the "
                             "decoder decided something the reader should have: "
                          << e.what();
        }
    }

    // Vacuity guard. A sweep that refused nothing would pass every assertion
    // above while proving nothing at all.
    EXPECT_GT(refused, 0)
        << "no mutation was refused, so this row asserted nothing about malformed input";

    c_schema.release(&c_schema);
}

/// A truncated buffer is refused at every truncation point, and a buffer with
/// anything left over is refused too.
///
/// The prefix sweep is deterministic rather than probabilistic: the null
/// bitfield sits at the front and is untouched, so every field the full row read
/// a shorter one must also read, and it must run out. The trailing-byte half is
/// the batch-level `VerifyFullyConsumed` - a count and a buffer that disagree,
/// which is exactly how a framing bug reaches production silently.
TEST(NanoarrowCodec, DecodeRefusesTruncatedAndOverlongBuffers) {
    const Fixture fixture = Nested();
    const std::vector<uint8_t> valid = EncodeAllRows(*fixture.batch);
    ASSERT_FALSE(valid.empty());

    ArrowSchema c_schema = {};
    ASSERT_TRUE(arrow::ExportSchema(*fixture.batch->schema(), &c_schema).ok());
    NanoarrowCodec codec(c_schema);

    for (size_t prefix = 0; prefix < valid.size(); ++prefix) {
        ArrowArray decoded = {};
        EXPECT_THROW(codec.DecodeRows(valid.data(), prefix, fixture.batch->num_rows(), &decoded),
                     std::invalid_argument)
            << "a " << prefix << "-byte prefix of a " << valid.size()
            << "-byte encoding was accepted";
        EXPECT_EQ(decoded.release, nullptr) << "a failed decode handed back a partial array";
    }

    {
        std::vector<uint8_t> overlong = valid;
        overlong.push_back(0x00);
        ArrowArray decoded = {};
        EXPECT_THROW(
            codec.DecodeRows(overlong.data(), overlong.size(), fixture.batch->num_rows(), &decoded),
            std::invalid_argument)
            << "a trailing byte was ignored; the count and the buffer are allowed to disagree";
        EXPECT_EQ(decoded.release, nullptr);
    }

    // The control: the unmutated buffer decodes. Without it, every row above
    // would still pass if DecodeRows simply always threw.
    {
        ArrowArray decoded = {};
        ASSERT_NO_THROW(
            codec.DecodeRows(valid.data(), valid.size(), fixture.batch->num_rows(), &decoded));
        ASSERT_NE(decoded.release, nullptr);
        decoded.release(&decoded);
    }

    c_schema.release(&c_schema);
}

/// A map with a composite key is refused at OPEN, not half-decoded at row 1.
///
/// The encoder would happily write one; the decoder cannot read one back in a
/// single pass, because a map's keys all arrive before its values and holding a
/// half-built struct aside until its value shows up is a second decoder. The
/// proto mapping never produces such a key, so the two halves are kept honest by
/// refusing it where the refusal is cheap and nameable.
TEST(NanoarrowCodec, RefusesAMapWithACompositeKeyNamingTheField) {
    auto key_type = arrow::struct_({arrow::field("part", arrow::int32())});
    auto map_type = arrow::map(key_type, arrow::int32());
    auto schema = arrow::schema({arrow::field("lookup", map_type)});

    ArrowSchema c_schema = {};
    ASSERT_TRUE(arrow::ExportSchema(*schema, &c_schema).ok());

    try {
        NanoarrowCodec codec(c_schema);
        ADD_FAILURE() << "a composite map key was accepted; decode cannot read one back";
    } catch (const fletcher::PubSubError& e) {
        EXPECT_EQ(e.status(), fletcher::PubSubStatus::kInvalidArgument);
        EXPECT_NE(std::string(e.what()).find("lookup"), std::string::npos)
            << "the refusal did not name the offending field: " << e.what();
    }

    c_schema.release(&c_schema);
}

/// Decode's argument checks, which are the binding's and not the reader's: a
/// null destination, a negative count, and a null pointer that claims a length.
TEST(NanoarrowCodec, DecodeRejectsImpossibleArguments) {
    const Fixture fixture = Nested();
    const std::vector<uint8_t> valid = EncodeAllRows(*fixture.batch);

    ArrowSchema c_schema = {};
    ASSERT_TRUE(arrow::ExportSchema(*fixture.batch->schema(), &c_schema).ok());
    NanoarrowCodec codec(c_schema);

    ArrowArray decoded = {};
    EXPECT_THROW(codec.DecodeRows(valid.data(), valid.size(), 1, nullptr), std::invalid_argument);
    EXPECT_THROW(codec.DecodeRows(valid.data(), valid.size(), -1, &decoded), std::invalid_argument);
    EXPECT_THROW(codec.DecodeRows(nullptr, valid.size(), 1, &decoded), std::invalid_argument);
    EXPECT_EQ(decoded.release, nullptr);

    // Zero rows out of zero bytes is not an error: it is the empty batch, and a
    // subscriber that receives one should get an empty array rather than a throw.
    ArrowArray empty = {};
    ASSERT_NO_THROW(codec.DecodeRows(nullptr, 0, 0, &empty));
    ASSERT_NE(empty.release, nullptr);
    EXPECT_EQ(empty.length, 0);
    empty.release(&empty);

    c_schema.release(&c_schema);
}

// ---------------------------------------------------------------------------
// D-BIND-39 — dictionaries, per the wire spec §"Dictionary Types"
// ---------------------------------------------------------------------------

/// Build a `dictionary(int32, utf8)` column from indices into `values`.
std::shared_ptr<arrow::Array> DictionaryColumn(const std::vector<std::string>& values,
                                               const std::vector<int32_t>& indices) {
    arrow::StringBuilder value_builder;
    EXPECT_TRUE(value_builder.AppendValues(values).ok());
    std::shared_ptr<arrow::Array> dictionary = value_builder.Finish().ValueOrDie();

    arrow::Int32Builder index_builder;
    EXPECT_TRUE(index_builder.AppendValues(indices).ok());
    std::shared_ptr<arrow::Array> index_array = index_builder.Finish().ValueOrDie();

    return arrow::DictionaryArray::FromArrays(arrow::dictionary(arrow::int32(), arrow::utf8()),
                                              index_array, dictionary)
        .ValueOrDie();
}

/// A dictionary column goes on the wire as its VALUE type, one value per row.
///
/// This is the parity case the whole ruling was about: the indices are a columnar
/// optimisation with no meaning in a single row, so what reaches the wire is
/// exactly what a plain `utf8` column would have put there — which is what lets a
/// C# publisher and a C++ subscriber agree on a schema carrying a dictionary.
///
/// The oracle is the plain column rather than a byte literal, because a literal
/// would have to be regenerated by the same encoder it is meant to check.
TEST(NanoarrowCodec, ADictionaryColumnEncodesItsValues) {
    auto dict_batch = arrow::RecordBatch::Make(
        arrow::schema({arrow::field("v", arrow::dictionary(arrow::int32(), arrow::utf8()))}), 3,
        {DictionaryColumn({"alpha", "beta", "gamma"}, {2, 0, 1})});

    arrow::StringBuilder plain_builder;
    ASSERT_TRUE(plain_builder.AppendValues({"gamma", "alpha", "beta"}).ok());
    auto plain_batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("v", arrow::utf8())}),
                                                3, {plain_builder.Finish().ValueOrDie()});

    EXPECT_EQ(EncodeAllRows(*dict_batch), EncodeAllRows(*plain_batch))
        << "a dictionary column did not encode as its value type";
}

/// Decode gives back the VALUE type, and the codec says so before it is asked.
///
/// The consequence that makes this more than an encoder change: what a caller
/// BINDS and what it gets back are no longer the same schema. A binding that
/// imported a decoded array against the bind schema would be telling Arrow to
/// read a dictionary's buffers from an array that has none, so `decoded_schema()`
/// exists and this row is what holds it to what decode actually produces.
TEST(NanoarrowCodec, ADictionaryDecodesAsItsValueTypeAndTheCodecSaysSo) {
    auto schema = arrow::schema(
        {arrow::field("v", arrow::dictionary(arrow::int32(), arrow::utf8()), /*nullable=*/true)});
    auto batch = arrow::RecordBatch::Make(schema, 2, {DictionaryColumn({"alpha", "beta"}, {1, 0})});

    const std::vector<uint8_t> encoded = EncodeAllRows(*batch);

    ArrowSchema c_schema = {};
    ASSERT_TRUE(arrow::ExportSchema(*schema, &c_schema).ok());
    NanoarrowCodec codec(c_schema);

    // The field keeps its own name and nullability: those belong to the FIELD,
    // not to the dictionary's value type, which carries neither.
    ASSERT_EQ(codec.decoded_schema().n_children, 1);
    EXPECT_STREQ(codec.decoded_schema().children[0]->name, "v");
    EXPECT_STREQ(codec.decoded_schema().children[0]->format, "u") << "decode should give utf8";
    EXPECT_EQ(codec.decoded_schema().children[0]->flags & ARROW_FLAG_NULLABLE, ARROW_FLAG_NULLABLE);

    ArrowArray decoded = {};
    codec.DecodeRows(encoded.data(), encoded.size(), 2, &decoded);

    // Imported against the DECODED schema, which is the whole point.
    ArrowSchema decoded_copy = {};
    ASSERT_EQ(ArrowSchemaDeepCopy(&codec.decoded_schema(), &decoded_copy), NANOARROW_OK);
    auto imported = arrow::ImportRecordBatch(&decoded, &decoded_copy).ValueOrDie();

    ASSERT_EQ(imported->num_rows(), 2);
    auto column = std::static_pointer_cast<arrow::StringArray>(imported->column(0));
    EXPECT_EQ(column->GetString(0), "beta");
    EXPECT_EQ(column->GetString(1), "alpha");

    c_schema.release(&c_schema);
}

/// A dictionary whose values are not scalars is refused, naming the field.
///
/// The spec's own restriction, and the reason is worth keeping next to it: the
/// wire carries a dictionary as ONE VALUE where the format says one value goes,
/// so a struct or list value type would have to smuggle its own framing in there.
/// `arrow-bridge` refuses it too — this is parity, not a second opinion.
TEST(NanoarrowCodec, ADictionaryWithANestedValueTypeIsRefusedByName) {
    auto nested =
        arrow::dictionary(arrow::int32(), arrow::struct_({arrow::field("x", arrow::int32())}));
    auto schema =
        arrow::schema({arrow::field("id", arrow::int32()), arrow::field("category", nested)});

    ArrowSchema c_schema = {};
    ASSERT_TRUE(arrow::ExportSchema(*schema, &c_schema).ok());

    try {
        NanoarrowCodec codec(c_schema);
        ADD_FAILURE() << "a dictionary with a struct value type was accepted; it has no "
                         "single-row form";
    } catch (const fletcher::PubSubError& e) {
        EXPECT_EQ(e.status(), fletcher::PubSubStatus::kInvalidArgument);
        EXPECT_NE(std::string(e.what()).find("category"), std::string::npos)
            << "the refusal did not name the offending field: " << e.what();
        EXPECT_NE(std::string(e.what()).find("value type"), std::string::npos)
            << "the refusal did not say what was wrong with it: " << e.what();
    }

    c_schema.release(&c_schema);
}

}  // namespace
