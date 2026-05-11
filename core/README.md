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

The container is built from `.devcontainer` at the repo root and includes GCC 12, CMake, Conan 2, and Node 24 pre-configured. The same image covers every Fletcher component. There are three ways to work with it:

#### Option A — VS Code devcontainer (interactive)

Open the repository in VS Code and select **Reopen in Container**. The `postCreateCommand`
in `devcontainer.json` installs the Conan configuration automatically on first launch.
Once inside, the following commands run directly in the integrated terminal:

1. Install dependencies and configure the build tree:
```bash
conan install . --build=missing -pr:a=Ubuntu22-gcc-12-Debug -o "&:run_tests=True"
```

2. Build the library and tests
```bash
conan build . -pr:a=Ubuntu22-gcc-12-Debug -o "&:run_tests=True"
```

3. Run the unit tests directly via CTest
```bash
ctest --test-dir build/Debug --output-on-failure
```

4. Create and publish to the local Conan cache (headers + cmake module)
```bash
conan create . --build=missing -pr:a=Ubuntu22-gcc-12-Debug -o "&:run_tests=True"
```

5. Verify the package is in the local cache
```bash
conan list "eiva-fletcher-core:*"
```

> Steps 1–3 are for iterating during development — they do not write to the Conan cache.
> Step 4 is required to make the package available to other packages on the same machine.

---

#### Option B — Docker interactive shell from the host

Equivalent to Option A but driven from a host terminal rather than VS Code.


1. Build the image from the devcontainer folder (only needed once or after Dockerfile changes)
```bash
docker build -t fletcher-build .devcontainer
```

2. Start an interactive shell with the repo mounted
```bash
docker run --rm -it \
  -v $(pwd):/workspace \
  -w /workspace/core \
  fletcher-build \
  bash
```

3. Inside the container — replicate the postCreateCommand from devcontainer.json
```bash
conan config install https://github.com/eivacom/conan-configuration.git
```  

4. Build, test and package as normal
```bash
conan install . --build=missing -pr:a=Ubuntu22-gcc-12-Debug -o "&:run_tests=True"
```
```bash
conan build . -pr:a=Ubuntu22-gcc-12-Debug -o "&:run_tests=True"
````
```bash
ctest --test-dir build/Debug --output-on-failure
```
```bash
conan create . --build=missing -pr:a=Ubuntu22-gcc-12-Debug -o "&:run_tests=True"
```

> Build artefacts written inside the container (e.g. `core/build/`) are visible on the
> host after the container exits because the workspace is mounted at `/workspace`.

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
                  Publishes to conan-eiva Artifactory
```

### Build profiles

| Job | Runner | Profile | Build type |
|---|---|---|---|
| `build-windows` | `windows-server-core-ltsc2025` | `Visual-Studio-2022-v143-x64-Release` | Release |
| `build-linux` | `ubuntu_24.04_x64` (Docker) | `Ubuntu22-gcc-12-Release` | Release |

### Linux Docker container

The Linux job builds and tests entirely inside the consolidated Docker image
built from `.devcontainer` at the repo root. The container image is cached in
Harbor under a single shared key so every Fletcher workflow reuses it:

```
dockerrepo.eiva.com/fletcher/devcontainer:cache
```

### Package handoff

Because `eiva-fletcher-core` is a header-only library it produces a single
platform-independent package ID. The Windows build saves the package to a
GitHub Actions artifact which the `upload` job restores and publishes:

```
conan cache save  →  actions/upload-artifact  →  actions/download-artifact  →  conan cache restore  →  conan upload
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
        self.requires("eiva-fletcher-core/0.1.3-alpha")

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

find_package(eiva-fletcher-core CONFIG REQUIRED)

add_executable(my-project src/main.cpp)

# Either the full target name:
target_link_libraries(my-project PRIVATE eiva-fletcher-core::eiva-fletcher-core)

# Or the convenience alias provided by fletcher-core-target.cmake:
target_link_libraries(my-project PRIVATE fletcher::core)
```

### 3. Include the headers

```cpp
#include "core/envelope.hpp"
#include "core/positional_io.hpp"
#include "core/types.hpp"
#include "core/write_buffer.hpp"
```

### 4. Minimal usage example

```cpp
#include "core/envelope.hpp"
#include "core/positional_io.hpp"
#include "core/types.hpp"
#include "core/write_buffer.hpp"

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
