# Fletcher — Architecture Documentation

This directory contains the architecture documentation for Fletcher, an Arrow-native row serialization and pub/sub library system.

## Structure

```
fletcher.md                    # Documentation landing page
architecture-overview.md       # Main architecture document
component-diagram.md           # Component detail and dependency graph
data-flow-diagrams.md          # Encode/decode, pub/sub, browser flows
wire-format-specification.md   # Positional wire format, type mapping, envelope
technology-decisions.md        # Technology decision log (TD-001 through TD-007)
```

## Viewing

All diagrams use [Mermaid](https://mermaid.js.org/) syntax, which renders natively in GitHub, GitLab, and most Markdown viewers.

## Contributing

1. Create a branch for your changes.
2. Edit Markdown files and Mermaid diagrams.
3. Open a pull request — reviewers can see diffs and rendered diagrams in GitHub.
