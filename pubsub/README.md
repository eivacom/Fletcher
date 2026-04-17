# fletcher-pubsub

A static library facilitating the Fletcher Publish-Subscriber logic.

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

Create the Conan package and run test_package:

```bash
conan create . -pr:a=Visual-Studio-2022-v143-x64-Debug
```

Create the Conan package with unit tests:

```bash
conan create . -pr:a=Visual-Studio-2022-v143-x64-Debug -o "&:run_tests=True"
```

### Linux (devcontainer)

Open the repo in the provided devcontainer (`.devcontainer/`). Profiles are installed automatically via `conan config install`.

Build locally:

```bash
conan build . --build=missing -pr:a=Ubuntu22-gcc-12-Debug
```

Build locally and run unit tests:

```bash
conan build . --build=missing -pr:a=Ubuntu22-gcc-12-Debug -o "&:run_tests=True"
```

Create the Conan package and run test_package:

```bash
conan create . -pr:a=Ubuntu22-gcc-12-Debug
```

Create the Conan package with unit tests:

```bash
conan create . -pr:a=Ubuntu22-gcc-12-Debug -o "&:run_tests=True"
```

## Using the plugin
The static library bust be included...