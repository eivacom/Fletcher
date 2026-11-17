# D10C-11 design review — cycle 1 of 2

**Verdict: APPROVE-WITH-DEBT(4).** No BLOCKERs. No STOP-AND-ASK: no ruling, locked decision or d10 law is contradicted.
**Proof of read:** `plans/RIX-rulings.md` read top to bottom: 684 lines, 54 `## ` entries; the last is "## 2026-11-17 — Fletcher
Linking Exception: legal approval; pack it" (:677).
**Verified in the tree:** `PreviewConsumer.csproj` is unmodified, and its `csproj-references` region (:26-35) equals README :88-98;
`run.sh:51,96-98,103-112`; `Program.cs:49-55,76-83,87-113,163-179`; `minimal.sh:85` still holds the fixed `3.4.32` string;
`ci.package-consumer.yml:163,184,191-193` runs both scripts with no YAML change needed; `assemble_preview_bundle.sh:34-35`
requires `Eiva.Fletcher.Server`; `NativeLoader.cs:155-166` (the "That file is the server/edge build" sentence) and `:226-233`.

## The PM's five questions
1. **Purpose, and audit 0a's conditions: met, with one partial (DEBT-1).** On the server configuration, batches arrive and the
   only shim loaded is `fletcher-c-abi-server` from `runtimes/<rid>/native/`. C#'s batcher has been gone since D10C-7, so those
   batches can only come from domain 7. That is :566's first half. On the base configuration, per-row publish and subscribe run
   on the edge shim, the output holds no `*fletcher-c-abi-server*` file, and `SubscribeBatched` is refused with
   NotSupported/Seam naming `Eiva.Fletcher.Server`. That is :566's second half, L6 and L7.
   (1) T46 is re-anchored, not deleted (:650 (2)). There is no rows arm in base. The arm does not assert the shim's own text,
   which 0a (1) asks for under :575 (2) (DEBT-1).
   (2) The shim check is per configuration, and exactly one shim may be loaded. Accepting either file is unrepresentable.
   (3) `minimal.sh` covers both builds at the floor. Below the floor, the server build must be refused for its own file and
   must not fall back. The edit at :85 is owed, and the design says so.
   (4) The docs are listed. README :225-235 and :257-288 already name the server build, so narrowing the list to :145-154 is
   right. There are two small gaps (DEBT-4).
2. **Two configurations: sound.** The base project is byte-equal to the README by construction: it is not edited, and the copy
   is made in the work folder, which `check_readme_snippets.py` never reads. A copy plus `dotnet add package` gives the
   README's project plus exactly the line a user adds. The grep at :97-98 stops a no-op add, and the stop condition for an SDK
   that refuses `[v]` keeps that grep. A wrong argument fails in both directions; I traced both, and in neither is the
   failure silent.
3. **The floor pattern only tolerates reporting order. It does not weaken L15.** The proof of the floor is
   `check_shim_dependencies.sh:52-60`. It requires the server shim's *highest* GLIBCXX to EQUAL `shim_floors.env`, and it runs
   on the server shim at `ci.dotnet.yml:189` and `:379`. `minimal.sh` never proved the floor's value. The dynamic loader
   reports the first unmet version it reaches, so even the base arm's exact `3.4.32` cannot catch a 3.4.33 or a 3.4.31 floor.
   `minimal.sh` proves D-BIND-172's behaviour instead: the app is refused with the OS's reason and the floor sentence, for its
   own file, with no fallback. `3.4.3[12]` keeps all of that. The design's tripwire, however, points at the wrong site
   (DEBT-2).
4. **Local evidence is acceptable for the close under :408 (2), as qualified evidence.** :408 (2) makes a green CI run the
   condition for *lifting* a mark, not for closing a stage. It is not, as design :85 puts it, "owed at the close". What ran is
   narrower than the round's standing qualifier, though: Linux only and local; the net8.0 legs of `run.sh` rolled forward to
   .NET 10; `minimal.sh` ran through a wrapper the design says it cannot use (:91); Windows was not run. The mark must say
   exactly that (DEBT-3). PM note: this is the round's last item, so the round closes with its purpose proven only locally.
   That stays an owner-visible line at round close, not a stage blocker.
5. **Size: honest.** My count of the authored diff by region is about +125 / −28. With the docs (about +12 / −8), the stage is
   about net +105, roughly 172 lines touched, against a 250 ceiling. That is 3× the ladder's 10-40, but the growth is
   plumbing for both configurations and both arms. There is no new scope, type or surface. The design is about 111 lines
   (cap 150) and adds 0 public surface. Files-to-delete is real (none; T46 re-anchored). There is no coexistence bridge.

## DEBT (appended to `design-debt.md`)
- **DEBT-1 (step 3):** the base arm should also require the shim's text: `&& e.Message.Contains("fletcher-c-abi-server")`.
  0a (1) and :575 (2) say "shim text plus one sentence". The exact shape is pinned at unit level
  (`SubscriberArrowTests.cs:190-199`), so asserting presence is enough here.
- **DEBT-2 (design premise, step 3):** the STOP-AND-ASK at design :65-66 can never fire. `PlatformFloor(rid)`
  (`NativeLoader.cs:226`) is keyed by RID, so both builds share one floor sentence. Re-point the premise at
  `check_shim_dependencies.sh:59` on the server shim. In the `-E` pattern, escape the dots: `GLIBCXX_3\.4\.3[12]' not found`.
- **DEBT-3 (close, PM):** the evidence qualifier should state exactly what ran (question 4). Re-run `minimal.sh`'s server
  below-floor arm after the :85 edit: logs from before the edit do not cover the PM's pattern. Record the wrapper's command.
  Drop "owed at the close" (design :85): the qualifier stays until a green run.
- **DEBT-4 (docs):** d10 L5 and L6 should cite T46 and `minimal.sh` as their from-packages tests, not only L7's mark. Audit 0a
  lists all three under "Laws with no test". The README's "add `Eiva.Fletcher.Server`" step must be prose or a
  `snippet: none` block: no source region holds the server reference, which exists only at `run.sh:96` (D-BIND-147).
