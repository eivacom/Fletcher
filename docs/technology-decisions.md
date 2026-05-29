# Technology Decision Log

This document records significant technology decisions, the rationale behind them, alternatives considered, and known risks. It is a living document updated as the architecture evolves.

---

## TD-001: Nanoarrow for Edge-Compatible Arrow Type System

**Status:** Accepted

**Context:** Fletcher must run on edge devices and embedded systems where the full Apache Arrow C++ library (~100 MB) is impractical. The system still needs Arrow schema representation, IPC serialization, and type system support on these constrained targets.

**Decision:** The core pub/sub path — generated message code, the `PubSub` interface, schema IPC serialization, transport providers, and the `PositionalWriter`/`PositionalReader` — depends only on [nanoarrow](https://arrow.apache.org/nanoarrow/) (~100 KB), Apache's lightweight C library that implements the Arrow type system and C Data Interface. Server-side features (batching, view classes, `Codec`) use the full Arrow C++ library through a separate adapter layer.

**Rationale:** Nanoarrow provides Arrow schema construction, IPC serialization, and the C Data Interface without the full C++ runtime. This enables edge binaries to publish and subscribe to Arrow-typed data streams with minimal footprint. The Arrow C Data Interface (`ArrowSchema`, `ArrowArray`) provides zero-copy interoperability between nanoarrow and Arrow C++ on the server side — schemas can be imported/exported without serialization overhead. Nanoarrow is maintained by the Apache Arrow project, ensuring specification compliance and ongoing support.

**Alternatives considered:** Custom lightweight schema format (would require translation layers and lose Arrow ecosystem interoperability), full Arrow C++ with static linking and aggressive LTO (still too large for MCU targets), FlatBuffers-based schema transport (adds a dependency and format translation without gaining Arrow ecosystem access).

**Risks:** Nanoarrow's API is lower-level than Arrow C++ and requires more careful memory management. Some Arrow features (compute kernels, builder patterns) are not available through nanoarrow — these are relegated to the server tier. Nanoarrow is relatively young, though backed by the Arrow project.

---

## TD-002: Positional Wire Format Replacing Self-Describing Tagged Format

**Status:** Accepted

**Context:** The original wire format included an 8-byte schema hash, a 2-byte field count, and per-field headers (field number, wire type, length) for self-describing decode. For a typical 10-field row, this added ~110 bytes of overhead — significant for small, high-frequency sensor messages. The self-describing nature was underutilized because subscribers always needed the full schema for proper decode anyway.

**Decision:** The wire format was replaced with a positional encoding: a compact null bitfield followed by field payloads in schema order, with no per-field metadata. The publisher's schema is delivered to subscribers out-of-band via schema transport at the provider level, guaranteeing both sides share the same schema.

**Rationale:** The positional format reduces per-row overhead from ~110 bytes to ~14 bytes for a typical 10-field row (~87% reduction). This matters for high-frequency sensor telemetry where thousands of rows per second are common. The schema transport mechanism (companion DDS topics with TRANSIENT_LOCAL durability) ensures subscribers always receive the schema before data, making per-row self-description redundant. The format maps naturally to the `PositionalWriter`/`PositionalReader` API, enabling zero-allocation encode/decode on the hot path.

**Alternatives considered:** Protobuf wire format directly (efficient for variable-length data but not columnar-aware, requires descriptor for decode), Cap'n Proto (zero-copy but schema-dependent and less ecosystem support), keeping the tagged format with optimization (diminishing returns — the fundamental per-field overhead remains).

**Risks:** The positional format requires schema agreement between publisher and subscriber — a schema version mismatch produces corrupt data rather than a clean error. This is mitigated by the schema transport mechanism, but direct byte-level debugging is harder than with self-describing formats. The legacy tagged codec (`RowCodec`) has been removed from the codebase — it was fully superseded by the positional codec and generated code.

---

## TD-003: Protocol Buffers for Schema Definition and Code Generation

**Status:** Accepted

**Context:** Generating typed serialization code requires a schema definition language. The system already operates in environments where Protocol Buffers are used for service definitions, and `.proto` files provide a mature, well-tooled IDL with support for nested messages, repeated fields, maps, and well-known types.

**Decision:** Schema definitions live in `.proto` files. A custom `protoc` compiler plugin (`protoc-gen-fletcher`) reads `.proto` files and generates C++ ArrowRow classes, Arrow view classes, and TypeScript interfaces. The plugin hooks into the standard `protoc` build process.

**Rationale:** Protocol Buffers provide a mature IDL with excellent tooling (editors, linters, documentation generators). The `protoc` plugin mechanism allows generating custom code alongside standard protobuf code without forking the compiler. Proto field numbers provide stable field identity for schema evolution — they survive renames and reorderings. The `.proto` ecosystem is widely understood, reducing the learning curve. Service definitions with RPC methods provide a natural mapping to pub/sub topics.

**Alternatives considered:** Custom IDL (maximum flexibility but requires building tooling from scratch), Apache Avro schemas (JSON-based, good for schema evolution but no code generation plugin mechanism), Arrow schema directly in code (no IDL benefits — schema drift between languages, no generation).

**Risks:** The proto-to-Arrow mapping is not lossless: some proto constructs (`oneof`, recursive messages, `Any`, `Struct`) cannot be represented in Arrow and are unsupported. Enum values are stored as int32, losing the symbolic name at the Arrow level. Teams using Fletcher must maintain `.proto` files even if they don't otherwise use Protocol Buffers.

---

## TD-004: DDS for Pub/Sub Transport

**Status:** Accepted

**Context:** Sensor data must be transported reliably across vessel networks, from edge devices to workstations, with configurable QoS. The transport must handle both desktop/server environments and resource-constrained embedded devices.

**Decision:** Two DDS implementations are used: [eProsima Fast DDS](https://fast-dds.docs.eprosima.com/) (`fastdds-pubsub-provider`) for desktop and server environments, and [eProsima Micro XRCE-DDS](https://github.com/eProsima/Micro-XRCE-DDS-Client) (`xrcedds-pubsub-provider`) for MCU and embedded targets. Both implement the `PubSub` interface and interoperate through the DDS domain (XRCE-DDS via an Agent bridge).

**Rationale:** DDS provides built-in QoS policies (RELIABLE, TRANSIENT_LOCAL, KEEP_ALL) that implement at-least-once delivery without application-level retry logic. The RTPS discovery protocol enables automatic peer discovery on a network segment. XRCE-DDS provides a client/agent split where the thin client runs on the MCU (<75 KB Flash) while the Agent bridges to the full DDS network. Both providers produce the same `Envelope` wire format, so data flows seamlessly between edge and server. DDS is widely used in robotics and maritime systems, aligning with the target deployment environment.

**Alternatives considered:** MQTT (lighter protocol but weaker QoS guarantees, requires a broker, less natural for peer-to-peer discovery), Zenoh (promising but less mature, smaller ecosystem), ZeroMQ (no built-in QoS or discovery), custom TCP/UDP (maximum control but high implementation cost for reliability, discovery, and QoS).

**Risks:** Fast DDS has a significant binary footprint (~10+ MB), though this only affects desktop/server targets where it is acceptable. DDS configuration (XML profiles, discovery servers for multi-host) can be complex. The `PubSub` abstraction means adding alternative transports is straightforward if DDS proves unsuitable for a specific deployment.

---

## TD-005: Schema Transport via Companion Topics

**Status:** Accepted

**Context:** The positional wire format requires publisher and subscriber to share the same schema. The schema must be available to late-joining subscribers without requiring them to know the schema in advance.

**Decision:** For each data topic, the transport provider creates a companion topic (suffixed `/__schema`) with TRANSIENT_LOCAL durability and KEEP_LAST(1) QoS. The schema is serialized as Arrow IPC bytes via nanoarrow and published once. Late-joining subscribers read the retained schema automatically. The `Subscribe()` method returns the schema in the `SubscriptionResult`.

**Rationale:** Companion topics leverage existing DDS infrastructure and QoS for schema delivery — no separate schema registry or out-of-band coordination needed. TRANSIENT_LOCAL durability means the schema survives publisher restarts (within the DDS domain lifetime). KEEP_LAST(1) ensures only the current schema is retained, not a history. The mechanism is transparent to generated code: publishers provide the schema at topic creation, subscribers receive it at subscription time. The same pattern works for both FastDDS and XRCE-DDS providers. For the gateway, the schema is delivered in the `subscribed` JSON response in both structured JSON and base64 Arrow IPC forms.

**Alternatives considered:** Embed schema hash and lookup table (requires a schema registry service), self-describing wire format (per-row overhead, addressed in TD-002), shared configuration file (fragile, not suitable for dynamic environments), separate gRPC schema service (adds a dependency and failure mode).

**Risks:** Schema updates require topic recreation — the companion topic retains one schema version. In practice, schema changes correspond to new deployment versions, making this acceptable. If the companion topic is not yet populated when a subscriber joins (rare race condition), the subscriber will retry.

---

## TD-006: WebSocket Split Protocol for Browser Delivery

**Status:** Accepted

**Context:** Browser clients need to receive live sensor data and interact with the pub/sub layer. Browsers cannot use DDS or Arrow Flight directly. The protocol must handle both control messages (subscribe, unsubscribe, list topics) and high-throughput binary data delivery.

**Decision:** The `gateway` component (a Boost.Beast WebSocket server) uses a split WebSocket protocol: JSON text frames for control messages (human-readable, debuggable) and binary frames for data delivery (no base64 overhead). The gateway depends only on nanoarrow — it walks the nanoarrow `ArrowSchema` directly to produce JSON schema responses and passes row data through as raw bytes.

**Rationale:** WebSocket is universally supported in browsers. The text/binary split provides the best of both worlds: control messages are human-readable for debugging (browser developer tools show JSON directly), while data messages avoid the ~33% overhead of base64 encoding binary data in JSON. Subscription IDs are stringified in JSON to avoid JavaScript Number precision loss for uint64 values. The nanoarrow-only dependency means the gateway can run alongside edge-tier components without requiring Arrow C++.

**Alternatives considered:** gRPC-Web (requires a proxy, adds complexity, less natural for streaming), Server-Sent Events (unidirectional, no binary support), pure JSON protocol (simpler but ~33% overhead on binary data), pure binary protocol (harder to debug, requires custom tooling).

**Risks:** WebSocket connections are stateful, complicating horizontal scaling behind a load balancer (sticky sessions needed). The custom binary frame format requires the `gateway-client-ts` library (`fletcher-gateway-client` on npm) — standard WebSocket tools cannot decode data frames. Connection management (reconnect, session recovery) is the client's responsibility.

---

## TD-007: Two-Tier Architecture (Edge / Server Split)

**Status:** Accepted

**Context:** The system must deploy across a wide range of targets: MCUs and embedded Linux gateways with <75 KB Flash, vessel workstations with full operating systems, and server infrastructure. A single dependency profile cannot serve all targets.

**Decision:** The architecture is split into two tiers sharing the same wire format and `PubSub` interface. The edge tier depends only on nanoarrow (~100 KB) and includes generated message classes, positional I/O, transport providers, and the `gateway`. The server tier adds Apache Arrow C++ and includes the `Codec` (in `arrow-bridge`), `pubsub-arrow` adapter, and view classes.

**Rationale:** The split allows edge binaries to participate fully in the pub/sub network (publish, subscribe, schema transport) without the full Arrow C++ library. The nanoarrow/Arrow C++ boundary is bridged seamlessly via the Arrow C Data Interface — `OwnedSchema` converts to `shared_ptr<arrow::Schema>` with zero copy. Generated code produces two headers: `.fletcher.pb.h` (edge, nanoarrow only) and `.fletcher.arrow.pb.h` (server, Arrow C++), so the same `.proto` definition serves both tiers. The wire format is byte-identical between tiers, so a row encoded on an MCU decodes correctly on a server.

**Alternatives considered:** Single tier with full Arrow C++ everywhere (simpler code but excludes embedded targets), separate serialization format for edge (interoperability problems, duplicate code), dynamic linking with optional Arrow C++ (fragile, hard to test, platform-dependent).

**Risks:** Developers must be aware of which tier their code targets — using Arrow C++ types in edge code will not compile. The two-header generation adds complexity to the build. Some server-tier features (view classes, `Codec`) cannot be used on edge targets, requiring different code paths for the same logical operation.
