<!-- Space: Software -->
<!-- Parent: Architecture Overview -->
<!-- Title: Component and Dependency Diagram -->

# Component and Dependency Diagram

## System Context

```mermaid
graph TB
    subgraph External
        PROTO[(".proto Files<br/>Message & Service<br/>Definitions")]
        SENSORS["Sensors / Data Sources<br/>(Row-at-a-time)"]
        BROWSERS["Browser Clients"]
        SINKS["Data Sinks<br/>Parquet, Data Lake,<br/>Query Engine"]
    end

    subgraph System ["Fletcher"]
        CODEGEN["Code Generation Layer<br/>(protoc-gen-fletcher)"]
        PUBSUB["Pub/Sub Transport Layer"]
        CODEC["Codec Layer"]
        BATCH["Batching Layer"]
        WEBGW["WebGateway"]
        WEBCL["WebClient (TypeScript)"]
    end

    PROTO -->|Build-time| CODEGEN
    CODEGEN -->|Generated classes| SENSORS
    SENSORS -->|EncodedRow via<br/>PubSub| PUBSUB
    PUBSUB -->|Raw bytes| CODEC
    CODEC -->|ArrowRow| BATCH
    BATCH -->|arrow::Table| SINKS
    PUBSUB -->|WebSocket| WEBGW
    WEBGW -->|Binary frames| WEBCL
    WEBCL -->|Typed objects| BROWSERS
```

## Component Detail

```mermaid
graph TB
    subgraph codegen_layer ["Code Generation (build-time)"]
        PROTOC["protoc + protoc-gen-fletcher<br/><i>Reads .proto files</i>"]
        GEN_EDGE[".fletcher.pb.h<br/><i>Message class + schema<br/>(nanoarrow only)</i>"]
        GEN_SERVER[".fletcher.view.pb.h<br/><i>View class with typed getters<br/>(Arrow C++)</i>"]
        GEN_TS[".fletcher.ts<br/><i>TypeScript interface +<br/>SchemaDescriptor</i>"]

        PROTOC --> GEN_EDGE
        PROTOC --> GEN_SERVER
        PROTOC --> GEN_TS
    end

    subgraph edge_tier ["Edge Tier (nanoarrow only, ~100 KB)"]
        NANOARROW["nanoarrow<br/><i>Arrow type system,<br/>schema, IPC</i>"]

        subgraph pubsub_core ["PubSub Core"]
            PROVIDER["PubSub<br/><i>Abstract transport interface</i>"]
            DRIVER["Driver<br/><i>Fan-out, subscription IDs,<br/>topic registry</i>"]
            POS_IO["PositionalWriter / Reader<br/><i>Header-only encode/decode</i>"]
            ENVELOPE["Envelope<br/><i>Row + attachments bundle</i>"]
            SCHEMA_IPC["Schema IPC<br/><i>Nanoarrow serialization</i>"]
        end

        subgraph providers ["Transport Providers"]
            FASTDDS["FastDDSPubSubProvider<br/><i>Desktop / server DDS</i>"]
            XRCEDDS["XrceDDSPubSubProvider<br/><i>MCU / embedded, &lt;75 KB</i>"]
        end

        WEB_GATEWAY["WebGateway<br/><i>Boost.Beast WebSocket server</i>"]

        NANOARROW --- pubsub_core
        PROVIDER --> FASTDDS
        PROVIDER --> XRCEDDS
        DRIVER --> WEB_GATEWAY
    end

    subgraph server_tier ["Server Tier (Arrow C++)"]
        ARROW_CPP["Apache Arrow C++<br/><i>Full columnar library</i>"]

        POS_CODEC["PositionalCodec<br/><i>ArrowRow encode/decode</i>"]
        PUBSUB_ARROW["PubSubArrow<br/><i>Arrow C++ adapter<br/>over nanoarrow provider</i>"]

        ARROW_CPP --- POS_CODEC
        POS_CODEC --> PUBSUB_ARROW
    end

    subgraph browser_tier ["Browser (TypeScript)"]
        WEBCLIENT["FletcherClient<br/><i>WebSocket manager</i>"]
        TS_CODEC["Positional Codec (TS)<br/><i>Encoder / Decoder</i>"]
        OBJ_BACKEND["Object Backend<br/><i>Record&lt;string, unknown&gt;</i>"]
        ARROW_BACKEND["Arrow Backend<br/><i>Apache Arrow JS (planned)</i>"]

        WEBCLIENT --> TS_CODEC
        TS_CODEC --> OBJ_BACKEND
        TS_CODEC --> ARROW_BACKEND
    end

    GEN_EDGE -->|Uses| pubsub_core
    GEN_SERVER -->|Uses| POS_CODEC
    GEN_TS -->|Consumed by| WEBCLIENT
    PUBSUB_ARROW -->|Wraps| PROVIDER
    WEB_GATEWAY -->|WebSocket| WEBCLIENT
```

## Dependency Graph

```mermaid
graph TB
    NANOARROW["nanoarrow<br/><i>vendored, ~100 KB</i>"]

    PUBSUB["pubsub<br/><i>nanoarrow only, edge-ready</i>"]

    FASTDDS_PROV["fast_dds_pubsub_provider<br/><i>nanoarrow + Fast DDS</i>"]
    XRCE_PROV["xrce_dds_pubsub_provider<br/><i>nanoarrow + Micro XRCE-DDS<br/>&lt;75 KB Flash</i>"]
    ROW_CODEC["row_codec<br/><i>Arrow C++, server-side</i>"]

    PUBSUB_ARROW["pubsub_arrow<br/><i>pubsub + row_codec<br/>+ Arrow C++</i>"]

    WEB_GW["web_gateway<br/><i>pubsub + Boost.Beast<br/>nanoarrow only</i>"]
    PROTO_INT["proto_integration<br/><i>test suite</i>"]

    NANOARROW --> PUBSUB
    PUBSUB --> FASTDDS_PROV
    PUBSUB --> XRCE_PROV
    PUBSUB --> ROW_CODEC
    PUBSUB --> WEB_GW
    ROW_CODEC --> PUBSUB_ARROW
    ROW_CODEC --> PROTO_INT

    style NANOARROW fill:#e8f5e9,stroke:#2e7d32
    style PUBSUB fill:#e8f5e9,stroke:#2e7d32
    style FASTDDS_PROV fill:#e8f5e9,stroke:#2e7d32
    style XRCE_PROV fill:#e8f5e9,stroke:#2e7d32
    style WEB_GW fill:#e8f5e9,stroke:#2e7d32
    style ROW_CODEC fill:#e3f2fd,stroke:#1565c0
    style PUBSUB_ARROW fill:#e3f2fd,stroke:#1565c0
    style PROTO_INT fill:#fff3e0,stroke:#e65100
```

Legend:
- Green: edge tier (nanoarrow only)
- Blue: server tier (Arrow C++)
- Orange: test-only

## Two-Tier Deployment

```mermaid
graph LR
    subgraph config ["What Changes Between Tiers"]
        direction TB
        C1["Arrow dependency<br/><i>nanoarrow → Arrow C++</i>"]
        C2["Codec level<br/><i>PositionalWriter → PositionalCodec</i>"]
        C3["Schema type<br/><i>OwnedSchema → shared_ptr&lt;arrow::Schema&gt;</i>"]
        C4["Generated headers<br/><i>.fletcher.pb.h → + .fletcher.view.pb.h</i>"]
    end

    subgraph invariant ["What Stays the Same"]
        direction TB
        I1["Wire format (byte-identical)"]
        I2["PubSub interface"]
        I3["Transport providers (DDS)"]
        I4["Schema transport mechanism"]
        I5["Envelope serialization"]
        I6["WebGateway protocol"]
    end

    style config fill:#fff3e0,stroke:#e65100
    style invariant fill:#e8f5e9,stroke:#2e7d32
```

## Project Structure

```mermaid
graph TB
    subgraph root ["ArrowRowSerializer/"]
        direction TB
        CMAKE["CMakeLists.txt<br/><i>Root build, Conan integration</i>"]
        CONAN["conanfile.txt<br/><i>Dependencies</i>"]

        subgraph third ["third_party/"]
            NANO_V["nanoarrow/<br/><i>Vendored amalgamation</i>"]
        end

        subgraph pubsub_dir ["PubSub/"]
            PS_INC["include/pubsub/<br/><i>Provider, Driver, OwnedSchema,<br/>WriteBuffer, PositionalIO,<br/>Envelope, Types</i>"]
            PS_SRC["src/"]
            PS_TEST["tests/ (24 cases)"]
        end

        subgraph codec_dir ["Codec/"]
            C_INC["include/<br/><i>PositionalCodec, RowCodec,<br/>ArrowRowView, C API</i>"]
            C_SRC["src/"]
            C_TEST["tests/ (92 cases)"]
            C_RUST["bindings/rust/"]
        end

        subgraph psa_dir ["PubSubArrow/"]
            PSA_INC["include/pubsub_arrow/<br/><i>PubSubArrow adapter</i>"]
            PSA_TEST["tests/ (7 cases)"]
        end

        subgraph plugin_dir ["ProtoPlugin/"]
            PP_SRC["src/<br/><i>generator, type_mapper, main</i>"]
            PP_TEST["tests/ (36 cases)"]
        end

        subgraph integ_dir ["ProtoIntegration/"]
            PI_PROTO["proto/<br/><i>Test .proto files</i>"]
            PI_TEST["tests/ (76 cases)"]
        end

        subgraph dds_dir ["FastDDSPubSubProvider/"]
            DDS_ALL["<i>Full DDS provider</i>"]
        end

        subgraph xrce_dir ["XrceDDSPubSubProvider/"]
            XRCE_ALL["<i>XRCE-DDS provider</i>"]
        end

        subgraph webgw_dir ["WebGateway/"]
            WG_ALL["<i>WebSocket server + example</i>"]
        end

        subgraph webcl_dir ["WebClient/"]
            WC_ALL["<i>TypeScript client (37 tests)</i>"]
        end
    end
```
