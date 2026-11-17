# D10C-5 code review (step 4b, full lane): diff base 3ae1e8d plus the untracked server project and controls

**Verdict: 0 blocking, 4 should-fix, 6 nit.** Nothing produces a silent wrong answer today.

## Should-fix
- **S1 (should-fix, conf. medium-low): the README server relinking route probably fails at `core`.** `README.package.md:285-289` passes `-o "&:variant=server"` to *every* `conan create`. Only `c-abi` (and `gateway`) declare `variant`, and CI passes the option to `c-abi` alone (`ci.dotnet.yml:130-139`). Conan 2 is likely to reject a consumer-scoped option that the recipe does not declare. Nothing runs this snippet (`snippet: none`). Fix: mirror CI, with a plain loop and then one `conan create c-abi -o "&:variant=server"`.
- **S2 (should-fix, conf. high): no control covers the server allow-list.** C3 adds a file under `runtimes/`, but `runtimes/.*` is on the allow-list, so C3 proves the exact-set row and not the allow-list. A broken `grep -vE` (for example, one that matches everything) passes all nine controls. Add C10: put `lib/net8.0/Fletcher.Server.dll` (or `build/x.targets`) in the server package. It must fail and name "allow-list".
- **S3 (should-fix, conf. medium): "only the server package carries the server shim" is checked only under `runtimes/`.** Only the server package has an allow-list. A shim under `contentFiles/`, `build/` or `lib/` in `Eiva.Fletcher`, Model or GatewayClient passes and could reach a base-only app's output folder, where the loader prefers it (d10 L5). The cheap rule: refuse any entry in any package named `(lib)?fletcher-c-abi(-server)?\.(so|dll)` unless it is on its owner's `runtimes/` row.
- **S4 (should-fix, conf. high on the mechanism, low on reach): the no-group parse should be refused at the door.** For a nuspec with no TFM `<group>` (flat `<dependency>` entries), `native_deps` falls back to `("(no group)", "")` and reports "none", so the parse never sees a flat server dependency. Today it is refused only by accident: `read -r tfm have` splits the label into `tfm="(no"` and `have="group) none"`, which never equals anything. If the label is renamed, Model or GatewayClient with a flat server dependency passes silently. Also, `< <(native_deps …)` drops python's exit status, so a crash means zero comparisons. Today the readme and nuspec checks would fail loudly first. Fix: have python print an explicit error when there are no TFM groups or a `<dependency` sits outside one, and capture the output with `deps=$(…)` so `set -e` sees a failure.

## Nits
- `^Apache Arrow$` (`check_third_party_notice.sh:38`) is not tied to arrow's section. It has the same looseness as the nanoarrow rule.
- The server allow-list admits anything under `package/` and `_rels/`. Tighten it to `_rels/\.rels` and `package/services/metadata/core-properties/[^/]+\.psmdcp`.
- The `"$PKG has no lib/ assembly"` check passes for the server only through `_._`. The message is wrong for that package, and the rule is not.
- The placeholder hooks the private target `_GetPackageFiles`. `TargetsForTfmSpecificContentInPackage` is the public extension point and gives `lib/$(TargetFramework)/_._` per inner build without the item batching.
- The csproj is 57 lines with about 25 lines of comment. Each comment carries a reason, but the header (lines 5-14) restates the design and the README; 3 lines would do. This is not a defect.
- The Arrow licence written as a tuple repr (`('Apache-2.0',)`) is **cosmetic for the checks**. Every rule matches the bullet prefix `^  \* arrow `, and the Linux/Windows diff compares two renders from the same recipe. It is a wording defect in a legal notice, though. The fix belongs in the deployer (`", ".join` for a tuple or list) and touches the gateway and protoc notices too, so it is a separate item.

## Checked and sound
- CRLF: Git Bash strips `\r` in `$(…)` (tested). Process substitution gets an explicit `tr -d '\r'`. `member` writes raw bytes.
- The `grep -q` early exit cannot trip pipefail: awk writes its small output once.
- `"${VARIANT[@]}"` when empty under `set -u` is fine on bash 4.4 and later (Git Bash 5, Ubuntu 24.04).
- The per-group comparison is correct, and C9 proves it.
- The ceiling arithmetic is right: 12,848,400 + 4,300,000 = 17,148,400, which rounds up to 18,000,000, in the same decimal MB as `INTEROP_CEILING`.
- No `.snupkg`: `IncludeSymbols=false` overrides `Directory.Build.props:46`, and CD asserts that the file is absent.
- Step order is right: the server notice check runs before Pack, and the controls run after the package check and before Upload.
- Server lockfiles are written on both legs. The stripped server copy is the one tested and packed.
- Each control works on a fresh copy, the source folder is never written, and the ceiling env vars are scrubbed.

## Not verifiable by reading
- CI as a whole.
- The Windows server notice and lockfile, and P4: Arrow's OS-dependent recipe defaults could make the Windows server graph differ. That would fail loudly at the notice diff, which is a STOP-AND-ASK point.
- `check_conan_lockfile.sh … server` under Git Bash on Windows.
- CI's re-confirmation of `SERVER_CEILING`: CI uses its own Windows shim, while the local run used the one from the Conan cache.
- Trusted Publishing for the new package id.
- The README server route (S1).

## RECORD
- The design's Summary and forcing table say "seven controls"; nine landed. The evidence row explains C8 and C9 (the 0b audit). The design text is stale.
- The upload step names in `ci.dotnet.yml:245,374` ("the Linux/Windows shim, its notice and its lockfile") were not pluralised, though the download steps were.
