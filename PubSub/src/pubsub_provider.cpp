#include <pubsub/pubsub_provider.hpp>

namespace fletcher {

namespace {

std::string JoinTopic(const std::vector<std::string>& segs) {
    std::string out;
    for (size_t i = 0; i < segs.size(); ++i) {
        if (i > 0) out += '/';
        out += segs[i];
    }
    return out;
}

}  // namespace

void PubSubProvider::PublishDirect(
    const std::vector<std::string>& topic_segments,
    RowEncoder encoder,
    const Attachments& attachments) {
    // Default: encode into a temp vector, decode back to ArrowRow, call Publish.
    EncodedRow temp;
    temp.reserve(128);
    VectorWriteBuffer buf(temp);
    encoder(buf);

    auto* codec = FindCodec(JoinTopic(topic_segments));
    if (!codec) {
        // No codec registered — cannot decode, fall through to raw path.
        // This shouldn't happen if CreateTopic was called with a schema.
        throw std::runtime_error(
            "PublishDirect: no codec for topic " + JoinTopic(topic_segments));
    }

    auto row = codec->DecodeRow(temp.data(), temp.size());
    Publish(topic_segments, row, attachments);
}

void PubSubProvider::RegisterCodec(const std::string& topic_key,
                                   std::shared_ptr<arrow::Schema> schema) {
    if (schema)
        codecs_[topic_key] = std::make_unique<RowCodec>(std::move(schema));
}

RowCodec* PubSubProvider::FindCodec(const std::string& topic_key) const {
    auto it = codecs_.find(topic_key);
    return it != codecs_.end() ? it->second.get() : nullptr;
}

}  // namespace fletcher
