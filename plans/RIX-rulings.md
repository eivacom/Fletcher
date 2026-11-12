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

## 2026-11-11 — The D7F-3 ladder gains rungs 6(d) and 6(e)
> Test plan 1: "Keep it, amend ladder (Recommended)"
> Test plan 2: "Add as rung 6(e) (Recommended)"

*(Chosen options, recorded as the labels the owner selected.)*

**Rulings:** **both 2026-11-11 L9 rulings are proven by a test in D7F-3.** Rung 6(d) (an Unsubscribe
from a row-delivery thread returns at once) stays and is added to the ladder and the design. A new rung 6(e)
tests the destruction case: destroying the subscriber inside its own batch callback still delivers the
queued batches and then the closing batch. Source: step-0b audit `plans/reviews/D7F-3-audit-0b.md`.
**Applies to:** `plans/D7F-test-ladder.md` rung 6; the D7F-3 design's test table.

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
C# on desktop loads the server variant. A second, separately loaded native library was rejected.
(3) **No interim fixes to the C# batcher.**
**Applies to:** the domain map (planned order of work); the rounds D7O, then the shim round, then
the C# switch round.

## 2026-11-12 — A batched subscription may have no timeout
> "Add 'never' to C++ (Recommended)"

*(Chosen option.)* Its text: "Domain 7 gains a no-deadline option (an owner ruling amending L4/L5,
plus a small change to C++'s BatchOptions). C# keeps its current behaviour. Since a round may touch
only one domain, this becomes a small domain-7 round that runs first."
**Applies to:** domain 7 L4, L5 (amended 2026-11-12), §4; round D7O.

## 2026-11-12 — Batch options out of range are refused, never clamped
> "Refuse both (Recommended)"

*(Chosen option.)* Its text: "C++'s batched Subscribe refuses a negative timeout and max_rows < 1
with an invalid-argument error, one rule in domain 7; the shim passes the refusal through and C#
throws as it does today. Visible change: C++ callers passing such values get an error, and C#
callers passing MaxRows < 1 now get an exception instead of 1."
**Applies to:** domain 7 §1 (the "Refuses: validating `BatchOptions`" item is withdrawn), new law
L15, Q5 (resolved); register BC-8; round D7O.

## 2026-11-12 — The shim is a domain of its own; C#'s extras stay out of domain 7
> Domain boundary: "Shim only"
> Scope: "Keep them out (Recommended)"

**Rulings:** (1) **The new domain is the C ABI shim alone, both variants.** Each language binding
over it (C#, later Rust) is a domain of its own. (2) **BC-2's per-row gap and BC-7 are not
prerequisites of the C# switch** and stay registered and deferred: C#'s per-row `Subscribe` is a
batched one with `MaxRows = 1` (D-BIND-59), and a managed handler's exception is caught, and counted,
in C#'s own trampoline (`HandlerFaulted`, `AbsorbedCallbackFailures`).
**Applies to:** the domain map; register BC-2, BC-7.

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

## 2026-11-12 — Collision resolved: no interim fixes, and C# does not ship first
> "No fixes; don't ship it first (Recommended)"

*(Chosen option.)* Its text: "Revise the earlier answer: the C# batcher stays as it is,
non-compliant, and that is acceptable only because C#'s batched subscribe does not ship to main (PR
#129) before the switch lands. If shipping becomes urgent, this question comes back."
**Resolution:** **revision in place** of the earlier ruling (2) above ("keeping it compliant"). The
later "no interim fixes" stands. **Condition:** PR #129 does not merge to `main` with the managed
batcher in it; if that changes, this ruling is re-opened, not assumed.
**Applies to:** the C# switch round's ordering; PR #129.

## 2026-11-12 — D7O hand-over: huge timeouts, and a huge max_rows
> LC-1: "Keep it (Recommended)"
> Ladder Q1: "Fix in D7O (Recommended)"

**Rulings:** (1) **L5's sentence "a timeout too long for the clock to represent means no deadline,
never an early or wrapped one" is confirmed** as law (it was authored by round-designer, audit LC-1).
(2) **A `max_rows` larger than any window will reach is used as given and fixed in D7O** (stage
D7O-3): a failed pre-size never makes a decodable schema unusable.
**Applies to:** domain 7 L5, L15, L8; D7O ladder rungs 3 and 4.

## 2026-11-12 — D7O-2: the deadline definition says some windows have none
> "(a) Amend it (Recommended)"

*(Chosen option, D7O-2 Stage Brief Decision 1.)* Its text: "Text becomes '…for a timeout the clock
can reach; otherwise none (L5)'. Users: no change, and the document matches what they get.
Architecture: agrees with law L5; closes audit finding F1, as the round's definition of done
requires."
**Ruling:** **d7 §2's "Deadline" definition is amended** to the wording above, by D7O-2.
**Applies to:** `docs/domains/d7-batch-assembly.md` §2; audit F1 (`plans/D7O-pre-round-audit.md`).

## 2026-11-12 — D7O close: line overruns accepted
> "Accept, as explained"
> Round close: "Run round close (Recommended)"

*(Chosen options.)* Text of the first: "Record that the overruns are accepted: the causes are comments
and assertion messages plus one review-found safety fix, all well under the ceiling. No process
change."
**Ruling:** **D7O-1, D7O-2 and D7O-3's >50% production-line overruns are accepted**; no process change.
**Applies to:** the D7O close findings in `plans/D7O-progress-log.md`.
