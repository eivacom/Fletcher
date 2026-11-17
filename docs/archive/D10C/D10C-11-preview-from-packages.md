# D10C-11 — The purpose, proven from the packages (rung 12; T46 re-anchored)

*Ejected from the light lane: 152 touched lines (+126 / −26) in 3 files, against 60. This records the design as
`plans/D10C-11-brief.md` states it and as authored (uncommitted, `integration-tests/preview-bundle-consumer`).*

## Summary
The preview-bundle consumer is built from the packed packages in two configurations. **base** references
`Eiva.Fletcher` only. It must publish, subscribe per row and decode on the edge shim, have no server file in its
output, and get the named NotSupported naming `Eiva.Fletcher.Server` from `SubscribeBatched` (d10 L6, L7).
**server** is the same project plus `Eiva.Fletcher.Server`. It must get its batches on the server shim, and the
edge shim must not be loaded (L5, L15). Together these close the red window declared on T46 (ruling :650 (2)).

## Design
- **One project, two configurations (copy plus `dotnet add package`).** `run.sh:51` copies `PreviewConsumer` to
  `PreviewConsumerServer`. `run.sh:96-98` adds `Eiva.Fletcher.Server` at `[$VERSION]` with `--no-restore`, then
  greps the copy's csproj to confirm the reference is there. The copy gets the one line a user adds, and the base
  project stays byte-equal to the README's `csproj-references` snippet. `run.sh:103-112` runs base, then server,
  then the gRPC consumer, for each of net8.0 and net10.0. Rejected alternative: a conditional reference in the
  csproj with a compile-time define. It would also have worked, but the project under test would then no longer
  be the README's project.
- **The configuration is told, not inferred** (`Program.cs:49-55`): the second argument is `base` or `server`.
  Inferring it from what loaded would make the shim check tautological. A wrong argument fails in both
  directions (see Corner cases), so it cannot hide a defect.
- **T46 re-anchored** (audit 0a (1)): `Program.cs:76-83`.
  - server: `Check("native batch", NativeBatch(...))`, as before.
  - base: `CheckBatchedSubscribeRefused` (`:160-181`) requires `FletcherException` with status `NotSupported`,
    origin `Seam`, and a message containing `Eiva.Fletcher.Server`. A success is a failure, and so is any other
    exception (reported with its type, status and origin).
  - The `native-batch` snippet region is unchanged. The README snippet check (D-BIND-142) still holds, and the
    server configuration now runs that snippet's code.
- **Shim check per configuration** (audit 0a (2)), `Program.cs:87-113`. It collects every loaded module named as
  *either* shim and requires exactly one, at `runtimes/<rid>/native/<this configuration's file>`. In base, no
  file matching `*fletcher-c-abi-server*` may exist anywhere under the app's folder.
- **`minimal.sh`** (audit 0a (3)): `run_in` takes the project folder and the configuration.
  - Ubuntu 24.04 (`noble`), .NET 8 and 10: both configurations must print `OK`.
  - Debian 12 (`bookworm-slim`), below the floor: the base build keeps today's three greps.
  - The server build must fail, with the floor sentence, `found its native shim`, and `That file is the server
    build ('fletcher-c-abi-server')`. It must **not** print `That file is the edge build`. The edge shim has the
    same floor, so Debian 12 refuses it too: a fallback there could not run and would show only as a refusal
    naming the edge file. The at-floor exactly-one check is what proves no fallback where it could run.
- **Below-floor grep (PM decision, recorded in the log).** The server shim's measured floor is `GLIBCXX_3.4.32`,
  the same as the edge shim's (d10 L15, D10C-5). The OS, however, names the first unmet version it hits, which
  is `GLIBCXX_3.4.31` for the server shim on Debian 12. So the server arm requires:
  - the floor sentence, `glibc 2.34 and the GCC 13 C++ runtime`, which names `GLIBCXX_3.4.32`;
  - the OS reason, matched as a regex (`grep -E`): `GLIBCXX_3\.4\.3[12]' not found` (dots escaped, `' not found` kept).

  The floor itself is not loosened. The base arm keeps its exact `3.4.32` grep. Applied in `minimal.sh`'s server
  arm, with a comment giving the reason (design review DEBT-2, audit 0b conditions a and b).
- **No CI YAML edit.** `ci.package-consumer.yml` already runs `run.sh` on ubuntu-24.04 and windows-2022, and
  `minimal.sh` on Linux. Both configurations ride those steps.

## Corner cases forbidden
- **Either shim file accepted** (tolerance of a forbidden fallback): unrepresentable. Each configuration names its
  one file, and a second loaded shim fails the check.
- **A rows-expected arm in base** (forbidden by L6): absent. The base arm can only pass on the named refusal.
- **Argument and build mismatched**: refused in both directions.
  - `base` on a server build fails: the server file is in the output, and the batched subscribe succeeds.
  - `server` on a base build fails: the edge shim is loaded, and the refusal is raised.
  - A missing or unknown argument is refused at the door (usage, exit 2).
- **Fallback below the floor**: the server arm fails if the refusal names the edge file.
- **The add-package step silently doing nothing**: refused at the door by the grep at `run.sh:97`.
- Handled residue: none.

## Premises and stop conditions
- The server shim's floor equals the edge shim's (L15, measured in D10C-5). *Corrected:* the OS's reason names
  3.4.31, not 3.4.32, hence the PM's pattern. The old tripwire (a differing floor sentence) could never fire:
  `PlatformFloor(rid)` is keyed by RID, so both builds print one sentence. **STOP-AND-ASK** now fires at
  `check_shim_dependencies.sh:59` on the server shim (highest GLIBCXX_ != `shim_floors.env`): a D-BIND-149 finding.
- `dotnet add package --no-restore` accepts an exact `[v]` range offline. If an SDK refuses it, insert the same
  line into the copy with `sed`, and keep the grep check.
- The loader's refusal text (`NativeLoader.cs:157-166`) is the stable observable. The test greps for its sentences
  and does not call the loader (ground rules).

## Forcing-test mapping
| Test | Green because | Red before, for the right reason |
|---|---|---|
| T46 base (`native batch` refused) | the rung-7 edge refusal (D10C-7) plus the arm above | old T46: the uncaught NotSupported killed the process (declared red, :650 (2)) |
| T46 server (`native batch` rows) | D10C-5's package plus D10C-4's loader plus D10C-7's switch | no server configuration existed |
| Shim check, both configurations | `NativeLoader` prefers a present server shim (L5) | the edge-only check failed the server configuration (rehearsal control) |
| `minimal.sh`, both builds | same as above; the floor is unchanged | the server arm was absent |

**Green on arrival:** everything except the old T46. D10C-4, -5 and -7 built the behaviour, so this stage writes
tests and harness only, with no production change. The rows arms, the gateway routes, fastdds and gRPC were
already green. The red evidence is the old T46 and two mismatched-argument controls, rehearsed once and logged,
not committed.
**Evidence so far:** the bundle was **assembled locally** (`scripts/assemble_preview_bundle.sh` over locally
packed packages) because CI has not run on this branch. A green CI run is what lifts the marks (ruling :408 (2)); it does not gate the close, and the qualifier says what ran.

## Risks / Unknowns
- Size: ejected from the light lane. The overrun is the two-configuration plumbing in `minimal.sh` and the
  diagnostic messages, not new scope. It is still far below the 250-line ceiling.
- Local runs: `run.sh` runs in `fletcher-build:latest` (.NET 10 SDK only), so it needs `DOTNET_ROLL_FORWARD=Major`
  and `FLETCHER_PREVIEW_ALLOW_ROLL_FORWARD=1`. `minimal.sh` needs a Linux shell with Docker, not Git Bash. CI's
  Linux runner is its run of record.
- No coexistence window. Nothing in this rung is scheduled for later deletion.

## Files-to-touch
- `integration-tests/preview-bundle-consumer/dotnet/PreviewConsumer/Program.cs`, `run.sh`, `minimal.sh`: authored
  (+126 / −26), then DEBT-1 (base arm also requires the shim's own text) and the PM's floor pattern.
- Docs, still owed (audit 0a (4)):
  - `dotnet/README.package.md:145-154`: batches need `Eiva.Fletcher.Server`, otherwise the named error; handler
    calls come one at a time, in window order; rows before the schema are counted, then assembled (L13,
    :551 (4), :258 (3)).
  - `dotnet/PREVIEW-TESTERS.md:10`: add the server package.
  - `integration-tests/preview-bundle-consumer/README.md`: the two configurations, the argument, and the server
    run in `minimal.sh`.
  - `scripts/preview/README-preview.md:11,33-35` (fix cycle 1, compliance F1; the bundle's own README, shipped in
    every bundle): lists `Eiva.Fletcher.Server` and its two server lockfiles, and the reference guidance adds it
    beside `Eiva.Fletcher` for batched Arrow subscriptions.
  - `docs/domains/d10-csharp-binding.md:254`: L7 becomes `[built]` (D10C-11), and its CI qualifier lifts only on
    a green run.

## Files-to-delete
None. T46 is re-anchored, not deleted (audit 0a (1)). Its old base-only rows arm is replaced, not kept. Nothing
else is retired: this rung adds proof over behaviour that D10C-4, -5 and -7 already built.
