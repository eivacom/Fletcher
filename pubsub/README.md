# fletcher-pubsub

A static library providing the high-level Fletcher pub/sub API. It defines the
abstract `PubSubProvider` interface (raw byte buffers + nanoarrow schemas) and
two concrete client classes — `Publisher` and `Subscriber` — that both take a
`shared_ptr<PubSubProvider>` in their constructor. `Subscriber` adds
multi-subscriber fan-out and a topic registry on top of any concrete provider.
Wire-compatible Arrow IPC schema serialization helpers are included.

Headers are located under `include/fletcher/pubsub/` and consumed as `#include <fletcher/pubsub/<header>.hpp>`:
- `provider.hpp` — `PubSubProvider` abstract interface and supporting types.
- `publisher.hpp` — `Publisher` (CreateTopic + Publish + topic registry).
- `subscriber.hpp` — `Subscriber` (Subscribe + Unsubscribe + multi-subscriber fan-out + subscription IDs).
- `owned_schema.hpp` — RAII wrapper around `ArrowSchema`.
- `schema_ipc.hpp` — Arrow IPC schema serialize/deserialize helpers.

A vendored copy of [nanoarrow](https://github.com/apache/arrow-nanoarrow) 0.8.0
(amalgamation: core + IPC + flatcc) is bundled under `third_party/nanoarrow/`
and built into the package as a static library. Its headers are exposed because
they appear in the public pubsub interface.

---

## Building locally

Requires [Conan 2](https://docs.conan.io/2/) and CMake 3.15+.

---

### Windows

Build locally:
```bash
conan build . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release
```

Build locally and run unit tests:
```bash
conan build . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release -o "&:run_tests=True"
```

Create the Conan package:
```bash
conan create . -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release
```

Create the Conan package with unit tests:
```bash
conan create . -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release -o "&:run_tests=True"
```

---

### Linux (devcontainer)

See the repo root's [Development environment](../README.md#development-environment) section for how to open the devcontainer (VS Code or manual Docker). Once inside, from this directory:

1. Install dependencies and configure the build tree:
```bash
conan install . --build=missing -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release -o "&:run_tests=True"
```

2. Build the library and tests:
```bash
conan build . -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release -o "&:run_tests=True"
```

3. Run the unit tests directly via CTest:
```bash
ctest --test-dir build/Debug --output-on-failure
```

4. Create and publish to the local Conan cache:
```bash
conan create . --build=missing -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release -o "&:run_tests=True"
```

5. Verify the package is in the local cache:
```bash
conan list "fletcher-pubsub:*"
```

Steps 1–3 iterate during development without writing to the Conan cache. Step 4 publishes the package locally so downstream Fletcher packages can pick it up.

---

## CI pipeline

The build workflow is defined in `.github/workflows/ci.pubsub.yml`.
It is `workflow_call`-only — invoked from `ci.pr.yml` for pull requests
touching `pubsub/**` and from `cd.pubsub.yml` on `pubsub-v*` tag
pushes. The matching upload job lives in `cd.pubsub.yml`, not here.

```
ci.pr.yml (PRs) / cd.pubsub.yml (tag push)
        │
        ├──────────────────────────────────────┐
        ▼                                      ▼
build-windows                            build-linux
windows-2022                             ubuntu-latest
Native runner                            Docker container (.devcontainer)
Profile: Windows-msvc194-                Profile: Linux-gcc13-
         x86_64-Release                            x86_64-Release
        │                                      │
        └──────────────────┬───────────────────┘
                           │ both must pass
                           ▼ (only on tag push)
                        upload
              (cd.pubsub.yml job)
              Creates GitHub Release with
              fletcher-pubsub-{windows,linux}-conan-package.tgz
```

### Build profiles

| Job | Runner | Profile | Build type |
|---|---|---|---|
| `build-windows` | `windows-2022` | `.conan-profiles/Windows-msvc194-x86_64-Release` | Release |
| `build-linux` | `ubuntu-latest` (Docker) | `.conan-profiles/Linux-gcc13-x86_64-Release` | Release |

### Package handoff

Unlike `fletcher-core` (header-only, single platform-independent ID),
`fletcher-pubsub` is a real compiled static library: the Windows and Linux
jobs each produce their own platform-specific package. The `upload` job in
`cd.pubsub.yml` downloads both workflow artifacts and attaches them as
GitHub Release assets.

---

## Consuming the package

### 1. Add the dependency in your `conanfile.py`

```python
from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout


class MyProjectConan(ConanFile):
    name = "my-project"
    version = "1.0.0"
    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        self.requires("fletcher-pubsub/0.3.2-alpha")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        CMakeDeps(self).generate()
        CMakeToolchain(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
```

### 2. Link the target in your `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.15)
project(my-project CXX)

find_package(fletcher-pubsub CONFIG REQUIRED)

add_executable(my-project src/main.cpp)

# Either the full target name:
target_link_libraries(my-project PRIVATE fletcher-pubsub::fletcher-pubsub)

# Or the convenience alias provided by fletcher-pubsub-target.cmake:
target_link_libraries(my-project PRIVATE fletcher::pubsub)
```

### 3. Include the headers

```cpp
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub/publisher.hpp>
#include <fletcher/pubsub/subscriber.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/schema_ipc.hpp>
```
