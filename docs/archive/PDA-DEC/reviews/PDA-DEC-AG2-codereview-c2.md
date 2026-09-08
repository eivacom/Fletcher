# PDA-DEC-AG2 — code review, cycle 2 (scoped re-review)

Diff base `102e56d`, working tree, uncommitted. Increment reviewed: the bulk
decode builder and everything routed through it (+398 / −36).

Not re-reviewed, per scope: the sealed container's `Set` / `Find` / `KeyAt`
semantics, the wire-order change, and the `PubSubError` NUL escape — cycle 1
covered them and this increment does not change them except where the builder
now feeds them.

Counts: **0 blocking · 1 should-fix · 3 nits · 2 RECORD**.
Cycle-1 B1 is **discharged**. Cycle-1 S1 is **discharged structurally**.
Cycle-1 S2 remains open by PM decision, reasoned and accepted; not re-filed.

Built and ran on this box (msvc-194 x86_64 Release):
`core_tests` **44/44** — `EnvelopeTest.ADescendingAttachmentKeyOrderDoesNotMakeDecodeQuadratic`
in **43 ms**; `conformance_seam_vocabulary` 13/13, `conformance_registry` 22/22,
`conformance_copy_accounting` 11/11, `conformance_fastdds` 40/40.
`fastdds-pubsub-provider/build` still cannot be reconfigured (stale Conan cache
entry for `fletcher-pubsub`, unchanged from cycle 1), so
`test_fletcher_sample_pub_sub_type.cpp` was read rather than run; the code path it
asserts (`ParseEnvelopeBody` + builder) was exercised directly instead.

---

## The blocking finding: verified fixed, independently measured

I rebuilt the measurement from scratch against the current header (standalone
probe, MSVC 19.4 `/O2`, `Attachments` alone, empty `Blob`s, warm reused
container). Everything below is my own reading, not the PM's:

| k | `Set` desc | `Set` asc | builder desc | builder asc |
|---|---|---|---|---|
| 1 000 | 1.48 ms | 0.041 ms | 0.084 ms | 0.012 ms |
| 7 000 | 67.9 ms | 0.348 ms | 0.732 ms | 0.086 ms |
| 20 000 | 536 ms | 1.02 ms | 2.03 ms | 0.244 ms |
| 200 000 | **58 641 ms** | 13.1 ms | **23.6 ms** | 2.49 ms |

The 200 000-key claim (58 745 ms → 38 ms) reproduces: 58.6 s through `Set`,
23.6 ms through the builder for the build alone, 43 ms for the whole decode
including buffer construction. The ascending path is also genuinely cheaper than
`Set` (2.5 ms vs 13.1 ms at k=200 000), so the conforming case did not pay for
the pathological one. Entry counts identical on every row.

**The red is a real red, not a hang.** `core/build/tests/core_tests[1]_tests-Release.cmake`
carries `set_tests_properties(EnvelopeTest.ADescendingAttachmentKeyOrderDoesNotMakeDecodeQuadratic
… TIMEOUT 30)`, and `ctest -V` prints `Test timeout computed to be: 30` for that
id. 58.6 s measured against a 30 s cap: the regression reddens as a ctest
Timeout, as claimed.

**No crafted order defeats the fast path.** Probed at k=200 000: ascending with
only the final pair swapped → 24.1 ms and a correctly sorted result (the flag
falls the moment order breaks, including at the very end); all-200 000-duplicates
of one key → 22.1 ms, `size()==1`; uniform random shuffle → 44.9 ms. Worst case
observed is the sort path, which is what it should be.

**Edges are correct.** k=0 → `size()==0`, no compare, no sort. k=1 → one entry,
no compare (the `!entries.empty()` guard). Equal keys set `ascending_ = false`
(`!(back < key)` is true for equality), so the fast path is never taken over a
duplicate-bearing run. `stable_sort` + keep-last-of-run gives last-wins: probed
`m,a,m,m,z` → `m` resolves to the third `m`. Empty key first still takes the fast
path and sorts first, per A2.

**The unreachability claim holds.** A grep over the tree finds exactly three
`AttachmentsWireBuilder` construction sites — `envelope.hpp:171`,
`envelope_codec.hpp:98`, `legacy_fletcher_topic_type.hpp:119` — and no remaining
`memchr`, no `insert_or_assign`, and no `Attachments::Set` call anywhere that
takes wire bytes (the only non-test `Set` outside `core` is
`xrcedds-pubsub-provider/test_package/src/example.cpp`, a local encoder). The
XRCE receive path reaches wire bytes only through `DeserializeEnvelope`. The NUL
refusal is real and acted on: probed, `Append` returns `false` and appends
nothing; `ParseEnvelopeBody` and the legacy decoder turn that into `return false`
(destructor then empties the container), `DeserializeEnvelope` into
`std::invalid_argument`. `PubSubError` derives from `std::runtime_error` and
`std::invalid_argument` from `std::logic_error`, so
`SeamVocabulary.AnAttachmentKeyThatWouldTruncateIsRefused` leg 2's two catch
clauses are genuinely disjoint and cannot pass by accident.

**`Finish()` cannot throw into a transport frame.** MSVC's `stable_sort` uses
`_Optimistic_temporary_buffer`, whose constructor is `noexcept` and degrades to
stack space rather than throwing `bad_alloc`; `Entry`'s move is noexcept
(`std::string` and `shared_ptr` moves both are); the comparator does not throw.
So routing the decoders through the builder did not put a new throw where the
three `memchr` guards used to keep one out.

**No reserve on a wire-supplied count.** The specific hazard flagged in the brief
is absent: `Append` is a plain `push_back`, and neither decoder reserves against
`attach_count`. See nit N1 for the residual allocation difference.

---

## SHOULD-FIX

### S3 — `Append` after `Finish()` is representable, and it produces exactly the silent wrong answer the class was built to forbid

*Confidence: high that the hole exists; low that it is reachable today. Forbidding
direction, one line.*

`core/include/fletcher/core/types.hpp:346-357`. The builder spends a destructor
and a `finished_` flag to forbid one half of this hazard — an *unfinished* builder
empties the container, because, in its own words, "`Find`'s binary search would
silently miss on an unsorted vector, which is the one way this could have become a
wrong answer instead of a dropped sample". The mirror case is not forbidden:
`Append` does not consult `finished_`, so an append after `Finish()` leaves the
vector unsorted **and** leaves `finished_` true, so the destructor does not clear
it either. The container is then handed on looking valid, `Find` silently misses,
and positional enumeration re-encodes a non-ascending sequence onto the wire —
a wrong answer, not a dropped sample, and the one outcome the design says it is
avoiding.

No current decoder does this: all three call `Finish()` as the last statement of
the builder's scope. That is why it is should-fix and not blocking. But it is held
by call-site discipline in three independently written parsers, which is the same
shape as cycle-1 S1 — the finding this increment just fixed structurally.

**Acceptable fix (one line):** refuse it at the door —
`if (finished_) return false;` at the top of `Append`, or `assert(!finished_)`
beside it if the false return would be read as "malformed key". Either makes the
state unrepresentable rather than merely unvisited.

*(Same class, same fix, so noted here rather than separately: copying an
`Attachments` while a builder is live also yields a sorted-looking, unsorted
value. Not reachable today — no decoder copies the out-param inside the loop.)*

---

## Nits

- **N1** Peak allocation on the *sort* path is modestly above the `unordered_map`'s,
  not equal to it: `vector` growth holds old+new at a doubling boundary and
  `stable_sort` takes a further ≤ n/2 elements. Same O(k), no wire-count reserve,
  so no new hazard class — but AG2-DEBT-11's "the memory exposure is exactly what
  shipped before" is approximate rather than exact.
- **N2** `legacy_fletcher_topic_type.hpp:120` has no `att_count` sanity bound
  analogous to `envelope_codec.hpp`'s `(total - pos) / 8`; it is bounded only by
  the loop's own buffer-exhaustion returns. Benchmark-only, decodes bytes it
  produced.
- **N3** `copy_accounting.cpp:167` calls `deep.Set(std::string(delivered.KeyAt(i)), …)`,
  which throws `PubSubError` on a NUL-bearing key. Unreachable — every key in that
  harness is a literal — but it is a throw inside a delivery callback.

---

## RECORD (paperwork the tree contradicts — non-blocking, PM corrects in place)

- `plans/reviews/design-debt.md`, AG2-C2-DEBT-1 discharge: "the benchmark decoder
  got the same `memchr` guard (two lines)" — the tree now routes that decoder
  through `AttachmentsWireBuilder` and there is no `memchr` guard anywhere.
- `fastdds-pubsub-provider/src/internal/fletcher_sample_pub_sub_type.hpp:172-176`:
  "ParseEnvelopeBody clears `decoded_attachments` as its first act … would leave
  **the map** holding blobs" — it clears after the row-length checks, via the
  builder's constructor, and it is not a map.

All three cycle-1 RECORD items are corrected in the tree; none carry forward.
