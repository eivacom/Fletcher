# D9S-4a — architecture-conformance review (step 4a)

**Verdict: ISSUES (2, 0 blocking).** Diff `5b36a63..e766c3e` against `plans/D9S-4-batches-cross.md` (4a part),
the brief, design review cycles 1–2, design debt D9S-4, the 0b audit, `D9S-locked-decisions.md`, d9 §3–§4,
d7 L4/L5/L9/L10/L12, register BC-8 to BC-14, and ladder rungs 4–8.
**Proof of read:** I read `plans/RIX-rulings.md` top to bottom. It has **459 lines and 34 entries**; the last
heading (l.445) is **"2026-11-13 — D9S-4: one subscriber object; the interim handoff gap accepted"**.
**No tier-1 collision.** Entry l.445 (2), "lost uncounted during D9S-4/5 only", names L18, the law the
pre-round ruling "Report it as lost" (l.383) was ingested as, and limits itself to stages 4–5. So it is an
interim the earlier ruling knew of, not an implicit contradiction. L18 still binds at round close.

## Findings

**F1. The thunk catches its own throw (`try{…}catch(...){}`, `batches_server.cpp:100-123`) and does not throw
into domain 7, as design step 3 and H3 said. This conforms on authority. Non-blocking.**
- D-D9S-4 and d9 L18 say the thunk never throws into domain 7. Ruling l.458 (2) grants an outcome ("lost
  uncounted"), not a mechanism. The outcome matches domain 7's own silent `catch (...) {}`
  (`subscriber_arrow.cpp:276-280`, d7 L10). Design review cycle 1, point 2, called the two equivalent.
- Rung 8 still goes red first, because window k is still lost. The design is tier 3, so the code stands and
  the design text is amended (RECORD).
- **Carry to D9S-6.** The catch also encloses `on_batch`. Replacing it with L18's no-batch delivery, as the
  comment says, would make a callback that throws after a built window get a second delivery. Σrows + Σdropped
  would then exceed rows offered (BC-12; d7 L1 "none twice"). L18 covers steps 1–3 only, and DEBT-1's
  "steps 1–4" has the same flaw.

**F2. The rung-4 forcing test was reshaped after design and after 0b. Non-blocking.**
- **Design table, ladder rung 4 and brief l.7:** a finite timeout of at least 1 s, ending ROW_LIMIT,
  ROW_LIMIT, TIMEOUT.
- **Landed (`test_batches.cpp:283-355`):** `INT64_MAX` (no timeout), ending ROW_LIMIT, ROW_LIMIT, CLOSING via
  `fl_subscriber_destroy`. Lawful by d7 L5 (no timeout: a window closes only when full or at closing) and
  d7 L9 (closing delivers the rest), and it removes the timing premise 0b raised (0b l.29).
- **Lost:** a TIMEOUT batch that is imported and compared cell for cell. TIMEOUT still crosses in the
  truth-table server arm and in the no-batch test.
- 0b's CLEAR audited the old body (l.272). **Action:** amend the design row, brief l.7 and rung 4. The PM
  decides whether 0b appends a cycle.

## Checked and conformant (pressure points named in the brief)

- **BC-9 / L15 / BC-8.** `max_rows` and `timeout_ms` are read only to build `BatchOptions`
  (`batches_server.cpp:129`). The shim has no range check, window, count, timer, clock, queue or thread. A
  grep of `c-abi/src` finds 0 hits for all 16 guard patterns, plus `steady_timer` and `timer`. The guard's
  pattern list is a superset of the design's 7.
- **BC-10.** The door's first statement reads `fl_binding_variant()` (`binding.cpp:886`). CMake picks the
  backend in the same `if` block that sets the constant. The backends only `static_assert`. No production
  `#if` decides anything.
- **Arrow reaches the server build only.** `find_package`, `batches_server.cpp` and the PRIVATE link sit in
  the server branch only. The probe shim and the edge shim get `batches_edge.cpp`, which names no Arrow.
  `fletcher-c-abi-impl` and `handles.hpp` name no domain-7 type (`shared_ptr<void>`). In the recipe,
  `pubsub-arrow` is required under `variant == "server"` only. With tests off, the edge graph is unchanged
  (`arrow/*:shared` is inert without Arrow).
- **The fPIC exception is exactly as ruled (l.440).** Both recipes have `options`, `default_options` (True)
  and a Windows `del` in `config_options()`, identical to `pubsub/conanfile.py:31-53`. Nothing else changes
  in domains 7 or 2. Nothing changes under `dotnet/`, `core/` or `pubsub/`.
- **L14.** Zero-initialised structs, and one guard (`ReleaseUnmoved`) that tests each `release`
  independently, so a partial move is handled. The guard runs before the catch on every exit. No `fl_schema`
  is handed out. A null batch gives `array == schema == NULL` and `num_rows` 0.
- **L13 / B1.** `out_id = id | kBatchedIdTag` (bit 63, file-local, `binding.cpp:115`).
  `fl_subscriber_unsubscribe` is untouched. A tagged id falls through to `Subscriber::Unsubscribe`'s
  unknown-id no-op (`subscriber.cpp:648-658`).
- **D-D9S-1 / L12.** The edge build refuses before reading any argument: OUTs are untouched and nothing is
  allocated or stored. The message names "edge" and `fletcher-c-abi-server`, with `FL_ORIGIN_SEAM`.
- **L11.** `batches_mu` covers only the get-or-create. The thunk captures `(on_batch, ctx)` only.
- **Destruction order.** `batches` is declared after `subscriber`, so it is destroyed first, while the
  provider is alive. `fl_subscriber_destroy` is a plain `delete`, so the order holds.
- **Debt.**
  - DEBT-2: no `default:`, with `/we4062` and `-Werror=switch` on this one file.
  - DEBT-3: the header now says that an arrival which is already ready stays ready.
  - DEBT-4: D-D9S-8 is annotated.
- **Doc marks.**
  - L14 and L15 are marked "**in part**", with rung 5 (unmoved release) and rungs 6/7 named as untested.
  - BC-13 marks the unmoved release untested.
  - Q12: the edge half is closed and the hand-over half stays open for D9S-6.
  - Q6: the attachments half stays open for 4b.
  - The register keeps BC-12 at `[designed-not-built]`.
- **Files-to-touch.** Production, tests and docs match. No Files-to-delete were ordered. The three stale texts
  (`conanfile.py` and `tests/CMakeLists.txt` narrowed to the edge shim, the `FL_ORIGIN_SEAM` comment widened)
  are edited.

## RECORD

- RECORD: design step 3 and H3 still say "throws into domain 7", and ladder rung 8's red line still says
  "without the catch". The landed thunk has an empty catch (F1); amend all three.
- RECORD: ruling l.456 (1) ("the provider difference is registered for domains 3/4") and the design's Docs
  line are not carried out. H1's provider split appears only in d9 Findings (b2), with no register row.
- RECORD: several new `[built]` marks lack the "Windows, by hand; CI and Linux unrun" qualifier (ruling
  l.416 (2)): d9 l.194, l.202, l.241, l.344, l.438; register BC-8, BC-13 and BC-14.
- RECORD: the d9 glossary still says "48 today" (l.88) and §4 says 49. `grep -c '^FL_ABI_EXPORT'` gives 49.
  The "Shim" row (l.81) lists no `pubsub-arrow` for the server build.
- RECORD: the d9 "Server(-tier) variant" row (l.85) still reads "no Arrow yet" in the same cell that marks the
  Arrow link `[built]`.
- RECORD: in register BC-8 and BC-9, the splice moved the shim's description ("it checks nothing…", "it
  forwards each batch once…") so that it reads as describing the owed test.
- RECORD: the brief and design declared 4a production as about +298/−12 (about 236 counted). It landed at
  +381/−10. That is the PM's owner question and not judged here.
