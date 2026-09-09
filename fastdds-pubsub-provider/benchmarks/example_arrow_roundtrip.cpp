// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// One Arrow batch, all the way out to DDS bytes and all the way back, with every intermediate type
// named and every buffer printed. No participant, no writer, no reader, nothing sent: the DDS step
// is FletcherSamplePubSubType::serialize / ::deserialize called directly, which is exactly what
// Fast DDS would call on a real write() and take_next_sample().
//
// Run it and read the output top to bottom:
//
//   ArrowArray                      the batch, C data interface (nanoarrow)
//     -> TransformRow               one row read out through ArrowArrayView
//     -> fletcher::EncodedRow       positional row bytes, via PositionalWriter
//     -> internal::PublishData      what the provider hands DataWriter::write
//     -> SerializedPayload_t        the DDS payload serialize() produced
//     -> internal::ReceivedData     what deserialize() filled in
//     -> TransformRow               decoded through PositionalReader
//   ArrowArray                      appended back into a fresh batch, then compared
//
// The loaned publish path is shown alongside step 4: no serialize() runs there at all, the row goes
// straight into the FletcherSample that Fast DDS will hand to the transport.
#include <fastcdr/Cdr.h>
#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fastdds/rtps/common/SerializedPayload.hpp>
#include <fletcher/core/types.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <memory>
#include <vector>

#include "envelope_codec.hpp"
#include "fletcher_sample.hpp"
#include "fletcher_sample_pub_sub_type.hpp"
#include "transform_batch.hpp"
#include "transport_data.hpp"

namespace {

using eprosima::fastdds::rtps::SerializedPayload_t;

// The payload bound this walkthrough runs at.
constexpr uint32_t kExamplePayloadBytes = 64 * 1024;
using fletcher::internal::FletcherSamplePubSubType;

// Stand-in for the payload Fast DDS lends; the shipped sample is an offset and a size.
struct FletcherSample {
    uint32_t length;
    uint8_t body[kExamplePayloadBytes];
};
using namespace fletcher::benchmarks;  // NOLINT(build/namespaces)

constexpr auto kXcdr1 = eprosima::fastdds::dds::DataRepresentationId_t::XCDR_DATA_REPRESENTATION;
constexpr int64_t kRows = 3;

void Rule(const char* title) {
    std::printf("\n=== %s ", title);
    for (size_t i = std::strlen(title); i < 92; ++i) std::putchar('=');
    std::putchar('\n');
}

void HexDump(const char* label, const uint8_t* data, size_t len, size_t limit = 48) {
    std::printf("  %-24s %zu B", label, len);
    const size_t shown = len < limit ? len : limit;
    for (size_t i = 0; i < shown; ++i) {
        if (i % 16 == 0) std::printf("\n      %04zx  ", i);
        std::printf("%02x ", data[i]);
    }
    if (shown < len) std::printf("\n      ... %zu more", len - shown);
    std::printf("\n");
}

void PrintRow(const char* label, const TransformRow& row) {
    std::printf(
        "  %-24s timestamp=%lld  pose[0..3]=%.2f %.2f %.2f %.2f  velocity[0..2]=%.2f %.2f %.2f\n",
        label, static_cast<long long>(row.timestamp), row.pose[0], row.pose[1], row.pose[2],
        row.pose[3], row.velocity[0], row.velocity[1], row.velocity[2]);
}

bool SameRow(const TransformRow& a, const TransformRow& b) {
    if (a.timestamp != b.timestamp) return false;
    for (int i = 0; i < kPoseValues; ++i) {
        if (a.pose[i] != b.pose[i]) return false;
    }
    for (int i = 0; i < kVelocityValues; ++i) {
        if (a.velocity[i] != b.velocity[i]) return false;
    }
    return true;
}

}  // namespace

int main() {
    // -----------------------------------------------------------------------------------------
    Rule("1. Arrow in: the schema and a batch of ArrowArray rows");
    // -----------------------------------------------------------------------------------------
    // fletcher::OwnedSchema owns an ArrowSchema. This is the same schema a publisher announces on
    // the companion <topic>/__schema channel, as Arrow IPC bytes — it never travels with a sample.
    const fletcher::OwnedSchema schema = TransformSchema();
    std::printf("  ArrowSchema              struct<%s: %s, %s: struct<%s>, %s: struct<%s>>\n",
                schema->children[0]->name, schema->children[0]->format, schema->children[1]->name,
                schema->children[1]->children[0]->format, schema->children[2]->name,
                schema->children[2]->children[0]->format);
    std::printf(
        "                           (Arrow format strings: `tsn:` timestamp[ns], `+w:16`"
        " fixed_size_list<double>[16])\n");

    ArrowBatch source(schema.get());
    BuildSourceBatch(source, kRows);
    const ArrowBatchView source_view(schema.get(), source.get());
    std::printf("  ArrowArray               %lld rows, %lld children\n",
                static_cast<long long>(source.get()->length),
                static_cast<long long>(source.get()->n_children));
    for (int64_t i = 0; i < kRows; ++i) PrintRow("  row out of Arrow", source_view.Row(i));

    // -----------------------------------------------------------------------------------------
    Rule("2. One row out of the batch, and encoded");
    // -----------------------------------------------------------------------------------------
    // What Publisher::Publish is given: a RowEncoder that writes the row into whatever buffer the
    // provider supplies. Here it goes into a std::vector first, only so the bytes can be shown; on
    // the real path the same closure writes straight into the DDS payload.
    const TransformRow row = source_view.Row(0);
    PrintRow("row", row);

    fletcher::VectorWriteBuffer buffer;
    EncodeRow(row, buffer);
    const fletcher::EncodedRow row_bytes = buffer.Finish();
    std::printf(
        "  PositionalWriter         1 null-bitfield byte + timestamp(8) + 2 nested structs\n");
    HexDump("fletcher::EncodedRow", row_bytes.data(), row_bytes.size());

    // -----------------------------------------------------------------------------------------
    Rule("3. The DDS type");
    // -----------------------------------------------------------------------------------------
    FletcherSamplePubSubType type(kExamplePayloadBytes);
    std::printf("  registered name          %s\n", type.get_name().c_str());
    std::printf("  max_serialized_type_size %u B  (4 encapsulation + 4 length + %u body)\n",
                type.max_serialized_type_size, kExamplePayloadBytes);
    std::printf("  is_bounded               %s  -> Fast DDS may use data-sharing\n",
                type.is_bounded() ? "true" : "false");
    std::printf("  is_plain(XCDRv1)         %s  -> and may hand out loans at both ends\n",
                type.is_plain(kXcdr1) ? "true" : "false");

    // -----------------------------------------------------------------------------------------
    Rule("4. Publish, serialising path: PublishData -> serialize() -> SerializedPayload_t");
    // -----------------------------------------------------------------------------------------
    // internal::SampleWriter builds this and calls DataWriter::write(&publishing); Fast DDS then
    // calls serialize() with it.
    const fletcher::Attachments no_attachments;
    fletcher::internal::PublishData publishing;
    const fletcher::PubSubProvider::RowEncoder row_encoder = [&row](fletcher::WriteBuffer& buffer) {
        EncodeRow(row, buffer);
    };
    publishing.encoder = &row_encoder;
    publishing.attachments = &no_attachments;

    SerializedPayload_t payload(type.max_serialized_type_size);
    if (!type.serialize(&publishing, payload, kXcdr1)) {
        std::fputs("serialize failed\n", stderr);
        return 1;
    }

    const uint32_t header = SerializedPayload_t::representation_header_size;
    uint32_t sample_length = 0;
    std::memcpy(&sample_length, payload.data + header, sizeof(sample_length));
    const uint8_t pad = payload.data[3];
    std::printf(
        "  encapsulation            %02x %02x %02x %02x  (id %02x = PLAIN_CDR little-endian,"
        " %u trailing pad byte%s)\n",
        payload.data[0], payload.data[1], payload.data[2], pad, payload.data[1], pad,
        pad == 1 ? "" : "s");
    std::printf("  FletcherSample::length   %u B  (row length 4 + row %zu + attachment count 4)\n",
                sample_length, row_bytes.size());
    std::printf("  payload.length           %u B of %u reserved — the tail of `body` is not sent\n",
                payload.length, payload.max_size);
    HexDump("SerializedPayload_t", payload.data, payload.length);

    // -----------------------------------------------------------------------------------------
    Rule("4b. Publish, loaned path: no serialize() at all");
    // -----------------------------------------------------------------------------------------
    // internal::LoanableSampleWriter gets this struct back from DataWriter::loan_sample() and fills
    // it in place. With data-sharing on, this *is* the memory the subscriber will read.
    auto loaned = std::make_unique<FletcherSample>();
    {
        fletcher::FixedWriteBuffer buffer(loaned->body, kExamplePayloadBytes);
        fletcher::internal::EncodeEnvelopeBody(buffer, *publishing.encoder, no_attachments);
        loaned->length = static_cast<uint32_t>(buffer.Position());
    }
    std::printf(
        "  FletcherSample           { uint32 length = %u; uint8 body[%u]; }  sizeof = %zu B\n",
        loaned->length, kExamplePayloadBytes, sizeof(FletcherSample));
    const bool same_body =
        loaned->length == sample_length &&
        std::memcmp(loaned->body, payload.data + header + fletcher::internal::kSampleLengthPrefix,
                    loaned->length) == 0;
    std::printf("  body matches serialised  %s\n", same_body ? "yes" : "NO");

    // -----------------------------------------------------------------------------------------
    Rule("5. Subscribe: deserialize() -> ReceivedData");
    // -----------------------------------------------------------------------------------------
    // internal::DataReaderListener owns one of these and passes it to take_next_sample(); Fast DDS
    // calls deserialize() into it. (The loaned listener instead reads the FletcherSample above in
    // place, with no copy — internal::ParseEnvelopeBody hands back a pointer into `body`.)
    fletcher::internal::ReceivedData received;
    if (!type.deserialize(payload, &received)) {
        std::fputs("deserialize failed\n", stderr);
        return 1;
    }
    HexDump("ReceivedData::row", received.decoded_row.data(), received.decoded_row.size());
    std::printf("  attachments              %zu\n", received.decoded_attachments.size());
    std::printf("  row bytes survived       %s\n",
                received.decoded_row == row_bytes ? "yes" : "NO");

    // -----------------------------------------------------------------------------------------
    Rule("6. Arrow out: decode the row and append it to a fresh batch");
    // -----------------------------------------------------------------------------------------
    ArrowBatch out(schema.get());
    for (int64_t i = 0; i < kRows; ++i) {
        // Per sample, this is the whole subscribe-side chain: the callback gets (data, len) and the
        // schema, decodes the row, and the batching subscriber appends it.
        const TransformRow original = source_view.Row(i);
        fletcher::internal::PublishData sending;
        const fletcher::PubSubProvider::RowEncoder original_encoder =
            [&original](fletcher::WriteBuffer& buffer) { EncodeRow(original, buffer); };
        sending.encoder = &original_encoder;
        sending.attachments = &no_attachments;

        SerializedPayload_t wire(type.max_serialized_type_size);
        fletcher::internal::ReceivedData arrived;
        if (!type.serialize(&sending, wire, kXcdr1) || !type.deserialize(wire, &arrived)) {
            std::fputs("round trip failed\n", stderr);
            return 1;
        }
        out.Append(DecodeRow(arrived.decoded_row.data(), arrived.decoded_row.size()));
    }
    out.Finish();

    const ArrowBatchView out_view(schema.get(), out.get());
    std::printf("  ArrowArray               %lld rows back\n",
                static_cast<long long>(out.get()->length));
    for (int64_t i = 0; i < kRows; ++i) PrintRow("  row back in Arrow", out_view.Row(i));

    // -----------------------------------------------------------------------------------------
    Rule("7. Compare");
    // -----------------------------------------------------------------------------------------
    bool equal = out.get()->length == kRows;
    for (int64_t i = 0; equal && i < kRows; ++i) {
        equal = SameRow(source_view.Row(i), out_view.Row(i));
    }
    std::printf("  %lld rows Arrow -> DDS -> Arrow: %s\n", static_cast<long long>(kRows),
                equal && same_body ? "IDENTICAL" : "MISMATCH");
    return equal && same_body ? 0 : 1;
}
