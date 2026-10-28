# BIND-9b — code review, all slices

**Subject:** what BIND-9b landed: the rulings (9b-0), packaging hardening (9b-1), the codec's null
refusal (9b-2), NativeAOT on Windows and over `fastdds` (9b-3), the C#-native cross-language rows
(9b-4), `Eiva.Fletcher.Protoc` (9b-5), `cd.dotnet` and the feed (9b-6), the licence research and
the 9a review's remaining debts (9b-7). Commits `653893b..b23f472`.

**Reviewed at:** `b23f472`. CI: `ci.pr` 40526778017 green 61/61 at `d52dd42`, every side workflow
green; the later commits change `plans/` only.

**Outcome: 4 BLOCKERs (all shipped text), 7 questions for a ruling, DEBT, NITs.** BIND-9b does not
close until the blockers are fixed and the questions ruled.

## How this review was done, and its limit

The method of BIND-4 to BIND-9a. Three reviewers with **fresh context** each took one area: (M) the
CI and packaging machinery (workflows, check scripts, MSBuild, recipes); (L) what ships and what it
claims (the packages, the notices, every document a user reads); (T) tests and acceptance (graded
in `BIND-9b-conformance.md`). None had seen the session that wrote the work; they read the rulings,
the tracker and the code and ran nothing. Out of scope by D-BIND-174: the `0.6.0-alpha` bump and the
preview retirement, which land last; the reviewers were asked to flag what would break when they do.
Every BLOCKER below and the claims M1, M3 and M4 were then checked against the tree by the session
before they were recorded (M1: `gh repo view` reports the repository PUBLIC). IDs keep the
reviewer's letter.

## BLOCKER

**B1 (L1) — the `Eiva.Fletcher.Protoc` README's snippet fails as written.**
`protoc/nuget/README.md:46-52`: the `Exec` runs `"$(Protoc)"`, which nothing defines (the package's
props set `FletcherProtocPlugin` and `FletcherProtoInclude` only; `README.package.md:105-106` sets
`<Protoc>` itself); a comment stands where the `chmod` should be; no `MakeDir` creates the
`--fletcher_out` folder, which protoc requires to exist. Pasted, the build runs `"" --plugin=...`.
No snippet check covers this README.

**B2 (L2, T6) — "Windows NativeAOT is not yet exercised", in every package.** `README.package.md:280-281`
and `dotnet/README.md:101-102`; `ci.dotnet.yml:359` publishes and runs `Fletcher.AotSmoke -r win-x64`
for both frameworks since 9b-3 (`2420e11`).

**B3 (L3) — the relinking route's CMake floor.** `README.package.md:253` says "CMake 3.15 or newer";
`xrcedds-pubsub-provider/CMakeLists.txt:4` requires 3.20, so the route fails at its fourth
`conan create` on 3.15 to 3.19.

**B4 (L4) — the npm README names the licence files the wrong way round.** `protoc/npm/README.md:96`:
"`COPYING` (the LGPL), `LICENSE`"; `COPYING` is the GNU GPL v3, `LICENSE` the LGPL. It ships in the
npm tarball.

## QUESTION

**Q1 (L5, T11) — does the LGPL analysis cover the release, and code that ends up inside the
consumer's own binary?** D-BIND-140 is worded "for the preview" and reasons about the native shim
only. The four managed packages are LGPL-3.0-or-later too; NativeAOT (and single-file, trimming)
compiles them into the application, which is LGPL-3.0 section 4(d)(0)'s case, not 4(d)(1)'s.
Since D-BIND-158 the Protoc targets compile `options.proto` (LGPL) into the consumer's assembly by
default, and no document states the licence of the generator's output. Options: (a) document the
obligations; (b) a linking/static-compilation exception for the managed assemblies,
`options.proto` and the generated code; (c) relicense the managed tier permissively; (d) state
NativeAOT unsupported for non-LGPL applications. Reviewer L recommends (b), (a) at minimum; T
recommends confirming D-BIND-140 (with D-BIND-173) for the release on the Q9 row.
**Ruled 2026-10-27: (b), D-BIND-175** (`LICENSE-EXCEPTION`, a draft until EIVA legal has
reviewed it; its wiring into the packages gates the bump).

**Q2 (M1) — the self-hosted feed runner and `NUGET_EIVA_API_KEY` are reachable from pull-request
code.** `cd.nuget-feed.yml:9-10` says the runner runs "never pull-request code"; nothing enforces it.
Any workflow can say `runs-on: [self-hosted, eiva-network]`, `ci.pr` runs on `pull_request`, and the
repository is public; the key is a plain repository secret any branch workflow or a `dotnet-v*` tag
on any commit can read. Options: (a) a runner group limited to this repository and workflow, the
key in an environment whose rules allow only `main` and the release tags, `environment:` on the
push job, a tag ruleset, approval for fork PRs; (b) accept, with an admin checklist. Reviewer:
(a), before an admin registers the runner (runbook step 7).
**Ruled 2026-10-28, recast: D-BIND-176.** The packages go to nuget.org (the maintainer's
correction: a public repository on GitHub-hosted runners cannot reach `nuget.eiva.com`), pushed
by Trusted Publishing from a GitHub-hosted job in the environment `nuget-org`; there is no
self-hosted runner and no stored key. The environment admits only the release tags and a tag
ruleset limits who cuts them (runbook step 6).

**Q3 (M2) — `Eiva.Fletcher.Protoc` is released without the consumer gate.** `cd.protoc` publishes
it after `check_protoc_package.sh` (structure only); `cd.dotnet`'s gate tests another build of it;
`cd.nuget-feed.yml:16` ("the packages the release gate tested") is wrong for a protoc tag. Options:
(a) accept for now (the merge-time tags share one commit `ci.pr` tested) and correct the comment;
(b) `cd.protoc` runs the consumer test first; (c) release the package from `cd.dotnet`. Reviewer:
(a) now, (b) later.

**Ruled 2026-10-28: (b) now, D-BIND-177.** nuget.org and npm never re-use a version (D-BIND-176),
so `cd.protoc` builds the dotnet packages and the gateway beside the plugin and passes the same
`ci.package-consumer.yml` gate as `cd.dotnet` before npm, the Release or nuget.org. The wrong
comment went with `cd.nuget-feed.yml`.

**Q4 (L6) — after the retirement no shipped document says where the packages come from.** Until the
feed runner exists they are the `dotnet-v*` Release assets, and `Eiva.Fletcher.Protoc` is on the
separate `protoc-v*` Release; `README-preview.md` and `PREVIEW-TESTERS.md` were the only guides.
Reviewer: a "Getting the packages" section in `dotnet/README.md` and a sentence in
`README.package.md` step 2.

**Ruled 2026-10-28: all three READMEs, D-BIND-178.** The reviewer's section and sentence, with
the answer D-BIND-176 gives (nuget.org, and each tag's Release carries the same files), and one
sentence in `protoc/nuget/README.md`.

**Q5 (T10) — the gate's non-preview path has never run on CI.** `ci.package-consumer.yml:95-104,
141-147` (version from the `Interop` file name, `BUNDLE_PROTOC_VERSION_MUST_MATCH=0`, the 1-day
artifact) is what `cd.dotnet`'s gate takes; every run on this branch takes the preview path. Options:
(A) the runbook names the retirement commit's `package-consumer` as the gate's rehearsal; (B) a
one-off dispatch before then. Reviewers T and M: (A).

**Ruled 2026-10-28: (A), D-BIND-179.** Runbook step 2 names the retirement commit's
`package-consumer` as the first non-preview run, to be confirmed green before the merge, and step
4's dry runs on `main` rehearse both release gates (`cd.dotnet`, and `cd.protoc` since D-BIND-177)
before any tag.

## DEBT

- **M3 — the two dry runs can cancel each other.** `ci.protoc.yml:6-8`:
  `group: ci.protoc-${{ github.ref }}`, `cancel-in-progress: true`; `cd.dotnet` and `cd.protoc`
  both call it, and runbook step 4 dispatches both on `main`. Add `${{ github.workflow }}` to the
  group, or dispatch one after the other.
- **M4 — the runbook does not retire the loose `options.proto` (D-BIND-171).**
  `assemble_preview_bundle.sh:24-25` copies it and line 46 requires it; line 28 defaults the branch to
  `feature/csharp-bindings`; `ci.package-consumer.yml:58-60` sparse-checks `protoc/include/fletcher`.
  `run.sh` and `minimal.sh` never read the loose copy.
- **M5 — release-only checks run first on the real tag.** `cd.protoc.yml:105-118`: the npm licence
  staging and its `npm pack --dry-run` check sit in the push-only `npm-publish` job; the dispatch
  skips them. Move the checks out, keep only `npm publish` gated.
- **T1 — the null check on sliced batches is barely tested.** One native case slices a top-level
  int column; nested slices (`Physical`, `data.Offset + index`) and every managed slice are untested.
  Add a sliced struct and a sliced list to the D-BIND-156 agreement theory.
- **T3 — "same MAJOR.MINOR" is never asserted.** `run.sh:34-40` reads the Protoc version and never
  compares it with `VERSION`.
- **T4 — nothing checks the thread-pool minimum is in effect.** A one-line test per assembly that
  `ThreadPool.GetMinThreads` is at least 32 (D-BIND-170).
- **L7 — text that dangles after the retirement.** `dotnet/README.md:3, 135-136` link
  `PREVIEW-TESTERS.md` and say "Next: BIND-9b"; `README.package.md:270` "(in a preview bundle, in its
  `nuget/` folder)"; `protoc/README.md:241` links the consumer folder (only if renamed).
- **L8 — the gateway README omits `COPYING`.** `gateway/README.md:50` and "Licences in the archive"
  name two files; the archives carry three (`ci.gateway.yml:168,277`).
- **L9 — the relinking route's text.** It is bash (Windows needs Git Bash, unsaid); Visual Studio
  2026 with the 2022 toolset needs `tools.microsoft.msbuild:vs_version=18` (found in the 9b-7
  rehearsal, recorded only in the tracker); Micro-CDR and Micro-XRCE-DDS-Client are fetched by git
  tag at build time, outside the lockfile, so "pins the third-party packages" overstates;
  `fletcher-c-abi/[*, include_prerelease]` deploys the newest in the cache, not necessarily the one
  just built.
- **L10, T5 — the gateway's glibc floor is stated, not guarded.** `gateway/README.md:70` says 2.38;
  no check compares measured with stated, as `shim_floors.env` does for the shim; `minimal.sh` runs
  the gateway on 2.39 only.

## NIT

- **M6** `cd.protoc` checks `LICENSE`/`COPYING` in a folder the checkout already filled
  (`cd.protoc.yml:151-155, 176-198`); download into a subfolder.
- **M7** no top-level `permissions:` in `cd.dotnet`, `cd.protoc`, `cd.nuget-feed` (as in the other
  `cd.*`).
- **M8** the notice's library-list `awk` is copied three times (`ci.dotnet.yml:430`,
  `ci.protoc.yml:336`, `check_third_party_notice.sh:24`).
- **M9** the `ref` and `package-version` inputs of `ci.dotnet`, `ci.protoc`, `ci.gateway` lose
  their callers at the retirement; decide whether they go with it.
- **M10** `ci.action-lint` does not trigger on `dotnet/Directory.Build.props`, which the tag-check
  test parses, so the bump commit will not re-run it.
- **M11** `ci.package-consumer.yml:169` pins `actions/setup-dotnet@v5`; the rest use `@v6`.
- **M12** `cd.dotnet`'s release job is not re-runnable after `gh release create` half-succeeds;
  nothing checks the tagged commit is on `main` (as the other `cd.*`).
- **T2** the native and managed codecs name a nested field differently (`readings[]`,
  `tags{}.value` against `readings.item`, `tags.value`); the agreement theory asserts only the
  shared part; BIND-10 documents one form or they are aligned.
- **T7** D-BIND-154's heading and the tracker's acceptance bullet still say glibc 2.38; D-BIND-169
  moved it to 2.34 without an amendment note on 154.
- **T8** the C# native peer does not hook `Subscriber.HandlerFaulted`, so a comparer exception
  shows as a missing row after about 40 s.
- **T9** untested: `FletcherCompileOptionsProto=false`, `FLETCHERPROTOC001`, the edition-2023
  refusal.
- **T12** the loader-floor guard is satisfied by a `<remarks>` comment as well as `PlatformFloor`;
  `minimal.sh:58` hard-codes the floor text, a fourth place to edit.
- **L-N** `README.package.md:235-236` "every pull request runs a consumer" (only those the filter
  selects); `c-abi/README.md:66-69` predates the lockfile; `protoc/npm/README.md:96` "attached to each
  GitHub Release" holds from the next `protoc-v`; root `README.md:372` points at gnu.org for a GPL text
  the repository now carries; `gateway/README.md:10` says `conan create` is invalid where `ci.gateway`
  runs it; `gateway/README.md:55,63` use a `gateway-v0.1.0-alpha` example tag.

## Checked and sound

The release path for `dotnet-v0.6.0-alpha` and `protoc-v0.6.0-alpha` end to end (tag pattern,
`verify.sh`, asset names, the gate before every attach and push, the dispatch kept away from npm,
the Release and the feed); the lockfile check (both directions, revisions included, fails on
empty); the relinking route against what `ci.dotnet` builds and the release attaches; the licence
files in every package, the npm tarball and both gateway archives; the vendored texts and NOTICE
files and their check on three lanes; the floors stated alike in every document, the loader and
`shim_floors.env`. Tests: the C#-native rows cross processes with topic-scoped lines; AotSmoke's
`fastdds` mode exits 0 only on the expected row off the main thread; `minimal.sh` cannot pass
vacuously; the null refusal is pinned in three codecs.
