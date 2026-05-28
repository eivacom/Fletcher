# Contributing to Fletcher

Thank you for your interest in contributing. Fletcher is an LGPL-3.0-or-later project and welcomes contributions from the community.

## Before you start

- Open an issue first for non-trivial changes so the approach can be discussed before you invest time coding.
- For small fixes (typos, obvious bugs) a PR without a prior issue is fine.

## Development environment

Fletcher's build toolchain lives entirely inside the devcontainer at `.devcontainer/`. All component READMEs assume you are working inside it.

**VS Code** — open the repo root and select *Reopen in Container*. CMake, Conan, GCC 13, Node and the formatters are preinstalled in the image.

**Manual Docker** — build the image once and mount the repo:

```bash
docker build -t fletcher-build .devcontainer
docker run --rm -it -v $(pwd):/workspace -w /workspace fletcher-build bash
```

Conan profiles live in [`.conan-profiles/`](.conan-profiles) and are referenced by relative path (`-pr:a=../.conan-profiles/<profile>`), so no profile-install step is needed. Then follow the individual component `README.md` for build and test commands.

## Repository layout

Each top-level directory is an independent component with its own version, CI workflow, and release cycle. A change that touches more than one component should be split into separate PRs unless the components must be updated atomically.

| Directory | Artifact |
|---|---|
| `core/` | `fletcher-core` (Conan, header-only) |
| `pubsub/` | `fletcher-pubsub` (Conan, static lib) |
| `arrow-bridge/` | `fletcher-arrow-bridge` (Conan, static lib) |
| `pubsub-arrow/` | `fletcher-pubsub-arrow` (Conan, static lib) |
| `fastdds-pubsub-provider/` | `fletcher-fastdds-pubsub-provider` (Conan, static lib) |
| `xrcedds-pubsub-provider/` | `fletcher-xrcedds-pubsub-provider` (Conan, static lib) |
| `protoc/` | `fletcher-protoc` (Conan, application) |
| `gateway/` | `gateway` (standalone executable) |
| `gateway-client-ts/` | `fletcher-gateway-client` (npm) |

## Coding conventions

### License headers

Every tracked source file must carry these two lines within its first ten lines, using the right comment prefix for the language (`//` for C/C++/TypeScript/Protobuf, `#` for Python/CMake/YAML/Dockerfile):

```
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) <year> The Fletcher Authors
//
```

The `license-headers` CI job checks every tracked file on each PR and will block merge if the headers are missing. Files that genuinely cannot carry a header (binary formats, strict-JSON files like `package.json`) must be added to the denylist in `.github/workflows/license-headers.yml`.

### C++ style

- All first-party code lives under `namespace fletcher { ... }`.
- Public headers go under `include/fletcher/<component_snake>/` and are consumed as `#include <fletcher/<component_snake>/<header>.hpp>`.
- CMake targets follow the `<conan-name>::<conan-name>` pattern (e.g. `fletcher-core::fletcher-core`).

### Naming

| Ecosystem | Pattern | Example |
|---|---|---|
| Conan package | `fletcher-<component>` | `fletcher-core` |
| npm package | `fletcher-<component>` | `fletcher-gateway-client` |
| CMake target | `<conan-name>::<conan-name>` | `fletcher-core::fletcher-core` |

## Submitting a pull request

1. Fork the repository and create a branch off `main`.
2. Make your changes. Keep each PR focused on a single component.
3. Ensure all new source files carry the license header (the CI check will catch missing ones).
4. Update the relevant component `README.md` if you are changing the public API or build instructions.
5. Open a PR against `main`. Fill in the PR template checklist.

CI runs automatically on every PR. All checks must pass before a maintainer will review.

## Commit messages

Use a short imperative subject line (≤ 72 characters). Reference an issue number when relevant (`Closes #N`). No strict format is enforced beyond that.

## Releases

Releases are cut by maintainers by pushing a component-prefixed git tag (`<component>-v<version>`). Contributors do not need to tag — bump the version in `conanfile.py` (or `package.json` for `gateway-client-ts`) in your PR and a maintainer will tag after merge.

## License

By submitting a contribution you agree that your work will be distributed under the [LGPL-3.0-or-later](LICENSE) license and that you are entitled to make it available under those terms. Add your name or organisation to [`AUTHORS`](AUTHORS) in your first PR if you would like to be listed.
