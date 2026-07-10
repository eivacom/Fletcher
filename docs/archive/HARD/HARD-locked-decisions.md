# HARD — Locked Decisions

Firm choices for the runtime & pubsub hardening round. The architect,
architecture reviewer, implementer, and compliance reviewer must honor these; a
proposed deviation is a **stop-and-ask**. Full rationale in
[docs/runtime-hardening-spec.md](../docs/runtime-hardening-spec.md).

1. **Wire format is byte-identical (H-INV-1).** Every fix changes ownership,
   error signalling, or code shape — never the encode→decode bytes. Existing
   round-trip / byte-identity assertions must stay green. HARD-1's
   FixedSizeBinary fix changes only whether the decoded scalar *owns* its buffer,
   not the bytes.

2. **Recoverable errors throw; they never abort (H-INV-2).** Replace
   `.ValueOrDie()` and discarded status codes with a thrown exception.
   `std::invalid_argument` for bad/untrusted input (the house type — matches
   `row_reader.hpp:38` and the existing `codec.cpp` throw sites);
   `std::runtime_error` for internal-invariant failure. No `abort()`/`assert`
   on a recoverable path.

3. **No exception escapes a DDS/XRCE callback or the session-pump thread
   (H-INV-3).** `#60`'s fix logs/captures `what()` then returns `false` — it does
   **not** rethrow out of `serialize`. Any provider fix that adds a throw must
   prove it cannot propagate across a middleware callback or the run-loop.

4. **The `#63` fix is precondition-first, lock-second.** The primary contract is
   a documented "no concurrent API calls in flight during destruction"
   precondition; a lock alone cannot cure use-*during*-destruction UB. A
   `lock_guard(mu)` in the destructor is added as defence-in-depth **only if it
   is shown deadlock-free** against the callback-outside-lock discipline. It must
   not introduce a lock-ordering inversion with `Publish`/listener paths.

5. **Public API signatures are unchanged except additive `[[nodiscard]]`
   (H-INV-4).** No new required parameters, no changed return types, no removed
   overloads. `#56` adds annotations only. A fix that needs to report a cause
   adds an *internal* diagnostic, not a signature change.

6. **HARD-7 is behaviour-preserving and output byte-identical.** Consolidating
   `JoinSegments`/`AppendFixed`/`BitfieldBytes` must not change any emitted or
   wire output. The **`BitfieldBytes` `int64_t` overflow-safe intent on the codec
   path is preserved** — do not silently narrow the codec's parameter to `size_t`
   when collapsing onto the `core` definition. Prefer deleting copies and
   including the existing source over introducing a new one.

7. **Runtime + pubsub only; the generator is untouched (H-INV-5).** No edit to
   `protoc/` or generated `.fletcher.*` output. The 25 generator-emitted
   `.ValueOrDie()` sites (the rest of #53), #55, and #59 are the **GEN** round.
   Editing the generator, or regenerating any committed fixture, is a
   stop-and-ask.

8. **`OwnedSchema` stays in pubsub this round.** `#54` fixes
   `OwnedSchema::DeepCopy` **in place** at
   `pubsub/include/fletcher/pubsub/owned_schema.hpp`. Moving the header out of
   pubsub is issue #21, a deferred architecture round — do not move the file or
   change its include path here.

9. **Every behavioural fix ships with a red-first negative/malformed-input
   test.** Consistent with the robustness-plan discipline: the forcing test must
   fail before the fix for the right reason (dangling read, `abort()`, silent
   empty, UAF, or missing diagnostic) and pass after. HARD-5 (docs) and the
   annotation half of HARD-6 are the only non-negative-test items; HARD-6's
   annotations are proven by a compile-fails-on-discard check.

10. **`#62` residual is a narrow point-fix, not a redesign.** HARD-4 fixes only
    the re-entrant-`Unsubscribe` use-after-free (copy the needed `TopicState`
    fields before invoking the callback / do not hold a `TopicState&` across the
    callback). It does **not** move XRCE callback dispatch outside the lock — the
    reported deadlock is already resolved (recursive mutex + single-thread pump),
    and that larger change is explicitly out of scope.

11. **Scope is the closed/actionable issue set only.** In: #52, #53 (runtime
    half), #54, #56, #57, #58, #60, #63, #65, and the #62 residual. Out: #55,
    #59, #75, #21, and #53's generated half. Adding an out-of-scope issue to this
    round is a stop-and-ask.
