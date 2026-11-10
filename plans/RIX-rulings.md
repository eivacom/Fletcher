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
