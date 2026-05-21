# fletcher

Multi-component C++ workspace. Each top-level directory is an independent component with its own version, CI workflow, and release cycle. Most components are Conan packages; a few are standalone executables or TypeScript libraries that do not publish a Conan recipe.

| Directory | Artifact | Type |
|---|---|---|
| `core/` | `fletcher-core` (Conan) | header-only library |
| `pubsub/` | `fletcher-pubsub` (Conan) | static library |
| `arrow-bridge/` | `fletcher-arrow-bridge` (Conan) | static library |
| `pubsub-arrow/` | `fletcher-pubsub-arrow` (Conan) | static library |
| `fastdds-pubsub-provider/` | `fletcher-fastdds-pubsub-provider` (Conan) | static library |
| `xrcedds-pubsub-provider/` | `fletcher-xrcedds-pubsub-provider` (Conan) | static library |
| `protoc/` | `fletcher-protoc` (Conan) | application (protoc plugin) |
| `gateway/` | `gateway` exe | standalone executable (no Conan package) |
| `gateway-client-ts/` | `fletcher-gateway-client` (npm) | TypeScript library |

Each component has its own `README.md` covering how to build, test and consume it. The `gateway/` directory ships only an exe — its `conanfile.py` is a local build driver and is never uploaded; consumers integrate through the WebSocket protocol, not a linkable artifact.

### Dependency graph

Build-time (Conan / npm) edges are solid; runtime edges (WebSocket, code-gen) are dashed. External deps (protobuf, Arrow, Boost, FastDDS, Micro XRCE-DDS, nlohmann_json, nanoarrow) are omitted — see each component's `README.md` for the exact set.

```mermaid
graph TD
    core["fletcher-core<br/>header-only"]
    pubsub["fletcher-pubsub<br/>static lib"]
    arrow_bridge["fletcher-arrow-bridge<br/>static lib"]
    pubsub_arrow["fletcher-pubsub-arrow<br/>static lib"]
    fastdds["fletcher-fastdds-pubsub-provider<br/>static lib"]
    xrcedds["fletcher-xrcedds-pubsub-provider<br/>static lib"]
    protoc(["fletcher-protoc<br/>plugin exe"])
    gateway(["gateway<br/>standalone exe"])
    client[("fletcher-gateway-client<br/>npm package")]

    pubsub --> core
    arrow_bridge --> core
    pubsub_arrow --> pubsub
    pubsub_arrow --> arrow_bridge
    fastdds --> pubsub
    fastdds --> core
    xrcedds --> pubsub
    xrcedds --> core
    gateway --> pubsub
    gateway --> core

    client -. WebSocket .-> gateway
    protoc -. generates code for .-> client
    protoc -. generates code for .-> core
```

---

## Development environment

A single devcontainer at `.devcontainer/` covers every Fletcher component. Component READMEs assume you are running inside it.

### VS Code (recommended)

Open the repository root in VS Code and select **Reopen in Container** (or run `Dev Containers: Reopen in Container` from the Command Palette). The `postCreateCommand` runs `conan config install https://github.com/eivacom/conan-configuration.git` on first launch, installing the EIVA Conan profiles and remote.

To pull the released Fletcher packages from `conan-eiva`, log in once per container:

```bash
conan remote login conan-eiva <username>
```

Then follow each component's README for the component-specific build, test, and consumption commands.

### Manual Docker

If you don't use VS Code, run an interactive shell directly. Build the image once:

```bash
docker build -t fletcher-build .devcontainer
```

Start a shell with the repo mounted:

```bash
docker run --rm -it -v $(pwd):/workspace -w /workspace fletcher-build bash
```

Inside the container, replicate what the `postCreateCommand` does in VS Code:

```bash
conan config install https://github.com/eivacom/conan-configuration.git
```

Then `cd <component>` and follow the same build / test commands the component README documents.

### CI image cache

Every component workflow's Linux job runs `docker buildx build` against a shared BuildKit cache in Harbor (via the `setup-devcontainer-image` composite action):

```
dockerrepo.eiva.com/fletcher/devcontainer:cache
```

The action uses both `--cache-from` and `--cache-to`, so unchanged layers are served from cache and any layer that does rebuild refreshes the cache for the next run. The image is tagged locally as `fletcher-build` and used by the rest of the Linux job.

---

## Releasing

Releases are cut by pushing a **component-prefixed git tag** that matches the
version in the package's `conanfile.py`. The CI workflow for that component
then runs builds on Windows + Linux and, if both succeed, publishes the
package to the `conan-eiva` Artifactory remote.

### Tag format

```
<component>-v<MAJOR>.<MINOR>.<PATCH>[-<pre-release>]
```

| Component | Tag prefix | Example |
|---|---|---|
| `core/` | `core-v` | `core-v0.1.0-alpha` |
| `protoc/` | `protoc-v` | `protoc-v0.1.0-alpha` |
| `pubsub/` | `pubsub-v` | `pubsub-v0.1.0-alpha` |
| `arrow-bridge/` | `arrow-bridge-v` | `arrow-bridge-v0.1.0-alpha` |
| `pubsub-arrow/` | `pubsub-arrow-v` | `pubsub-arrow-v0.1.0-alpha` |
| `fastdds-pubsub-provider/` | `fastdds-pubsub-provider-v` | `fastdds-pubsub-provider-v0.1.0-alpha` |
| `xrcedds-pubsub-provider/` | `xrcedds-pubsub-provider-v` | `xrcedds-pubsub-provider-v0.1.0-alpha` |

`gateway-client-ts/` does not yet release through tag-push CI.

The component prefix is required so that pushing a tag triggers exactly one
package's workflow — not all of them.

### Cutting a release

1. Bump the package's version in `<component>/conanfile.py` and merge to `main`.
2. From `main`, tag and push:

   ```bash
   git fetch origin
   ```

   ```bash
   git checkout main
   ```

   ```bash
   git pull
   ```

   ```bash
   git tag <component>-v<version>
   ```

   ```bash
   git push origin <component>-v<version>
   ```

   Example for `core` 0.1.1-alpha (next bump after the current 0.1.0-alpha):

   ```bash
   git tag core-v0.1.1-alpha
   ```

   ```bash
   git push origin core-v0.1.1-alpha
   ```

3. Watch the workflow run for that component complete on
   [GitHub Actions](https://github.com/eivacom/Fletcher/actions). The `upload`
   job is gated on
   `github.event_name == 'push' && startsWith(github.ref, 'refs/tags/')`,
   so only tag pushes publish to Artifactory — `workflow_dispatch` and
   `pull_request` runs build and test but never upload.

### Notes

- Tag and `conanfile.py` version **must match**. The workflow verifies
  this and the upload job fails fast if they differ.
- The upload job also fails if the package version is already published
  on `conan-eiva` — re-releasing an existing version requires bumping
  `conanfile.py` first.
- Releases are independent per component. Bumping `core` does not require
  re-releasing the others.
- Pre-release suffixes (`-alpha`, `-beta`, `-rc1`, …) are part of the
  version and go into the tag.

---

## License

Fletcher is licensed under the GNU Lesser General Public License v3.0
or later (LGPL-3.0-or-later). The full license text is in
[`LICENSE`](LICENSE). Every Fletcher source file carries an
`SPDX-License-Identifier: LGPL-3.0-or-later` header for
machine-readable attribution.

LGPL-3.0 incorporates GPL-3.0 by reference, so binary redistributors
that fall under LGPL section 3(b) need a copy of the GPL-3.0 text in
addition to `LICENSE`. The Fletcher source repo ships only `LICENSE`
(LGPL); the GPL-3.0 text is available verbatim at
<https://www.gnu.org/licenses/gpl-3.0.txt>.

Copyright is held by "The Fletcher Authors" collectively — the people
and organisations listed in [`AUTHORS`](AUTHORS). The project was
started by EIVA A/S in 2026 and is open to outside contributions
under the same license.

Third-party code vendored under `pubsub/third_party/nanoarrow/`
(nanoarrow, including the flatcc sources bundled inside its
amalgamation) is distributed under its own upstream licenses and is
not re-licensed by Fletcher's headers.
