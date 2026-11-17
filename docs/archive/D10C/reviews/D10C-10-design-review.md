# D10C-10 design review — cycle 1 of 2

**Verdict: APPROVE-WITH-DEBT(3). No BLOCKER, no STOP-AND-ASK.** Reviewed: design `plans/D10C-10-handover.md` and brief
`plans/D10C-10-brief.md` at `a6d64ea`.
**Proof of read:** I read `plans/RIX-rulings.md` top to bottom: **667 lines, 52 `## ` entries**. The last is `## 2026-11-16 — D10C-7:
the switch and the deletion are one stage` (:660). The rulings that bind here are :383 (1), :408 (2), :512, :551 (3) and :636.

## The seven questions
1. **`BatchStatus.Failure` (`FletcherException?`).** d10 names it: §4 :472 ("the failure in `BatchStatus`"), L9's settled text
   (:295-297) and D26. The ladder leaves its shape open (:40). `FletcherException` already carries exactly {Status, Origin,
   Message} (`FletcherException.cs:86-127`), and `Handover = 4` exists (`:77`). D-BIND-19 (1), "one numbered error type per side",
   favours reusing it. The member is null unless `fl_batch.failure` is non-NULL. Sound.
2. **The split.** `ToException(…, in FlError …)` disposes nothing. `ThrowIfFailed` keeps the only throw and its `finally`
   dispose. The thunk never calls it, and the shim disposes after the callback (`batches_server.cpp:169`). L8 holds.
   L3 holds too: status and origin are cast as given. D-BIND-54 only picks a type from the given pair, and the no-message
   fallback is rule 1 as it stands (`Errors.cs:83-90`). Neither is a re-check.
3. **CI staging.** This is not a d9 edit. `conan build` writes only `build/` and `CMakeUserPresets.json`, both gitignored
   (`.gitignore:1,3`). The recipe admits `variant=server` with `run_tests=True` (`conanfile.py:77-83, 105-145`), and
   `skip_test` stops `cmake.test()`. The target already exists (`tests/CMakeLists.txt:217-252`), and rung 11 itself says
   "no d9 edit". P2 is the right stop.
   The exactly-one check is sound. Zero or several files fail by name. The real server shim cannot pass in its place:
   it has no `fl_test_*` export, and the test asserts `fired == 1`. Only `Fletcher.Tests` consumes `FletcherServerShim`, so no
   second file lands under the server name. P3 has a precedent: CopyOracle imports its test-only symbols by name from the
   module it stages (`CopyOracleTests.cs:340-346`).
4. **The message assertion.** Vacuity-safe. It fails on a null `Failure`, and on a message that never crossed, which arrives as
   the fallback. A schema-less setup cannot pass either: dropped rows close no row-limit window (d10 :415), so `fired`
   stays 0. It does not prove the message verbatim (a string C# invented would pass). The byte-exact leg drives the same
   thunk and carries that half. Accepted under audit (c).
5. **The internal L8 leg.** It is lawful as a leg, and as the only proof of this clause. By construction the clause has no
   outside observable: `fl_error_dispose` zeroes the struct (`binding.cpp:270-274`), so the shim's later dispose is a no-op.
   The promise the user sees, that the failure reaches the handler, is proven from outside, and the brief's Risks disclose
   the rest. The leg reaches less than the design says it does: DEBT-1.
6. **Decision 1.** See below.
7. **Size.** My pricing:
   - `SubscriberArrow.cs`: about +25
   - `Errors.cs`: about +12
   - the new csproj: about +40 (CopyOracle's is 48)
   - `ci.dotnet.yml`: about +20 (two jobs)
   - `Fletcher.slnx`: +1

   That is about +100, inside the declared +70..+120 and far below 250. Tests are not counted under the round's rule. The
   ladder estimated 40-75; the excess is the new project and the second job. The figure is honest. New public surface: 1.
   The design is 124 lines by `wc -l` and 115 non-blank, so it meets the 120 cap only by the non-blank count. Your count is
   the PM's call; I raise no finding.

## Decision 1: open, (a) is right, one framing fix
- **Open.** No ruling covers C#'s own take-over failure. :551 (3) and :512 are worded for the shim's failure ("the failure's
  status, origin `Handover` (4)").
  - L9 does not settle it: its "not lost silently" is met by the rows-lost count, and "absorbed" is defined for handler
    failures only (d10 :89).
  - L2 rules out only the variant of (b) that reuses origin 4.
- **The spec already says (a).** d10 L9's build-state line (:287-288) records it, from D10C-7's residue (a). The question is
  therefore "confirm the spec's text", with a safe default. The `:2061` leg (`AbsorbedCallbackFailures == 0`, `:2109`) and
  the planned `Assert.Null(status.Failure)` pin the answer.
- **Shape.** It complies: a Users line and an Architecture line for each option, a recommendation and a default. The
  recommendation's grounds of :551 (3)'s scope and L9's count hold. "It follows L2" is one-sided: DEBT-2.

## DEBT (also recorded in `plans/reviews/design-debt.md`)
- **DEBT-1 (step 3, the L8 leg).** Design :94 says "disposing a copy is detected by the same predicate". That is false if
  "copy" means a struct copy. `var e = *f; fl_error_dispose(ref e)` frees the buffer and leaves `*f` intact, so the leg stays
  green. The shim then double-frees at `:169`; whether that crashes the external cells depends on the allocator.
  Fix: restate the control as "an in-place dispose turns it red", and name the copy case as outside the leg.
  Optional one-line scan: `SubscriberArrow.cs` references no `fl_error_dispose` (true today).
- **DEBT-2 (PM, in the brief before the owner reads it).** (b)'s Architecture line (brief :35-37) describes only the
  origin-4 variant. The `Exception?` variant in the Background (:40-41) does not strain L2. Its cost is different: the new
  member's public type changes, and handlers must type-test to read status and origin.
  Fix: say so in that line, rewritten in place with no extra lines. Ground the recommendation on :551 (3)'s scope and on
  d10 L9 :287-288, which already records (a).
- **DEBT-3 (step 3, a new leg).** The handled residue (design :54-56) has no test. If its `try` is left out, the window is
  lost silently: the exception escapes `Deliver`, and the thunk's outer catch swallows it (`SA:559-562`).
  Fix: add a leg that feeds a crafted failure with `MessageLen > int.MaxValue`; the `checked` cast at `Errors.cs:92`
  throws deterministically. Assert one delivery: a null batch, rows counted, reason kept, and `Failure == null`.
