# RIX — Row Index for Surge LogTables — Plan

Round plan for carrying a per-row index (Surge's `_ingest_offset`) on Fletcher topics that declare
one, **without** putting it in the `.proto` data model and **without** changing the wire format.

Status: **draft — not started.** Branch: `feature/row-index`, off `main`, rebased before the PR.

**Prerequisites:**
- Domains 2 and 7 are specified: [docs/domains/](../docs/domains/), owner review pending.
- A domain 7 fix round has fixed L1 (drops never reported) and L12 (batches delivered out of
  order), which this round's subscriber side depends on.

The runbook (config, test ladder, tracker) is written by `round-designer` once those are done.
Until then the stage table below is a sketch.

## Why

Surge stores sensor data in **LogTables**. Every row in a LogTable has an `_ingest_offset`: a dense
`UInt64` row number with no gaps. Surge accepts a batch only if its offsets start at the table's
frontier and have no gaps, and the producer's durable Parquet files (`part-<start>-<end>.parquet`)
must cover the same offsets. The producer (e.g. Perception's drivers) already numbers its rows for
its own WAL and Parquet files, so that number has to travel through Fletcher to whoever writes it
into Surge.

Two constraints shape the design:

1. **The index is not business data.** It is bookkeeping for Surge and its ecosystem, so it must
   not appear in the `.proto` data model. It must not appear in the data model's Arrow schema as
   a column either: a generated RecordBatch accessor refuses a batch whose column count differs
   from the message's field count (D-RBA-4, [docs/recordbatch-accessor-spec.md:159](../docs/recordbatch-accessor-spec.md#L159)).
2. **Whether a topic carries an index is decided when the topic is created**, not per sample.

Reference (not part of this round): the Surge LogTable contract is on Monorepo `main`
(`modules/Surge/crates/surge-table`, `modules/Surge/docs/architecture/domains/d1-logtable-ingest.md`).
The Fletcher → Surge ingester is on Monorepo branch `feature/surge_ingester_fletcher/22555`; today it
strips a driver-supplied `_ingest_offset` *column* (commit `5358661`). SurgeClient's side is owned by
another team.

## Design in one paragraph

The topic's publisher declares a row index by naming a **writer id** at `CreateTopic`. The
Publisher records that declaration as two keys in the topic's **Arrow schema metadata** (never as
a field), so it reaches every subscriber through the existing `__schema` announcement and takes part
in the existing schema-conflict check. Each published row carries one **reserved attachment** holding
a version byte, the index and a hash of the writer id. The envelope, the DDS type and the row codec
are unchanged. On the receive side, the Arrow tier removes the reserved attachment, refuses rows from
any other writer, and hands the index back as a `UInt64Array` that lines up with the batch. A
one-call helper merges it into an `_ingest_offset` column for consumers that want it in the batch.

## Locked decisions

A proposed deviation from any of these is stop-and-ask.

- **D-RIX-1 — No wire-format change.** The envelope (`[ROW_LEN][ROW][ATT_COUNT]{attachment}*`),
  the FastDDS `FletcherSample` type, XRCE serialization, the gateway frames and the row codec are
  byte-identical to today. The round adds *conventions*: one reserved attachment key and two schema
  metadata keys, documented in the wire spec.
- **D-RIX-2 — Not in the data model.** No `.proto` change, no protoc change, no Arrow field. The
  declaration lives in schema *metadata*; the per-row value lives in an *attachment*.
- **D-RIX-3 — No provider changes.** Everything is done in the Publisher/Subscriber tiers
  (`pubsub`, `pubsub-arrow`). The seam struct `TopicOptions` is **not** extended; the declaration is
  a separate Publisher-tier parameter, so providers never see it and the `TopicOptions::empty()`
  rule ("a provider with no notion of a field refuses it `kNotSupported`") is untouched.
- **D-RIX-4 — The publisher supplies the index; Fletcher never numbers rows.** The index must
  survive producer restarts and match the producer's own Parquet files, which only the producer can
  guarantee.
- **D-RIX-5 — No counting check.** Fletcher does not check that indexes are dense or increasing.
  It carries what it is given. (Owner decision, 2026-11-10.)
- **D-RIX-6 — One writer per indexed topic, and a second writer is refused.** (Owner decision,
  2026-11-10.) A writer is identified by its **writer id**, which is stable across restarts of the
  same producer. Mechanism under §Refusing a second writer.
- **D-RIX-9 — The writer id is required.** Declaring a row index without a non-empty writer id
  is `kInvalidArgument`. A random default would make every producer restart look like a second
  writer. (Owner decision, 2026-11-10.)
- **D-RIX-10 — The writer hash is 8 bytes** (FNV-1a 64). (Owner decision, 2026-11-10.)
- **D-RIX-11 — The merged column is Surge's: `_ingest_offset`, `UInt64`, column 0.** This is the
  name and position Surge's LogTables register (SurgeClient README, "Tables": "the batch's schema
  plus `_ingest_offset` (UInt64) as the first column"). It is a fixed constant, not a parameter, so
  it cannot drift from Surge. (Owner decision, 2026-11-10.)
- **D-RIX-12 — RIX deletes the trailing-field helpers.** `AppendTrailingUint64Field` and
  `ReadTrailingUint64Field` (`core/include/fletcher/core/positional_io.hpp:158-260`, PR #104) carry
  `_ingest_offset` as a trailing schema field, which the first ruling forbids. RIX deletes them and
  the tests that pin them (`core/tests/test_positional_io.cpp:86-203`,
  `arrow-bridge/tests/test_codec.cpp:752-831`). Those tests' subject is now forbidden, so they are
  deleted, never re-anchored. Monorepo calls the helpers in production
  (`data-acquisition.drivers/modules/RowBatcherFletcher/src/FletcherRowCodec.cpp`), so the round
  ships as a breaking `fletcher-core` 0.4 release, and those callers stay on 0.3 until they move.
  (Owner decision, 2026-11-10: "1. (a) RIX should delete them".)
- **D-RIX-13 — The row index is the caller's.** Fletcher never supplies, renumbers, reorders, fills or
  checks an index. Keeping indexes compact and in sequence is the caller's responsibility. (Owner
  ruling, 2026-11-10; domain 7 L14.)
- **D-RIX-7 — Reserved attachments are invisible at the Arrow tier.** `SubscriberArrow` strips them
  from the per-row `Attachments` it delivers. The raw `Subscriber` tier passes attachments through
  unchanged, as it passes everything else, and gets a parse helper instead.
- **D-RIX-8 — Generated accessors keep working.** The index is delivered *beside* the batch by
  default. Merging it into the batch is an explicit call whose result is documented as not
  accessor-compatible.

## The conventions

### Topic declaration: schema metadata

Set by `Publisher::CreateTopic` when a row index is declared; never written by users.

| Key | Value |
|---|---|
| `fletcher.row_index` | `"1"`: the version of the per-row attachment layout below |
| `fletcher.row_index.writer` | the writer id, UTF-8, non-empty |

These keys take part in the schema bytes, so the existing conflict check covers them for free.
An indexed declaration against an unindexed one conflicts, as does one writer id against another.

### Per-row value: reserved attachment

- **Reserved key prefix:** any key starting with byte `0x01` belongs to Fletcher.
  `Publisher::Publish` refuses caller-supplied attachments with that prefix (`kInvalidArgument`).
  `Attachments` itself is not changed. The existing `max_payload_bytes` key on the `__schema`
  channel doesn't start with `0x01` and is unaffected.
- **Key:** `"\x01" "f"` (two bytes).
- **Blob, version 1, 17 bytes, little-endian:**

  | Offset | Size | Field |
  |---|---|---|
  | 0 | 1 | version = `1` |
  | 1 | 8 | row index (`uint64`) |
  | 9 | 8 | writer hash: FNV-1a 64 of the writer id |

- **Per-row cost:** 4 (key length) + 2 (key) + 4 (blob length) + 17 = **27 bytes**. A later version can add fields
  (e.g. a source timestamp) behind the version byte without a new key.

## Refusing a second writer

DDS has no central authority, so "refuse" means two different things depending on where the
second writer is.

1. **Same provider instance (in-process, deterministic).** A second `CreateTopic` of an indexed
   topic with a different writer id produces different schema bytes. Every provider's existing
   check refuses that with `kSchemaConflict`. The same writer id re-declaring is idempotent, as
   today, and is treated as the same writer.
2. **Another process.** The second writer's `CreateTopic` cannot be refused at the moment it is
   called: the first writer may not have been discovered yet, and Fletcher's providers refuse only
   local conflicts. The refusal happens **at every subscriber instead**. A subscriber knows the
   writer id from the schema it follows, and drops every row whose writer hash does not match,
   counting it in a new `BatchStatus::rows_foreign_writer` and logging once per topic. This is the
   guarantee that matters: **no Arrow batch handed to a consumer ever mixes two writers' indexes.**
3. **Restart.** A restarted producer re-declares with the same writer id, produces identical schema
   bytes, and its rows are accepted at once. That is why the writer id is supplied by the producer
   and is not random.

Accepted limits, to be documented:

- Two processes using the **same** writer id are indistinguishable and both accepted. This is
  misconfiguration, as with Surge's appender ids.
- If both writers are live, a subscriber that sees the second writer's announcement first follows
  the second writer. FastDDS already keeps the first announcement and logs a conflict
  (`fast_dds_pubsub_provider.cpp:388`). Two live writers are an error either way, and it is reported.
- The gateway shares one `Publisher` across all WebSocket sessions and passes no options at
  `create_topic`, so **indexed topics cannot be created through the gateway in this round**.
  Gateway subscribers receive the reserved attachment as an ordinary attachment (raw tier).

## API sketch

Names are proposals; the shapes are what this round commits to.

```cpp
// pubsub/include/fletcher/pubsub/row_index.hpp (new)
struct RowIndexDeclaration { std::string writer; };   // empty writer == no row index
struct RowBookkeeping { uint64_t index; uint64_t writer_hash; };
std::optional<RowBookkeeping> ReadRowBookkeeping(const Attachments&);  // raw-tier helper

// Publisher (pubsub)
void CreateTopic(segments, OwnedSchema, const TopicOptions& = {}, const RowIndexDeclaration& = {});
void Publish(segments, const RowEncoder&, uint64_t row_index, const Attachments& = {});
//   indexed topic + Publish without an index  -> kInvalidArgument
//   unindexed topic + Publish with an index   -> kInvalidArgument

// PublisherArrow (pubsub-arrow): the same overloads for CreateTopic, Publish, PublishDirect.

// SubscriberArrow (pubsub-arrow)
using IndexedBatchCallback = std::function<void(std::shared_ptr<arrow::RecordBatch> batch,
                                                std::shared_ptr<arrow::UInt64Array> row_index,
                                                std::vector<Attachments> attachments,
                                                BatchStatus status)>;
SubscribeResult SubscribeIndexed(segments, IndexedBatchCallback, BatchOptions = {}, TopicOptions = {});

// Zero-copy merge: the index becomes column 0, named kRowIndexColumnName (D-RIX-11).
// Not accessor-compatible (D-RIX-8).
inline constexpr std::string_view kRowIndexColumnName = "_ingest_offset";
std::shared_ptr<arrow::RecordBatch> WithRowIndexColumn(
    const std::shared_ptr<arrow::RecordBatch>& batch,
    const std::shared_ptr<arrow::UInt64Array>& row_index);
//   refused (kInvalidArgument) if `batch` already has a column named kRowIndexColumnName
```

What subscribers see:

- **The existing batched `Subscribe` on an indexed topic:** rows delivered as today, the reserved
  attachment stripped, the index discarded, foreign-writer rows refused.
- **`SubscribeIndexed` on an unindexed topic:** every row is counted in `rows_dropped`, and the
  mismatch is logged once when the schema arrives.
- **`SubscribeIndexed` on an indexed topic, row without the reserved attachment or with a malformed
  blob:** counted in `rows_dropped`, like a row that fails to decode.

`BatchStatus` gains `int64_t rows_foreign_writer = 0`. This is source-compatible; it changes the
layout of a public struct, so note it in the changelog.

## Stages

Each stage lands green with its own tests before the next starts.

| # | Stage | Scope | Done when |
|---|---|---|---|
| S1 | **Bookkeeping codec and reserved prefix** | New `row_index.hpp/.cpp` in `pubsub`: key constant, encode/decode of the version 1 blob, FNV-1a 64, `ReadRowBookkeeping`. `Publisher::Publish` refuses caller keys with the `0x01` prefix. | Unit tests: round-trip; wrong size, version or key refused; prefix refusal; existing `max_payload_bytes` announcements unaffected. |
| S2 | **Declaring the topic and publishing (core)** | `RowIndexDeclaration` parameter on `Publisher::CreateTopic`: validate the writer id (non-empty UTF-8), write the two metadata keys into the schema before the conflict check, store the hash per topic. `Publish` overload with index; mismatch refusals. | In-process tests: index arrives in the attachment; same writer re-declares idempotently; different writer refused `kSchemaConflict`; indexed against unindexed refused; Publish-mismatch refusals. |
| S3 | **Arrow publishing** | `PublisherArrow` overloads forwarding to S2. | Round-trip test through in-process with `ArrowRow`. |
| S4 | **Arrow subscribing** | `RecordBatchBatcher`: reads `fletcher.row_index*` from the schema at `SetSchema`; per row, removes the reserved attachment, checks the writer hash, appends to a `UInt64Builder`; `rows_foreign_writer`; `SubscribeIndexed`; `WithRowIndexColumn`. | Tests: index aligned row for row across row-limit, timeout and closing flushes, including with dropped rows; foreign-writer rows refused and counted; the cases under "What subscribers see"; a generated C++ accessor accepts the `SubscribeIndexed` batch; `WithRowIndexColumn` is zero-copy (same buffer address), puts `_ingest_offset` `UInt64` at column 0, and refuses a batch that already has that column. |
| S5 | **Cross-provider conformance** | Extend `integration-tests/pubsub-conformance` with an indexed-topic subject for in-process, FastDDS and XRCE. | Index survives each transport; two processes with different writer ids: the second's rows refused at the subscriber; a restart with the same writer id is accepted with no gap; payloads are byte-identical to an unindexed topic apart from the attachment (proves D-RIX-1). |
| S6 | **Docs** | [docs/wire-format-specification.md](../docs/wire-format-specification.md): new "Reserved attachments" and "Row index metadata" sections stating the envelope is unchanged; [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md) §3.2: reserved prefix; `pubsub-arrow` README: `SubscribeIndexed`, `WithRowIndexColumn`, the accessor caveat; the limits listed above. | Docs reviewed against the code. |
| S7 | **Delete the trailing-field helpers** (D-RIX-12) | Remove `AppendTrailingUint64Field` / `ReadTrailingUint64Field` and the tests that pin them; bump `fletcher-core` to 0.4; release note pointing Monorepo's callers to row bookkeeping. | Neither symbol exists in the tree; the full suite is green. |


## Out of scope (follow-ups)

- **Gateway and TS client:** creating indexed topics over WebSocket, and a TS helper for the
  reserved attachment.
- **C# binding:** the `Eiva.Fletcher` wrapper used by the Monorepo SurgeIngester lives outside
  this repo. It needs `SubscribeIndexed` (or the merged batch) exposed over the C Data Interface.
- **Producers:** the drivers move `_ingest_offset` from their schema to `RowIndexDeclaration` plus
  the `Publish` index. The SurgeIngester then stops stripping a column and starts using the
  delivered index, coordinated with the SurgeClient team.
- **A counting check** (D-RIX-5): may be added later as an opt-in subscriber-side option.

## Open questions

None open. The writer id, writer hash width and merged column were settled on 2026-11-10 as
D-RIX-9, D-RIX-10 and D-RIX-11.
