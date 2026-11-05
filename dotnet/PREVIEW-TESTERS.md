# Fletcher for .NET: the preview, for testers

Until pull request [#129](https://github.com/eivacom/Fletcher/pull/129) merges, the C# bindings
are not released anywhere. Instead, every push to its branch, `feature/csharp-bindings`, builds a
**preview bundle**: one download holding everything needed to try them. Each bundle is kept for
30 days. Use the newest one.

| In the bundle | |
|---|---|
| `nuget/` | The NuGet packages, `Eiva.Fletcher`, `Eiva.Fletcher.Interop` (with the native library for Linux and Windows), `Eiva.Fletcher.Model`, `Eiva.Fletcher.GatewayClient`, and `Eiva.Fletcher.Protoc`, the code generator for C# projects |
| `gateway/` | The Fletcher gateway for Linux and Windows, built from the same commit. Use this one; the released 0.5 gateway does not work with the C# gateway client |
| `protoc/` | The code generator `protoc-gen-fletcher` for Linux and Windows outside NuGet, and `fletcher/options.proto` |
| `README-preview.md` | How to use all of it, step by step |

## Getting the newest bundle

You need a GitHub account (signed in); the repository is public.

Always start from pull request [#129](https://github.com/eivacom/Fletcher/pull/129) itself, not
from a list of runs filtered by branch name: a fork can have a branch of the same name.

**In the browser:** on the pull request, open **Checks**, then **ci.pr**, then **Summary**, and
under **Artifacts** download `fletcher-preview-0.6.0-pr.129.<run>` (about 20 MB). If the newest
push is still building, the previous commit's run has the previous bundle.

**With the GitHub CLI**, the bundle of the pull request's newest commit:

```bash
HEAD=$(gh pr view 129 --repo eivacom/Fletcher --json headRefOid --jq .headRefOid)
RUN=$(gh run list --repo eivacom/Fletcher --workflow ci.pr.yml --commit "$HEAD" --status success --limit 1 --json databaseId --jq '.[0].databaseId')
gh run download "$RUN" --repo eivacom/Fletcher --pattern 'fletcher-preview-0.6.0-pr.129.*'
```

The CLI unpacks the bundle into a folder of the same name. Either way, check that the name says
`pr.129` and that the bundle's `VERSION` file names a commit of pull request #129.

## Using it

Unpack the download and read its `README-preview.md`. Each package also carries a README (shown
by your IDE's package manager, or `README.md` inside the `.nupkg`) that walks from a `.proto`
file to publishing and subscribing, natively and through the gateway, with code that CI compiles
and runs. In short:

1. Add the bundle's `nuget/` folder as a package source (a `nuget.config` beside your solution)
   and reference the packages at the bundle's exact version, `0.6.0-pr.129.<run>`.
2. Generate C# for your `.proto` files with `protoc` and `protoc-gen-fletcher`, which
   `Eiva.Fletcher.Protoc` brings (`--fletcher_opt=csharp`, and `csharp_gateway` for the gateway
   client).
3. To use the gateway, unpack `gateway/` for your platform and run it; connect
   `Eiva.Fletcher.GatewayClient` to it.

Needed on the machine: .NET 8 or .NET 10; on Linux glibc 2.34 and the GCC 13 C++ runtime or newer
(Ubuntu 24.04, Debian 13, RHEL 10 or newer); on Windows the latest Visual C++ 2015-2022
redistributable (x64), 14.40 or newer.

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
