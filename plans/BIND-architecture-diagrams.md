# Fletcher — Architecture & Data-Flow Diagrams (BIND round input)

Companion to [BIND-csharp-bindings.md](BIND-csharp-bindings.md). Diagrams are
Mermaid, so this file renders directly on GitHub, in VS Code, and in any Mermaid-
aware Markdown viewer.

> ⚠️ **Read the legend first.** This document deliberately mixes three maturity
> levels. Treating a planned box as existing code would be the main way to misread
> it.

## Legend

| Marker | Meaning |
|---|---|
| `<<shipped>>` | Exists on `main` today (`8ef1d0e`) and is tested in CI |
| `<<in-flight>>` | Round **PDA**, on `feature/protocol-driver-abi` — **design documents only, no ABI code written yet**, 8 commits ahead of `main`, unmerged |
| `<<planned>>` | Round **BIND** — this plan. **Nothing exists yet.** |
| `<<provisional>>` | Planned *and* the design is still an open decision |

## Language support — what actually exists today

This matters for reading the sequence diagrams: only C++ has a complete pub/sub
path right now.

| Language | Encode / decode | Publish / subscribe | Arrow read side | Status |
|---|---|---|---|---|
| **C++** | ✅ generated row classes + `Codec` | ✅ `Publisher` / `Subscriber`, all providers | ✅ views + accessors | complete |
| **TypeScript** | ✅ managed codec in `@eiva/fletcher-gateway-client` | ⚠️ via gateway WebSocket only, and **hand-wired** — no generated `Publisher`/`Subscriber` (that is BIND-T) | ❌ | partial |
| **Rust** | ❌ *(planned — BIND-Rust)* | ❌ *(planned — BIND-Rust)* | ✅ `.fletcher.rs` RecordBatch accessor | read side today |
| **C#** | ❌ | ❌ | ❌ | planned (BIND) |

**Rust has no pub/sub path *yet*.** Today the only Rust in the tree is the
generated `.fletcher.rs` accessor, which reads `RecordBatch`es something else
produced — there is no Rust codec and no Rust transport. It is absent from the
pub/sub sequence diagrams because **there is nothing to draw yet**, not because
Rust is excluded by design: PDA-L5 states verbatim that *"BIND-C#/BIND-Rust are
full pub/sub clients"*, and GIR-3 locks the chain
**GIR → BIND-C# → BIND-Rust → RIR**. Rust pub/sub is therefore **planned**, one
round behind C#, and figures 07–08 are the template it will follow.

---

## 1. Class diagram — codec and pub/sub core (shipped)

```mermaid
classDiagram
    direction TB

    class PositionalWriter {
        <<shipped>>
        +BeginRow(fieldCount)
        +WriteInt32(v)
        +WriteString(v)
        +BeginStruct(numFields)
    }
    class PositionalReader {
        <<shipped>>
        +IsNull(i) bool
        +ReadInt32() int32
        +ReadStruct(numFields)
    }
    class WriteBuffer {
        <<shipped>>
        +Append(data, len)
        +AppendByte(b)
        +AppendZeros(len)
        +Position() size_t
        +PatchU32(offset, value)
        +PatchByte(offset, bits)
        #AppendSlow(data, len)*
    }
    class VectorWriteBuffer {
        <<shipped>>
        +Finish() vector~uint8_t~
    }
    class FixedWriteBuffer {
        <<shipped>>
    }
    class Envelope {
        <<shipped>>
        +row : EncodedRow
        +attachments : Attachments
    }

    WriteBuffer <|-- VectorWriteBuffer
    WriteBuffer <|-- FixedWriteBuffer
    PositionalWriter ..> WriteBuffer : writes into
    Envelope ..> PositionalWriter : wraps row bytes

    class PubSubProvider {
        <<interface>>
        +CreateTopic(segments, OwnedSchema)*
        +Publish(segments, RowEncoder, Attachments)*
        +Subscribe(segments, SubscribeCallback) SubscriptionResult*
        +Unsubscribe(segments)*
    }
    class Publisher {
        <<shipped>>
        +CreateTopic(segments, OwnedSchema)
        +Publish(segments, RowEncoder, Attachments)
    }
    class Subscriber {
        <<shipped>>
        +Subscribe(segments, cb) SubscribeResult
        +Unsubscribe(id)
    }
    note for Subscriber "owns fan-out: one provider callback per topic, N local subscribers"

    class OwnedSchema {
        <<shipped>>
        +DeepCopy(ArrowSchema*)
    }

    class FastDDSPubSubProvider {
        <<shipped>>
    }
    class XrceDDSPubSubProvider {
        <<shipped>>
    }
    class InProcessProvider {
        <<shipped>>
    }
    note for InProcessProvider "lives in gateway/src/main.cpp today — PDA promotes it to a component"


    PubSubProvider <|-- FastDDSPubSubProvider
    PubSubProvider <|-- XrceDDSPubSubProvider
    PubSubProvider <|-- InProcessProvider
    Publisher --> PubSubProvider
    Subscriber --> PubSubProvider
    PubSubProvider ..> OwnedSchema : carries
    PubSubProvider ..> WriteBuffer : supplies window to encoder

    class Codec {
        <<shipped>>
        +EncodeRow(ArrowRow) EncodedRow
        +DecodeRow(EncodedRow) ArrowRow
    }
    note for Codec "server tier — requires Arrow C++"

    class PublisherArrow {
        <<shipped>>
        +CreateTopic(segments, arrow::Schema)
        +Publish(segments, ArrowRow, Attachments)
    }
    class SubscriberArrow {
        <<shipped>>
        +Subscribe(segments, ArrowRow cb)
        +Subscribe(segments, RecordBatch cb)
    }

    PublisherArrow --> Publisher
    PublisherArrow --> Codec
    SubscriberArrow --> Subscriber
    SubscriberArrow --> Codec
```

### Generated artifacts, per language

One IR, N backends — GIR's design, and the reason a C# backend is cheap.

```mermaid
flowchart LR
    PROTO["your .proto"] --> PLUGIN["protoc-gen-fletcher"]
    PLUGIN --> IR["ir::IrNode<br/><i>language-neutral</i><br/>no language type strings"]

    IR --> CPPB["cpp_backend<br/>type_table + schema /<br/>decode / view visitors"]
    IR --> TSB["ts_backend<br/>type_table + visitor"]
    IR --> CSB["csharp_backend<br/>type_table + visitor<br/>+ view visitor"]

    CPPB --> GCPP["<b>.fletcher.pb.h</b> row class + pub/sub<br/><b>.fletcher.arrow.pb.h</b> view + ToArrowRow<br/><b>.fletcher.accessor.pb.h</b> RecordBatch accessor"]
    TSB --> GTS["<b>.fletcher.ts</b> interface + TypedSchema<br/>+ topic constants<br/><i>Publisher / Subscriber: BIND-T</i>"]
    CPPB --> GRS["<b>.fletcher.rs</b> RecordBatch accessor only<br/><i>still on FieldKind until round RIR</i>"]
    CSB --> GCS["<b>.fletcher.cs</b> row class<br/><b>.fletcher.arrow.cs</b> view<br/><b>.fletcher.accessor.cs</b> accessor"]

    PLUGIN --> IPC["<b>.ipc</b> Arrow IPC schema<br/><i>language-neutral</i>"]

    classDef planned stroke-dasharray: 5 5
    class CSB,GCS planned
```

Opt tokens: `--fletcher_opt=` `ts` · `ipc` · `accessor` · `rust` (shipped), plus
`csharp` · `csharp_accessor` (planned).

---

## 2. Class diagram — the two ABIs and the C# binding (planned + in-flight)

The two ABIs point in **opposite directions**; conflating them is the classic
mistake (PDA-L5).

```mermaid
classDiagram
    direction TB

    class PubSubProvider {
        <<interface>>
    }

    class DriverProvider {
        <<in-flight>>
        wraps a loaded driver
    }
    note for DriverProvider "PDA-L8: the driver ABI goes BELOW PubSubProvider, not in place of it"

    class driver_abi_h {
        <<in-flight>>
        role 1 : driver vtable (driver implements)
        role 2 : host callbacks (Fletcher implements)
        role 3 : driver management (apps call)
    }
    class fletcher_write_buffer {
        <<in-flight>>
        ctx, data, capacity, pos
        grow(self, data, len)
        grow_zeros(self, len)
    }
    note for fletcher_write_buffer "window + refill hooks: ONE crossing per refill, not per append"

    class fletcher_blob {
        <<in-flight>>
        retain() / release()
    }

    PubSubProvider <|-- DriverProvider
    DriverProvider --> driver_abi_h : roles 1-2
    driver_abi_h ..> fletcher_write_buffer : shared vocabulary
    driver_abi_h ..> fletcher_blob : shared vocabulary

    class FastDDS_driver {
        <<in-flight>>
        config: XML QoS profile
    }
    class XRCE_driver {
        <<in-flight>>
        config: key=value
    }
    class InProcess_driver {
        <<in-flight>>
        reference driver
    }
    driver_abi_h <|.. FastDDS_driver
    driver_abi_h <|.. XRCE_driver
    driver_abi_h <|.. InProcess_driver

    class binding_abi_h {
        <<planned>>
        descriptor-driven encode/decode
        pub/sub verbs
        role 3 driver management
    }
    note for binding_abi_h "Fletcher is the CALLEE here. No driver data-plane function is reachable."

    binding_abi_h ..> fletcher_write_buffer : REUSES verbatim
    binding_abi_h ..> fletcher_blob : REUSES verbatim
    binding_abi_h --> Publisher_cpp : calls
    binding_abi_h --> Subscriber_cpp : calls

    class Publisher_cpp {
        <<shipped>>
        fletcher::Publisher
    }
    class Subscriber_cpp {
        <<shipped>>
        fletcher::Subscriber
    }

    class Fletcher_Interop {
        <<planned>>
        P/Invoke + native assets per RID
    }
    class Fletcher_Core_cs {
        <<planned>>
        row types, TypedSchema, envelope
    }
    class Fletcher_Arrow_cs {
        <<planned>>
        managed, Apache.Arrow
    }
    class Fletcher_PubSub_cs {
        <<planned>>
        IPubSubProvider, Publisher, Subscriber
        runtime driver selection
    }
    class Fletcher_PubSubArrow_cs {
        <<planned>>
        PublisherArrow / SubscriberArrow
    }
    class Fletcher_GatewayClient_cs {
        <<planned>>
        managed WebSocket - NO native assets
    }

    Fletcher_Interop --> binding_abi_h
    Fletcher_Core_cs --> Fletcher_Interop
    Fletcher_Arrow_cs --> Fletcher_Core_cs
    Fletcher_PubSub_cs --> Fletcher_Core_cs
    Fletcher_PubSubArrow_cs --> Fletcher_Arrow_cs
    Fletcher_PubSubArrow_cs --> Fletcher_PubSub_cs
    Fletcher_GatewayClient_cs --> Fletcher_Core_cs
```

---

## 3. Sequence — C++ publish (shipped)

The zero-copy path: the provider supplies the buffer window, and the generated
encoder writes positional bytes **directly into the transport payload**.

```mermaid
sequenceDiagram
    autonumber
    participant App as C++ app
    participant Gen as Generated<br/>SensorFeed_StreamPublisher
    participant Pub as fletcher::Publisher
    participant Prov as PubSubProvider<br/>(FastDDS)
    participant Buf as WriteBuffer window
    participant Net as DDS transport

    App->>Gen: row.set_temperature(23.5) …
    App->>Gen: Publish(row)
    Gen->>Pub: Publish(segments, encoder)
    Note over Gen,Pub: encoder is a lambda capturing the row
    Pub->>Prov: Publish(segments, encoder, attachments)
    Prov->>Prov: loan a transport payload
    Prov->>Buf: expose {data, capacity, pos}
    Prov->>Gen: encoder(buffer)
    Gen->>Buf: AppendZeros(null bitfield)
    Gen->>Buf: AppendFixed / Append(string) …
    Gen->>Buf: PatchByte(nullBitfieldOffset, bits)
    Gen->>Buf: PatchU32(lenOffset, len)
    Note over Buf: back-patching happens IN the window —<br/>only overflow reaches the virtual slow path
    Buf-->>Prov: pos = row length
    Prov->>Net: send (no copy of row bytes)
```

## 4. Sequence — C++ subscribe (shipped)

```mermaid
sequenceDiagram
    autonumber
    participant Net as DDS transport
    participant Prov as PubSubProvider
    participant Sub as fletcher::Subscriber
    participant Gen as Generated subscriber
    participant App as C++ app

    App->>Sub: Subscribe(segments, cb)
    Sub->>Prov: Subscribe(segments, multiplexCb)
    Note over Sub: first Subscribe per topic installs ONE provider<br/>callback — later ones just register locally (fan-out)
    Prov-->>Sub: SubscriptionResult (schema future)

    Net->>Prov: sample arrives (transport thread)
    Prov->>Prov: schema known? else buffer until it is
    Prov->>Sub: cb(data, len, const schema&, const attachments&)
    Note over Prov,Sub: all three are BORROWED for the call only.<br/>One callback at a time per subscription —<br/>the thread may differ between samples.
    Sub->>Gen: fan out to each registered callback
    Gen->>Gen: PositionalReader over data → typed row
    Gen->>App: on_message(SensorReading, attachments)
    Note over App: to retain schema/attachments, COPY them
```

## 5. Sequence — TypeScript subscribe over the gateway (shipped)

Browser-safe: pure managed codec, no native code, no WASM requirement.

```mermaid
sequenceDiagram
    autonumber
    participant TS as TS app
    participant Cli as FletcherClient
    participant GW as gateway (C++)
    participant Sub as fletcher::Subscriber
    participant Prov as PubSubProvider

    TS->>Cli: connect()
    Cli->>GW: WebSocket open
    TS->>Cli: subscribe(Topic, Schema, cb)
    Cli->>GW: text frame {"action":"subscribe","topic":"…"}
    GW->>Sub: Subscribe(segments, …)
    Sub->>Prov: Subscribe(…)
    GW-->>Cli: text {"type":"subscribed","subId":"…","schema":{…},"schemaIpc":"base64…"}

    Prov->>Sub: sample (data, len, schema, attachments)
    Sub->>GW: deliver
    GW-->>Cli: binary frame [SUB_ID:8][ENVELOPE]
    Cli->>Cli: parse frame → decode envelope
    Cli->>Cli: positional decode using generated TypedSchema
    Cli->>TS: cb(typedRow, attachments)
    Note over TS,Cli: today the app passes Topic + Schema by hand.<br/>BIND-T generates Publisher/Subscriber classes<br/>that bind both in.
```

## 6. Sequence — C# publish (planned; one hop provisional)

```mermaid
sequenceDiagram
    autonumber
    participant App as C# app
    participant Gen as Generated<br/>SensorReading (C#)
    participant CsPub as Fletcher.PubSub<br/>Publisher(T)
    participant Int as Fletcher.Interop<br/>(P/Invoke)
    participant ABI as binding ABI (C)
    participant Cpp as fletcher::Publisher
    participant Drv as DriverProvider → driver
    participant Net as Fast DDS / XRCE

    App->>Gen: row.Temperature = 23.5 …
    App->>CsPub: Publish(row)
    CsPub->>Gen: build ArrowArray + ArrowSchema
    Note over CsPub,Gen: ⚠️ PROVISIONAL — open decision 1.<br/>Arrow C Data Interface is the recommendation:<br/>arbitrary nesting free, batch-capable, zero-copy export.
    CsPub->>Int: encode(schemaHandle, arrowArray, writeBuffer)
    Int->>ABI: P/Invoke (blittable, no marshalling)
    ABI->>Cpp: Publish(segments, encoder, attachments)
    Cpp->>Drv: Publish(…)
    Drv->>ABI: expose write-buffer window {data, capacity, pos}
    ABI->>ABI: descriptor-driven codec writes positional bytes
    Note over ABI: ONE boundary crossing per window refill,<br/>not one per field. Strings transcode UTF-16→UTF-8<br/>(the one place a copy is unavoidable).
    Drv->>Net: send
```

## 7. Sequence — C# subscribe (planned)

```mermaid
sequenceDiagram
    autonumber
    participant Net as Fast DDS / XRCE
    participant Drv as driver → DriverProvider
    participant Cpp as fletcher::Subscriber
    participant ABI as binding ABI (C)
    participant Int as Fletcher.Interop
    participant CsSub as Fletcher.PubSub<br/>Subscriber(T)
    participant App as C# app

    App->>CsSub: Subscribe(cb)
    CsSub->>Int: subscribe(topic, callbackPtr, ctxHandle)
    Note over Int: [UnmanagedCallersOnly] static fn ptr<br/>+ GCHandle context token
    Int->>ABI: subscribe(…)
    ABI->>Cpp: Subscribe(segments, …)
    Cpp->>Drv: Subscribe(…)

    Net->>Drv: sample (native thread)
    Drv->>Cpp: deliver
    Cpp->>ABI: cb(data, len, schema, attachments)
    ABI->>Int: invoke managed fn ptr
    Int->>CsSub: ReadOnlySpan(byte) + schema + attachments
    Note over Int,CsSub: ALL THREE are borrowed for the call.<br/>No async. Nothing may escape.<br/>Copy what you keep.
    CsSub->>CsSub: decode → typed row (generated ReadFrom)
    CsSub->>App: cb(SensorReading, attachments)
    Note over App: no thread affinity: the delivering thread<br/>may differ between samples
```

---

## 8. Deployment view — which packages a consumer takes

```mermaid
flowchart LR
    subgraph edge["Edge / device (nanoarrow only)"]
        E1["generated .fletcher.pb.h"]
        E2["fletcher-core"]
        E3["fletcher-pubsub"]
        E4["XRCE driver"]
    end
    subgraph server["Server / desktop"]
        S1["fletcher-arrow-bridge"]
        S2["fletcher-pubsub-arrow"]
        S3["Fast DDS driver"]
    end
    subgraph dotnet[".NET consumer (planned)"]
        D1["Eiva.Fletcher.Core"]
        D2["Eiva.Fletcher.Interop<br/>native assets per RID"]
        D3["Eiva.Fletcher.Arrow"]
        D4["Eiva.Fletcher.PubSub"]
    end
    subgraph browser["Browser / WASM"]
        B1["@eiva/fletcher-gateway-client"]
        B2["Eiva.Fletcher.GatewayClient<br/>no native assets"]
    end

    E4 -->|DDS| S3
    S3 --> D2
    D1 --> D2
    D3 --> D1
    D4 --> D1
    B1 -.->|WebSocket| GW["gateway exe"]
    B2 -.->|WebSocket| GW
    GW --> S3

    classDef planned stroke-dasharray: 5 5
    class dotnet,D1,D2,D3,D4,B2 planned
```

**Why `GatewayClient` carries no native assets:** WASM cannot load them. Browser
and WASM clients must keep the managed WebSocket path, which is why D-BIND-13
isolates native assets to `Interop` alone and CI asserts `GatewayClient` has no
`runtimes/` folder.

---

## Known gaps in these diagrams

1. **The binding ABI's function set is not designed yet.** Diagrams 2, 6 and 7 show
   the *shape* agreed so far, not a reviewed header. The value-transfer hop in
   diagram 6 is explicitly marked provisional (open decision 1).
2. **PDA is design-only.** No driver ABI code exists; `DriverProvider`,
   `fletcher_write_buffer` and the three drivers are from the spec, and the spec is
   still moving (it changed on 2026-08-31).
3. **`InProcessProvider` is not a component yet** — it lives inside
   `gateway/src/main.cpp`. PDA-L9 promotes it.
4. **Rust is absent from the pub/sub diagrams** because it has no pub/sub path
   *yet*. It is **planned** (BIND-Rust, one round after BIND-C#), not excluded by
   design — see the support table above.
5. **`FastDDSProviderOptions` still exists.** Diagram 2 shows Fast DDS configured by
   XML profile, which is the PDA-L9 target state, **not** today's code.
