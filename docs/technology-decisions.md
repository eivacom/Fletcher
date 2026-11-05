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

**Context:** Telemetry must be transported reliably across local networks, from edge devices to workstations, with configurable QoS. The transport must handle both desktop/server environments and resource-constrained embedded devices.

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

**Risks:** WebSocket connections are stateful, complicating horizontal scaling behind a load balancer (sticky sessions needed). The custom binary frame format requires a Fletcher gateway client — `gateway-client-ts` (`@eiva/fletcher-gateway-client` on npm) or `Eiva.Fletcher.GatewayClient` (NuGet) — standard WebSocket tools cannot decode data frames. Connection management (reconnect, session recovery) is the client's responsibility.

---

## TD-007: Two-Tier Architecture (Edge / Server Split)

**Status:** Accepted

**Context:** The system must deploy across a wide range of targets: MCUs and embedded Linux gateways with <75 KB Flash, workstations with full operating systems, and server infrastructure. A single dependency profile cannot serve all targets.

**Decision:** The architecture is split into two tiers sharing the same wire format and `PubSub` interface. The edge tier depends only on nanoarrow (~100 KB) and includes generated message classes, positional I/O, transport providers, and the `gateway`. The server tier adds Apache Arrow C++ and includes the `Codec` (in `arrow-bridge`), `pubsub-arrow` adapter, and view classes.

**Rationale:** The split allows edge binaries to participate fully in the pub/sub network (publish, subscribe, schema transport) without the full Arrow C++ library. The nanoarrow/Arrow C++ boundary is bridged seamlessly via the Arrow C Data Interface — `OwnedSchema` converts to `shared_ptr<arrow::Schema>` with zero copy. Generated code produces two headers: `.fletcher.pb.h` (edge, nanoarrow only) and `.fletcher.arrow.pb.h` (server, Arrow C++), so the same `.proto` definition serves both tiers. The wire format is byte-identical between tiers, so a row encoded on an MCU decodes correctly on a server.

**Alternatives considered:** Single tier with full Arrow C++ everywhere (simpler code but excludes embedded targets), separate serialization format for edge (interoperability problems, duplicate code), dynamic linking with optional Arrow C++ (fragile, hard to test, platform-dependent).

**Risks:** Developers must be aware of which tier their code targets — using Arrow C++ types in edge code will not compile. The two-header generation adds complexity to the build. Some server-tier features (view classes, `Codec`) cannot be used on edge targets, requiring different code paths for the same logical operation.

---

## TD-008: The Pub/Sub Seam as a Frozen Contract with Uniform Provider Selection

**Status:** Accepted

**Context:** Two efforts were queued on opposite sides of one seam — a pure C driver ABI *below* `PubSubProvider` (so a protocol becomes a runtime-loadable driver) and C#/Rust language bindings *above* it — and the owner required them to proceed in parallel, meeting only at that interface. As it stood the seam could not carry either: the types crossing it had no stated C-expressible ownership model, failures crossed as C++ exceptions, the delivery contract was prose honoured by review, and *which* provider was in use was a compile-time decision baked into every caller. Two teams mirroring a moving interface weeks apart, without talking, is how they diverge silently.

**Decision:** The seam is a single **frozen contract** — [docs/pubsub-interface-spec.md](pubsub-interface-spec.md), whose §12 states what is frozen, who may act on each class of text, and how each handoff condition was verified. Four properties are normative. **Selection** is uniform: one registry turns a *selector* plus a *configuration* into a provider, a selector is a name or a path by its shape alone, and whether the provider is built in or loaded through an ABI is invisible above the seam. **Configuration** is a small typed core (`{max_payload_bytes, domain_id}`) plus an opaque per-protocol document Fletcher transports and never parses — Fast DDS's native XML QoS profile text, `key=value` for XRCE; the publisher announces its bound on `__schema` and Fast DDS subscribers follow it; per-topic `TopicOptions` (profile name, publisher bound) ride two optional seam methods. **Failure** is one error type carrying one stable numbered cause (`PubSubError` over `PubSubStatus`), appended only, published as a table beside the header that defines it. **Ownership** of every crossing type is written normatively in the header, and schema arrival is a waitable handle with typed outcomes rather than a `std::shared_future`. Neither later round may change any of it: finding the seam insufficient is a stop-and-ask against the spec, not a local workaround and not a change landed inside an ABI round.

**Rationale:** Freezing buys parallelism, which was the whole objective. A single numbered cause is what two independent bindings can mirror without drifting, because exceptions cannot cross a C boundary and an assortment of standard exception types plus a prose map cannot be mirrored twice identically. A typed core plus an opaque document keeps Fletcher from learning any protocol's vocabulary while still letting an operator configure one fully. Name-or-path selection by shape means one configuration string means the same thing in every build, and the driver ABI adds only a resolver behind the same `Create` call — pinned by a `static_assert` on that member's whole type. The delivery contract stops being prose: it is a conformance suite run against every provider, with a copy-accounting oracle making zero-copy falsifiable, and both later rounds inherit those suites as their oracle rather than re-deriving the rules.

**Alternatives considered:** Letting each later round amend the seam as it discovered gaps (the fastest start, and the silent-divergence failure this decision exists to prevent). A shared C header or shared buffer struct between the two ABIs (couples their release cadence and forces one round to wait on the other — explicitly rejected by the owner). Defining the binding ABI as pass-through of driver-ABI types (works only when the provider happens to be a loaded driver, and breaks for a built-in one). Keeping protocol-typed configuration structs at the seam (leaves Fletcher knowing one protocol's vocabulary) or having Fletcher parse a config file (a parser and its failure modes for a format only the provider understands). Keeping the `shared_future` schema wait beside the new handle as a C++ convenience (two mechanisms drift, and it leaves the binding path untested because no in-tree caller would exercise it).

**Risks:** The freeze buys parallelism at the price of a stop-and-ask whenever either round finds the seam short — and the round most likely to find it short is the one adding loading. One condition (nothing above the seam branching on built-in versus loaded) is held by the freeze rather than by any machine, because a machine check would have to add the very distinction the decision removes. The contract was frozen on evidence produced by local runs on one Windows machine plus one WSL compile, with no automated build yet run on the branch, so Linux behaviour is unverified and a Linux-only difference is routed to the owner as a question (spec §12.4). Two transport blind spots survive with named owners: the receive-side data-sharing defect and the unbounded-wait clause, which no test in this tree can pin honestly.

---

## TD-009: The .NET Binding Wraps the One Native Codec Through a C ABI

**Status:** Accepted

**Context:** .NET desktop and server applications had to publish and subscribe on the same networks as the C++ nodes, with the same rows, over every transport Fletcher has (Fast DDS, Micro XRCE-DDS, in-process), and from generated code as C++ does. Fletcher's positional wire format had one C++ implementation for the edge (the generated `WriteTo`/`ReadFrom` over `positional_io`) and one Arrow-native one on the server (`arrow-bridge`'s `Codec`); a .NET binding would either call one of them or become a third. The seam (TD-008) had been frozen so that a binding above it and a driver ABI below it could proceed in parallel, and requires zero-copy for rows and attachments, enforced by a copy-accounting oracle. C++ types and exceptions cannot cross into .NET.

**Decision:** C# calls the native codec; it never reimplements it. A pure C99 binding ABI (`c-abi/include/fletcher/abi/binding.h`), shipped as one self-contained shared library per RID (`fletcher-c-abi`, with Fletcher's core, pub/sub, the three built-in providers, Fast DDS, Micro XRCE-DDS and nanoarrow linked in), exposes the seam's registry, publisher and subscriber and a **schema-driven codec over nanoarrow**: open once per schema, bind once per Arrow batch, publish a row straight into the transport's window; no encode entry point returns bytes. Values cross as **Arrow C Data Interface** arrays, so nesting needs no ABI surface of its own. Above it, the Arrow tier is managed, over `Apache.Arrow`; generated C# rows convert to and from Arrow and write no wire byte. Transports are chosen by selector string through the seam's registry, so C# has no per-transport code and implements no provider. Four NuGet packages ship it: `Eiva.Fletcher` (pub/sub and the codec), `Eiva.Fletcher.Interop` (the P/Invoke surface and the shim, the only package with native assets), `Eiva.Fletcher.Model` (the types generated code refers to, Arrow only) and `Eiva.Fletcher.GatewayClient`; the code generator ships as `Eiva.Fletcher.Protoc`. The one exception is the gateway client, a managed port of the TypeScript client with its own codec, because a browser or WASM application has no native option.

**Rationale:** One wire-format implementation reached by every binding means drift cannot happen, and the cross-language byte-parity harness shrinks from a load-bearing correctness proof to an ABI conformance test: the shim's codec is held byte-identical to `arrow-bridge`, and a C# publisher's batches are held to what Arrow C++ hands the same codec. One native codec writing into the provider's window keeps the single zero-copy path the seam requires, where a managed codec beside a native transport would cross the boundary twice; the copy oracle scores C#'s real publish at zero encode copies. The C Data Interface is what nanoarrow already is, carries arbitrary nesting by construction and allows batch calls, so the ABI stays small however many messages a user defines. An official .NET Arrow implementation exists and the C Data Interface hands over its buffers free, so binding Arrow C++ for the Arrow tier would be strictly worse. Selecting transports through the seam's registry gives every built-in provider for free, and every later driver too.

**Alternatives considered:** A managed port of the codec (the first plan, reversed by the maintainer on 2026-08-31: a second implementation to keep in step, and a row that crosses the native boundary twice on its way to a transport). Wrapping `arrow-bridge`'s Arrow C++ codec (drags full Arrow C++ into the native package). A row-builder ABI of per-field calls (`set_int32`, `list_append`, …), which grows with the type system and needs entry points, ownership rules and tests for every nesting construct. Binding Fast DDS and XRCE-DDS individually (per-transport code and QoS marshalling, where the seam already takes an opaque per-protocol document). Six packages split along the C++ components (no dependency-free tier is left once `Apache.Arrow` is a base dependency).

**Risks:** A package with native assets cannot run in WASM, which keeps browser and WASM clients on the gateway, and every platform is a RID to build and test (`linux-x64` and `win-x64` today; glibc 2.34 and `GLIBCXX_3.4.32` on Linux). Fletcher is LGPL-licensed and the shim links it statically, so the shim stays a separate, replaceable shared library and the packages document how to rebuild it from the release's commit and Conan lockfile; it must also be the only copy in a process, which it checks at load. A generated row that is not yet Arrow is converted to a one-row Arrow batch before it crosses, at 3.8 µs, 19.6× the C++ generated publisher, most of it `Apache.Arrow`'s own building and export; batching is structural through bind, so a row already in a bound batch publishes at 1.95× and a batch at 1.23× per row. The costs were measured on Windows and accepted (D-BIND-48, D-BIND-49). Strings are transcoded between UTF-16 and UTF-8 by `Apache.Arrow`, a per-value cost the zero-copy guarantee states rather than hides. The gateway client is a second codec by exception, held to the native one byte for byte and to the C++ goldens. On Windows the statically linked Fast DDS exports some 3500 names of its own from the shim; accepted under the package's size ceiling.
