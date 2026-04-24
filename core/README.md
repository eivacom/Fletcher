# fletcher-core

A header-only library facilitating the Fletcher Publish-Subscriber logic.

Headers are located under `include/core/`:
- `envelope.hpp`
- `positional_io.hpp`
- `types.hpp`
- `write_buffer.hpp`

## Building

Requires [Conan 2](https://docs.conan.io/2/) and CMake 3.15+.

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

> Note: `conan build` only runs the build step and does not produce the package output. Use `conan create` to populate the Conan cache with the headers.

### Linux (devcontainer)
Pending...

## Using the plugin
The static library bust be included...