# D10C-11 — Stage Brief (2026-11-17)

**In one sentence:** the round's purpose is proven from the shipped packages. An app that adds the server package gets batches from Fletcher's one batcher on the server shim. An app on the base package alone publishes, subscribes per row and decodes on the small shim, has no server shim in its output, and gets the named "not supported, add Eiva.Fletcher.Server" error when it asks for batches.
**Forcing test:** the preview-bundle consumer (T46, `native batch`), built from the packages in two configurations on Linux and Windows, plus the minimal-machine run on Linux. It closes the declared red window (ruling :650 (2)).

## Interfaces
| Surface | Change | Why |
|---|---|---|
| none public | — | Test harness only. The consumer program takes the configuration it was built as (`base` or `server`) as its second argument. |

## Deleted
Nothing. T46 is **re-anchored, not deleted** (audit 0a (1)): the old base-only "rows expected" arm is replaced by a refusal arm in the base configuration and a rows arm in the server configuration. The shim-file check becomes one check per configuration (0a (2)).

## Corner cases forbidden vs handled
**Forbidden:** a check that accepts either shim file (that would tolerate a forbidden fallback, d10 L5). Each configuration names its one expected file, and exactly one shim module may be loaded. A rows arm in the base configuration (d10 L6). A configuration argument that does not match the build: both directions fail loudly. `base` on a server build fails because the server file is present and batches succeed. `server` on a base build fails because the edge shim was loaded and the refusal was raised. A missing or unknown argument is refused at the door (usage, exit 2). On the server build below the floor, a refusal that names the edge file, which would mean it fell back.
**Handled:** none.

## How
**Shape: one consumer project, two configurations, both driven by `run.sh`; no CI YAML edit.** `run.sh` copies `PreviewConsumer` to a second folder, `PreviewConsumerServer`, and adds `Eiva.Fletcher.Server` to the copy with `dotnet add package --no-restore`, which is literally what a user does. The original project stays byte-equal to the README's `csproj-references` snippet. `run.sh` restores and runs both projects for net8.0 and net10.0, passing `base` or `server`.
- **`Program.cs`:** `server` mode runs `Check("native batch", …)` as today. `base` mode expects `FletcherException` with status `NotSupported`, origin `Seam`, and a message containing `Eiva.Fletcher.Server`. If the call succeeds instead, that is recorded as a failure. The `native-batch` snippet region is unchanged, so the README check still holds, and the server configuration now runs that snippet.
- **Shim check (:71-81):** it collects every loaded module named like either shim and requires exactly one, at `runtimes/<rid>/native/<that mode's file>`. In `base` mode, no `fletcher-c-abi-server` file may exist anywhere in the output (d10 L7).
- **`minimal.sh`:** the Ubuntu 24.04 runs (.NET 8 and 10) require `OK` from both builds. Below the floor (Debian 12), the base build keeps today's three greps. The server build must also print `That file is the server build ('fletcher-c-abi-server')` and must **not** print `That file is the edge build`, which proves there was no fallback.
- **Docs (audit 0a (4)):**
  - `README.package.md:145-154`: batches need `Eiva.Fletcher.Server`, otherwise the named error; handler calls come one at a time, in window order; rows that arrive before the schema are counted as dropped, and assembly starts once the schema arrives.
  - `PREVIEW-TESTERS.md:10`: add the server package.
  - The consumer README: describe the two configurations and the `minimal.sh` server run.
  - d10 L7's mark becomes `[built]` (D10C-11), and its CI qualifier lifts only on a green run (:408 (2)).
- **Controls, rehearsed once and logged, not committed:** the two mismatched-argument runs above must go red.
- **Where it runs:** `ci.package-consumer.yml`'s existing two steps, on ubuntu-24.04 (run.sh, then minimal.sh) and windows-2022 (run.sh).
- **Locally:** run `run.sh` inside `fletcher-build:latest` against a bundle taken from this branch's CI run (`gh run download`, the one-day `ci-package-bundle-*` artifact). The image ships only the .NET 10 SDK, so set `DOTNET_ROLL_FORWARD=Major` and `FLETCHER_PREVIEW_ALLOW_ROLL_FORWARD=1`. `minimal.sh` needs a Linux shell with Docker, such as WSL, not Git Bash (README :78-80), so CI's Linux runner is its run of record.

## Decisions for you
None. Whether to use one project with a switch or two copies is a mechanism choice: I took the copy plus `dotnet add package`, because it keeps the base project equal to the README and mirrors the user's own step.

## Risks accepted / debt carried
- **Premise, STOP-AND-ASK if false:** the server shim has the same Linux floor as the edge shim (d10 L15, measured in D10C-5). If the server build's below-floor log does not name `GLIBCXX_3.4.32`, stop and report it as a D-BIND-149 finding. Do not loosen the grep.
- **Premise:** `dotnet add package --no-restore` writes the reference without network access. If it does not, a `sed` insert of the same line into the copy is the equivalent.
- **Size (said plainly):** the code and scripts fit the light lane at about 50 touched lines in 3 files. The docs that audit 0a (4) orders add about 15 lines in 4 more files. If docs count toward admission, this is about 65 lines in 7 files, which is over. That is the PM's call. I recommend staying light, because the docs are prose corrections only.

## Numbers
Declared net lines: code and scripts about +40 / −12 (`Program.cs` ≈25, `run.sh` ≈10, `minimal.sh` ≈15 touched); docs about +12 / −8 · new public surface: 0 · design cycles used: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did
not predict, fix cycles used>.
