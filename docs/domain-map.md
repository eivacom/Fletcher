# Fletcher — Domain Map

**Mode: survey.** Code is ground truth; the documents named below are claims to be checked against
it. The domain boundaries are the owner's (confirmed 2026-11-10, see the rulings in
[plans/RIX-rulings.md](../plans/RIX-rulings.md)); the descriptions of what each domain covers come
from a light reading of the tree at `9a4780b` (branch `feature/row-index`), not from a full survey.

## Brief

- **Question:** what are Fletcher's domains, where are their boundaries, and which of them are
  specified well enough for a round to operate in?
- **Corpus:** `docs/` (architecture overview, wire-format, provider-interface, accessor and
  driver-ABI specs), the module READMEs, and the tree.
- **Authority:** owner rulings and a domain's own laws are peers at the top. Specs come below
  them, and stage charters and the code come below the specs. A spec that disagrees with a ruling
  is corrected with the owner's approval, never silently.
- **Method:** owner-confirmed boundaries; capabilities assigned to domains by reading what the code
  does, not what it is called.

## The rule this map follows

A domain is a set of responsibilities with laws of its own. **The application-facing classes
(`Publisher`, `Subscriber`, `PublisherArrow`, `SubscriberArrow`) are not a domain.** They are the
boundary through which applications reach the domains. Anything that lives in them and is owned by
no domain is a candidate new domain (owner ruling, 2026-11-10).

## Domain inventory

Status meanings:

- `specified`: numbered laws, each marked with its build state; a round may run in it.
- `draft`: boundary known, laws not settled; no round may run in it until it is promoted.
- `anticipated`: named only.

| # | Domain | Owns | Main documents | Status |
|---|---|---|---|---|
| 1 | **Data model and code generation** | the protoc plugin; generated message classes, view classes, RecordBatch accessors; the schema and schema metadata generated from a `.proto` | [fletcher-options.md](fletcher-options.md), [recordbatch-accessor-spec.md](recordbatch-accessor-spec.md) | `draft`: the specs exist, but no numbered laws |
| 2 | **Row encoding and the envelope** | the positional row codec (`core`, `arrow-bridge`), the envelope, the attachments byte layout, **the meaning of reserved attachment keys, and the meaning of schema metadata keys that are not data model, such as `fletcher.row_index*`** (owner rulings, 2026-11-10) | [wire-format-specification.md](wire-format-specification.md) | `draft`: promoted before RIX (owner ruling, 2026-11-10) |
| 3 | **The provider interface** | `PubSubProvider`: topic creation, schema delivery over `__schema`, conflict refusal (`internal/schema_conflict.hpp`, shared by `Publisher`), the delivery contract, threading and lifetime, `TopicOptions`. **Local fan-out and subscription lifetime in `Subscriber` belong here** (spec §6, §7 clause 4). | [pubsub-interface-spec.md](pubsub-interface-spec.md) | `draft`: a detailed spec with numbered clauses, but not laws in the required form |
| 4 | **Transport providers** | FastDDS, XRCE, in-process: how each meets domain 3's contract, including the meaning of `max_payload_bytes` on `__schema` (owner ruling, 2026-11-10) | provider READMEs | `draft` |
| 5 | **Gateway and browser client** | the WebSocket gateway, the TS client | wire spec (gateway frames), `gateway-client-ts` | `draft` |
| 6 | **Protocol driver ABI** | loading providers across the ABI boundary | [protocol-driver-abi-spec.md](protocol-driver-abi-spec.md) | `draft` |
| 7 | **Batch assembly** | turning a stream of rows into columnar batches: when windows close (row count, timeout, offset overflow), loss accounting, row-aligned side data (`RecordBatchBatcher`, [pubsub-arrow/src/subscriber_arrow.cpp](../pubsub-arrow/src/subscriber_arrow.cpp)) | none; its rules are only in code comments | `draft`: built, unspecified. Promoted by the next round (owner ruling, 2026-11-10) |
| 8 | **Row bookkeeping** | per-row information that is not business data, starting with Surge's row index: its declaration at topic creation, its single writer, its delivery beside the batch | [plans/RIX-row-index.md](../plans/RIX-row-index.md) (plan, not a spec) | `draft`: not built. Created by round RIX |

The numbers are labels, not a ranking. They differ from the first proposal: the application tier
(proposed as domain 5) was withdrawn, so the gateway and the driver ABI moved up to 5 and 6.

## Planned order of work (owner rulings, 2026-11-10)

1. **Domains 2 and 7 specified:** drafts at [domains/d2-row-encoding.md](domains/d2-row-encoding.md)
   and [domains/d7-batch-assembly.md](domains/d7-batch-assembly.md), awaiting the owner's review of
   their laws.
2. **Domain 7 fix round:** L1's violations (drops never reported) and L12's (batches delivered
   concurrently and out of order).
3. **RIX:** creates domain 8, and deletes the PR #104 trailing-field helpers.

Not yet placed: protoc's refusal of `fletcher.` metadata keys, which is domain 1 code
(d2-row-encoding Q3a).

## Where this map is incomplete

- **No domain is `specified`.** Every existing document predates this map and states its rules as
  prose or as spec clauses, not as numbered laws marked with build state. Until a domain is
  promoted, no round may run in it.
- **Schema metadata keys are split between two owners.** Keys that are not data model
  (`fletcher.row_index*`) belong to domain 2. Keys generated from a `.proto` (`proto_package`,
  `proto_message`, `field_number`, `metadata_from_option` keys) stay with domain 1 (confirmed
  2026-11-10). Domain 3 transports all of them and compares their bytes, without owning what any
  key means.
- **Language bindings are unmapped.** The C# binding (`Eiva.Fletcher`, used by the Monorepo
  SurgeIngester) lives outside this repo. Rust has only generated accessors.
- **Shared kernel not assessed.** `core/internal/delivery_frame.hpp` is used by both `Subscriber`
  and the providers (PDA-DEC-AG1). It sits in domain 3 here, but this has not been checked.
- **Verified:** boundaries by the owner on 2026-11-10; capability assignments by reading the code
  at `9a4780b`, which is a light check, not a survey.

## Capability census (light)

| Capability | Where it lives | Domain |
|---|---|---|
| `.proto` → C++/Rust/TS code | `protoc/` | 1 |
| Positional row encode/decode | `core/`, `arrow-bridge/` | 2 |
| Envelope and attachments | `core/include/fletcher/core/envelope.hpp`, `types.hpp` | 2 |
| Topic declaration, conflict refusal | providers, `pubsub/src/publisher.cpp` (shared comparison) | 3 |
| Schema delivery (`__schema`) | providers | 3, 4 |
| Local fan-out, unsubscribe barrier | `pubsub/src/subscriber.cpp` | 3 |
| DDS / XRCE / in-process transport | `fastdds-pubsub-provider/`, `xrcedds-pubsub-provider/`, `pubsub/src/in_process_provider.cpp` | 4 |
| WebSocket bridge | `gateway/`, `gateway-client-ts/` | 5 |
| Provider loading across the ABI | `pubsub/` (provider registry), PDA | 6 |
| Row stream → RecordBatch | `pubsub-arrow/src/subscriber_arrow.cpp` | 7 |
| Surge row index | not built | 8 |
