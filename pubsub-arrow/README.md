# fletcher-pubsub-arrow

Server-side Arrow C++ wrapper around the `eiva-fletcher-pubsub` `Driver`.
Bridges the gap between the nanoarrow-based pub/sub core (raw bytes +
`ArrowSchema` C structs) and Apache Arrow C++ types (`arrow::Schema`,
`ArrowRow`).

```cpp
#include <pubsub_arrow/pubsub_arrow.hpp>

fletcher::PubSubArrow ps(provider);
ps.CreateTopic({"orders", "v1"}, arrow_schema);
ps.Publish({"orders", "v1"}, arrow_row);
ps.Subscribe({"orders", "v1"}, [](fletcher::ArrowRow row,
                                  fletcher::Attachments att) { ... });
```

Internally owns a `Driver` and a per-topic `Codec` (from
`eiva-fletcher-arrow-bridge`) so callers can publish and subscribe with
Arrow scalars; the wire format remains byte-identical to what edge code
produces via the raw `Driver`.

---

## Building locally

Requires [Conan 2](https://docs.conan.io/2/) and CMake 3.15+.

### Windows

```bash
conan create . -pr:a=Visual-Studio-2022-v143-x64-Release -o "&:run_tests=True"
```

### Linux (devcontainer)

See the repo root's [Development environment](../README.md#development-environment) section for how to open the devcontainer (VS Code or manual Docker). Once inside, from this directory:

```bash
conan create . --build=missing -pr:a=Ubuntu22-gcc-12-Release -o "&:run_tests=True"
```

---

## Consuming the package

```python
def requirements(self):
    self.requires("eiva-fletcher-pubsub-arrow/0.1.0-alpha")
```

```cmake
find_package(eiva-fletcher-pubsub-arrow CONFIG REQUIRED)
target_link_libraries(my-target PRIVATE fletcher::pubsub-arrow)
```

```cpp
#include <pubsub_arrow/pubsub_arrow.hpp>
```

`pubsub-arrow` re-exports its dependencies (`eiva-fletcher-pubsub`,
`eiva-fletcher-arrow-bridge`, `arrow::arrow`) transitively, so
consumers don't need to declare them separately.

---

## CI pipeline

`.github/workflows/fletcher-pubsub-arrow.yml` runs `build-windows` +
`build-linux` on every PR touching `pubsub-arrow/**`, and an `upload`
job that publishes platform-specific package binaries to Artifactory
on a `pubsub-arrow-v*.*.*` release-tag push.
