# Fletcher for .NET

Arrow-typed publish/subscribe for .NET over Fletcher's one wire format, with the codec running
natively. Version `@VERSION@`, built from commit `@COMMIT@` of
[eivacom/Fletcher](https://github.com/eivacom/Fletcher). The four packages share one version and
are used together at the same version.

| Package | What it is | Native assets |
|---|---|---|
| `Eiva.Fletcher` | Publishers, subscribers, the codec and the Arrow tier, over a provider chosen by name (`inprocess`, `fastdds`, `xrce`) | through `Eiva.Fletcher.Interop` |
| `Eiva.Fletcher.Interop` | The P/Invoke surface and the native shim for `linux-x64` and `win-x64`; referenced by `Eiva.Fletcher`, not used directly | **yes**, `runtimes/{rid}/native/` |
| `Eiva.Fletcher.GatewayClient` | A managed WebSocket client for the Fletcher gateway | none |
| `Eiva.Fletcher.Model` | The types generated C# refers to: lossless `Timestamp` and `Duration`, `ListView` and `MapView` | none |

An application that publishes and subscribes natively references `Eiva.Fletcher`; one that talks
to a gateway references `Eiva.Fletcher.GatewayClient`. Both reference `Eiva.Fletcher.Model` for
their generated code. The C# for your messages comes from Fletcher's `protoc` plugin,
`protoc-gen-fletcher` (`--fletcher_opt=csharp` for the model and the native publisher and
subscriber, `csharp_gateway` for the gateway pair, `csharp_accessor` for the Arrow view, and
`csharp_model_only` to leave the native pair out).

Every package targets .NET 8 and .NET 10.

## The native shim (`Eiva.Fletcher`, `Eiva.Fletcher.Interop`)

`Eiva.Fletcher.Interop` carries one native file per platform, the shim `fletcher-c-abi`. It is
self-contained: Fletcher's core, pub/sub and providers, Fast DDS, Micro XRCE-DDS and nanoarrow are
linked into it. `Eiva.Fletcher.GatewayClient` and `Eiva.Fletcher.Model` load no native code, and
nothing in this section applies to them.

### Platform requirements

- **linux-x64:** glibc 2.38 and the GCC 13 C++ runtime (`libstdc++` with `GLIBCXX_3.4.32`) or
  newer: Ubuntu 24.04, Debian 13, RHEL 10 or newer. The shim loads only `libstdc++`, `libgcc_s`,
  `libm` and `libc`.
- **win-x64:** the Microsoft Visual C++ 2015-2022 redistributable (x64). Otherwise the shim loads
  only DLLs every Windows has.

The shim checks at load that it was built for this version of the managed packages; a shim from
another version is refused with a message saying so.

### Replacing the shim (LGPL-3.0)

Fletcher is licensed under the LGPL-3.0-or-later (`LICENSE` in this package). The shim is a shared
library that the .NET assembly loads at run time, so you may replace it with one you build from the
corresponding source, commit `@COMMIT@`:

```bash
git clone https://github.com/eivacom/Fletcher.git
cd Fletcher
git checkout @COMMIT@
```

then build `c-abi/` as its `README.md` describes (`conan create` for `core`, `pubsub`, the two
providers and `c-abi`, with the profile for your platform), and put the resulting
`libfletcher-c-abi.so` (Linux) or `fletcher-c-abi.dll` (Windows) in place of
`runtimes/<rid>/native/` in this package, or beside your published application.

**NativeAOT.** The packages are trimming- and NativeAOT-compatible, and the shim stays a separate
shared library beside a NativeAOT application. Linking the shim statically into the application is
not supported, and an application that did so would form a different combined work under the
LGPL, with the obligations that brings.

## Third-party licences

`THIRD-PARTY-LICENSES.txt` in this package lists the third-party libraries linked into the shim,
with their licence texts.
