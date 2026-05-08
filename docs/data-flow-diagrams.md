<!-- Space: Software -->
<!-- Parent: Architecture Overview -->
<!-- Title: Data Flow Diagrams -->

# Data Flow Diagrams

## Sensor-to-Subscriber Pipeline

```mermaid
sequenceDiagram
    participant S as Sensor Driver
    participant AR as ArrowRow (generated)
    participant PW as PositionalWriter
    participant PP as PubSub
    participant NET as DDS Network
    participant PS as PubSubArrow (server)
    participant PC as Codec
    participant APP as Application

    S->>AR: Populate typed setters
    AR->>PW: EncodeTo(WriteBuffer&)
    PW->>PW: Write null bitfield + payloads

    Note over PP: Zero-copy: RowEncoder writes<br/>directly into provider buffer

    AR->>PP: Publish(RowEncoder, Attachments)
    PP->>NET: Transport encoded bytes

    NET->>PS: Raw bytes callback
    PS->>PC: DecodeRow(bytes)
    PC->>PC: Read null bitfield + payloads
    PC->>PS: ArrowRow (vector<Scalar>)
    PS->>APP: Typed callback with ArrowRow + Attachments
```

## Schema Transport Flow

```mermaid
sequenceDiagram
    participant PUB as Publisher
    participant PP as PubSub
    participant DDS as DDS Domain
    participant SUB as Subscriber

    PUB->>PP: CreateTopic(segments, OwnedSchema)
    PP->>PP: Serialize schema via nanoarrow IPC
    PP->>DDS: Create data topic "pkg/svc/method"
    PP->>DDS: Create companion topic "pkg/svc/method/__schema"<br/>(TRANSIENT_LOCAL, KEEP_LAST 1)
    PP->>DDS: Publish schema IPC bytes on companion topic

    Note over DDS: Schema is retained for<br/>late-joining subscribers

    SUB->>PP: Subscribe(segments, callback)
    PP->>DDS: Subscribe to data topic
    PP->>DDS: Read from companion schema topic
    DDS->>PP: Schema IPC bytes
    PP->>PP: Deserialize to OwnedSchema
    PP->>SUB: SubscriptionResult { schema }

    Note over PUB,SUB: Both sides now share the same schema.<br/>Positional wire format works without<br/>per-field metadata.

    PUB->>PP: Publish(RowEncoder)
    PP->>DDS: Encoded bytes (data topic)
    DDS->>PP: Raw bytes callback
    PP->>SUB: callback(data, len, attachments)
```

## Browser Delivery Flow (WebGateway)

```mermaid
sequenceDiagram
    participant PUB as C++ Publisher
    participant DRV as Driver
    participant WG as WebGateway
    participant WS as WebSocket
    participant WC as WebClient (TS)
    participant APP as Browser App

    PUB->>DRV: Publish on topic

    Note over WC,WG: Connection setup

    WC->>WS: Connect ws://host:port
    WC->>WG: JSON: {"action":"subscribe","topic":"pkg/svc/method"}
    WG->>DRV: Subscribe with fan-out
    DRV->>WG: SubscriptionResult { schema, subId }

    WG->>WS: JSON: {"type":"subscribed","subId":"1",<br/>"topic":"...","schema":{fields...},<br/>"schemaIpc":"base64..."}
    WS->>WC: Parse schema descriptor

    Note over WC,WG: Data delivery

    PUB->>DRV: Publish row + attachments
    DRV->>WG: Subscriber callback (bytes)
    WG->>WS: Binary frame: [SUB_ID:8][ENVELOPE:rest]
    WS->>WC: Deserialize envelope
    WC->>WC: Positional decode using schema
    WC->>APP: Typed object callback

    Note over WC,WG: Publish from browser

    APP->>WC: publish(topic, row, attachments)
    WC->>WC: Positional encode
    WC->>WS: Binary frame: [TOPIC_LEN:2][TOPIC:N][ENVELOPE:rest]
    WS->>WG: Parse binary frame
    WG->>DRV: Publish on topic
```

## Code Generation Flow

```mermaid
sequenceDiagram
    participant DEV as Developer
    participant PROTO as .proto files
    participant PROTOC as protoc
    participant PLUGIN as protoc-gen-fletcher
    participant CMAKE as CMake Build

    DEV->>PROTO: Define messages and services
    CMAKE->>PROTOC: add_custom_command invokes protoc
    PROTOC->>PLUGIN: CodeGeneratorRequest (stdin)

    PLUGIN->>PLUGIN: Topological sort (DFS)<br/>dependency-first ordering
    PLUGIN->>PLUGIN: Map fields via type_mapper<br/>Proto → Arrow types
    PLUGIN->>PLUGIN: Detect cross-file references<br/>→ #include directives

    par Generate outputs
        PLUGIN->>CMAKE: .fletcher.pb.h (nanoarrow only)
        PLUGIN->>CMAKE: .fletcher.arrow.pb.h (Arrow C++)
    and TypeScript (if --fletcher_opt=ts)
        PLUGIN->>CMAKE: .fletcher.ts (interfaces + schema)
    end

    PLUGIN->>PROTOC: CodeGeneratorResponse (stdout)
    CMAKE->>CMAKE: Compile generated headers with targets
```

## Attachment (Sidecar Blob) Flow

```mermaid
sequenceDiagram
    participant APP as Application
    participant ROW as ArrowRow
    participant PUB as Publisher
    participant PP as PubSub
    participant ENV as Envelope
    participant SUB as Subscriber

    APP->>ROW: set_image_ref("frame_001")
    APP->>APP: Load blob data

    APP->>PUB: Publish(row, {{"frame_001", blob}})
    PUB->>ENV: SerializeEnvelope(encodedRow, attachments)
    ENV->>ENV: [ROW_LEN:4][ROW:N][ATT_COUNT:4]<br/>[KEY_LEN:4][KEY][BLOB_LEN:4][BLOB]...
    PUB->>PP: Transport serialized envelope

    PP->>SUB: Raw envelope bytes
    SUB->>ENV: DeserializeEnvelope(bytes)
    ENV->>SUB: EncodedRow + Attachments map
    SUB->>SUB: Decode row, access attachment by key

    Note over APP,SUB: Attachments are a transport-layer concern.<br/>The codec is unaware of them.
```

## Multi-Provider Interoperability

```mermaid
graph TB
    subgraph edge ["Edge Device (MCU)"]
        XRCE_PUB["XRCE-DDS Publisher<br/><i>nanoarrow, &lt;75 KB</i>"]
    end

    subgraph agent ["XRCE-DDS Agent"]
        AGENT["MicroXRCEAgent<br/><i>Bridges to full DDS</i>"]
    end

    subgraph vessel ["Vessel Workstation"]
        DDS_SUB["FastDDS Subscriber<br/><i>nanoarrow</i>"]
        ARROW_PS["PubSubArrow<br/><i>Arrow C++ adapter</i>"]
    end

    subgraph browser ["Browser"]
        WG["WebGateway<br/><i>WebSocket server</i>"]
        WC["WebClient (TS)<br/><i>Positional codec</i>"]
    end

    XRCE_PUB -->|"XRCE-DDS<br/>(UDP/Serial)"| AGENT
    AGENT -->|"Full DDS<br/>(RTPS)"| DDS_SUB
    DDS_SUB --> ARROW_PS
    DDS_SUB --> WG
    WG -->|WebSocket| WC

    style edge fill:#fff3e0,stroke:#e65100
    style vessel fill:#e8f5e9,stroke:#2e7d32
    style browser fill:#e3f2fd,stroke:#1565c0
```
