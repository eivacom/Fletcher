# BIND-9a — code review, all slices

**Subject:** what BIND-9a landed: the native shim in `Eiva.Fletcher.Interop` with its licence
files and checks (9a-0), the per-push preview bundle (9a-1), the package README and the
bundle-only consumer (9a-2), the tester note (9a-3) and the README walkthrough checked against the
consumer (9a-4). Commits `eee0a3e^..8f5b911`.

**Reviewed at:** `8f5b911`. CI: `ci.pr` 39580014921 green 60/60, every side workflow green.

**Outcome: 6 BLOCKERs, 10 questions for a ruling, DEBT, NITs.** BIND-9a does not close until the
blockers are fixed and the questions ruled.

> **CLOSED 2026-10-22, on CI at `133631a` (`ci.pr` 39592797595, 60/60 on its third attempt; the two
> one-off Windows timing failures in unchanged tests are in the tracker's BIND-9a section, one widened,
> one on PR #129's watch list).** Fixed and ruled 2026-10-21: all six blockers fixed and rehearsed
> with mutants; all ten questions ruled (D-BIND-145 to 152, the D-BIND-139 amendment, the VC++
> wording) and implemented. DEBT fixed now: T9 (with Q8), T-N1, M10 (which then caught MSBuild turning `\` into
> `/` in the packed README on Linux), M13, M14 (dead `rm -f`), L12, L15 (a note in the bundle
> README), L17, L18, L19, L20. T11 is accepted as is: the guard's logic was read sound by two
> reviewers and it cannot be exercised failing without moving `main`. Moved to BIND-9b: T10, T12,
> T13 with M6, T14, M7, L14, the Q9 research. Left as NITs: T-N5, T-N6, M11 (shown to work
> locally), M12, the rest of M15, L16.

## How this review was done, and its limit

The method of BIND-4 to BIND-8. Three reviewers with **fresh context** each took one area: (M) the
CI and packaging machinery (workflows, check scripts, MSBuild, recipes and deployer); (L) what ships
and what it claims (the packages, the licence notices, every document a tester reads, checked
against a downloaded bundle, `pr.129.338`); (T) tests and acceptance (graded in
`BIND-9a-conformance.md`). None had seen the session that wrote the work; they read the rulings, the
tracker and the code and ran nothing. Every BLOCKER below was then reproduced against the tree or
the bundle by the session before it was recorded; IDs keep the reviewer's letter (M, L, T), and a
finding raised by several reviewers lists all their IDs.

## BLOCKER

**B1 (M1, T5) — the shim dependency check passes on an empty list.**
`scripts/check_shim_dependencies.sh`: when `objdump -p` yields no `NEEDED` line, or `dumpbin`'s
heading is not the English "following dependencies" (a VS language pack; MSYS not converting
`//dependents`), `deps` is empty, the loop adds nothing to `bad`, and the script prints
"self-contained". D-BIND-134's "checked on every pack" rests on it. Fix: fail on an empty list and
require `libc.so.6` (Linux) or `KERNEL32.dll` (Windows) in it.

**B2 (M2) — a fork's PR from a branch of the same name builds a bundle, and the tester note would
pick it.** `ci.pr.yml` sets `bundle` from `github.head_ref` alone, which carries no repository; the
tester note selects runs by branch name (`gh run list --branch feature/csharp-bindings --limit 1`,
the browser filter `branch:feature/csharp-bindings`). A fork PR from `feature/csharp-bindings` whose
run needs no approval would appear first, and a tester would install its shim and gateway. Fix: set
`bundle` only when `github.event.pull_request.head.repo.full_name == github.repository` and the PR
is #129; have the note tell testers to confirm `pr.129` in the name and the PR's head commit in
`VERSION` (or start from PR #129's checks).

**B3 (L1) — the relinking route's build command fails as written.** `README.package.md` says "build
`c-abi/` as its `README.md` describes"; `c-abi/README.md`'s Building block ends with
`conan create . … -o "&:run_tests=True"`, which requires `fletcher-arrow-bridge`, `arrow` and
`gtest` (`c-abi/conanfile.py:79-80`) that the sequence never creates; and neither README says where
the built file lands (the Conan package's `lib/`, `bin/` on Windows). D-BIND-140 asks for the route
"with the command". Fix: the exact sequence in the package README without `run_tests`, where the
file lands, "replace the file in `runtimes/<rid>/native/`"; correct `c-abi/README.md` and its
"exactly the above" claim.

**B4 (L2) — the plugin compiles in vendored nanoarrow and flatcc, and its notice omits them.**
`protoc/CMakeLists.txt:14,44` builds `protoc/third_party/nanoarrow` (with flatcc) into the plugin;
the bundle's `protoc/THIRD-PARTY-LICENSES.txt` lists only protobuf and zlib. The deployer reads
`licenses/vendored/` only from first-party dependencies, never from the root package, and the
protoc recipe packages none. Fix: the protoc recipe packages `licenses/vendored/nanoarrow/` as
pubsub's does; the deployer also reads the root package's; `ci.protoc` asserts nanoarrow in the
notice, as `ci.dotnet` does for the shim.

**B5 (L3, T-N7) — the README says the shim is refused when it is from another version; only the
ABI is checked.** `README.package.md`: "The shim checks at load that it was built for this version
of the managed packages". `NativeLoader.VerifyAbiVersion` compares the binding ABI (exact minor
before 1.0), not the package version or commit. Fix: say what is checked, and "use a shim built
from the same commit".

**B6 (L4, T-N2) — the README's handler-thread claim is wrong for native batches.**
`README.package.md`: "Handlers run on the provider's or the client's delivery thread", and
"Batches from a gateway work the same way as natively". `SubscriberArrow` runs the handler on the
delivery thread (row limit), a timer thread (timeout) and the unsubscribing thread (closing), not
serialised, completing out of order (`SubscriberArrow.cs:61-71`, D-BIND-64); the gateway batcher is
serialised and in order (`GatewayBatch.cs`). Fix: state both rules, and that step 4's plain `List`
is safe only for `inprocess`, which delivers before `Publish` returns.

## QUESTION (for a ruling; one at a time, each with a recommendation)

- **Q1 (M3, L5) — the nuspec and the symbols name the merge commit.** **RULED 2026-10-21 as recommended (D-BIND-145).** Confirmed in `pr.129.338`:
  every nuspec has `<repository … commit="4353dcc…"/>` (the `refs/pull/129/merge` commit) and
  SourceLink points there, while the README and `VERSION` name `5e41f62`. The trees are equal
  (D-BIND-143), but the merge commit disappears at the next push. *Recommended:* when `bundle` is
  set, the jobs that build bundle content check out `github.event.pull_request.head.sha` (D-BIND-143
  already proves it is the same tree), and `check_dotnet_packages.sh` compares the nuspec's commit.
- **Q2 (L9) — an Arrow subscriber that subscribes before the topic's publisher exists drops every
  row.** **RULED 2026-10-21 as recommended (D-BIND-146): a defect until shown otherwise.** Found writing 9a-4 (over `inprocess`: a `null` batch, all rows dropped), yet
  `SubscriberArrow.cs:487` says "a subscriber that subscribed first still decodes", every C# test
  declares the topic first, and across processes nobody can order the two. *Recommended:* treat it
  as a defect until shown otherwise: reproduce it in `Fletcher.Tests` (and against C++ and
  `fastdds`), fix it if it is one; until then the README scopes the rule to one process.
- **Q3 (T3, M9) — the snippet checker ignores a code block without a marker, and passes with
  none.** **RULED 2026-10-21 (D-BIND-147): every fenced block is a snippet or marked `<!-- snippet: none -->`, at least one must match, and the syntax is described in a maintainers' HTML comment at the top of `README.package.md`, in the consumer's README and in the checker's header.** The relinking `bash` block is unmarked, so "every code block is code a test runs"
  (D-BIND-144) is not what is checked. *Recommended:* every fenced block is a snippet or carries an
  explicit exemption marker (`<!-- snippet: none -->`), and at least one snippet must match.
- **Q4 (T4, T6, M15) — the Windows dependency rule is a heuristic.** **RULED 2026-10-21 as recommended (D-BIND-148).** Any DLL in the runner's
  System32 passes, and `msvcp140*`/`vcruntime140*` admit the debug CRT (`msvcp140d.dll`), which
  `windows-2022` has and tester machines do not. *Recommended:* an exact allow-list as on Linux (the
  measured list of D-BIND-134 plus `api-ms-win-crt-*`).
- **Q5 (T7, M4, L13) — the floors are printed, never asserted.** **RULED 2026-10-21 (D-BIND-149): the measured highest `GLIBC_` and `GLIBCXX_` must EQUAL the stated floors (a lower one fails too, so the README is updated with it).** A build-image change to GCC 14
  would make the README's `GLIBCXX_3.4.32` wrong with every check green. *Recommended:* assert now:
  the measured maxima equal the stated floors.
- **Q6 (T2, M8, L21) — "Re-run all jobs" makes a second build under the same version.** **RULED 2026-10-21 as recommended (D-BIND-150): `0.6.0-pr.129.<run>.<attempt>` when the attempt is above 1. Implementation note: the version is computed ONCE, as a `detect-changes` output (also M13), because `run_attempt` also rises on "re-run failed jobs", where the jobs that passed keep attempt 1's outputs; one output keeps every job on one version in both kinds of re-run.**
  `run_number` survives a re-run; `run_attempt` does not. *Recommended:* append the attempt when it
  is above 1 (`0.6.0-pr.129.<run>.<attempt>`).
- **Q7 (T1, M5, L10) — the bundle root has no `THIRD-PARTY-LICENSES.txt`** **RULED 2026-10-21 as recommended: D-BIND-139 and the tracker amended to "per component"; no artifact change.**, though D-BIND-139
  lists one; each component carries its own and `README-preview.md` says so. *Recommended:* amend
  the ruling's text to "per component"; no artifact change.
- **Q8 (T8) — only `inprocess` runs from the packaged shim.** **RULED 2026-10-21 as recommended (D-BIND-151): a single-process `fastdds` route in the consumer, and every route checks that nothing extra arrives (T9).** The statically linked eProsima chain
  and anything it loads at run time are never exercised from the package, while the README
  advertises `fastdds`. *Recommended:* add a single-process `fastdds` route to the consumer now.
- **Q9 (L6, L7, L8) — three licence-text gaps, repository-wide rather than 9a's.** **RULED 2026-10-21 as recommended (D-BIND-152): `COPYING` (GPL-3.0) shipped beside `LICENSE` everywhere now; Fast DDS's `thirdparty/` and the Apache `NOTICE` question verified as a BIND-9b item before the first release.** Fast DDS's own
  vendored third-party code may need texts its ConanCenter package does not carry (unverified);
  Apache-2.0 `NOTICE` files are not carried (nanoarrow upstream has one); the LGPL-3.0 text is
  shipped without the GPL-3.0 text it incorporates. *Recommended:* add `COPYING` (GPL-3.0) beside
  `LICENSE` everywhere now (cheap); verify Fast DDS's `thirdparty/` and the `NOTICE` question as a
  9b item before the first release.
- **Q10 (L11) — the VC++ floor is understated.** **RULED 2026-10-21 as recommended (wording, in every README that names it).** Built with MSVC 19.4x and importing
  `msvcp140_atomic_wait.dll`, the binaries need a redistributable at least as new as the toolset.
  *Recommended:* "the latest Visual C++ 2015-2022 redistributable (x64), 14.40 or newer".

## DEBT

- **T9** — three routes stop reading at the expected count, so a duplicate delivery goes unseen;
  wait briefly and require nothing more. Fix with Q8.
- **T10** — the bundled `fletcher/options.proto` is never used by the consumer. 9b.
- **T11** — the D-BIND-143 tree guard has never run in its failing direction; rehearse once or
  record it as accepted. Close.
- **T12** — the relinking route and `README-preview.md`'s `protoc` command are not executed by any
  test (the command also lacks the well-known-types include, L18). 9b (relinking), close (L18).
- **T13, M6** — the plugin's single notice is read off the Linux graph only and its content is not
  asserted; diff the Windows graph as the shim's legs are. With B4.
- **T14** — the floors are not tested on a minimal machine (no VS, glibc 2.38). 9b, with D-BIND-41.
- **M7** — after the merge, a change in `pubsub/` or `xrcedds-pubsub-provider/` that loses a
  vendored licence reruns neither the gateway nor the dotnet lane. 9b.
- **M10** — the packed README is compared with its template only by hand; script it (also covers
  the version, T-N4). Close.
- **L12** — `dotnet/README.md` is stale (GatewayClient "still empty", "makes no AOT claim",
  "Next: BIND-5…") and testers now reach it. Close.
- **L14** — corresponding-source reproducibility depends on ConanCenter's state (no lockfile). 9b,
  with the release.

## NIT

T-N1 (.NET 8 not asserted), T-N3 (zero snippets pass, with Q3), T-N4 (package version not checked,
with M10), T-N5 (gateway exit status and batch errors ignored), T-N6 (`FreePort` race, can only
fail), M11 (the unrestored format check: shown to work locally with a whitespace mutant, exit 2;
record it), M12 (`bundle` also uploads Conan `.tgz` the bundle never uses), M13 (the version
expression written three times; one inlined SHA), M14 (dead `rm -f` in `ci.protoc`; `cd.protoc`
now downloads the licence artifact unused), M15 rest (redundant `! -name`, `setup-dotnet@v5` vs
`@v6`, Model not checked for native dependencies), L15 (notice titles say `0.5.0-alpha`), L16
(GatewayClient's and Model's notice speaks of a binary beside it), L17 (the topic rule also needs
`returns (google.protobuf.Empty)`, no server stream, the message in the same file), L18
(`README-preview.md`'s `protoc` command lacks `-I <tools>`, omits `csharp_model_only`, does not say
C++ headers are written too), L19 (`./gateway` on Linux), L20 (`README-preview.md` does not say to
reference `0.6.0-alpha`; "Model as needed" though generated code always needs it).

## Checked and found sound (all three reviewers)

Package content exactly as D-BIND-13 and D-BIND-134 require, measured in the bundle; the shim's
floors measured in the bundle match the README today (`GLIBC_2.38`, `GLIBCXX_3.4.32`); the shim's
and the gateway's notices cover the libraries linked into them, vendored ones included; version
ranking as D-BIND-138 says, and testers are told to clear caches; the restore isolation of the
consumer (`--configfile`, `<clear/>`, source mapping, an empty cache, the project copied out of the
checkout); the verdict path (non-empty expected rows, timeouts that fail, exit codes propagated);
the shim-origin check; `github.head_ref` reaching the shell only through `env`; `pr_gate` wiring;
artifact names agreeing between producers and consumers; version and commit wiring into packages,
README, `VERSION` and artifact name (Q1 the exception); the MSBuild targets (evaluation vs execution
time, `[MSBuild]::Escape`, both-or-neither shims); the deployer's vendored entries; no `bundle`-gated
step publishes, tags or releases.
