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
fletcher-core/0.1.0: Package 'ed88f90664dcf54031da76dc58cc87e345eb3ff8' built
fletcher-core/0.1.0: Build folder C:\Users\mkb\.conan2\p\b\fletc7f5a5ce355669\b\build
fletcher-core/0.1.0: Generating the package
fletcher-core/0.1.0: Packaging in folder C:\Users\mkb\.conan2\p\b\fletc7f5a5ce355669\p
fletcher-core/0.1.0: Calling package()
fletcher-core/0.1.0: package(): Packaged 4 '.hpp' files: envelope.hpp, positional_io.hpp, types.hpp, write_buffer.hpp
fletcher-core/0.1.0: Created package revision e2249563785f3cff903218aa880f21fa
fletcher-core/0.1.0: Package 'ed88f90664dcf54031da76dc58cc87e345eb3ff8' created
fletcher-core/0.1.0: Full package reference: fletcher-core/0.1.0#a9a59f2494d596b474f35c655ed3b8d7:ed88f90664dcf54031da76dc58cc87e345eb3ff8#e2249563785f3cff903218aa880f21fa
fletcher-core/0.1.0: Package folder C:\Users\mkb\.conan2\p\b\fletc604fa8c2f7309\p


### Linux (devcontainer)
Pending...

## Using the plugin
The static library bust be included...