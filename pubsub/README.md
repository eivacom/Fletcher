# fletcher-pubsub

A static library providing the high-level Fletcher pub/sub API. It defines the
abstract `PubSub` provider interface (raw byte buffers + nanoarrow schemas) and
a `Driver` wrapper that adds multi-subscriber fan-out and a topic registry on
top of any concrete provider. Wire-compatible Arrow IPC schema
serialization helpers are included.

Headers are located under `include/fletcher/pubsub/` and consumed as `#include <fletcher/pubsub/<header>.hpp>`:
- `pubsub.hpp` — `PubSub` interface and supporting types.
- `driver.hpp` — `Driver` (multi-subscriber fan-out, topic registry).
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
conan build . --build=missing -pr:a=Visual-Studio-2022-v143-x64-Debug
```

Build locally and run unit tests:
```bash
conan build . --build=missing -pr:a=Visual-Studio-2022-v143-x64-Debug -o "&:run_tests=True"
```

Create the Conan package:
```bash
conan create . -pr:a=Visual-Studio-2022-v143-x64-Debug
```

Create the Conan package with unit tests:
```bash
conan create . -pr:a=Visual-Studio-2022-v143-x64-Debug -o "&:run_tests=True"
```

---

### Linux (devcontainer)

See the repo root's [Development environment](../README.md#development-environment) section for how to open the devcontainer (VS Code or manual Docker). Once inside, from this directory:

1. Install dependencies and configure the build tree:
```bash
conan install . --build=missing -pr:a=Ubuntu22-gcc-12-Debug -o "&:run_tests=True"
```

2. Build the library and tests:
```bash
conan build . -pr:a=Ubuntu22-gcc-12-Debug -o "&:run_tests=True"
```

3. Run the unit tests directly via CTest:
```bash
ctest --test-dir build/Debug --output-on-failure
```

4. Create and publish to the local Conan cache:
```bash
conan create . --build=missing -pr:a=Ubuntu22-gcc-12-Debug -o "&:run_tests=True"
```

5. Verify the package is in the local cache:
```bash
conan list "fletcher-pubsub:*"
```

Steps 1–3 iterate during development without writing to the Conan cache. Step 4 publishes the package locally so downstream Fletcher packages can pick it up.

---

## CI pipeline

The workflow is defined in `.github/workflows/fletcher-pubsub.yml` and runs on
every pull request touching `pubsub/**`.

```
pull_request / workflow_dispatch
        │
        ├──────────────────────────────────────┐
        ▼                                      ▼
build-windows                            build-linux
Windows Server Core LTSC 2025            Ubuntu 24.04 x64
Native runner                            Docker container (.devcontainer)
Profile: Visual-Studio-2022-             Profile: Ubuntu22-gcc-12-Release
         v143-x64-RelWithDebInfo
        │                                      │
        └──────────────────┬───────────────────┘
                           │ both must pass
                           ▼
                        upload
                  Publishes to conan-eiva Artifactory
```

### Build profiles

| Job | Runner | Profile | Build type |
|---|---|---|---|
| `build-windows` | `windows-server-core-ltsc2025` | `Visual-Studio-2022-v143-x64-RelWithDebInfo` | RelWithDebInfo |
| `build-linux` | `ubuntu_24.04_x64` (Docker) | `Ubuntu22-gcc-12-Release` | Release |

### Package handoff

Unlike `fletcher-core` (header-only, single platform-independent ID),
`fletcher-pubsub` is a real compiled static library: the Windows and Linux
jobs each produce their own platform-specific package, and the `upload` job
restores both before publishing.

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
        self.requires("fletcher-pubsub/0.1.0-alpha")

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
#include <fletcher/pubsub/driver.hpp>
#include <fletcher/pubsub/pubsub.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/schema_ipc.hpp>
```
