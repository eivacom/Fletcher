#ifndef FLETCHER_INCLUDE_PUBSUB_SCHEMA_IPC_HPP_
#define FLETCHER_INCLUDE_PUBSUB_SCHEMA_IPC_HPP_

#include <arrow/type_fwd.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace fletcher {

/// Serialize an Arrow schema to IPC stream format (schema-only, no batches).
/// The result is a self-contained byte buffer suitable for storage or
/// transmission.
std::vector<uint8_t> SerializeSchemaIpc(const arrow::Schema& schema);

/// Deserialize an Arrow schema from IPC stream bytes produced by
/// SerializeSchemaIpc.
std::shared_ptr<arrow::Schema> DeserializeSchemaIpc(const uint8_t* data,
                                                     size_t len);

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_SCHEMA_IPC_HPP_
