# BIND-9a — conformance against the acceptance and the rulings

Graded at `8f5b911` by the fresh-context tests-and-acceptance reviewer (T) of the BIND-9a review
(`BIND-9a-codereview.md`), from the tree only; CI results are taken from the records, which the
tree cannot prove. MET, PARTLY, NOT MET.

## The forcing tests (tracker, BIND-9a)

| # | Bullet | Grade | Evidence or gap |
|---|---|---|---|
| F1 | A consumer project using only the unpacked bundle | MET | `run.sh` copies the project out of the checkout; the CI job checks out only the consumer's folder |
| F2 | Generates its C# with the bundled plugin | MET | `PreviewConsumer.csproj`: plugin and include must exist, the three outputs must be written; `options.proto` unused (T10) |
| F3 | Restores from the bundled packages alone | MET | `run.sh`: `<clear/>`, source mapping `Eiva.Fletcher*` to the bundle, `--configfile`, an empty cache, exact versions |
| F4 | Publishes and subscribes natively, the shim loaded from the package | MET, `inprocess` only | `Program.cs` shim-origin check; `fastdds`/`xrce` not run from the package (Q8) |
| F5 | Reaches the bundled gateway through `GatewayClient` | MET | rows and batch routes |
| F6 | On Linux and Windows | MET | `preview-consumer` matrix `ubuntu-24.04`, `windows-2022`, in `pr_gate` |
| F7 | `Interop` exactly the two RID folders, none elsewhere | MET | `check_dotnet_packages.sh` |
| F8 | Shim dependencies only the system and the C/C++ runtime | Linux MET, Windows PARTLY | exact Linux list; Windows heuristic (Q4), empty parse passes (B1) |
| F9 | Probe shim in no package | MET | `check_dotnet_packages.sh` |
| F10 | Both licence files in every package | MET | `check_dotnet_packages.sh` |
| F11 | The notice lists the libraries linked into the shim | MET | the pack job's two-leg diff and named libraries |

## The rulings

| Ruling | Grade | Evidence or gap |
|---|---|---|
| D-BIND-134 | MET; Windows check PARTLY | as F7 to F11; B1, Q4 |
| D-BIND-135 | MET | gateway release steps opened by `bundle`, archives checked |
| D-BIND-136 | MET | plugin binaries and `options.proto` shipped; the plugin's notice incomplete (B4) |
| D-BIND-137 | MET | no `bundle`-gated step tags, releases or publishes |
| D-BIND-138 | MET, with a caveat | a full re-run reuses the version (Q6) |
| D-BIND-139 | PARTLY | no root notice (Q7); re-run version (Q6); fork branch name (B2) |
| D-BIND-140 | PARTLY | route stated but not reproducible as written (B3); nuspec names another commit (Q1) |
| D-BIND-141 | MET as stated; not guarded | floors not asserted (Q5); VC++ wording (Q10) |
| D-BIND-142 | MET | README packed, filled and checked; claims B5, B6 |
| D-BIND-143 | MET | logic sound; never run failing (T11) |
| D-BIND-144 | PARTLY | unmarked code blocks unchecked (Q3) |

## The rest of the BIND-9a section

One bundle per push, 30 days, named as ruled: MET. dotnet, gateway and protoc on every push to the
branch: MET. Nothing released; `Eiva.Fletcher.Protoc` stays in 9b: MET. The README tells testers to
delete bundles and clear caches at `0.6.0-alpha`: MET. Slices 9a-0 to 9a-4 delivered: MET in the
tree.

## After the review's fixes (2026-10-21, before CI)

F8 Windows: MET (exact allow-list, D-BIND-148; an empty parse fails, B1). F4: MET for `inprocess`
and `fastdds` (D-BIND-151). D-BIND-139: MET (B2 fixed; root notice amended to per component, Q7;
re-run version, D-BIND-150). D-BIND-140: MET (the relinking route rehearsed as written, B3; the
nuspec names the PR head, D-BIND-145). D-BIND-141: MET and guarded (D-BIND-149). D-BIND-144: MET
(every block checked or exempt, D-BIND-147). The CI result is recorded in the tracker.
