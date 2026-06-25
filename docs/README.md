# Fletcher — Architecture Documentation

This directory contains the architecture documentation for Fletcher, an Arrow-native row serialization and pub/sub library system.

## Structure

```
architecture-overview.md       # Main architecture document — start here
component-diagram.md           # Component detail and dependency graph
data-flow-diagrams.md          # Encode/decode, pub/sub, browser flows
wire-format-specification.md   # Positional wire format, type mapping, envelope
recordbatch-accessor-spec.md   # Column-oriented C++/Rust RecordBatch accessors (accessor/rust opts)
technology-decisions.md        # Technology decision log (TD-001 through TD-007)
fletcher-options.md            # (fletcher.flatten) schema-flattening option
```

The project [README](../README.md) is the top-level introduction and getting-started guide; this directory holds the deeper architecture references.

## Viewing

All diagrams use [Mermaid](https://mermaid.js.org/) syntax, which renders natively in GitHub, GitLab, and most Markdown viewers.

## Contributing

1. Create a branch for your changes.
2. Edit Markdown files and Mermaid diagrams.
3. Open a pull request — reviewers can see diffs and rendered diagrams in GitHub.
