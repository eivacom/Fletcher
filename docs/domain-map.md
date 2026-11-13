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
| 2 | **Row encoding and the envelope** | the positional row codec (`core`, `arrow-bridge`), the envelope, the attachments byte layout, **the meaning of reserved attachment keys, and the meaning of schema metadata keys that are not data model, such as `fletcher.row_index*`** (owner rulings, 2026-11-10); **the shim's row codec, `NanoarrowCodec` (`c-abi/src/nanoarrow_codec.*`), hosted by domain 9** (owner ruling, 2026-11-13) | [domains/d2-row-encoding.md](domains/d2-row-encoding.md); [wire-format-specification.md](wire-format-specification.md) | **`specified`** 2026-11-10 |
| 3 | **The provider interface** | `PubSubProvider`: topic creation, schema delivery over `__schema`, conflict refusal (`internal/schema_conflict.hpp`, shared by `Publisher`), the delivery contract, threading and lifetime, `TopicOptions`. **Local fan-out and subscription lifetime in `Subscriber` belong here** (spec §6, §7 clause 4). | [pubsub-interface-spec.md](pubsub-interface-spec.md) | `draft`: a detailed spec with numbered clauses, but not laws in the required form |
| 4 | **Transport providers** | FastDDS, XRCE, in-process: how each meets domain 3's contract, including the meaning of `max_payload_bytes` on `__schema` (owner ruling, 2026-11-10) | provider READMEs | `draft` |
| 5 | **Gateway and browser client** | the WebSocket gateway, the TS client | wire spec (gateway frames), `gateway-client-ts` | `draft` |
| 6 | **Protocol driver ABI** | loading providers across the ABI boundary | [protocol-driver-abi-spec.md](protocol-driver-abi-spec.md) | `draft` |
| 7 | **Batch assembly** | turning a stream of rows into columnar batches: when windows close (row count, timeout, offset overflow), loss accounting, row-aligned side data (`RecordBatchBatcher`, [pubsub-arrow/src/subscriber_arrow.cpp](../pubsub-arrow/src/subscriber_arrow.cpp)) | [domains/d7-batch-assembly.md](domains/d7-batch-assembly.md) | **`specified`** 2026-11-10; L1 and L12 fixed by D7F; L4/L5 amended and L15 added 2026-11-12, built by D7O |
| 9 | **C ABI shim** | the C ABI (`c-abi/`) that language bindings load, in two variants built from one source: the edge variant (no Arrow C++, ever: it must run on embedded devices) and a server-tier variant that adds domain 7's batched subscribe (owner rulings, 2026-11-12); both answer a variant query, and the edge build refuses a batched subscribe by name; the server variant ships in its own .NET package; it hosts domain 2's row codec (owner rulings, 2026-11-13) | [domains/d9-c-abi-shim.md](domains/d9-c-abi-shim.md); `c-abi/include/fletcher/abi/binding.h`; BIND decisions in [plans/BIND-csharp-bindings.md](../plans/BIND-csharp-bindings.md). The two-variant shim partially supersedes BIND Decision 6 (a shim without Arrow C++): the server variant carries Arrow C++, a cost the owner was told when ruling; the shim round annotates Decision 6 (D7O close, 2026-11-12, from `docs/archive/D7O/D7O-pre-round-audit.md` R-4), and D-BIND-13 and -17 (revised 2026-11-13) | **`specified`** 2026-11-13; the server variant, the variant query and the edge refusals not built |
| 10 | **C# binding** | the C# packages that load the shim (`Eiva.Fletcher`, `.Interop`, `.Model`). The C# gateway client stays with domain 5, generated C# with domain 1 (owner ruling 2026-11-12: each language binding is its own domain) | [plans/BIND-csharp-bindings.md](../plans/BIND-csharp-bindings.md) | `anticipated`: created by the C# switch round |
| 8 | **Row bookkeeping** | per-row information that is not business data, starting with Surge's row index: its declaration at topic creation, its single writer, its delivery beside the batch | [plans/RIX-row-index.md](../plans/RIX-row-index.md) (plan, not a spec) | `draft`: not built. Created by round RIX |

The numbers are labels, not a ranking. They differ from the first proposal: the application tier
(proposed as domain 5) was withdrawn, so the gateway and the driver ABI moved up to 5 and 6.

## Planned order of work (owner rulings, 2026-11-10)

1. **Domains 2 and 7 specified:** done 2026-11-10.
2. **Domain 7 fix round:** L1's violations (drops never reported) and L12's (batches delivered
   concurrently and out of order).
3. **RIX:** creates domain 8, and deletes the PR #104 trailing-field helpers.

**Added 2026-11-12** (owner rulings, [plans/RIX-rulings.md](../plans/RIX-rulings.md)): C# moves onto
domain 7's batcher, and its managed copy is deleted. The rounds, one domain each:

4. **D7O** (domain 7): a batched subscription may have no timeout; out-of-range options are refused
   (L4, L5, L15).
5. **Shim round** (creates domain 9): the shim's server-tier variant exposes the batched subscribe.
6. **C# switch round** (creates domain 10): C#'s `SubscriberArrow` runs on it; the managed batcher is
   deleted after its tests are checked for cases domain 7's suite lacks. C#'s batched subscribe does
   not ship to `main` (PR #129) before this lands. If shipping becomes urgent, the question returns
   to the owner; it is re-opened, not assumed (ruling 2026-11-12, "No fixes; don't ship it first").

Deferred: protoc's refusal of `fletcher.` metadata keys (domain 1 code, d2-row-encoding Q3a), until
domain 1 is specified (owner ruling, 2026-11-10).

## Where this map is incomplete

- **Only domains 2, 7 and 9 are `specified`** (9 since 2026-11-13). The other existing documents predate this map and state their rules as
  prose or as spec clauses, not as numbered laws marked with build state. Until one of those
  domains is promoted, no round may run in it.
- **Schema metadata keys are split between two owners.** Keys that are not data model
  (`fletcher.row_index*`) belong to domain 2. Keys generated from a `.proto` (`proto_package`,
  `proto_message`, `field_number`, `metadata_from_option` keys) stay with domain 1 (confirmed
  2026-11-10). Domain 3 transports all of them and compares their bytes, without owning what any
  key means.
- **Language bindings are only named.** Domain 10 is `anticipated` (2026-11-12); domain 9, the shim
  under them, is `specified` (2026-11-13). The C#
  binding now lives in this repo (`dotnet/`, merged at `ff5be3e`). Rust has only generated
  accessors. The C# gateway client batches on its own (register BC-9), unassigned until domain 5 is
  specified.
- **Rulings awaiting domain 10.** These 2026-11-12 rulings, all about C#, stay in the ledger until
  domain 10 has a document: no interim fixes to the C# batcher; "C# keeps its current behaviour" (no
  timeout); C# `MaxRows < 1` throwing after the switch ("Refuse both"); a C# late schema follows
  domain 7. (D7O close, 2026-11-12.)
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
| The C surface bindings load: handles, status containment, version handshake, single-copy check, built-in registry | `c-abi/include/fletcher/abi/binding.h`, `c-abi/src/binding.cpp`, `containment.*`, `handles.hpp`, `single_copy.*`, `builtins.cpp` | 9 |
| Binding-tier row codec (encode/decode over nanoarrow) | `c-abi/src/nanoarrow_codec.*` (hosted by domain 9) | 2 |
| Server-tier batched subscribe across the C ABI, variant query | not built | 9 (over 7) |
| Surge row index | not built | 8 |
