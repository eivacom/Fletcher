# Component and Dependency Diagram

## System Context

```mermaid
graph TB
    subgraph External
        PROTO[(".proto Files<br/>Message & Service<br/>Definitions")]
        PRODUCERS["Producing Applications<br/>(Row-at-a-time)"]
        BROWSERS["Browser Clients"]
        DOTNET_APPS[".NET Applications"]
        SINKS["Arrow Consumers<br/>Storage, Analytics,<br/>Forwarding"]
    end

    subgraph System ["Fletcher"]
        CODEGEN["Code Generation Layer<br/>(protoc-gen-fletcher)"]
        PUBSUB["Pub/Sub Transport Layer"]
        CODEC["Codec Layer"]
        BATCH["Batching Layer"]
        WEBGW["gateway"]
        WEBCL["gateway-client-ts"]
        CABI["C ABI shim<br/>(fletcher-c-abi)"]
        DOTNET[".NET binding<br/>(Eiva.Fletcher)"]
        DOTNET_GW["Eiva.Fletcher.GatewayClient"]
    end

    PROTO -->|Build-time| CODEGEN
    CODEGEN -->|Generated classes| PRODUCERS
    CODEGEN -->|Generated C#| DOTNET_APPS
    PRODUCERS -->|EncodedRow via<br/>PubSub| PUBSUB
    PUBSUB -->|Raw bytes| CODEC
    CODEC -->|ArrowRow| BATCH
    BATCH -->|arrow::Table| SINKS
    PUBSUB -->|WebSocket| WEBGW
    WEBGW -->|Binary frames| WEBCL
    WEBCL -->|Typed objects| BROWSERS
    DOTNET_APPS -->|Arrow batches| DOTNET
    DOTNET -->|P/Invoke| CABI
    CABI -->|Encoded rows| PUBSUB
    WEBGW -->|Binary frames| DOTNET_GW
    DOTNET_GW -->|Arrow batches| DOTNET_APPS
```

## Component Detail

```mermaid
graph TB
    subgraph codegen_layer ["Code Generation (build-time)"]
        PROTOC["protoc + protoc-gen-fletcher<br/><i>Reads .proto files</i>"]
        GEN_EDGE[".fletcher.pb.h<br/><i>Message class + schema +<br/>publisher/subscriber<br/>(nanoarrow only)</i>"]
        GEN_SERVER[".fletcher.arrow.pb.h<br/><i>View class + ToArrowRow<br/>(Arrow C++)</i>"]
        GEN_TS[".fletcher.ts<br/><i>TypeScript interface +<br/>SchemaDescriptor +<br/>publisher/subscriber<br/>opt: ts</i>"]
        GEN_ACCESSOR[".fletcher.accessor.pb.h<br/><i>RecordBatch accessor (C++)<br/>opt: accessor (Arrow C++)</i>"]
        GEN_RUST[".fletcher.rs<br/><i>RecordBatch accessor (Rust)<br/>opt: rust (arrow crate)</i>"]
        GEN_CS[".fletcher.cs + .fletcher.native.cs<br/><i>C# row class + schema +<br/>native publisher/subscriber<br/>opt: csharp</i>"]
        GEN_CS_GW[".fletcher.gateway.cs<br/><i>C# gateway publisher/subscriber<br/>opt: csharp_gateway</i>"]
        GEN_CS_ACC[".fletcher.arrow.cs + .fletcher.accessor.cs<br/><i>C# view + RecordBatch accessor<br/>opt: csharp_accessor</i>"]

        PROTOC --> GEN_EDGE
        PROTOC --> GEN_SERVER
        PROTOC --> GEN_TS
        PROTOC --> GEN_ACCESSOR
        PROTOC --> GEN_RUST
        PROTOC --> GEN_CS
        PROTOC --> GEN_CS_GW
        PROTOC --> GEN_CS_ACC
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

    subgraph dotnet_tier [".NET Binding (NuGet)"]
        CABI_SHIM["fletcher-c-abi<br/><i>C ABI shim, one shared library per RID:<br/>NanoarrowCodec, registry, pub/sub,<br/>built-in providers linked in</i>"]
        NET_INTEROP["Eiva.Fletcher.Interop<br/><i>P/Invoke + the shim</i>"]
        NET_CORE["Eiva.Fletcher<br/><i>Publisher, Subscriber,<br/>SubscriberArrow, FletcherCodec</i>"]
        NET_MODEL["Eiva.Fletcher.Model<br/><i>Timestamp, Duration,<br/>ListView, MapView</i>"]
        NET_GW["Eiva.Fletcher.GatewayClient<br/><i>FletcherClient + managed codec</i>"]
        APACHE_ARROW_NET["Apache.Arrow (.NET)"]

        NET_INTEROP -->|P/Invoke| CABI_SHIM
        NET_CORE --> NET_INTEROP
        NET_CORE --> APACHE_ARROW_NET
        NET_MODEL --> APACHE_ARROW_NET
        NET_GW --> APACHE_ARROW_NET
    end

    GEN_EDGE -->|Uses| pubsub_core
    GEN_SERVER -->|Uses| POS_CODEC
    GEN_TS -->|Consumed by| WEBCLIENT
    GEN_CS -->|Uses| NET_CORE
    GEN_CS -->|Uses| NET_MODEL
    GEN_CS_GW -->|Uses| NET_GW
    GEN_CS_ACC -->|Uses| NET_MODEL
    CABI_SHIM -->|Links| pubsub_core
    CABI_SHIM -->|Links| providers
    PUBSUB_ARROW -->|Wraps| PROVIDER
    WEB_GATEWAY -->|WebSocket| WEBCLIENT
    WEB_GATEWAY -->|WebSocket| NET_GW
```

## Dependency Graph

Every monorepo package and how it depends on every other one. Arrows point from a dependency to its consumer (X → Y means Y depends on X). External libraries are included where they cross a package boundary that matters for understanding deployment.

```mermaid
graph TB
    NANOARROW["nanoarrow<br/><i>external, vendored, ~100 KB</i>"]
    PROTOBUF["protobuf<br/><i>external, 3.21.12</i>"]
    ARROW_CPP["Apache Arrow C++<br/><i>external, 23.0.1</i>"]
    BEAST["Boost.Beast/Asio<br/><i>external, header-only</i>"]
    FAST_DDS["Fast DDS<br/><i>external, 3.4.3</i>"]
    XRCE_DDS["Micro XRCE-DDS<br/><i>external</i>"]
    ARROW_NET["Apache.Arrow (.NET)<br/><i>external, 23.0.0</i>"]

    CORE["core"]
    PUBSUB["pubsub"]
    CODEC["arrow-bridge"]
    PUBSUB_ARROW["pubsub-arrow"]
    FASTDDS_PROV["fastdds-pubsub-provider"]
    XRCE_PROV["xrcedds-pubsub-provider"]
    WEB_GW["gateway"]
    PROTOC["protoc<br/><i>protoc-gen-fletcher plugin</i>"]
    PROTO_INT["integration-tests/<br/>protoc-arrow-bridge"]
    GW_CLIENT["gateway-client-ts<br/><i>npm: @eiva/fletcher-gateway-client</i>"]
    C_ABI["c-abi<br/><i>fletcher-c-abi shared library</i>"]
    NET_PKGS["dotnet/<br/><i>NuGet: Eiva.Fletcher, .Interop, .Model</i>"]
    NET_GW_PKG["dotnet/<br/><i>NuGet: Eiva.Fletcher.GatewayClient</i>"]
    PROTOC_PKGS["protoc packages<br/><i>npm: @eiva/protoc-gen-fletcher<br/>NuGet: Eiva.Fletcher.Protoc</i>"]

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

    PUBSUB --> C_ABI
    FASTDDS_PROV --> C_ABI
    XRCE_PROV --> C_ABI

    C_ABI --> NET_PKGS
    ARROW_NET --> NET_PKGS
    ARROW_NET --> NET_GW_PKG

    PROTOC --> PROTOC_PKGS

    WEB_GW -.->|WebSocket runtime| GW_CLIENT
    WEB_GW -.->|WebSocket runtime| NET_GW_PKG

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
    style PROTOC_PKGS fill:#f3e5f5,stroke:#6a1b9a
    style C_ABI fill:#e0f2f1,stroke:#00695c
    style NET_PKGS fill:#e0f2f1,stroke:#00695c
    style NET_GW_PKG fill:#e0f2f1,stroke:#00695c
    style ARROW_NET fill:#e0f2f1,stroke:#00695c
```

Legend:
- **Green** — edge tier (nanoarrow + external transports). Builds and runs without Apache Arrow C++.
- **Blue** — server tier. Adds Apache Arrow C++ on top of the edge tier.
- **Purple** — build-time code generator. Produces source files consumed at compile time, not linked at runtime.
- **Orange** — test-only artifact.
- **Pink** — TypeScript / npm. No Conan dependency on the C++ packages; the dotted arrow indicates a runtime WebSocket-protocol relationship to the `gateway`.
- **Teal** — the .NET binding. `c-abi` statically links `pubsub` and both providers (with Fast DDS, Micro XRCE-DDS and nanoarrow) into one shared library per RID, which `Eiva.Fletcher.Interop` carries; `Eiva.Fletcher.GatewayClient` loads no native code and, like the TypeScript client, reaches the `gateway` over WebSocket. The code generator's packages (purple) bring the `protoc` plugin to a build: the NuGet package carries the binary for each RID and gives MSBuild its path, for the project's own protoc call; the npm package downloads it from the GitHub Release on first use.

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

The monorepo is a polyrepo of mostly Conan-packaged C++ components — most top-level C++ component directories have their own `conanfile.py` and are built independently into the local Conan cache. Some top-level directories, such as `integration-tests/`, contain nested C++ components rather than a root-level `conanfile.py`. TypeScript packages get a `-ts` suffix so the language is obvious from a directory listing, and they use npm rather than Conan; `dotnet/` is one .NET solution of four NuGet packages, built with the .NET SDK over the `c-abi/` shim.

```mermaid
graph TB
    subgraph root ["Fletcher/"]
        direction TB
        ROOT_README["README.md<br/><i>Top-level repo overview + TODO backlog</i>"]
        DOCS["docs/<br/><i>Architecture documentation</i>"]
        WORKFLOWS[".github/workflows/<br/><i>ci.&lt;component&gt;.yml, cd.&lt;component&gt;.yml<br/>per-component CI and release</i>"]

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
            PP_PKG["npm/, nuget/<br/><i>the plugin's npm and NuGet packages</i>"]
        end

        subgraph cabi_dir ["c-abi/"]
            CA_INC["include/fletcher/abi/<br/><i>binding.h, the C ABI</i>"]
            CA_SRC["src/<br/><i>entry points, NanoarrowCodec</i>"]
            CA_TEST["tests/"]
        end

        subgraph dotnet_dir ["dotnet/"]
            DN_SRC["src/<br/><i>the four packages</i>"]
            DN_TEST["tests/"]
        end

        subgraph integ_dir ["integration-tests/"]
            PI_ALL["<i>One directory per lane:<br/>protoc-arrow-bridge, pubsub-conformance,<br/>gateway-dotnet-crosslang, …</i>"]
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
            WC_ALL["<i>TypeScript client<br/>(npm: @eiva/fletcher-gateway-client)</i>"]
        end
    end
```
