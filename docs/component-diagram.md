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
        WEBGW["gateway"]
        WEBCL["gateway-client-ts"]
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
        GEN_SERVER[".fletcher.arrow.pb.h<br/><i>View class + ToArrowRow<br/>(Arrow C++)</i>"]
        GEN_TS[".fletcher.ts<br/><i>TypeScript interface +<br/>SchemaDescriptor</i>"]

        PROTOC --> GEN_EDGE
        PROTOC --> GEN_SERVER
        PROTOC --> GEN_TS
    end

    subgraph edge_tier ["Edge Tier (nanoarrow only, ~100 KB)"]
        NANOARROW["nanoarrow<br/><i>Arrow type system,<br/>schema, IPC</i>"]

        subgraph core_lib ["Core (header-only)"]
            POS_IO["PositionalWriter / Reader<br/><i>Header-only encode/decode</i>"]
            ENVELOPE["Envelope<br/><i>Row + attachments bundle</i>"]
            TYPES["Types, WriteBuffer<br/><i>EncodedRow, Blob, Attachments</i>"]
        end

        subgraph pubsub_core ["PubSub"]
            PROVIDER["PubSub<br/><i>Abstract transport interface</i>"]
            DRIVER["Driver<br/><i>Fan-out, subscription IDs,<br/>topic registry</i>"]
            SCHEMA_IPC["Schema IPC<br/><i>Nanoarrow serialization</i>"]
        end

        subgraph providers ["Transport Providers"]
            FASTDDS["fastdds-pubsub-provider<br/><i>Desktop / server DDS</i>"]
            XRCEDDS["xrcedds-pubsub-provider<br/><i>MCU / embedded, &lt;75 KB</i>"]
        end

        WEB_GATEWAY["gateway<br/><i>Boost.Beast WebSocket server</i>"]

        core_lib --- pubsub_core
        NANOARROW --- pubsub_core
        PROVIDER --> FASTDDS
        PROVIDER --> XRCEDDS
        DRIVER --> WEB_GATEWAY
    end

    subgraph server_tier ["Server Tier (Arrow C++)"]
        ARROW_CPP["Apache Arrow C++<br/><i>Full columnar library</i>"]

        POS_CODEC["arrow-bridge::Codec<br/><i>ArrowRow encode/decode</i>"]
        PUBSUB_ARROW["pubsub-arrow<br/><i>Arrow C++ adapter<br/>over nanoarrow provider</i>"]

        ARROW_CPP --- POS_CODEC
        POS_CODEC --> PUBSUB_ARROW
    end

    subgraph browser_tier ["TypeScript Client"]
        WEBCLIENT["FletcherClient<br/><i>WebSocket manager</i>"]
        TS_CODEC["Positional Codec (TS)<br/><i>Encoder / Decoder</i>"]
        OBJ_BACKEND["ObjectBackend<br/><i>Record&lt;string, unknown&gt;</i>"]
        ARROW_BACKEND["ArrowBackend<br/><i>Apache Arrow JS<br/>(stub, not implemented)</i>"]

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

Every monorepo package and how it depends on every other one. Arrows point from a dependency to its consumer (X → Y means Y depends on X). External libraries are included where they cross a package boundary that matters for understanding deployment.

```mermaid
graph TB
    NANOARROW["nanoarrow<br/><i>external, vendored, ~100 KB</i>"]
    PROTOBUF["protobuf<br/><i>external, 3.21.12</i>"]
    ARROW_CPP["Apache Arrow C++<br/><i>external, 23.0.1</i>"]
    BEAST["Boost.Beast/Asio<br/><i>external, header-only</i>"]
    FAST_DDS["Fast DDS<br/><i>external, 2.14.3</i>"]
    XRCE_DDS["Micro XRCE-DDS<br/><i>external</i>"]

    CORE["core"]
    PUBSUB["pubsub"]
    CODEC["arrow-bridge"]
    PUBSUB_ARROW["pubsub-arrow"]
    FASTDDS_PROV["fastdds-pubsub-provider"]
    XRCE_PROV["xrcedds-pubsub-provider"]
    WEB_GW["gateway"]
    PROTOC["protoc<br/><i>protoc-gen-fletcher plugin</i>"]
    PROTO_INT["integration-tests/<br/>protoc-arrow-bridge"]
    GW_CLIENT["gateway-client-ts<br/><i>npm: fletcher-gateway-client</i>"]

    CORE --> PUBSUB
    NANOARROW --> PUBSUB

    CORE --> CODEC
    ARROW_CPP --> CODEC

    PUBSUB --> FASTDDS_PROV
    CORE --> FASTDDS_PROV
    FAST_DDS --> FASTDDS_PROV

    PUBSUB --> XRCE_PROV
    CORE --> XRCE_PROV
    XRCE_DDS --> XRCE_PROV

    PUBSUB --> PUBSUB_ARROW
    CODEC --> PUBSUB_ARROW
    ARROW_CPP --> PUBSUB_ARROW

    PUBSUB --> WEB_GW
    BEAST --> WEB_GW

    PROTOBUF --> PROTOC

    PUBSUB --> PROTO_INT
    CODEC --> PROTO_INT
    PROTOC --> PROTO_INT
    PROTOBUF --> PROTO_INT

    WEB_GW -.->|WebSocket runtime| GW_CLIENT

    style CORE fill:#e8f5e9,stroke:#2e7d32
    style NANOARROW fill:#e8f5e9,stroke:#2e7d32
    style PUBSUB fill:#e8f5e9,stroke:#2e7d32
    style FASTDDS_PROV fill:#e8f5e9,stroke:#2e7d32
    style XRCE_PROV fill:#e8f5e9,stroke:#2e7d32
    style WEB_GW fill:#e8f5e9,stroke:#2e7d32
    style FAST_DDS fill:#e8f5e9,stroke:#2e7d32
    style XRCE_DDS fill:#e8f5e9,stroke:#2e7d32
    style BEAST fill:#e8f5e9,stroke:#2e7d32
    style CODEC fill:#e3f2fd,stroke:#1565c0
    style PUBSUB_ARROW fill:#e3f2fd,stroke:#1565c0
    style ARROW_CPP fill:#e3f2fd,stroke:#1565c0
    style PROTOC fill:#f3e5f5,stroke:#6a1b9a
    style PROTOBUF fill:#f3e5f5,stroke:#6a1b9a
    style PROTO_INT fill:#fff3e0,stroke:#e65100
    style GW_CLIENT fill:#fce4ec,stroke:#ad1457
```

Legend:
- **Green** — edge tier (nanoarrow + external transports). Builds and runs without Apache Arrow C++.
- **Blue** — server tier. Adds Apache Arrow C++ on top of the edge tier.
- **Purple** — build-time code generator. Produces source files consumed at compile time, not linked at runtime.
- **Orange** — test-only artifact.
- **Pink** — TypeScript / npm. No Conan dependency on the C++ packages; the dotted arrow indicates a runtime WebSocket-protocol relationship to the `gateway`.

Note: a few C++ packages declare `core` as a *direct* requirement even though they would also pick it up transitively via `pubsub`. The direct edge is shown because it reflects the actual conanfile (set with `transitive_headers=True` for header visibility) and matters when copying a single package out of the monorepo.

## Two-Tier Deployment

```mermaid
graph LR
    subgraph config ["What Changes Between Tiers"]
        direction TB
        C1["Arrow dependency<br/><i>nanoarrow → Arrow C++</i>"]
        C2["Codec level<br/><i>PositionalWriter → Codec</i>"]
        C3["Schema type<br/><i>OwnedSchema → shared_ptr&lt;arrow::Schema&gt;</i>"]
        C4["Generated headers<br/><i>.fletcher.pb.h → + .fletcher.arrow.pb.h</i>"]
    end

    subgraph invariant ["What Stays the Same"]
        direction TB
        I1["Wire format (byte-identical)"]
        I2["PubSub interface"]
        I3["Transport providers (DDS)"]
        I4["Schema transport mechanism"]
        I5["Envelope serialization"]
        I6["gateway WebSocket protocol"]
    end

    style config fill:#fff3e0,stroke:#e65100
    style invariant fill:#e8f5e9,stroke:#2e7d32
```

## Project Structure

The monorepo is a polyrepo of mostly Conan-packaged C++ components — most top-level C++ component directories have their own `conanfile.py` and are built independently into the local Conan cache. Some top-level directories, such as `integration-tests/`, contain nested C++ components rather than a root-level `conanfile.py`. (`gateway/` is the current exception among component directories; its Conan packaging is a tracked follow-up.) TypeScript packages get a `-ts` suffix so the language is obvious from a directory listing, and they use npm rather than Conan.

```mermaid
graph TB
    subgraph root ["Fletcher/"]
        direction TB
        ROOT_README["README.md<br/><i>Top-level repo overview + TODO backlog</i>"]
        DOCS["docs/<br/><i>Architecture documentation</i>"]
        WORKFLOWS[".github/workflows/<br/><i>fletcher-&lt;component&gt;.yml<br/>per-component CI</i>"]

        subgraph core_dir ["core/"]
            CORE_INC["include/core/<br/><i>Types, WriteBuffer,<br/>PositionalIO, Envelope</i>"]
            CORE_TEST["tests/"]
        end

        subgraph pubsub_dir ["pubsub/"]
            PS_INC["include/pubsub/<br/><i>Provider, Driver, OwnedSchema</i>"]
            PS_SRC["src/"]
            PS_TEST["tests/"]
        end

        subgraph ab_dir ["arrow-bridge/"]
            AB_INC["include/arrow_bridge/<br/><i>Codec, ArrowRowView,<br/>CRS utils</i>"]
            AB_SRC["src/"]
            AB_TEST["tests/"]
        end

        subgraph psa_dir ["pubsub-arrow/"]
            PSA_INC["include/pubsub_arrow/<br/><i>Arrow C++ adapter</i>"]
            PSA_TEST["tests/"]
        end

        subgraph plugin_dir ["protoc/"]
            PP_SRC["src/<br/><i>generator, type_mapper, main</i>"]
            PP_TEST["tests/"]
        end

        subgraph integ_dir ["integration-tests/protoc-arrow-bridge/"]
            PI_PROTO["proto/<br/><i>Per-scenario .proto files</i>"]
            PI_TEST["tests/<br/><i>Per-scenario test files</i>"]
        end

        subgraph dds_dir ["fastdds-pubsub-provider/"]
            DDS_ALL["<i>Full DDS provider</i>"]
        end

        subgraph xrce_dir ["xrcedds-pubsub-provider/"]
            XRCE_ALL["<i>XRCE-DDS provider</i>"]
        end

        subgraph webgw_dir ["gateway/"]
            WG_ALL["<i>WebSocket server + example</i>"]
        end

        subgraph webcl_dir ["gateway-client-ts/"]
            WC_ALL["<i>TypeScript client<br/>(npm: fletcher-gateway-client)</i>"]
        end
    end
```
