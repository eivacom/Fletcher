# fletcher-pubsub

A static library providing the high-level Fletcher pub/sub API. It defines the
abstract `PubSub` provider interface (raw byte buffers + nanoarrow schemas) and
a `Driver` wrapper that adds multi-subscriber fan-out and a topic registry on
top of any concrete provider. Wire-compatible Arrow IPC schema
serialization helpers are included.

Headers are located under `include/pubsub/`:
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

The container is built from `.devcontainer` at the repo root and includes GCC 12, CMake, Conan 2, and Node 24 pre-configured. The same image covers every Fletcher component.

#### Option A — VS Code devcontainer (interactive)

Open the repository in VS Code and select **Reopen in Container**. The
`postCreateCommand` in `devcontainer.json` installs the Conan configuration
automatically on first launch.

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
conan list "eiva-fletcher-pubsub:*"
```

---

#### Option B — Docker interactive shell from the host

1. Build the image (only needed once or after Dockerfile changes):
```bash
docker build -t fletcher-build .devcontainer
```

2. Start an interactive shell with the repo mounted:
```bash
docker run --rm -it \
  -v $(pwd):/workspace \
  -w /workspace/pubsub \
  fletcher-build \
  bash
```

3. Inside the container:
```bash
conan config install https://github.com/eivacom/conan-configuration.git
```

4. Build, test and package as normal:
```bash
conan create . --build=missing -pr:a=Ubuntu22-gcc-12-Debug -o "&:run_tests=True"
```

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

### Linux Docker container

The Linux job builds and tests entirely inside the consolidated Docker image
built from `.devcontainer` at the repo root. The container image is cached in
Harbor under a single shared key so every Fletcher workflow reuses it:

```
dockerrepo.eiva.com/fletcher/devcontainer:cache
```

### Package handoff

Unlike `eiva-fletcher-core` (header-only, single platform-independent ID),
`eiva-fletcher-pubsub` is a real compiled static library: the Windows and Linux
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
        self.requires("eiva-fletcher-pubsub/0.1.0-alpha")

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

find_package(eiva-fletcher-pubsub CONFIG REQUIRED)

add_executable(my-project src/main.cpp)

# Either the full target name:
target_link_libraries(my-project PRIVATE eiva-fletcher-pubsub::eiva-fletcher-pubsub)

# Or the convenience alias provided by fletcher-pubsub-target.cmake:
target_link_libraries(my-project PRIVATE fletcher::pubsub)
```

### 3. Include the headers

```cpp
#include "pubsub/driver.hpp"
#include "pubsub/pubsub.hpp"
#include "pubsub/owned_schema.hpp"
#include "pubsub/schema_ipc.hpp"
```
