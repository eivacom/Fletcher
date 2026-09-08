# PDA-DEC close — execution-corpus inspection (forward-relevant content sweep)

**Inspector:** close-inspection agent · **Date:** 2026-09-08 · **Read-only:** no file
outside this one was created, edited or moved.

Scope: PDA-DEC plan / progress log / design docs / briefs / reviews, plus the stranded
RBA and GIR artifacts in `plans/`. **Excluded by assignment** (another inspector):
`plans/PDA-DEC-rulings.md`, `plans/PDA-decouple-locked-decisions.md`,
`plans/reviews/design-debt.md`. **Excluded as active working set:** `plans/PDA-ABI-*`,
`plans/BIND-csharp-inherited-constraints.md`, `plans/RIR-rba-onto-ir.md`.

**Inventory correction.** The actual counts differ slightly from the assignment:
**15** PDA-DEC design docs and **15** briefs (items 1, 1H, 2..9, A1, A4, A5, AG1, AG2),
**54** `plans/reviews/PDA-DEC-*` files (the extras over 52 are the AG1 `-c3` pair, the
AG2 `-c2` triple, and the two BIND verification files), **10** `plans/RBA-*.md` (not 13),
23 `plans/reviews/RBA-*`, 4 `plans/GIR-*.md`, 2 `plans/reviews/GIR-*`.

---

## 0. Headline

**The kept corpus is in unusually good shape.** This round systematically pushed its
*"why the mechanism has this shape"* rationale **down into headers, source comments and
component READMEs** as it went, rather than leaving it in the design docs. Repeated
spot-checks confirmed it: A4's total lock order is written out in
`pubsub/src/subscriber.cpp:150-190`; AG1's containment argument is the file header of
`pubsub/include/fletcher/pubsub/delivery_channel.hpp`; 1H's port-ownership mechanism,
its IPv4-only rule, its foreign-beats-ours rule, its `#error` third-state refusal and
the `g_udp_port_ownership_query` indirection are all in
`integration-tests/fastdds-xrce-interop/tests/test_interop.cpp:184-215, 330-420`; 1H's
"duplicated, not shared" reason is in `integration-tests/fastdds-xrce-interop/README.md:48`;
GIR-13's escape-in-the-sink-never-in-the-visitor trap is
`protoc/src/cpp_backend_schema_visitor.cpp:248-252` and its octal-never-hex rule is
`protoc/include/option_metadata.hpp:88-103`; PDA-DEC-2's provenance-over-counting
argument and its rejected alternatives are
`integration-tests/pubsub-conformance/README.md:210-250`; the strict-equality-not-containment
reason is `integration-tests/pubsub-conformance/include/fletcher/conformance/copy_accounting.hpp:171-172`.

So the migration list is **short by merit, not by haste**. Three content items only. The
**stale-kept-doc list is the more valuable half of this report** — five findings, two of
them flatly contradicted by what landed.

---

## 1. MIGRATE actions

Grouped by destination.

### → `gateway/README.md` (§WebSocket protocol) and `docs/wire-format-specification.md` (§Control Messages)

**`MIGRATE: plans/PDA-DEC-A5-topic-name-integrity.md §Wire bytes (the accepted-set table, lines 125-150) + the comment at gateway/src/ws_session.cpp:149-162 → gateway/README.md §WebSocket protocol, cited from the §Control Messages table in docs/wire-format-specification.md:180-195`**

Governs: **what a remote WebSocket client may name a topic, and what changed under it.**
This is the one **user-visible breaking change of the round that no kept doc records.**
PDA-DEC-A5 made §3.5's six refusals total at the seam, and `WsSession::SplitTopic`
**stopped dropping empty pieces** — so for a browser/TS client:

- `"a//b"`, `"/a/b"`, `"a/b/"`, `"//a//b//"` previously all resolved to the segment list
  `{"a","b"}` and silently exchanged rows with `"a/b"`. They now produce an
  `{"type":"error"}` frame. **Existing clients relying on that tidy-up break.**
- a topic string carrying a NUL, or any segment beginning `__`, is now refused;
- a `/`-joined name over **246 bytes** is refused.

Neither `gateway/README.md`'s protocol section (lines 148-175) nor
`docs/wire-format-specification.md`'s control-message table (lines 180-195) — the two
documents a client author actually reads — says any of this. The rule itself is well
stated in frozen spec §3.5 and in `provider.hpp:50-59`, but those are the **C++ seam**
surface; the gateway is the remote surface, and the mapping from "topic string" to
"segment list" happens only there. One short paragraph in the gateway README, cited from
the wire-format spec's control table, closes it. `gateway-client-ts/README.md`'s API
table (lines 72-76) would benefit from the same one-line pointer.

### → `docs/recordbatch-accessor-spec.md` §10 (or `protoc/README.md` §RecordBatch accessors)

**`MIGRATE: plans/GIR-progress-log.md GIR-1 §Carry-forwards (line 19) — the "field literally named row" accessor collision → docs/recordbatch-accessor-spec.md §10 (Out of scope / known limitations)`**

Governs: **a live code-generation defect that hits any current user.** A `.proto` field
literally named `row` collides with the RBA accessor's generated `int64_t row` index
parameter, and the generated accessor **does not compile**. It was worked around
*fixture-only* in GIR-1 (`ServiceRequest.row` → `payload`, wire-neutral) and the emitter
was left read-only under GIR locked decision #3. Verified still live:
`protoc/src/recordbatch_accessor_emitter.cpp` emits `…(int64_t row) const` at :263, :268,
:285, :290, :301, :307 with no name-collision guard.

It is recorded **only** in the progress log being archived. It is **not** in
`docs/recordbatch-accessor-spec.md`, **not** in `plans/RIR-rba-onto-ir.md` (which does
carry the *other* two RBA caps — the depth-2/3 unroll and the scalar-leaf nested-list
guard — so the omission looks like an oversight rather than a decision), and **not** in
`protoc/README.md`, which has a "reserved keys" note for `metadata_from_option` (line 97)
but nothing about reserved *field* names. One line: *"a field named `row` currently
collides with the accessor's row index and fails to compile; rename the field or omit
`--fletcher_opt=accessor`. RIR owns the fix."*

### → `docs/fletcher-options.md` §Limitations

**`MIGRATE: plans/GIR-13-option-metadata-on-ir.md §4 (the ArrowCharView / NUL-truncation bullet, lines 281-289) → docs/fletcher-options.md §Limitations (after line 236)`**

Governs: **what a mapped metadata value can carry.** `ArrowCharView` is `strlen`-based
and the in-process sink uses `key.c_str()`/`value.c_str()`, so a mapped option value
containing a zero byte is **truncated at that byte** — identically on both paths, which is
why it was accepted rather than fixed ("NUL-bearing metadata is not a promised
capability"). `docs/fletcher-options.md` §Value rendering (lines 174-187) enumerates every
rendering rule and §Limitations (221-236) lists five limitations; neither mentions this.
The *mechanism* is well guarded (`EscapeCppStringLiteralTest.EmbeddedNulIsEscapedAsThreeDigitOctal`)
— what is missing is the one user-facing sentence. Note the deliberate contrast with the
seam, which after PDA-DEC-AG2 **escapes** zero bytes rather than truncating (spec §5.1,
§3.2 clause A3): a reader who knows the seam's rule will assume the wrong thing here.

### Not content, but triggered by the sweep: archiving breaks 6 inbound links from KEPT docs

Must be handled in the same change or the kept corpus ships broken links:

| Kept doc | Line | Points at |
|---|---|---|
| `docs/pubsub-interface-spec.md` | 9 | `../plans/PDA-decouple-locked-decisions.md` |
| `docs/pubsub-interface-spec.md` | 10 | `../plans/PDA-decouple-interface.md` |
| `docs/pubsub-interface-spec.md` | 1007 | `../plans/PDA-decouple-progress-log.md` |
| `docs/pubsub-interface-spec.md` | 1024 | `plans/PDA-DEC-rulings.md` (code span, not a link) |
| `docs/recordbatch-accessor-spec.md` | 6 | `../plans/RBA-locked-decisions.md` |
| `docs/recordbatch-accessor-spec.md` | 7 | `../plans/RBA-recordbatch-accessor.md` |

`docs/robustness-plan.md:23` and `:372` refer to `plans/GIR-*` as a **glob in a code
span**, already asserting *"the per-item execution artifacts (`plans/GIR-*`) are
archived"* — see §4, where that assertion is currently false. The
`docs/archive/HARD/**` precedent is the model: the archived files' own cross-links were
rewritten to `docs/archive/HARD/<file>.md` relative form
(`docs/archive/HARD/runtime-hardening-spec.md:9-10`).

---

## 2. STALE findings in the kept corpus

Five. The first three are the class an external reviewer already caught us on.

### `STALE: xrcedds-pubsub-provider/README.md:35 (§Schema discovery) — the 5-second schema poll no longer exists`

> *"When `Subscribe` is called before `CreateTopic` (subscriber-side), it polls the
> `__schema` topic for up to 5 seconds to retrieve the schema."*

**Flatly contradicted by what landed.** PDA-DEC-3 replaced the wait with `SchemaArrival`
and the provider now creates a **persistent companion `__schema` reader** whose samples
resolve the arrival. The code says so in as many words —
`xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp:906-911`: *"route its samples to
OnTopic, which resolves the schema future when a publisher announces the schema. **No
poll, no callback swap, no throw** — so a subscriber can subscribe before any publisher
exists."* No `5000`, no poll loop and no timeout survives anywhere in that component. The
README still describes a bounded blocking poll, which also reads as a contradiction of
frozen spec §7 clause 5 (*"`Subscribe` never blocks"*).

### `STALE: xrcedds-pubsub-provider/README.md:23-25 (§Topic name) — describes the join and nothing else, on the one provider §3.5 rules 2 and 6 exist for`

The whole section is two sentences: segments are joined with `/`, here is an example. It
predates PDA-DEC-A5 entirely and records **none** of what landed:

- the join is the **seam's**, not the provider's discretion (§3.5), and the joined name
  *is* the topic's identity;
- the six `kInvalidArgument` refusals at `internal::RequireSegments`;
- **that this provider is the reason for rule 2** — XRCE hands the name to a `const char*`
  API with no length form (`schema_name.c_str()` at `:920`), so an embedded NUL truncated
  the name on the wire, and PDA-DEC-A5 found the **participant** name truncated the same
  way. *That is a silent-wrong-answer defect this README described the mechanism of and
  never the fix.*
- the **246-byte** cap this provider inherits ("XRCE inherits both, its Agent building the
  Fast DDS entities from the name the client sent" — spec §3.5);
- that the `__` prefix is **reserved** — directly relevant two lines below, where the same
  section documents deriving `<topic>/__schema`.

Compare `fastdds-pubsub-provider/README.md:135-150`, whose equivalent section carries all
of it, names `RequireSegments`, cites the ruling date and names the pinning test
(`Segments.NamesThatWouldTruncateOnTheWireAreRefused`). The two providers' READMEs should
not diverge on a rule that binds both.

### `STALE: gateway/README.md:179 (§Tracked gaps) — per-topic QoS from the gateway CLI shipped in PDA-DEC-6, and the same file contradicts itself`

> *"Per-topic QoS is not configurable from the gateway CLI — the `fastdds` provider uses
> Fletcher's default QoS profile for every topic."*

PDA-DEC-6 landed `--provider-config FILE`, and **line 38 of this same README** already
documents the mechanism: *"a profile named after the `/`-joined topic for a per-topic
override."* The tracked gap was closed by the round and the entry should be deleted. As it
stands the file answers the same question two ways 140 lines apart.

### `STALE: docs/technology-decisions.md:77 (TD-005) — "Subscribe() returns the schema in the SubscriptionResult"`

TD-005's Decision and Rationale paragraphs both describe synchronous schema delivery —
*"The `Subscribe()` method returns the schema in the `SubscriptionResult`"* and
*"subscribers receive it at subscription time."* Since PDA-DEC-3, `SubscriptionResult`
carries a **`SchemaArrival`** (`pubsub/include/fletcher/pubsub/provider.hpp:39-41`) — a
waitable handle with a typed outcome — and the field's *name* is still `schema`, which is
exactly what makes the sentence read as still-true. Note TD-008 (line 125) states the new
model correctly, so the decision log disagrees with itself. The narrowest fix is TD-005
gaining one sentence deferring to §3.4 / TD-008 (TD entries are historical records, so
rewriting the Decision would be wrong).

### `STALE: docs/data-flow-diagrams.md:48-54 — the schema-transport sequence still shows the schema read completing before Subscribe returns`

```
SUB->>PP: Subscribe(segments, callback)
PP->>DDS: Read from companion schema topic
DDS->>PP: Schema IPC bytes
PP->>PP: Deserialize to OwnedSchema
PP->>SUB: SubscriptionResult { schema }
```

The four steps are sequenced *inside* the `Subscribe` call, which contradicts §7 clause 5
(`Subscribe` never blocks) and §3.4 (the arrival is waited on afterwards, with a typed
outcome). `docs/architecture-overview.md:86` and `:164` were both corrected by PDA-DEC-9
and now state the handle model precisely; this diagram was not swept with them.
Minor and adjacent: **line 82**, `DRV->>WG: SubscriptionResult { schema, subId }` —
`SubscriptionResult` has exactly one member and no `subId` (the gateway assigns that
itself).

### Checked and found NOT stale (recorded so the next reader need not re-check)

- **`FastDDSProviderOptions` / `XrceConfig`** — zero occurrences in any kept doc or README
  except as explicit past-tense retirement records (`docs/pubsub-interface-spec.md:654-665`,
  `xrcedds-pubsub-provider/README.md:60`). The retirement is clean.
- **The `unordered_map` → sealed-container wire-order change** — recorded in *four* kept
  places, each correctly: `docs/wire-format-specification.md:151-165` (ASCENDING order in
  the byte layout, plus the `std::hash`-order predecessor, the TS-client exception and the
  zero-byte-key rule), `docs/data-flow-diagrams.md:150-155`,
  `fastdds-pubsub-provider/README.md:529, 535, 536` (with the benchmark numbers marked
  historical rather than deleted) and `benchmarks/README.md:28`. Best-handled change in
  the round.
- **`kReentrantCall` and the refusal on all four methods** — `core/README.md:41`,
  `fastdds-pubsub-provider/README.md:516`, `xrcedds-pubsub-provider/README.md:199`, each
  citing §6 clause 6 and each explaining what that provider paid.
- **The registry** — `docs/architecture-overview.md:15` and `§7.4:271-289`, root
  `README.md:38`, `docs/technology-decisions.md:125-129` (TD-008). All in registry terms.
- **`core/README.md`'s header inventory** — complete, matches `core/include/fletcher/core/`.
- **`core/README.md`'s taxonomy table** — 11 rows, `kReentrantCall = 10` present, and
  machine-compared by `Taxonomy.PublishedNumbersMatchTheEnum`.
- **`docs/protocol-driver-abi-spec.md`** — AG2's ruling-46 amendments landed at `:1-12`,
  `§0:26-31` and `§0.2:88-95`; the withdrawn multi-language-driver licence is gone.

### One borderline, reported not asserted

`pubsub/README.md:11-15` and `:180-186` list the component's headers. **Five headers
landed in `pubsub/include/fletcher/pubsub/` this round** — `provider_registry.hpp`,
`schema_arrival.hpp`, `in_process_provider.hpp`, `delivery_channel.hpp`,
`payload_bound.hpp` (all first added in `6c541e9`, PR #126) — and **none of the five is
listed**, in either the inventory or the "Include the headers" example. The registry is
the round's headline mechanism and lives in this component; its README's opening paragraph
still describes only *"the abstract `PubSubProvider` interface and two concrete client
classes."* Borderline because that README is a build-and-consume document rather than a
reference, and `core/README.md` is the one that carries reference material — but an
inventory that omits five of eleven headers is wrong on its own terms, and one of the
omissions is `ProviderRegistry`.

---

## 3. Execution-only confirmations, per group

Each group below was **read**, not assumed. Where the group's forward-relevant content
went is named, because that is the evidence the archiving is safe.

### PDA-DEC — the 15 design docs · **execution-only, safe to archive**

Read the full Design section of 1, 1H, 2, 3, 4, 5, 7, A1, A4, A5, AG1, AG2 and the
headings plus targeted sections of 6, 8, 9. Every load-bearing rationale was traced to a
kept destination — frozen spec §3-§8 for the vocabulary, taxonomy, selection,
configuration, threading and delivery rules; `integration-tests/pubsub-conformance/README.md`
for the suite architecture, the subject/axis-gate design, the negative controls and every
"what green does NOT prove" limit; the component READMEs for the per-provider costs; and
**the headers and source comments for the mechanism arguments** (see §0 for citations).
Two notes worth recording:

- **AG1's design doc is a superseded intermediate.** It designs the *narrow* rule — refuse
  re-entrancy on `Unsubscribe` only, keep the other three permitted, and add a loopback
  delivery gate with a deferral deque. The owner's later ruling replaced it with the
  uniform rule now in §6 clause 6 (all four methods, every provider). Archiving is
  correct; a future reader who found this doc *outside* the archive could mistake it for
  the contract.
- **The one thing the design docs hold that the kept corpus does not** is the *rejected*
  alternatives with their reasons — A4's atomic-counter-plus-condvar, PDA-DEC-2's
  sanitizer / `LD_PRELOAD` / throughput-proxy alternatives. That is exactly what an
  archive is for. Not a migration: nothing current depends on them, and the surviving
  choice's rationale is already in the code.

### PDA-DEC — the 15 Stage Briefs · **execution-only, safe to archive**

Read `PDA-DEC-9-brief.md` in full and skimmed the rest. Uniform shape: one-sentence
framing, an interface-change table, deletions, forbidden-vs-handled, **numbered decisions
for the owner with a recommendation and a default**, risks accepted, declared line numbers,
and an *"as landed"* footer reconciling declared against actual. They are the owner's
decision instrument for a decision **already taken** — every decision they pose was
answered, and each answer is in the rulings ledger (another inspector's file) and in the
frozen spec text it produced. Pure execution-only by construction, and the strongest case
in the corpus.

### PDA-DEC — the 54 review files · **execution-only, safe to archive**

Read `RBA-design-review-codex-cycle3.md` fully as the shape-representative, plus targeted
sections of the PDA-DEC design reviews, and traced the two verification files
(`PDA-DEC-9-bind-stopask-verification.md`, `PDA-DEC-PR126-bind-pass2-verification.md`)
against their outcomes. Findings are BLOCKER/DEBT/NIT against a design or an
implementation at a point in time; every one either landed as a change (and is therefore
in the code, the spec or a README) or was booked as debt (and is therefore in
`design-debt.md`, which the other inspector holds). One recurring pattern worth naming:
several reviews' most valuable output was a **corrected fact** — 1H's ~876 ms measurement
refuted as an instrument artifact and re-measured at 28-89 ms; the `/proc/net/udp6` read
that made Linux stricter than Windows; AG2's "no in-tree message holds a NUL" shown false.
In every case the **corrected** fact is in the kept corpus and only the correction history
is here. Safe.

### PDA-DEC — the plan/tracker (`plans/PDA-decouple-interface.md`) · **execution-only, safe to archive**

All 15 rows 🟢. Its two genuinely load-bearing sections are the **DoD handoff table** and
the **six verified conditions** — both superseded by frozen spec **§12.2**, which carries
the same six rows with more evidence and the honest `mechanical` / `by-reading` /
`by-construction` labels. Its user stories are restated as spec §§3-8. Its sequencing
notes and PM rulings are historical. One caveat: it is the target of
`docs/pubsub-interface-spec.md:10` — see §1's link table.

### PDA-DEC — the progress log (`plans/PDA-decouple-progress-log.md`, 1,453 lines) · **execution-only, safe to archive**

Read the structure and sampled entries. Per-item: forcing test, design path, step-2
verdict, what landed, files touched, reviews, verification, commit, carry-forwards. It is
cited **from the frozen spec** twice — §7.2 (`:1007`) for the Fast DDS data-sharing
defect's evidence and §12.2 for the taxonomy-mutation record — so it is *referenced*
forward even though it is not *normative* forward. Both citations survive as archive
citations; see §1's link table. No carry-forward in it is undischarged: the round's open
questions were routed to PDA-ABI-7 (data-sharing), to PDA-ABI (re-permitting re-entry,
zero-copy receive) and to §12.3's inherited-blind-spot list, all in the frozen spec.

### RBA — 10 plan files + 23 review files · **execution-only, safe to archive**

Read `RBA-progress-log.md` in full, `RBA-locked-decisions.md` in full, the plan's Goal and
design sections, and `RBA-design-review-codex-cycle3.md`. **The round closed clean:**
RBA-7's log entry ends *"Carry-forwards / stop-and-asks: none — round complete"*, and every
earlier carry-forward is traceable to discharge — the C++/Rust `StructArray` windowing
asymmetry (RBA-2 → honoured in RBA-5/6 and **corrected in spec §7 and in D-RBA-7** under a
user-authorised stop-and-ask), the C++-`nullptr` ≡ Rust-empty-map metadata equivalence
(RBA-3 → asserted explicitly in the RBA-7 capstone), the two-package import fixture
(RBA-5 → landed RBA-6a). `RBA-locked-decisions.md` names
`docs/recordbatch-accessor-spec.md` as *"the **oracle** — wins on any contradiction"* and
itself only a digest, and that spec's §0-§10 carries every D-RBA-1..10 rationale in full.
The only RBA content not in the kept corpus is the `row`-named-field defect, which came in
via **GIR**'s log — migrated in §1. Two caveats: (a) `docs/archive/RBA/` does not exist and
must be created; (b) the spec's status line links to two of these files — see §1's table.

### GIR — 4 plan files + 2 review files · **execution-only, safe to archive** (see §4 for the duplicate question)

Read `GIR-progress-log.md` in full, `GIR-locked-decisions.md` in full,
`GIR-generator-ir-rewrite.md`'s Goal, and `GIR-13`'s Design §4/§5 plus its headings. The
GIR-1..GIR-11 end-state was **already migrated** into `docs/robustness-plan.md` at the
2026-07-11 close, which says so explicitly at `:23` and `:365-374`. GIR-13's content lives
in `docs/fletcher-options.md` §113-236 (the authoritative spec, which GIR-13 itself names
as *"unchanged by this item"*), `protoc/README.md:90-97`, and the code comments cited in
§0. Two residues, both migrated in §1: the `row`-named-field defect, and the NUL
truncation limitation. One further small staleness, recorded here rather than in §2
because the sweep itself resolves half of it: **`docs/robustness-plan.md:365-372` says the
`plans/GIR-*` artifacts "are archived" — which is currently false for these four files, and
becomes true when the sweep lands**; and its `:16` *"Delivered as 11 rebased items
(GIR-1..GIR-11)"* record does not mention that **GIR-13** landed later, on a different
branch, after that consolidation was written. Worth one clause so the round record is not
misread as complete at 11.

---

## 4. GIR: duplicates, or genuinely unarchived?

**Genuinely unarchived. They are moves, not deletions.** Verified by direct listing, not
by inference.

`docs/archive/GIR/` holds **11 design docs** — `GIR-1` through `GIR-11`, no gaps, no 12,
no 13 — plus a `reviews/` subdirectory holding **23 review files**, also `GIR-1`..`GIR-11`
only (`GIR-1` has three: codereview, conformance and a codereview-fix-verification).
That is the 12 top-level entries the assignment counted: 11 files + the `reviews/`
directory.

The **4 files still in `plans/`** are disjoint from all of it:

| File | In archive? | Why it was left behind |
|---|---|---|
| `plans/GIR-13-option-metadata-on-ir.md` | **No** — archive stops at GIR-11 | GIR-13 was developed *after* the 2026-07-11 GIR close, on `hard/3-7-consolidated` (PR #124, squash-merged `499624d` 2026-08-27). The close that archived GIR-1..11 predates it. |
| `plans/GIR-generator-ir-rewrite.md` | **No** | The plan/tracker. The GIR close migrated the *end-state* into `docs/robustness-plan.md` but left the tracker in place. |
| `plans/GIR-locked-decisions.md` | **No** | Same — and it is still the live citation target of GIR-13's header. |
| `plans/GIR-progress-log.md` | **No** | Same. Note it now carries GIR-13's entry too. |

And the **2 review files** in `plans/reviews/` are `GIR-13-codereview.md` and
`GIR-13-conformance.md` — GIR-13's own pair, absent from `docs/archive/GIR/reviews/` for
the same reason.

**Conclusion: nothing to delete. Move all 6 into `docs/archive/GIR/` (the two reviews into
its existing `reviews/`), which brings the GIR archive to the 13 items the round actually
delivered and makes `docs/robustness-plan.md:372`'s standing claim true.** The `HARD`
archive is the shape precedent: plan, locked decisions, progress log, per-item designs and
the round's spec, all in one directory with their cross-links rewritten relative.

---

## 5. Summary

| Action | Count | Where |
|---|---|---|
| MIGRATE (content) | 3 | §1 |
| MIGRATE (link repair triggered by archiving) | 6 links in 2 kept docs | §1 |
| STALE | 5, plus 1 borderline | §2 |
| Execution-only, safe to archive | all 7 groups | §3 |
| GIR duplicates | **none** — 6 genuine moves | §4 |
