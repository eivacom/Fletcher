# D9S-6 design review, cycle 1: NEEDS-REWORK (2 BLOCKERs; B2 is STOP-AND-ASK). DEBT 3.

**Proof of read:** `plans/RIX-rulings.md`, top to bottom: 503 numbered lines (502 by `wc -l`), highest entry l.493, "2026-11-14 — D9S-5b:
a refused batched subscribe never delivers". l.478 (one closing delivery) honoured; l.383 (D30) and l.445 (gap ends) except for B1.

## B1. The hand-over is not "everything or nothing": code before its `try` can throw
Design l.21 moves `sets` (`std::deque`) and `set_ptrs` (`std::vector`) out of the `try` so they outlive `on_batch`; today they are
inside it (`batches_server.cpp:112-113`). libstdc++'s `std::deque` default constructor allocates (map and first node); MSVC debug
builds allocate iterator proxies. A `bad_alloc` there, the cause the design calls realistic (l.49), leaves the thunk with no
delivery. Domain 7 swallows it (d7 L10) and the window is lost uncounted: the hazard L18 exists to close. Corner case 5 is false,
and the forcing test cannot see it, because its fault point is inside the `try`.
*Acceptable fix (forbid, not handle; one line):* state "nothing before the hand-over's `try` can throw" and build both containers
inside it (e.g. a `std::optional<…>` declared before, emplaced inside). Cite that line in corner case 5.

## B2. Decision 1 sets a principle against a detail but is framed as a trade-off; its default amends the principle. STOP-AND-ASK
Tier 1 does not settle it: D30 and D-D9S-4 say nothing of a message. Every d9 text points to (b): §1, the principle ("turning
every C++ failure into a status plus message"); L2 (a failure *is* "a seam status number plus its message"); L18's reading note;
§4's planned `failure`; rung 8. Only L18's operational sentence (silent on a message) and the ladder's no-signature-change line
(tier 2) point to (a). The brief concedes (a) "strains d9 §1". So (a) is an exception to a principle and needs the owner's word.
It cannot be what happens when the owner says nothing. The framing also leans towards (a):
- "the batcher's answer unchanged" is false: under (a) the shim replaces a batch that was built;
- neither the round purpose (one batcher; (b) adds none) nor L2 (no "one channel" clause) supports (a);
- (b)'s real L2 tension, "a value the caller owns" against "borrowed for the call" (L11, `h:1011`), is not named;
- under (a), Files-to-touch omits the §1 amendment.

*Acceptable fix (brief; design l.12 and §5):* ask "keep d9 §1 for this failure (b), or rule an exception (a), as a decision of
record with §1 amended". Default to (b): it amends only the ladder line, which the PM records. Ground any (a) recommendation in
what supports it: d7 L7/L10 parity, D30's text, no new surface. Give branch (b) its test assertion (status, message, origin 4),
its ownership line and its files, cutting elsewhere so the design stays within 150 lines.

## Pressure tests
1. **D-D9S-4: no path to a shipped artefact; no STOP-AND-ASK.** The target exists only under `FLETCHER_BUILD_TESTS`
   (`CMakeLists.txt:262-266`). `batches_server.cpp` is a direct source of the shipped target, not in `fletcher-c-abi-impl`
   (`:48-51`, `:136`), so the fault target compiles its own copy with its own definitions (P1 holds). The `package()` patterns
   (`conanfile.py:164-175`) match the fault shim under neither stem. No `install()` or `add_compile_definitions` exists in
   `c-abi/`. Only the fault shim exports `fl_test_*`. `exports_sources` carries the hook's *source* in the recipe, as it does
   `probe/*`; no binary. What remains is DEBT-1.
2. **Thunk.** One call site after the `try`, and the catch writes a fresh struct, so any throw inside the `try` still yields
   exactly one delivery. The loss, `rows_dropped + num_rows`, is L18's wording: a no-batch or zero-row window adds 0, because
   domain 7 already counted it, and the total is conserved. The one hole is B1.
   **`abort()` fits L1.** L1 bans an exception crossing at an entry point; an abort is not an exception, the thunk is not an entry
   point, and L1 already calls termination "not an exception crossing". It is reachable only through a `Reason` outside the three
   enumerators, which the build forbids. Every alternative breaks a law: a forged reason, a 0 (`h:1004`), or a silent loss (L18).
   So it is not a forbidden failure mode; DEBT-2 records it.
3. **Decision 1's shape is present** (user and architecture lines per option, a recommendation, a default). Substance: B2.
4. **Cap:** 150 lines by `wc -l`, visibly sized to it: l.52 and l.98 each join two items on one line. Branch (a) lost nothing
   that matters. Branch (b) is a four-line sketch, which matters only through B2.

## Check 7b
**Shipped production +34/−16, net +18, against 250: fits, about 7%.** The body agrees (`batches_server.cpp:63-140` and the
`binding.h` comment). **The fault shim does not count:** it is test code in `c-abi/tests/`, configured only with tests on and
never shipped. Even counted it fits: net +93; +203 with tests; +228 with (b). No breach, no split. For close: the fault shim's
+75 is 2.5× rung 8's estimate of 15-30.

## DEBT (in `design-debt.md`, under D9S-6)
- **DEBT-1:** (iv) checks exports, not package contents. Since the package ID ignores `run_tests`, a server package built with
  tests on has the release ID, and only the name patterns keep the fault shim out.
- **DEBT-2:** record the `abort()` in d9 L1, beside the designed termination.
- **DEBT-3:** the hook counts hand-overs per process: one batched subscription per test cell (unlike `test_batches.cpp:715`).
