# Fletcher for .NET: the preview, for testers

Until pull request [#129](https://github.com/eivacom/Fletcher/pull/129) merges, the C# bindings
are not released anywhere. Instead, every push to its branch, `feature/csharp-bindings`, builds a
**preview bundle**: one download holding everything needed to try them. Each bundle is kept for
30 days. Use the newest one.

| In the bundle | |
|---|---|
| `nuget/` | The four NuGet packages, `Eiva.Fletcher`, `Eiva.Fletcher.Interop` (with the native library for Linux and Windows), `Eiva.Fletcher.Model`, `Eiva.Fletcher.GatewayClient` |
| `gateway/` | The Fletcher gateway for Linux and Windows, built from the same commit. Use this one; the released 0.5 gateway does not work with the C# gateway client |
| `protoc/` | The code generator `protoc-gen-fletcher` for Linux and Windows, and `fletcher/options.proto` |
| `README-preview.md` | How to use all of it, step by step |

## Getting the newest bundle

You need a GitHub account (signed in); the repository is public.

**In the browser:** open the
[ci.pr runs for the branch](https://github.com/eivacom/Fletcher/actions/workflows/ci.pr.yml?query=branch%3Afeature%2Fcsharp-bindings+is%3Asuccess),
click the newest run, and on its **Summary** page, under **Artifacts**, download
`fletcher-preview-0.6.0-pr.129.<run>` (about 20 MB). The same runs are reached from the pull
request: **Checks**, then **ci.pr**, then **Summary**.

**With the GitHub CLI:**

```bash
gh run list --repo eivacom/Fletcher --workflow ci.pr.yml --branch feature/csharp-bindings --status success --limit 1
gh run download <run id from the list> --repo eivacom/Fletcher --pattern 'fletcher-preview-*'
```

The CLI unpacks the bundle into a folder of the same name.

## Using it

Unpack the download and read its `README-preview.md`. Each package also carries a README (shown
by your IDE's package manager, or `README.md` inside the `.nupkg`) that walks from a `.proto`
file to publishing and subscribing, natively and through the gateway, with code that CI compiles
and runs. In short:

1. Add the bundle's `nuget/` folder as a package source (a `nuget.config` beside your solution)
   and reference the packages at the bundle's exact version, `0.6.0-pr.129.<run>`.
2. Generate C# for your `.proto` files with `protoc` and the bundle's `protoc-gen-fletcher`
   (`--fletcher_opt=csharp`, and `csharp_gateway` for the gateway client).
3. To use the gateway, unpack `gateway/` for your platform and run it; connect
   `Eiva.Fletcher.GatewayClient` to it.

Needed on the machine: .NET 8 or .NET 10; on Linux glibc 2.38 and the GCC 13 C++ runtime or newer
(Ubuntu 24.04, Debian 13, RHEL 10 or newer); on Windows the Microsoft Visual C++ 2015-2022
redistributable (x64).

**Moving to a newer bundle:** change the version you reference to the new bundle's and point the
package source at its `nuget/` folder. Every bundle has a version of its own, so nothing stale is
picked up from the NuGet cache.

## When 0.6.0-alpha is released

After the pull request merges, the packages are released as `0.6.0-alpha` on EIVA's NuGet feed.
NuGet ranks the preview versions above `0.6.0-alpha`, so then remove the preview package source,
delete the bundles, clear the NuGet cache (`dotnet nuget locals all --clear`) and reference
`0.6.0-alpha`.

## Problems and questions

Comment on pull request [#129](https://github.com/eivacom/Fletcher/pull/129), naming the bundle
(its `VERSION` file gives the version and the commit).
