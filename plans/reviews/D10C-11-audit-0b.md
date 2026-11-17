# D10C-11 step 0b audit (new-test): the preview-bundle consumer in two configurations

**Verdict: CLEAR.** No FORBIDDEN or FORBIDDEN-BY-TOLERANCE arm; no tier-1 disagreement; no scheduled restoration.
**Proof of read:** `plans/RIX-rulings.md` 684 lines, 54 `## ` entries, last "## 2026-11-17 — Fletcher Linking Exception: legal approval; pack it" (:677).
**Scope:** uncommitted diff vs `924adcb`: `Program.cs`, `run.sh`, `minimal.sh` (+ the brief's uncommitted mismatched-argument controls). **Yardstick:** :650 (2) "T46 … re-anchors it from packages"; :575 (1) "one present but unloadable stops the app loudly; no fallback"; :575 (2); :551 (1) "on the edge shim it throws a named NotSupported naming the server package"; d10 L5 "never the edge shim in place of a present server shim that will not load", L6, L7 "Everything but the batched subscribe works on the edge shim", L15.

| Arm | Class | Why |
|---|---|---|
| T46 `server`: `Check("native batch", …)` | LAWFUL | Re-anchored, not deleted (:650 (2)); rows only where the server package is referenced. |
| T46 `base`: `CheckBatchedSubscribeRefused` | LAWFUL | Success is recorded as a failure; requires NotSupported/Seam with `Eiva.Fletcher.Server` in the message (L6, :551 (1), :575 (2)). No rows arm survives in base. |
| Shim check (both modes) + base "no server file in output" | LAWFUL | Collects BOTH file names, requires exactly one, at that mode's `runtimes/<rid>/native/` path. An edge load in server mode fails, so the check does not tolerate a fallback (L5). The base check is L7. 0a conditions (1)-(3) are met. |
| `run.sh` copy + `dotnet add package … --no-restore`, restore from the bundle feed | LAWFUL | Law (b): driven from outside, from packages. The user's own step. The base project is untouched. |
| `minimal.sh` at floor (both configs, `OK` required) | LAWFUL | Each run carries its mode, so the shim check above is enforced inside the container. |
| `minimal.sh` below floor, base arm | LAWFUL | Unchanged. Its exact `3.4.32' not found` grep depends on the loader's reporting order (0a note), and it stays that way. |
| `minimal.sh` below floor, server arm (+ PM's `3.4.3[12]` decision) | LAWFUL, with conditions | It must name `That file is the server build` and must NOT name `That file is the edge build`, which is :575 (1)/L5 from packages. |
| Mismatched/missing-argument controls | LAWFUL | Both directions fail loudly (an uncaught refusal, or a success recorded as a failure plus a wrong file). A missing argument exits 2. |

**The grep decision.** It tolerates the loader's reporting order. It does not weaken L15. Debian 12 stops at the first unmet verneed entry, so its message names the first missing version, not the highest one. The floor's value is proven where L15 says it holds: `check_shim_dependencies.sh` requires the server shim's highest GLIBCXX to EQUAL `shim_floors.env` (`ci.dotnet.yml:189,379`). The "floor sentence" is the constant `PlatformFloor` (`NativeLoader.cs:228`), so it proves nothing about the shim. **Conditions:** (a) keep the `' not found` suffix, so the OS reason is matched and the constant floor sentence cannot satisfy the pattern; (b) use `grep -E`/basic regex, not `-F`, which would read the brackets literally and always fail; (c) the brief's Risks line ("Do not loosen the grep", STOP-AND-ASK) is tier-3 text this decision overrides. Record the reason in the brief/log at close. It is not a tier-1 matter.
**New VALUE subject:** none. Every arm is rung 12 as briefed (L5/L6/L7 from packages, L15 floor for the server build).
**Laws with no test, after this lands:** none in scope. L7's mark can move to `[built]` once CI runs it green (:408 (2)). The 0a condition (4) docs are not in this diff and remain owed.
