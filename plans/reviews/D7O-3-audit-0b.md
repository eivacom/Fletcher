# D7O-3 — step 0b audit (new-test) — **CLEAR**

**Proof-of-read:** `plans/RIX-rulings.md` read in full: 295 lines, 24 entries, last heading "2026-11-12 — D7O-2: the deadline definition says some windows have none". d7 §1 and §3 (L1-L15) read.
**Scope:** uncommitted diff vs `957a514`, `test_pubsub_arrow.cpp` section "D7O-3 (L15, L8, BC-8)": `MaxRowsLargerThanAnyWindowIsUsedAsGiven` (arms a, b, guard c), `AFailedPreSizeDoesNotMakeTheSchemaUnusable` (DEBT-3), `EveryBatchOptionIsRefusedExactlyWhenOutOfRange`; helpers `Sum`, `WaitForAccounted`, `ExpectXsRunFrom`, `TimedSink`, `SweepCell`.
**Yardstick:** L15 ("refused ... below 1 or ... negative"; "Every value it accepts is used as given"), L8 ("An undecodable schema is permanent"), ruling 2026-11-12 "Fix in D7O" ("a failed pre-size never makes a decodable schema unusable"), L1, L2, L4, L5, L6, L7, L12, BC-3, BC-8.

| Test | Class | Basis |
|---|---|---|
| Rung 4 (a), (b) | LAWFUL | INT64_MAX accepted and delivered, 0 dropped: L15 "used as given"; L8 (schema is decodable). |
| Rung 4 (c) | LAWFUL | Closes at exactly 131 073 = max_rows (L4 "closes at ... `max_rows` rows"); forbids a clamp (L15 "never adjusted"). |
| DEBT-3 cell | LAWFUL | Non-null zero-row `kClosing`, `rows_dropped` 1 = L6 (zero-row batch when a decoder exists), L7, L2 (0+1), and the ruling; BC-3 "decoder presence" now the constructor's verdict. |
| BC-8 sweep | LAWFUL | Oracle is `max_rows < 1 \|\| timeout < 0`, L15 verbatim: 27 refused (21 + 6), 15 accepted incl. timeout 0, 1 ms, clock max, `kNoTimeout`. No cell refuses an in-range value or accepts an out-of-range one. "≥100 years never fires" and the ≥1 ms floor are L5. |
| Helpers | LAWFUL | Accounting (L1/L2) and window order (L12); no subject of their own. |

- **Law (b):** public paths only (`CreateTopic`, public `PublishDirect` for the malformed row, `Subscribe`/`Unsubscribe`); the one internal read, `subscribe_with_options_count`, is the ladder's sanctioned observation. The INT32_MAX schema and the malformed row are both driven externally.
- **Timing:** one-sided. "Nothing" asserted over `Observe()` (200 ms); the floor is a lower bound measured from before `Publish`; upper bounds are the 2 s convention (arm (c) uses a looser 10 s).
- **Memory, green tree:** arm (c) holds about 1 MB (int32 + 1-byte utf8). The DEBT-3 pre-size asks for 65 536 x (2^31 - 1) ≈ 2^47 - 2^16 bytes. No wrap occurs, and the request cannot be met: Windows' commit limit refuses it, and a 47-bit user address space has no free contiguous range that size. The worst case is a vacuous pass, never a large resident allocation. Sinks outlive subscribers in every test.
- **New value subject:** none. Every assertion is rung 4, the ladder's sweep, or DEBT-3 (the ruling's own words).
- **Scheduled restoration:** none. **Tier-1 disagreement:** none.
- **Build-state:** L15's "used as given" for `max_rows` and the BC-8 sweep are `[designed-not-built]` until green. No false green: the asserting tests are red on `957a514` as declared.
