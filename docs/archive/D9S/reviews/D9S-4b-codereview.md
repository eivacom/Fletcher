# D9S-4b code review (light lane): 023152b..b09fc64

**Verdict: PASS.** 0 blocking, 0 should-fix, 2 nits.
**Proof of read:** `plans/RIX-rulings.md` has 476 lines. The highest entry, at line 470, is "2026-11-13 — D9S: tighter document caps for the remaining stages".

## Correctness (checked; no finding)
- **Moving the entries out is lawful.** The thunk takes `std::vector<Attachments>` by value. Domain 7 hands it over with `cb_(std::move(d.batch), std::move(d.atts), d.status)` (`subscriber_arrow.cpp:278`) and never reads `d.atts` again. The vector belongs to the thunk's frame, so `std::move(entry)` empties nothing that domain 7 still holds.
- **Lifetime.** `sets` (a deque, so element addresses stay put) and `set_ptrs` are declared before `out` inside the `try`. Both live through `on_batch` and are destroyed in reverse order on every exit, normal or exceptional. A retained blob outlives its set through `BlobOwner::refs`, because `~fl_attachments` only decrements the count.
- **Exception paths.** A throw while the sets are built (bad_alloc) or from `ReasonOf` or the export unwinds before `on_batch` runs. The deque destroys only its fully constructed elements, `ReleaseUnmoved` covers the Arrow structs, and `catch (...)` keeps the throw out of domain 7. Nothing is half delivered.
- **NULL iff `num_rows == 0`.** The list is NULL exactly when domain 7's list is empty. Domain 7's premise holds in the tree: `atts_` grows only on a successful `Append` (`:163`); a malformed row pushes nothing; a failed `Finish` clears the list through `DiscardPending`; a no-decoder window never appends. So no-batch and zero-row windows both give NULL. The shim does not re-check this (L15), as the brief intends.
- **Retain semantics match the row path.** The row path builds `fl_attachments view(atts)` on the stack around `on_delivery` (`binding.cpp:864`). 4b uses the same constructor and the same destruction after the callback, so `fl_blob_retain`/`fl_blob_release` behave the same. Cost: no bytes are copied, only one `BlobOwner` per entry, the same as the row path.
- **Test.** The window placement (row 1 last in window 1, row 2 first in window 2), the distinct sizes and the embedded-zero bytes catch a shifted, swapped, all-empty or NULL answer. The sink copies inside the callback. 4a's no-batch test also gains `!had_attachments`, which closes audit note 2.

## Conformance
It matches the brief.
- One field is appended after `schema`, typed `const fl_attachments* const*`, with the brief's documentation.
- The thunk change sits inside the existing `try`, before the export, and adds about 11 code lines.
- No count, window, timer, range or alignment check was added (L15). The deque is per call and keeps nothing between windows (BC-9).
- Only `c-abi/`, d9 and the audit changed. No domain 7, domain 2 or `dotnet/` file was touched, and the `fPIC` exception was not used further.
- Edge build (L13): `batches_edge.cpp` is untouched and the edge build never writes an `fl_batch`. The struct only grows at its end, which is the additive change the brief names.
- The "attachments: 4b" placeholder is gone from the code, the README, d9 §2, L13, L14, §4 and Q6. Nothing the brief said would go survived.
- Doc marks: every `[built]` flipped for 4b (§2 Server row, L13, L14 attachments clause, §4, Q6) is backed by code and a named test in the tree. I did not re-run the Windows evidence.

## Nits
- n1 (high): `set_ptrs.reserve(attachments.size())` would make the pointer list a single allocation. No correctness effect.
- n2 (medium, pre-existing, not 4b's): if `make_shared` throws partway through `fl_attachments`'s constructor (`handles.hpp:117`), the `BlobOwner`s already allocated leak. OOM only, shared with the row path.

RECORD:
- The register's BC-13 row was not given the "attachments half" that the brief's Numbers list. It still describes only the Arrow batch.
- d9 L14 still cites `h:1061-1066` for `fl_decode_rows`, which is now at `h:1188`. The citation was stale before 4b and was kept in the touched paragraph.
