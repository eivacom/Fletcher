# Fletcher — Architecture Documentation

This directory contains the architecture documentation for Fletcher, EIVA's Arrow-native row serialization and pub/sub library system.

## Structure

```
fletcher.md                    # Confluence parent page (under Architecture)
architecture-overview.md       # Main architecture document
component-diagram.md           # Component detail and dependency graph
data-flow-diagrams.md          # Encode/decode, pub/sub, browser flows
wire-format-specification.md   # Positional wire format, type mapping, envelope
technology-decisions.md        # Technology decision log (TD-001 through TD-007)
```

## Viewing

All diagrams use [Mermaid](https://mermaid.js.org/) syntax, which renders natively in GitHub, GitLab, and most Markdown viewers.

## Publishing to Confluence

Documents include Confluence front-matter comments (`<!-- Space: Software -->`, etc.) compatible with [Mark](https://github.com/kovetskiy/mark) for automated publishing.

### Manual publish

```bash
mark -u user@eiva.com -p API_TOKEN -b https://eiva.atlassian.net/wiki --mermaid-provider="mermaid-go" -f fletcher.md
mark -u user@eiva.com -p API_TOKEN -b https://eiva.atlassian.net/wiki --mermaid-provider="mermaid-go" -f architecture-overview.md
mark -u user@eiva.com -p API_TOKEN -b https://eiva.atlassian.net/wiki --mermaid-provider="mermaid-go" -f component-diagram.md
mark -u user@eiva.com -p API_TOKEN -b https://eiva.atlassian.net/wiki --mermaid-provider="mermaid-go" -f data-flow-diagrams.md
mark -u user@eiva.com -p API_TOKEN -b https://eiva.atlassian.net/wiki --mermaid-provider="mermaid-go" -f wire-format-specification.md
mark -u user@eiva.com -p API_TOKEN -b https://eiva.atlassian.net/wiki --mermaid-provider="mermaid-go" -f technology-decisions.md
```

### Confluence page hierarchy

Pages are published to the **Software** space under the existing **Architecture** page:

```
Architecture (existing)
  └── Fletcher                          ← fletcher.md
        └── Architecture Overview       ← architecture-overview.md
              ├── Component and Dependency Diagram
              ├── Data Flow Diagrams
              ├── Wire Format Specification
              └── Technology Decision Log
```

## Contributing

1. Create a branch for your changes.
2. Edit Markdown files and Mermaid diagrams.
3. Open a pull request — reviewers can see diffs and rendered diagrams in GitHub.
4. On merge to main, publish the updated docs to Confluence.
