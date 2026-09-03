# fletcher-core

A header-only library facilitating the Fletcher Publish-Subscriber logic.

Headers are located under `include/fletcher/core/`:
- `envelope.hpp`
- `positional_io.hpp`
- `status.hpp`
- `types.hpp`
- `write_buffer.hpp`

---

## Error taxonomy (published)

`fletcher/core/status.hpp` defines the pub/sub seam's failure vocabulary: **one error type
carrying one stable numbered cause** (`PubSubError` over `PubSubStatus`; owner ruling
2026-09-01, [pub/sub interface spec](../docs/pubsub-interface-spec.md) §5.1). The numbers are
published here because two independent language bindings need them in prose, beside the code
that defines them. **This table is the only enumeration of them** — the spec cites it and does
not restate it.

**Only `Name` and `Number` are machine-compared.** `core_tests`'
`Taxonomy.PublishedNumbersMatchTheEnum` reads this table off disk and compares it row for row
against the enum, so editing a name or a number on either side alone turns it red, and
appending an enumerator without publishing its row fails the build. `Meaning` is a reader's
summary; the normative wording is the doc comment on each enumerator in `status.hpp`.

| Name | Number | Meaning |
|---|---|---|
| `kOk` | 0 | Success. Present because both C boundaries need a success value in the same enum; `PubSubError` refuses it, so a boundary cannot report a failed call as a success. |
| `kInvalidArgument` | 1 | The caller passed something the seam refuses to interpret: an empty topic-segment list, a blob with bytes and no owner, a negative timeout. |
| `kSchemaConflict` | 2 | A topic was re-declared with a provably different schema (spec §7 clause 3). |
| `kTopicNotDeclared` | 3 | The topic has not been declared on this instance. |
| `kPayloadTooLarge` | 4 | The encoded sample does not fit the transport's payload bound. A `std::overflow_error` escaping a seam entry point maps here, normatively (spec §5.1). |
| `kTransportFailure` | 5 | The transport refused or failed: an endpoint that would not be created, a write that did not go out, a session that is gone. |
| `kNotSupported` | 6 | This provider does not implement the requested behaviour. |
| `kInternal` | 7 | The total catch-all. Anything with no better home arrives here carrying the original message — a taxonomy that lets an untyped exception through is not one. |
| `kPending` | 8 | A wait **outcome**, never thrown: the answer is not available yet, within the timeout that was asked for. |
| `kSubscriptionEnded` | 9 | A wait **outcome**, never thrown: the answer will never arrive, because the subscription that would have produced it is gone. |

Values are **fixed integers, appended only** — never renumbered, reordered, reused or removed,
because a boundary that has shipped one of these numbers to an application cannot take it back.
Making an append is a stop-and-ask against the spec and the owner allocates the number (spec
§12); the append carries its row in this table in the same change, which the guard above makes
mechanical rather than a request.

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

Create the Conan package (required to produce packaged headers under `include/fletcher/core/`):
```bash
conan create . -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release
```

Create the Conan package with unit tests:
```bash
conan create . -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release -o "&:run_tests=True"
```

> Note: `conan build` only runs the build step and does not produce the package output.
> Use `conan create` to populate the Conan cache with the headers.

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

4. Create and publish to the local Conan cache (headers + cmake module):
```bash
conan create . --build=missing -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release -o "&:run_tests=True"
```

5. Verify the package is in the local cache:
```bash
conan list "fletcher-core:*"
```

Steps 1–3 iterate during development without writing to the Conan cache. Step 4 publishes the package locally so downstream Fletcher packages can pick it up.

---

## CI pipeline

The build workflow is defined in `.github/workflows/ci.core.yml`.
It is `workflow_call`-only — invoked from `ci.pr.yml` for pull requests
touching `core/**` and from `cd.core.yml` on `core-v*` tag pushes.
The matching upload job lives in `cd.core.yml`, not here.

```
ci.pr.yml (PRs) / cd.core.yml (tag push)
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
              (cd.core.yml job)
              Creates GitHub Release with
              fletcher-core-{windows,linux}-conan-package.tgz
```

### Build profiles

| Job | Runner | Profile | Build type |
|---|---|---|---|
| `build-windows` | `windows-2022` | `.conan-profiles/Windows-msvc194-x86_64-Release` | Release |
| `build-linux` | `ubuntu-latest` (Docker) | `.conan-profiles/Linux-gcc13-x86_64-Release` | Release |

### Package handoff

Because `fletcher-core` is a header-only library it produces a single
platform-independent package ID. Each build job saves the Conan package
to a GitHub Actions workflow artifact; on a tag push the `upload` job in
`cd.core.yml` downloads both and attaches them as GitHub Release
assets:

```
conan cache save  →  actions/upload-artifact  →  actions/download-artifact  →  gh release create
```

The `upload` job only runs from `cd.core.yml` (tag push). PR runs
through `ci.pr.yml` build and test but produce no release artifacts.

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
        self.requires("fletcher-core/0.5.0-alpha")

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
#include "fletcher/core/status.hpp"
#include "fletcher/core/types.hpp"
#include "fletcher/core/write_buffer.hpp"
```

### 4. Minimal usage example

```cpp
#include "fletcher/core/envelope.hpp"
#include "fletcher/core/positional_io.hpp"
#include "fletcher/core/status.hpp"
#include "fletcher/core/types.hpp"
#include "fletcher/core/write_buffer.hpp"

#include <vector>

int main() {
    // Envelope round-trip
    fletcher::Envelope env;
    env.row = { 0x01, 0x02, 0x03, 0x04 };
    auto serialized = fletcher::SerializeEnvelope(env);
    // The two-argument form owns the ownership rule for you: it takes ONE shared copy of the
    // buffer, and only when the envelope carries attachments. There is no vector overload —
    // an attachment's Blob must not alias a buffer nothing keeps alive.
    auto restored   = fletcher::DeserializeEnvelope(serialized.data(), serialized.size());

    // Write positional wire format into a growable buffer; Finish() hands the bytes over
    fletcher::VectorWriteBuffer writeBuffer;
    fletcher::PositionalWriter writer(writeBuffer, 1 /*num_fields*/);
    writer.WriteBool(false);
    std::vector<uint8_t> raw = writeBuffer.Finish();

    // Share the buffer as a Blob (zero-copy shared_ptr)
    fletcher::Blob blob = std::make_shared<const std::vector<uint8_t>>(std::move(raw));

    // Failures cross the pub/sub seam as one type carrying one published number (see the
    // taxonomy above)
    const fletcher::PubSubError error(fletcher::PubSubStatus::kInvalidArgument, "empty topic");
    return static_cast<int>(error.status());
}
```
