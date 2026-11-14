# RIX — owner rulings

*Verbatim. Appended the moment a ruling is given. Never summarised, never deleted. A ruling is
superseded only by a later ruling recorded beside it.*

## 2026-11-10 — The index is not part of the data model
> "We don't want to actually add the index column to the data model description itself, because it is not really part of the business description of the data; it is mechanics needed by Surge and its ecosystem to keep track of row indices."

**Context:** framing the Fletcher work Surge's LogTables need.
**Applies to:** the `.proto` data model, the Arrow fields of a topic's schema, row bookkeeping (domain 8).

## 2026-11-10 — Decided when the topic is created
> "whether a given topic includes the index column should be decidable when the topic is created"

**Context:** same message as the ruling above.
**Applies to:** topic declaration (domain 3's interface), row bookkeeping (domain 8).

## 2026-11-10 — Option A, no wire-format change
> "Not having to update the wire format is a concern, so if we can implement the solution using A and this not touchign the wire format, I prefer that"

**Context:** choosing between carrying the index as a reserved attachment (A) and as a new typed
envelope slot (B).
**Applies to:** row encoding and the envelope (domain 2), row bookkeeping (domain 8).

## 2026-11-10 — Refuse a second publisher; no counting check
> "yes, refuse a second publisher on a topic that declared an index. For now, we will make no counting check"

**Context:** answering the two open points of the option-A design.
**Applies to:** row bookkeeping (domain 8), topic declaration and conflict refusal (domain 3).

## 2026-11-10 — Hash width, column name, writer id
> "Writer hash width should be 8. Merged column: Surge and LogTable already expects a certain name. The name here should match. Writer id: Required, yes"

**Context:** answering the plan's three open questions.
**Applies to:** row bookkeeping (domain 8).

## 2026-11-10 — The application tier is a boundary, not a domain
> "I agree on all except domain 5: To me that's not a separate domain, but rather the interface through which you talk to the other domains. It is a domain boundary, not a domain. Are there concrete things that that live in that interface that are not covered by the existing domains - in that casew THAT is likely a new domain"

**Context:** reviewing the proposed domain map. "Domain 5" was the application pub/sub tier
(`Publisher`, `Subscriber`, `PublisherArrow`, `SubscriberArrow`). "All" confirms the other six
proposed domains: data model and code generation; row encoding and the envelope; the provider
interface; transport providers; gateway and browser client; protocol driver ABI.
**Applies to:** [docs/domain-map.md](../docs/domain-map.md).

## 2026-11-10 — Two new domains; domain 2 owns reserved keys; specify batch assembly first
> "I agree on 1. 2. Owned by domain 2. 3. (a)"

**Context:** answering three questions:
1. Are batch assembly and row bookkeeping the right two new domains?
2. Who owns the meaning of reserved attachment keys: row bookkeeping (with domain 2 owning only
   the byte layout), or domain 2?
3. For batch assembly: (a) specify it first, in its own short round surveying the code, then run
   RIX; or (b) let RIX extend it narrowly and specify it later?

**Applies to:** [docs/domain-map.md](../docs/domain-map.md) (domains 7 and 8, domain 2's scope),
the order of rounds (batch assembly before RIX).

## 2026-11-10 — Specify domain 2 first; row-index metadata keys belong to domain 2
> "1. Agree on (a). 2. Well, good question: I think the index is not part of the data model, and so these keys shouldn't be either. I would say domain 2. Please proceed"

**Context:** answering two questions:
1. How to handle domain 2: (a) specify it in its own round before RIX, with the reserved
   namespace and the row-index key as two of its laws, marked designed-not-built; or (b) let RIX
   add those two laws and specify the rest later?
2. Who owns the meaning of schema metadata keys: domain 1, domain 3, or domain 2?

**Interpretation, to be confirmed:** this assigns the `fletcher.row_index*` metadata keys to
domain 2, because they are not data model. Keys generated from a `.proto` (`proto_package`,
`proto_message`, `field_number`, keys from `metadata_from_option`) stay with domain 1.
**Applies to:** [docs/domain-map.md](../docs/domain-map.md) (domain 2's scope); round order: batch
assembly and domain 2 are specified before RIX, in either order.

## 2026-11-10 — The row index is the caller's
> "You probably need another law in D7: The row index is always supplied by the caller, never by fletcher itself; it is the caller's responsibility that the rows are indexed compactly and in sequence"

**Context:** reviewing the draft of domain 7 (batch assembly), `docs/domains/d7-batch-assembly.md`.
**Applies to:** domain 7, as law L14. It also governs domain 8 (row bookkeeping), whose document RIX
creates and which must carry the same law. Consistent with D-RIX-4 and D-RIX-5 in
`plans/RIX-row-index.md`.

## 2026-11-10 — RIX deletes the PR #104 helpers; fix L1 and L12; ownership answers
> "And 1. (a) RIX should delete them. 2. Fix both. 3. Agree on all points. 4. I'll review when they are done"

**Context:** answering four points:
1. What to do with `AppendTrailingUint64Field` / `ReadTrailingUint64Field` (PR #104, `07ab48c`).
   These put `_ingest_offset` into the row as a trailing schema field and are used in production by
   Monorepo's `RowBatcherFletcher`. Options: (a) RIX deletes them, shipped as a breaking
   `fletcher-core` 0.4 release; (b) deprecate now and delete after the drivers move; (c) keep them,
   which would amend the ruling that the index is not part of the data model.
2. Domain 7's L1 (drops before any usable schema are never reported) and L12 (batches can reach the
   callback concurrently and out of order): fix both before RIX, or record them as known violations?
3. Four points, all agreed:
   - `max_payload_bytes` belongs to domain 4, not domain 2.
   - protoc should refuse schema metadata keys with the `fletcher.` prefix.
   - Domain 2 owns how Arrow types become bytes; domain 1 owns how proto types become Arrow types.
   - Metadata keys generated from a `.proto` (`proto_package`, `proto_message`, `field_number`)
     stay with domain 1. This confirms the interpretation recorded under the ruling "Specify
     domain 2 first; row-index metadata keys belong to domain 2".
4. Review the laws of both draft domain documents.

**Applies to:** RIX (deleting the helpers and the tests that pin them); a domain 7 fix round before
RIX (L1, L12); domains 1, 2 and 4 (ownership); `docs/domains/d2-row-encoding.md`,
`docs/domains/d7-batch-assembly.md`.

## 2026-11-10 — Laws of domains 2 and 7 confirmed; protoc prefix refusal deferred
> "Yes on all accounts"

**Context:** answering two questions:
1. Does the owner's review confirm the laws of `docs/domains/d2-row-encoding.md` and
   `docs/domains/d7-batch-assembly.md`, so that both domains become `specified`?
2. Should protoc's refusal of `fletcher.`-prefixed metadata keys (domain 1 code) be recorded as a
   known gap and handled when domain 1 is specified?

**Applies to:** domains 2 and 7 (now `specified`); domain 1 (the deferred refusal);
[docs/domain-map.md](../docs/domain-map.md).

## 2026-11-10 — Closing batch after an in-callback Unsubscribe; null-batch wording
> Question 1: "Deliver after it returns (Recommended)"
> Question 2: "Approve the rewording (Recommended)"

*(Chosen options, recorded as the labels the owner selected.)*

**Context:** the D7F pre-round audit (`plans/D7F-pre-round-audit.md`, LL-1 and B2) found two
problems.
- **Question 1:** L9, L12 and L1 cannot all hold when a batch callback calls `Unsubscribe` while
  rows are pending. The chosen option reads: "Amend L9: when Unsubscribe is called from inside a
  batch callback, the closing batch is delivered right after that callback returns, and nothing
  comes after it. Callbacks stay strictly one at a time and in order."
- **Question 2:** whether `SubscriberArrow`'s header and README may say: "batch is null when no
  batch could be built for this window: no usable schema has arrived yet, the schema was rejected
  (permanent), or finishing the batch failed; rows_dropped counts the window's rows in every case".

**Applies to:** domain 7, L9 (revised in place) and L7; round D7F; `pubsub-arrow` header
(`subscriber_arrow.hpp:106-111`) and README (`:94-98`). This resolves the collision between "2. Fix
both" and the confirmation of L9 and L12 in "Yes on all accounts": the ruling revises L9 and
supersedes neither of them.
*Ingested (D7O close, 2026-11-12): verified already in d7 — D8 and L9's "Revised 2026-11-10" note (Question 1); D9, L7 and Findings (a)2 (Question 2).*

## 2026-11-10 — Callbacks on Fletcher's batch thread; the 256 MiB test is labelled heavy
> Question 1: "Always Fletcher's batch thread (Recommended)"
> Question 2: "Label it 'heavy'"

*(Chosen options, recorded as the labels the owner selected.)*

**Context:** two plan-shape questions from the D7F test ladder (`plans/D7F-test-ladder.md`).
- **Question 1:** which thread runs the batch callback. The chosen option reads: "Every callback
  runs on the subscription's own batch thread, and full windows are queued for it in order." Its
  stated costs, which the owner accepted: "if the callback is slower than the data rate, batches
  pile up in memory (no back-pressure on the transport), and 10 existing tests that read results
  straight after Publish must wait for delivery instead."
- **Question 2:** whether the ~256 MiB test for L1's second violation runs by default. The chosen
  option reads: "It runs only when the heavy label is requested."

**Applies to:** domain 7 (§4, which thread calls the callback; L12's mechanism); round D7F's
ladder and the 10 batched tests to re-anchor; the test labels in `pubsub-arrow/tests`.
*Ingested (D7O close, 2026-11-12): verified already in d7 — D10, §4 "Who calls, and on which thread", L12 (Question 1); D11 and L1's Tests (Question 2).*

## 2026-11-10 — Queued windows are delivered before the closing batch
> "Queued batch, then closing (Recommended)"

*(Chosen option, recorded as the label the owner selected.)*

**Context:** Q-B from the D7F test ladder. A slow callback on the batch thread calls `Unsubscribe`
while a full window is already queued. The chosen option reads: "After the callback returns, the
waiting full window is delivered as its own batch (reason row limit), then the closing batch with
any remaining rows, then nothing. ... 'Right after the callback returns' then means 'the closing
batch is the last delivery, after anything already queued'."

**Applies to:** domain 7, L9 (revised in place a second time, which clarifies the first revision
and does not supersede it); D7F ladder, rung 6.
*Ingested (D7O close, 2026-11-12): verified already in d7 — L9's "Clarified 2026-11-10" note and law text; D10.*

## 2026-11-11 — Destroying the subscriber inside a batch callback, and unsubscribing from a row-delivery thread
> Decision 1: "(a) Yes, after the callback (Recommended)"
> Decision 2: "(b) Return at once (Recommended)"

*(Chosen options, recorded as the labels the owner selected; the full option text is in `plans/D7F-3-brief.md`, Decisions 1 and 2.)*

**Rulings:** (1) **destruction from inside the subscriber's own batch callback is treated like an in-callback
`Unsubscribe`**: queued batches, then the closing batch, arrive after the callback returns, and the callback
must outlive them. (2) **An `Unsubscribe` from a thread delivering rows on the subscription's own transport
returns at once**; the remaining batches, then the closing batch, still arrive in order on the batch thread,
and the caller may not free what those callbacks use until the closing batch arrives. This extends domain 3's
no-wait-in-delivery rule (interface spec §7 clause 6, 2026-09-04) to batches.
**Applies to:** domain 7 L9. Its exception widens to cover both cases, and the text must be amended to match
(D7F-3). Also the header `Unsubscribe` doc, README:93. Destroying the subscriber from a row-delivery thread stays forbidden (2026-09-05).
*Ingested (D7O close, 2026-11-12): verified already in d7 — D12 (Decision 1), D13 (Decision 2), L9's law text and "Widened 2026-11-11" note; register BC-6.*

## 2026-11-11 — The D7F-3 ladder gains rungs 6(d) and 6(e)
> Test plan 1: "Keep it, amend ladder (Recommended)"
> Test plan 2: "Add as rung 6(e) (Recommended)"

*(Chosen options, recorded as the labels the owner selected.)*

**Rulings:** **both 2026-11-11 L9 rulings are proven by a test in D7F-3.** Rung 6(d) (an Unsubscribe
from a row-delivery thread returns at once) stays and is added to the ladder and the design. A new rung 6(e)
tests the destruction case: destroying the subscriber inside its own batch callback still delivers the
queued batches and then the closing batch. Source: step-0b audit `plans/reviews/D7F-3-audit-0b.md`.
**Applies to:** `plans/D7F-test-ladder.md` rung 6; the D7F-3 design's test table.
*Ingested (D7O close, 2026-11-12): verified already in d7 L9 Tests (`UnsubscribeFromInsideACallbackNeverDeliversNested`, cases (d) and (e)); a test-plan ruling, no law or decision needed.*

## 2026-11-12 — C# moves onto the C++ batcher: one shim, two variants
> "Arrow C++ should never be in the native library, because it needs to be able to run on embedded
> devices. But the batcher isn't at all supposed to be delivered with the "embedded" part of
> Fletcher, so can't the batcher just be attached only to the part that has Arrow C++? I don't know
> if the C# batcher works with the embedded subset of Fletcher today, but being able to do so is not
> a requirement"
> "Let's build one shim in two variants, and no, don't do the interim fixes to the C# batcher"
> Earlier the same session, on whether C# keeps its own batcher: "Well, we shoulod only have one and
> it should be the C++ one, but which one is most mature? I don't want to just delete the C# batcher
> if it contains functionality that should be ported over to the C++ one first"

**Rulings:** (1) **There is one batch assembler, C++'s** (domain 7). C#'s managed `Batcher` in
`dotnet/src/Fletcher/SubscriberArrow.cs` is deleted once C# runs on it. (2) **The C ABI shim is built
in two variants from one source**: the edge variant exactly as today, never carrying Arrow C++ (it
must run on embedded devices); a server-tier variant that adds `pubsub-arrow`'s batched subscribe.
C# on desktop loads the server variant. A second, separately loaded native library was rejected. **Revised 2026-11-13** (entry "D9S pre-round"): the server build
loads only when the app references the server package; otherwise the edge build.
(3) **No interim fixes to the C# batcher.**
**Applies to:** the domain map (planned order of work); the rounds D7O, then the shim round, then
the C# switch round.
*Ingested (D7O close, 2026-11-12): (1) d7 D16, register BC-9; (2) domain-map row 9 and planned order 5–6. Not ingested: (3) "no interim fixes" — its domain (10) is only `anticipated`; named in domain-map "Where this map is incomplete".*
*Ingested (D9S pre-round, 2026-11-13): the 2026-11-13 revision of (2) — `docs/domains/d9-c-abi-shim.md` §1 (server variant), D19 (revision noted in place), D29, §6 (domain 10: what a C# app loads, and the C# switch round's consequence).*

## 2026-11-12 — A batched subscription may have no timeout
> "Add 'never' to C++ (Recommended)"

*(Chosen option.)* Its text: "Domain 7 gains a no-deadline option (an owner ruling amending L4/L5,
plus a small change to C++'s BatchOptions). C# keeps its current behaviour. Since a round may touch
only one domain, this becomes a small domain-7 round that runs first."
**Applies to:** domain 7 L4, L5 (amended 2026-11-12), §4; round D7O.
*Ingested (D7O close, 2026-11-12): verified already in d7 D14, L4, L5, §4. Not ingested: "C# keeps its current behaviour" — domain 10 is only `anticipated`; named in domain-map "Where this map is incomplete".*

## 2026-11-12 — Batch options out of range are refused, never clamped
> "Refuse both (Recommended)"

*(Chosen option.)* Its text: "C++'s batched Subscribe refuses a negative timeout and max_rows < 1
with an invalid-argument error, one rule in domain 7; the shim passes the refusal through and C#
throws as it does today. Visible change: C++ callers passing such values get an error, and C#
callers passing MaxRows < 1 now get an exception instead of 1."
**Applies to:** domain 7 §1 (the "Refuses: validating `BatchOptions`" item is withdrawn), new law
L15, Q5 (resolved); register BC-8; round D7O.
*Ingested (D7O close, 2026-11-12): verified already in d7 D15, L15, §1, Q5; register BC-8. Not ingested: the C# `MaxRows < 1` consequence — domain 10 is only `anticipated`; named in domain-map "Where this map is incomplete".*

## 2026-11-12 — The shim is a domain of its own; C#'s extras stay out of domain 7
> Domain boundary: "Shim only"
> Scope: "Keep them out (Recommended)"

**Rulings:** (1) **The new domain is the C ABI shim alone, both variants.** Each language binding
over it (C#, later Rust) is a domain of its own. (2) **BC-2's per-row gap and BC-7 are not
prerequisites of the C# switch** and stay registered and deferred: C#'s per-row `Subscribe` is a
batched one with `MaxRows = 1` (D-BIND-59), and a managed handler's exception is caught, and counted,
in C#'s own trampoline (`HandlerFaulted`, `AbsorbedCallbackFailures`).
**Applies to:** the domain map; register BC-2, BC-7.
*Ingested (D7O close, 2026-11-12): (1) domain-map rows 9 and 10 (verified); (2) register BC-2 and BC-7 status, "not a prerequisite of the C# switch".*

## 2026-11-12 — Earlier answers the same day: the C# batcher until it is removed (recorded late)
> Which domain owns the C# batcher: "New bindings domain"
> D-BIND-64: "There shoulnd't be a separate C# batch handler - so until it's removed, do as little as
> possible to maintain it while still keeping it compliant"
> Late schema in C#: "Follow domain 7 (Recommended)"

*(Recorded 2026-11-12 by round-designer, after a session crash lost them before they were written.
The questions' text is in that session's transcript; summarised here.)*
**Rulings:** (1) C#'s batcher was placed in a new bindings domain, not domain 7; the boundary was
then narrowed to the shim alone (entry above, "Shim only"), with each language binding its own
domain. (2) **D-BIND-64** (C# batch handlers concurrent and out of order "as in C++", nested closing
batch on an in-handler Unsubscribe) is no longer true of C++ since D7F; until the C# batcher is
removed it is maintained minimally but kept compliant. **Revised 2026-11-12** (entry below): no interim
fixes; C# does not ship to `main` before the switch. (3) **A C# subscription whose first row
arrives before its schema follows domain 7** (L8, L13): losses reported, assembly once the schema
arrives; not undecodable for life.
**Applies to:** D-BIND-64 and D-BIND-25 in `plans/BIND-csharp-bindings.md` (to be amended by the C#
switch round); ⚠ see the collision recorded next.
*Ingested (D7O close, 2026-11-12): (1) domain-map row 10 (verified); (2) as revised by the entry below, register BC-9 status (D-BIND-64 to be annotated by the C# switch round). Not ingested: (3) a C# late schema follows domain 7 — domain 10 is only `anticipated`; named in domain-map "Where this map is incomplete".*

## 2026-11-12 — Collision resolved: no interim fixes, and C# does not ship first
> "No fixes; don't ship it first (Recommended)"

*(Chosen option.)* Its text: "Revise the earlier answer: the C# batcher stays as it is,
non-compliant, and that is acceptable only because C#'s batched subscribe does not ship to main (PR
#129) before the switch lands. If shipping becomes urgent, this question comes back."
**Resolution:** **revision in place** of the earlier ruling (2) above ("keeping it compliant"). The
later "no interim fixes" stands. **Condition:** PR #129 does not merge to `main` with the managed
batcher in it; if that changes, this ruling is re-opened, not assumed.
**Applies to:** the C# switch round's ordering; PR #129.
*Ingested (D7O close, 2026-11-12): domain-map planned order item 6 (the re-open condition); register BC-9 status (verified).*

## 2026-11-12 — D7O hand-over: huge timeouts, and a huge max_rows
> LC-1: "Keep it (Recommended)"
> Ladder Q1: "Fix in D7O (Recommended)"

**Rulings:** (1) **L5's sentence "a timeout too long for the clock to represent means no deadline,
never an early or wrapped one" is confirmed** as law (it was authored by round-designer, audit LC-1).
(2) **A `max_rows` larger than any window will reach is used as given and fixed in D7O** (stage
D7O-3): a failed pre-size never makes a decodable schema unusable.
**Applies to:** domain 7 L5, L15, L8; D7O ladder rungs 3 and 4.
*Ingested (D7O close, 2026-11-12): (1) d7 D17, cited from L5; (2) verified already in d7 L8 (D7O-3 note) and L15.*

## 2026-11-12 — D7O-2: the deadline definition says some windows have none
> "(a) Amend it (Recommended)"

*(Chosen option, D7O-2 Stage Brief Decision 1.)* Its text: "Text becomes '…for a timeout the clock
can reach; otherwise none (L5)'. Users: no change, and the document matches what they get.
Architecture: agrees with law L5; closes audit finding F1, as the round's definition of done
requires."
**Ruling:** **d7 §2's "Deadline" definition is amended** to the wording above, by D7O-2.
**Applies to:** `docs/domains/d7-batch-assembly.md` §2; audit F1 (`plans/D7O-pre-round-audit.md`).
*Ingested (D7O close, 2026-11-12): d7 §2 "Deadline" row (wording verified, ruling cited).*
*Archived path (D9S pre-round, 2026-11-13): the audit cited in *Applies to*, `plans/D7O-pre-round-audit.md`, is now `docs/archive/D7O/D7O-pre-round-audit.md` (archived at D7O close).*

## 2026-11-12 — D7O close: line overruns accepted
> "Accept, as explained"
> Round close: "Run round close (Recommended)"

*(Chosen options.)* Text of the first: "Record that the overruns are accepted: the causes are comments
and assertion messages plus one review-found safety fix, all well under the ceiling. No process
change."
**Ruling:** **D7O-1, D7O-2 and D7O-3's >50% production-line overruns are accepted**; no process change.
**Applies to:** the D7O close findings in `plans/D7O-progress-log.md`.
*Not ingested: a process ruling about round D7O's own records, not a fact of any domain; it stands in `plans/D7O-progress-log.md`.*

## 2026-11-12 — D7O round close: sweep the missed rounds too
> "Archive them too (Recommended)"
> "Archive, re-point links (Recommended)"

**Rulings:** (1) **D7O's close also archives the closed rounds whose files were never archived** —
D7F, RBA, PDA-decouple and the GIR leftovers — each under its own `docs/archive/<round>/`, with
citing docs re-pointed. (2) `GIR-locked-decisions.md`, `PDA-DEC-rulings.md` and
`BIND-progress-log.md` are archived with their rounds and their citations re-pointed; nothing is
deleted, ledgers stay intact.
**Applies to:** round-close step 3; `plans/` afterwards holds only RIX, PDA-ABI, RIR, the BIND plan
and locked decisions, this shared ledger and `reviews/design-debt.md`.
*Not ingested: a round-close procedure ruling (archiving), not a fact of any domain.*

## 2026-11-13 — Domain 9 (C ABI shim): packaging, wrong build, codec owner, laws confirmed
> Packaging: "Separate server package"
> Wrong build: "Clear 'not supported' (Recommended)"
> Codec owner: "Domain 2 (Recommended)"
> Laws: "Confirm, adjusted by answers"

*(Chosen options.)* Texts: **Packaging** — "Users: apps that only publish/subscribe per row stay small;
apps wanting batches add a second package. Architecture: keeps D-BIND-157 for the base package;
revises D-BIND-13 and -17 (two packages each carrying a full shim, of which only one may load per
process, L5/L17)." **Wrong build** — "Users: the program can ask the shim which build it has, and a
batched subscribe on the edge build fails with a named 'not supported' error rather than a
missing-symbol crash. Architecture: L16 … via a query that both builds export; … the query would be
added to the edge too." **Codec owner** — "domain 2 already owns 'how a row is encoded' and its laws
(byte-identity across codecs) then cover every codec in one place; domain 9 only hosts it."
**Laws** — "Confirm L1–L17 as drafted in docs/domains/d9-c-abi-shim.md, amended only where your
answers above change them … Domain 9 becomes 'specified' and stage design can start."
**Rulings:** (1) **the server variant ships in its own .NET package**; D-BIND-157's ceiling stays on
the base package; **D-BIND-13 and D-BIND-17 are revised** (two packages, each a full shim; one shim per
process still holds). (2) **Both builds export a variant query; the batched entry points on the edge
build answer a named not-supported status** (L12 amended: the edge surface gains only that query and
those refusals). (3) **The shim's row codec is domain 2's**; domain 9 hosts it (d9 L10 moves to
domain 2). (4) **Domain 9's laws L1–L17 are confirmed** as amended by (1)–(3); domain 9 is `specified`.
**Applies to:** `docs/domains/d9-c-abi-shim.md`, `docs/domains/d2-row-encoding.md`,
`docs/domain-map.md` rows 2 and 9; BIND D-BIND-13, -17, -157, Decision 6 (annotations owed).
*Ingested (D9S pre-round, 2026-11-13): (1) d9 D22, L5, L17, §5 D8/D9/D18; (2) d9 D23, L12, L16, §4; (3) d9 D24, L10 (moved), and `docs/domains/d2-row-encoding.md` (former d9 L10); (4) d9 D25 and status `specified`; `docs/domain-map.md` rows 2 and 9 (all verified, written when domain 9 was specified). The owed annotations are now written: `plans/BIND-locked-decisions.md` D-BIND-1a rider ii, D-BIND-13, D-BIND-17, D-BIND-157 (kept for the base package); `plans/BIND-csharp-bindings.md` Decision 6 row and the Part 2 packaging prose.*

## 2026-11-13 — D9S pre-round: batch ownership, version, error code, what C# loads
> Batch owner: "Borrowed; keep by moving (Recommended)"
> Version: "One version; edit the line (Recommended)"
> Error code: "Reuse, widen meaning (Recommended)"
> C# loads: "Depends on package (Recommended)"

*(Chosen options; full texts in the D9S pre-round audit, B1, LC-1/OQ-1, LC-2, R-2.)*
**Rulings:** (1) **A batch handed to a callback is borrowed for the call; a binding keeps it only by
moving it (Arrow's move); what is not moved the shim frees when the callback returns** — d9 L11 holds,
d9 L14 is amended (resolves audit B1). (2) **Both builds share one ABI version, and both bump**; this
round may make **one named one-line edit in domain 10**: the C# loader's expected version, in the
same commit as the bump (a recorded scope exception). (3) **`FL_NOT_SUPPORTED` is reused; its meaning
widens to "this provider or this build cannot do this"**; no new status number. (4) **Revision in place
of 2026-11-12's "C# on desktop loads the server variant"**: a C# app loads the server build only if it
references the server package, otherwise the edge build. Consequence owned by the C# switch round:
C#'s per-row subscribe must work on the edge build without a batcher.
**Applies to:** d9 L2, L6, L11, L12, L14, §2; `c-abi/include/fletcher/abi/binding.h` status docs;
`dotnet/src/Fletcher.Interop/NativeLoader.cs` (one line); the 2026-11-12 entry (annotated).
*Ingested (D9S pre-round, 2026-11-13): (1) d9 L14 (amended; L11 unchanged), D26, register BC-13; (2) d9 L6, L12, D27, §7 Q8 and LC-1 (the `NativeLoader.cs` line itself moves with the bump, in the round, not now); (3) d9 §2 "Not supported", L2, L12, D28, §7 Q3 and LC-2; `binding.h` comments on `FL_NOT_SUPPORTED` and "Status of the surface"; `c-abi/README.md`. Not ingested: the seam's published table, `core/README.md:37` (domain 3's text), still reads "This provider does not implement the requested behaviour" (d9 §7 LC-2); (4) d9 §1, D19 (revised in place), D29, §6 (domain 10).*

## 2026-11-13 — D9S pre-round: a failed handoff is reported; corrections approved
> Lost handoff: "Report it as lost (Recommended)"
> Corrections: "Approve all as listed"

*(Chosen options; texts in the D9S pre-round audit, C5/BC-12 and the tier-2 findings.)*
**Rulings:** (1) **A batch the shim cannot hand over is reported, never lost silently**: the binding
receives a "no batch could be built" delivery for that window with its rows counted as dropped and
its reason kept; one site in the shim (register BC-12); domain 7 untouched. (2) **The tier-2 text
corrections listed in `plans/D9S-pre-round-audit.md` are approved** and applied before the round.
**Applies to:** d9 (a new law or an L14 clause), register BC-12, the audit's F-rows.
*Ingested (D9S pre-round, 2026-11-13): (1) d9 law L18 (a new law, not an L14 clause: its own question, site and test), D30, §2 "Hand-over", register BC-12; (2) d9 D31 and the F-rows: d9 (F1, F3, F4, F6, F7, F9, F15), BIND annotations (F17–F19), `binding.h` comments (F10, F22, F23), `c-abi/README.md` (F24–F26), these ledger notes (F27); register BC-10 to BC-15 and notes on BC-5, BC-8, BC-9.*

## 2026-11-13 — D9S-2: the server build is its own file
> "(a) Its own name (Recommended)"

*(Chosen option, D9S-2 Stage Brief Decision 1.)* Its text: "Users: both files can sit in one
folder, an operator can tell from the file which build is deployed, and a process that loads both
refuses the second by name. Architecture: serves one-shim-per-process (L17), the two-package ruling
(D22) and 'C# loads the server build only if it references the server package' (D29); the C# round
must teach C# the new name or rename it when packing."
**Ruling:** **the server build's native file has its own name** (`fletcher-c-abi-server`); the C#
switch round teaches C# that name or renames it when packing (domain 10).
**Applies to:** d9 Q5a (the shim-side half resolved); D9S-2 design; D9S-7 (both in one process).
*Ingested (D9S-2 close, 2026-11-13): `CMakeLists.txt` names the server shim `fletcher-c-abi-server` from the variant; d9 §2 Server variant row, L17, BC-15; package names stay domain 10's (Q5a).*

## 2026-11-13 — D9S-2 close: ceiling overrun accepted; CI evidence via the owner's draft PR
> Ceiling: "Accept, as explained"
> CI evidence: "You open a draft PR now"

*(Chosen options.)* Texts: "Record the overrun as accepted: it came from review-found safety fixes,
not planning drift. No process change; later stages keep the 250 ceiling." / "You open a draft PR from
feature/shim-server-variant (I don't open PRs); CI runs on every later push, and I record run IDs as
stages close."
**Rulings:** (1) **D9S-2's net +265 over the 250 ceiling is accepted**; no process change. (2) **The
owner opens a draft PR from `feature/shim-server-variant`**; from then on each stage close records the
CI run IDs, and the "Windows, by hand; CI and Linux unrun" marks are lifted once a green run covers them.
**Applies to:** D9S-2 close finding; the D9S evidence ledger; d9 and register marks narrowed at D9S-2.

## 2026-11-13 — D9S-3: a test-only Arrow dependency of the edge build is refused too
> "Narrow the docs (Recommended)"

*(Chosen option; D9S-3 compliance review F2.)* Its text: "Users: no change; a future recipe that adds
Arrow as a test-only dependency of the edge build fails CI and must instead add it only when tests are
built (as today). Architecture: L8 holds; BC-11 and ladder rung 3 are annotated 'with tests off, a
test-only Arrow dependency is refused too' — the stricter check becomes the stated rule, one site, no
exemption logic."
**Ruling:** **with tests off, the edge build's graph may hold no Arrow package at all, test-only or
not**; Arrow reaches test binaries only through dependencies added when tests are built.
**Applies to:** register BC-11, ladder rung 3, d9 L8's "How it is enforced"; `check_edge_arrow_free.sh` unchanged.

## 2026-11-13 — D9S-4: position-independent code for pubsub-arrow and arrow-bridge
> "Allow the build option (Recommended)"

*(Chosen option; D9S-4 design review B3.)* Its text: "Users: none — no behaviour or API change; the
server build links on Linux. Architecture: one-domain scope is bent by a named, build-only exception in
domain 7's and domain 2's recipes (an fPIC option copied from 'pubsub'); their code and laws are
untouched. Serves the round purpose (batches through the shim on Linux)."
**Ruling:** **D9S-4 may add an `fPIC` option (default on; deleted on Windows) to
`pubsub-arrow/conanfile.py` and `arrow-bridge/conanfile.py`**, exactly as `pubsub` has it — a named
scope exception; no other edit to domains 7 or 2.
**Applies to:** D9S-4 design branch (a); D-D9S-8's out-of-scope list (this one exception).

## 2026-11-13 — D9S-4: one subscriber object; the interim handoff gap accepted
> Decision 1: "(a) One object, documented (Rec.)"
> Decision 2: "(a) Accept until stage 6 (Rec.)"

*(Chosen options, D9S-4 Stage Brief.)* Texts: (1) "Users: nothing new to manage; destroying the
subscriber ends both kinds; a same-topic mix collides as above, which a user may not expect inside one
object. Architecture: the server build only adds one call (L12, L13); the shim forwards the transport's
rule rather than answering it (L4); the provider difference is registered for domains 3/4." (2) "Users:
no one is exposed — nothing ships mid-round and C# doesn't call this before a later round; those rows
vanish uncounted in stages 4–5 only. Architecture: L18 holds at round close and keeps its red-first test
in stage 6."
**Rulings:** (1) **one `fl_subscriber` holds both row and batched subscriptions**; the same-topic
collision is documented, not refused; the provider difference is registered for domains 3/4. (2) **a
failed conversion is lost uncounted during D9S-4/5 only**; L18 is built and proven red-first in D9S-6.
**Applies to:** D9S-4a design; d9 L4, L18; register (d3/d4 finding).

## 2026-11-13 — D9S-4a: ceiling overrun from documentation accepted
> "Accept, as explained (Rec.)"

*(Chosen option.)* Its text: "Users: none; the C interface keeps its full explanatory comments (who owns
a batch, when it is released, what each reason means). Architecture: the overrun is documentation, not
work — code is ~192 lines, within the ceiling's intent; recorded as an accepted close finding like D9S-2's."
**Ruling:** **D9S-4a's ~312 counted production lines (≈192 code) over the 250 ceiling are accepted.**
**Applies to:** D9S-4a close finding.

## 2026-11-13 — D9S: tighter document caps for the remaining stages
> "Tighter caps (Recommended)"

*(Chosen option.)* Its text: "You: about half as much to read per stage, and the same stops reach you. Architecture: serves
the round's purpose by spending effort on code, not prose."
**Ruling:** **D9S's remaining full-loop stages run with a 150-line design and 60-line reviews.** Light-lane caps are unchanged.
**Applies to:** D9S-5, D9S-6 (and any ejection). Trigger: the words-per-stage tripwire (log, D9S-4a close).

## 2026-11-14 — D9S-5: every close ends with one closing delivery; D9S-5 splits
> Idle close: "Always a final delivery (Rec.)"
> Split D9S-5: "Split and proceed (Rec.)"

*(Chosen options.)* Idle-close text: "Users: every close ends with exactly one closing delivery (empty when idle), so a binding always
knows when it may free its state. Architecture: keeps the ruling and d9 §1's 'who releases each'. Amends d7 L6 for closing and
needs a small domain-7 change, which this round's config forbids, so you authorise widening the round."
**Ruling:** **every close of a batched subscription ends with exactly one closing delivery, even when nothing is pending.**
Settles the collision between the 2026-11-11 entry (1)/(2) ("then the closing batch … until the closing batch arrives") and
d7 L6 ("A window with nothing pending delivers nothing") in favour of the ruling. d7 L6 is amended for closing, and the round is
widened to that one domain-7 law and its code. The split: D9S-5a (move/release, options, close from outside a callback) proceeds
now; D9S-5b carries the d7 L6 amendment, the domain-7 change and the in-callback close. Trigger: `plans/reviews/D9S-5-audit-0a.md`.
Still live: the empty delivery's exact shape (zero-row vs null batch) is D9S-5b's design and may need its own owner question.
`binding.h:867-877`'s "no reliable end signal" text is superseded once 5b lands.

## 2026-11-14 — D9S-5b: a refused batched subscribe never delivers
> "(a) Never (Rec.)"

*(Chosen option.)* Its text: "Users: a refusal means nothing ever arrives, so state can be freed as soon as the call returns.
Architecture: serves d9 §1 'who releases each' and matches every other refusal (L12, L15). Strains d7 L1 'no silent loss' (the few
caught rows are discarded uncounted), so the round widens by one more domain-7 clause."
**Ruling:** **a refused batched subscribe never delivers anything; the caller may free its state when the call returns.**
Binds: d7 L1 gains a clause for rows caught by a subscription refused before it was registered (they are discarded uncounted). The
round's widening grows by that one d7 L1 clause, in D9S-5b. Supersedes nothing; it narrows the 2026-11-14 "every close ends with one
closing delivery" ruling to subscriptions that were accepted. Trigger: `plans/reviews/D9S-5b-design-review.md` B1; brief Decision 1.
