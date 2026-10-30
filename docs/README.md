# Fletcher — Architecture Documentation

This directory contains the architecture documentation for Fletcher, an Arrow-native row serialization and pub/sub library system.

## Structure

```
architecture-overview.md       # Main architecture document — start here
component-diagram.md           # Component detail and dependency graph (C++, TypeScript, the .NET binding)
data-flow-diagrams.md          # Encode/decode, pub/sub, .NET, browser and code-generation flows
wire-format-specification.md   # Positional wire format, type mapping (Arrow, TypeScript, C#), envelope
recordbatch-accessor-spec.md   # Column-oriented C++/Rust/C# RecordBatch accessors (accessor/rust/csharp_accessor opts)
pubsub-interface-spec.md       # The pub/sub provider seam: crossing types, registry, delivery contract (round PDA-decouple)
protocol-driver-abi-spec.md    # Pure-C protocol driver ABI below that seam (round PDA-ABI)
technology-decisions.md        # Technology decision log (TD-001 through TD-009)
fletcher-options.md            # (fletcher.flatten) schema-flattening option
```

The project [README](../README.md) is the top-level introduction and getting-started guide, with the language support table and the cross-language pub/sub matrix; this directory holds the deeper architecture references. The .NET binding is documented beside its code: [`c-abi/README.md`](../c-abi/README.md) for the C ABI shim (ownership rules, codec surface, how a binding consumes it) and [`dotnet/README.md`](../dotnet/README.md) for the NuGet packages; its rationale is TD-009, its type mapping is in the wire-format specification and its accessor in the accessor specification (§11).

## Viewing

All diagrams use [Mermaid](https://mermaid.js.org/) syntax, which renders natively in GitHub, GitLab, and most Markdown viewers.

## Contributing

1. Create a branch for your changes.
2. Edit Markdown files and Mermaid diagrams.
3. Open a pull request — reviewers can see diffs and rendered diagrams in GitHub.
