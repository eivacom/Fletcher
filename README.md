# fletcher

Multi-package C++ workspace. Each top-level directory is an independent
Conan package with its own version, CI workflow, and release cycle:

| Directory | Conan / npm package | Type |
|---|---|---|
| `core/` | `eiva-fletcher-core` | header-only |
| `protoc/` | `eiva-fletcher-protoc` | application (protoc plugin) |
| `pubsub/` | `eiva-fletcher-pubsub` | static library |
| `arrow-bridge/` | `eiva-fletcher-arrow-bridge` | static library |
| `pubsub-arrow/` | `eiva-fletcher-pubsub-arrow` | static library |
| `fastdds-pubsub-provider/` | `eiva-fletcher-fastdds-pubsub-provider` | static library |
| `xrcedds-pubsub-provider/` | `eiva-fletcher-xrcedds-pubsub-provider` | static library |
| `gateway-client-ts/` | `eiva-fletcher-gateway-client` (npm) | TypeScript library |

Each package has its own `README.md` covering how to build, test and consume it.

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

A dedicated [`build-devcontainer`](.github/workflows/build-devcontainer.yml) workflow runs on `main` whenever `.devcontainer/**` changes and pushes a BuildKit cache to Harbor under

```
dockerrepo.eiva.com/fletcher/devcontainer:cache
```

Every component workflow's Linux job runs `docker buildx build --cache-from=...:cache` (via the `setup-devcontainer-image` composite action) and tags the resulting image locally as `fletcher-build`. When `.devcontainer/` is unchanged every layer is a cache hit; when a PR touches `.devcontainer/` only the affected layers are rebuilt. PR builds do not push to `:cache`.

---

## TODO

Identified improvements across the codebase, ordered by priority.

### P1 — Bugs

| # | File | Description |
|---|---|---|
| 1 ⚠️ | `arrow-bridge/src/scalar_codec.cpp:236` | `FixedSizeBinaryScalar` wraps a raw pointer without copying — the returned `Scalar` holds a dangling reference if the input buffer is destroyed. All other variable-length types copy via `Buffer::FromString`; this case must do the same. |
| 2 ⚠️ | `arrow-bridge/src/codec.cpp` (9 sites), `arrow-bridge/include/arrow_bridge/arrow_row_view.hpp` (4 sites) | `ValueOrDie()` on Arrow `Result<T>` aborts the process on error. The rest of the codebase throws `std::invalid_argument`. Replace each `.ValueOrDie()` call with a checked `.Value()` / `.ok()` guard that throws. (`protoc/src/generator.cpp` also emits `.ValueOrDie()` patterns into generated code — fix those once the runtime fix is in place.) |
| 3 | `pubsub/include/pubsub/owned_schema.hpp:53` | `ArrowSchemaDeepCopy` returns an `int` error code that `DeepCopy` silently discards. A failed copy leaves the schema empty with no diagnostic. Check the return value and throw. |
| 4 | `protoc/src/generator.cpp:953,997,1059,1089` | For unsupported proto→Arrow type mappings the generator writes `// TODO: unknown ... type` into the user's generated `.fletcher.pb.h` rather than failing. The generator should call `AddError()` on the plugin context so the user gets a protoc error at build time instead of silently broken generated code. |

### P2 — Missing `[[nodiscard]]`

Zero `[[nodiscard]]` annotations exist in the codebase. Silently discarding the return value of the functions below is always a bug:

| Function | File | Risk if ignored |
|---|---|---|
| `IsNull(int)` | `core/include/core/positional_io.hpp` | Reads wrong field value without warning |
| `PubSub::Subscribe(...)` | `pubsub/include/pubsub/pubsub.hpp` | Subscription ID lost; can never unsubscribe |
| `Driver::Subscribe(...)` | `pubsub/include/pubsub/driver.hpp` | Same |
| `Codec::EncodeRow(...)` | `arrow-bridge/include/arrow_bridge/codec.hpp` | Encoded bytes silently discarded |
| `Codec::DecodeRow(...)` | `arrow-bridge/include/arrow_bridge/codec.hpp` | Decoded row silently discarded |
| `SerializeEnvelope(...)` | `core/include/core/envelope.hpp` | Serialized bytes lost |
| `DeserializeEnvelope(...)` | `core/include/core/envelope.hpp` | Deserialized envelope lost |
| `OwnedSchema::DeepCopy(...)` | `pubsub/include/pubsub/owned_schema.hpp` | Schema copy silently dropped |
| `Driver::HasTopic(...)` | `pubsub/include/pubsub/driver.hpp` | Query result ignored |
| `Driver::ListTopics()` | `pubsub/include/pubsub/driver.hpp` | List silently discarded |

### P3 — Code duplication

| # | Files | Description |
|---|---|---|
| 5 | `pubsub/src/driver.cpp:17`, `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp:31` | `JoinSegments` is byte-identical in both files. The xrce provider already depends on `pubsub`; expose the helper from a `pubsub/detail/` internal header and remove the copy. |
| 6 | `arrow-bridge/src/codec.cpp:23`, `arrow-bridge/src/scalar_codec.cpp:16` | `AppendFixed<T>` is defined identically in both files (different anonymous namespaces). Move it into `scalar_codec.hpp` as a `detail` helper. |
| 7 | `arrow-bridge/src/codec.cpp:29`, `core/include/core/positional_io.hpp:144` | `BitfieldBytes` is defined separately in `codec.cpp` and `positional_io.hpp`. The `arrow-bridge` copy can delegate to a shared `detail` function in `scalar_codec.hpp`. |
| 8 | `pubsub/tests/test_driver.cpp:73` | `MockProvider::Join` duplicates the production `JoinSegments` logic. If the join separator ever changed the mock would silently diverge. Expose `JoinSegments` from a `pubsub/detail/` header so both production code and the mock use the same implementation. |

### P4 — Dead code

| # | File | Description |
|---|---|---|
| 9 | `arrow-bridge/src/scalar_codec.cpp:276` | `DecodeScalarFromReader`'s string/binary case uses `break` (not `return`) and falls through to an unreachable `throw` at line 276. Change each inner string-case `break` to `return` and remove the unreachable throw. |

### P5 — GeoArrow CRS deferred work

| # | File | Description |
|---|---|---|
| 10 | `protoc/src/generator.cpp:1107,1168,2135` | Three `// TODO: GeoArrow CRS ... will be restored in a later phase` comments mark intentionally deferred work. Convert to tracked issues and remove from source. |

### P6 — Diagnostics

| # | File | Description |
|---|---|---|
| 11 | `fastdds-pubsub-provider/src/fast_dds_pubsub_provider.cpp:166` | `catch (...)` in `FletcherTopicType::serialize` swallows all exceptions silently and returns `false`. The DDS middleware receives a serialization failure with no trace of the cause. Catch `std::exception` first, log or store the message, then fall through to the silent `false` return. |

### P7 — Thread safety

| # | Files | Description |
|---|---|---|
| 12 ⚠️ | `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp` | **Data race on the XRCE session.** The background `run_thread` calls `uxr_run_session_time` continuously without holding any lock. `CreateTopic`, `Publish`, and `Subscribe` hold `mu` and call session functions (`uxr_buffer_create_*`, `uxr_buffer_topic`, `uxr_run_session_until_all_status`) on the same session object. The Micro XRCE-DDS client is not thread-safe — all session operations must be serialized. Fix: protect all session access with a dedicated session mutex that the run thread also acquires, or stop/pause the run thread before every API call. |
| 13 ⚠️ | `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp:123` | **Deadlock: user callback invoked while `mu` is held.** `OnTopic` acquires `mu` before calling `tit->second.callback(...)`. Any call to `CreateTopic`, `Publish`, `Subscribe`, or `Unsubscribe` from within the callback deadlocks. The FastDDS provider correctly calls the user callback outside its lock. Fix: copy the callback pointer and schema under the lock, release the lock, then invoke the callback. |
| 14 | `fastdds-pubsub-provider/src/fast_dds_pubsub_provider.cpp:346` | **Destructor accesses `impl_->topics` without `mu`.** The destructor iterates the topics map and deletes DDS entities with no lock held. A concurrent `Publish` call (which holds `mu` and calls `ts.writer->write()`) racing with the destructor deleting `ts.writer` is undefined behaviour. Fix: acquire `mu` before iterating, or document that callers must ensure no concurrent calls are in flight during destruction. |
| 15 | `pubsub/src/driver.cpp:142` | **TOCTOU race in `CreateTopic`.** The lock is released between the duplicate-check and the call to `provider->CreateTopic()`, then re-acquired to insert the topic state. Two concurrent `CreateTopic` calls for the same topic both pass the initial check, both call the provider, and produce a double-insert. Fix: hold the lock for the entire operation, or insert a sentinel `TopicState` before releasing it so a second caller sees the duplicate. |
| 16 | `pubsub/include/pubsub/driver.hpp` | **Undocumented "last callback after Unsubscribe" behaviour.** The fan-out copies subscriber callbacks under `mu`, releases the lock, then calls them. A subscriber that unsubscribes between the copy and the call receives one final message. This is intentional but should be noted in the `Unsubscribe` doc comment. |

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
