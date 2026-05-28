# fletcher-core

A header-only library facilitating the Fletcher Publish-Subscriber logic.

Headers are located under `include/core/`:
- `envelope.hpp`
- `positional_io.hpp`
- `types.hpp`
- `write_buffer.hpp`

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

Create the Conan package (required to produce packaged headers under `include/core/`):
```bash
conan create . -pr:a=Visual-Studio-2022-v143-x64-Debug
```

Create the Conan package with unit tests:
```bash
conan create . -pr:a=Visual-Studio-2022-v143-x64-Debug -o "&:run_tests=True"
```

> Note: `conan build` only runs the build step and does not produce the package output.
> Use `conan create` to populate the Conan cache with the headers.

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

4. Create and publish to the local Conan cache (headers + cmake module):
```bash
conan create . --build=missing -pr:a=Ubuntu22-gcc-12-Debug -o "&:run_tests=True"
```

5. Verify the package is in the local cache:
```bash
conan list "fletcher-core:*"
```

Steps 1–3 iterate during development without writing to the Conan cache. Step 4 publishes the package locally so downstream Fletcher packages can pick it up.

---

## CI pipeline

The workflow is defined in `.github/workflows/fletcher-core.yml` and runs on every
push to `feature/fletcher-core` and on every pull request touching `core/**`.

```
push / pull_request
        │
        ├──────────────────────────────────────┐
        ▼                                      ▼
build-windows                            build-linux
Windows Server Core LTSC 2025            Ubuntu 24.04 x64
Native runner                            Docker container (.devcontainer)
Profile: Visual-Studio-2022-             Profile: Ubuntu22-gcc-12-Release
         v143-x64-Release
        │                                      │
        └──────────────────┬───────────────────┘
                           │ both must pass
                           ▼
                        upload
                  (push / workflow_dispatch only)
                  Creates GitHub Release (.tgz assets)
```

### Build profiles

| Job | Runner | Profile | Build type |
|---|---|---|---|
| `build-windows` | `windows-server-core-ltsc2025` | `Visual-Studio-2022-v143-x64-Release` | Release |
| `build-linux` | `ubuntu_24.04_x64` (Docker) | `Ubuntu22-gcc-12-Release` | Release |

### Package handoff

Because `fletcher-core` is a header-only library it produces a single
platform-independent package ID. The Windows build saves the package to a
GitHub Actions artifact which the `upload` job restores and publishes:

```
conan cache save  →  actions/upload-artifact  →  actions/download-artifact  →  gh release create
```

Upload is skipped on pull requests — it only runs on `push` and `workflow_dispatch`.

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
        self.requires("fletcher-core/0.1.0-alpha")

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

find_package(fletcher-core CONFIG REQUIRED)

add_executable(my-project src/main.cpp)

# Either the full target name:
target_link_libraries(my-project PRIVATE fletcher-core::fletcher-core)

# Or the convenience alias provided by fletcher-core-target.cmake:
target_link_libraries(my-project PRIVATE fletcher::core)
```

### 3. Include the headers

```cpp
#include "fletcher/core/envelope.hpp"
#include "fletcher/core/positional_io.hpp"
#include "fletcher/core/types.hpp"
#include "fletcher/core/write_buffer.hpp"
```

### 4. Minimal usage example

```cpp
#include "fletcher/core/envelope.hpp"
#include "fletcher/core/positional_io.hpp"
#include "fletcher/core/types.hpp"
#include "fletcher/core/write_buffer.hpp"

#include <vector>

int main() {
    // Envelope round-trip
    fletcher::Envelope env;
    env.row = { 0x01, 0x02, 0x03, 0x04 };
    auto serialized = fletcher::SerializeEnvelope(env);
    auto restored   = fletcher::DeserializeEnvelope(serialized);

    // Write positional wire format into a growable buffer
    std::vector<uint8_t> raw;
    fletcher::VectorWriteBuffer writeBuffer(raw);
    fletcher::PositionalWriter writer(writeBuffer, 1 /*num_fields*/);
    writer.WriteBool(false);

    // Share the buffer as a Blob (zero-copy shared_ptr)
    fletcher::Blob blob = std::make_shared<const std::vector<uint8_t>>(raw);
}
```
