# fletcher

Multi-package C++ workspace. Each top-level directory is an independent
Conan package with its own version, CI workflow, and release cycle:

| Directory | Conan package | Type |
|---|---|---|
| `core/` | `eiva-fletcher-core` | header-only |
| `protoc/` | `fletcher-protoc` | application (protoc plugin) |
| `pubsub/` | `eiva-fletcher-pubsub` | static library |
| `arrow-bridge/` | `eiva-fletcher-arrow-bridge` | static library |
| `pubsub-arrow/` | `eiva-fletcher-pubsub-arrow` | static library |

Each package has its own `README.md` covering how to build, test and consume it.

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
| `core/` | `core-v` | `core-v0.1.3-alpha` |
| `protoc/` | `protoc-v` | `protoc-v0.1.0` |
| `pubsub/` | `pubsub-v` | `pubsub-v0.1.0-alpha` |
| `arrow-bridge/` | `arrow-bridge-v` | `arrow-bridge-v0.1.0-alpha` |
| `pubsub-arrow/` | `pubsub-arrow-v` | `pubsub-arrow-v0.1.0-alpha` |

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

   Example for `core` 0.1.4-alpha:

   ```bash
   git tag core-v0.1.4-alpha
   ```

   ```bash
   git push origin core-v0.1.4-alpha
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
