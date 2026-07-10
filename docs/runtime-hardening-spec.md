# Runtime & PubSub Hardening (HARD) — Specification

Correctness / safety round over the **runtime** (arrow-bridge, core) and the
**pubsub layer** (pubsub, fastdds-pubsub-provider, xrcedds-pubsub-provider). It
closes a set of GitHub issues migrated from the old README TODO list, plus one
residual hazard uncovered while triaging them. **The protoc generator and
generated code are out of scope** (a separate "GEN" round owns those).

Execution plan / tracker: [plans/HARD-runtime-pubsub-hardening.md](../plans/HARD-runtime-pubsub-hardening.md).
Locked decisions: [plans/HARD-locked-decisions.md](../plans/HARD-locked-decisions.md).

## Goal

Bring the runtime wire path and the pubsub providers up to the correctness bar
the rest of the library holds: **no dangling references, no process aborts on
recoverable error, no silently-swallowed or discarded failures, no
use-after-free in provider callbacks**, and remove accumulated small hygiene
debt (`[[nodiscard]]`, duplicated helpers). Every behavioural fix lands with a
negative / malformed-input test that is red before the fix.

## Hard invariants (apply to every item)

- **H-INV-1 — Wire format is byte-identical.** No change to the encode→decode
  byte format. Fixes change *ownership*, *error signalling*, or *code shape* —
  never the bytes on the wire. Existing round-trip / byte-identity tests stay
  green.
- **H-INV-2 — Recoverable errors throw; they do not abort.** Replace
  abort-on-error (`.ValueOrDie()`, discarded status codes) with a thrown
  exception. `std::invalid_argument` is the house type for bad/untrusted input
  (matches `arrow-bridge/src/row_reader.hpp:38` and the ~14 existing throw sites
  in `codec.cpp`); `std::runtime_error` for internal-invariant failures.
- **H-INV-3 — No exception escapes a DDS/XRCE callback or session-pump thread.**
  Provider fixes must not let an exception propagate out of a middleware
  callback or the run-loop; doing so can tear down the process. Diagnostics are
  logged/captured, not rethrown across those boundaries.
- **H-INV-4 — Public API signatures are unchanged** except for adding
  `[[nodiscard]]` (additive) and, where a fix needs to report a cause, an
  internal diagnostic. No new required parameters, no changed return types.
- **H-INV-5 — Runtime + pubsub only.** No edit to `protoc/` or generated
  `.fletcher.*` output. The 25 generator-emitted `.ValueOrDie()` sites (part of
  #53), #55, and #59 belong to the GEN round. Touching the generator is a
  stop-and-ask.

---

## Item specifications

Each item cites the **current** file:line (the issue line numbers are stale —
the pubsub `Driver` was split into `Publisher`/`Subscriber` and PR #96 rewrote
the generator).

### HARD-1 — `DecodeScalarFromReader` decode correctness (#52 + #58)

Two defects in one function, `arrow-bridge/src/scalar_codec.cpp`
`DecodeScalarFromReader`:

- **#52 (memory safety, dangling reference).** The `FIXED_SIZE_BINARY` case
  ([scalar_codec.cpp:272-278](../arrow-bridge/src/scalar_codec.cpp#L272-L278))
  wraps the reader's transient pointer in a **non-owning** buffer:
  `std::make_shared<arrow::Buffer>(ptr, byte_width)` (`:276`). The returned
  `FixedSizeBinaryScalar` aliases the caller's input buffer and **dangles** once
  that buffer is freed. Every other variable-length case copies — the
  string/binary block copies via `arrow::Buffer::FromString(...)` (`:231-232`),
  and decimals read into value types. Fix: copy into an owned buffer, mirroring
  the string branch. Distinct from the (resolved) encode-side over-read that
  Arrow's size CHECK already guards — this is the decode path.
- **#58 (dead code / fall-through).** The string/binary block's inner `switch`
  ([:233-248](../arrow-bridge/src/scalar_codec.cpp#L233-L248)) has each of the
  six string ids `return`, but the inner `default: break;` (`:246-247`) and the
  outer `break;` (`:249`) fall through to a function-tail `throw` at
  [:317](../arrow-bridge/src/scalar_codec.cpp#L317) that duplicates the `default`
  case's throw (`:313-315`) and is **unreachable** (the outer case labels have
  already narrowed `type->id()` to the six handled ids). Fix: make the block
  self-terminating (no fall-through to an unreachable tail) and remove the
  duplicate `:317` throw, keeping the genuinely-reachable `default` throw. The
  function must not be able to fall off the end.

**Acceptance.** A decoded `FixedSizeBinary` scalar owns its bytes: decode, then
destroy/overwrite the source buffer, and the scalar still reads the original
bytes. Every string/binary/fixed-size-binary variant round-trips. An
unsupported Arrow type still throws (the `default`, reachable), never aborts.

### HARD-2 — Checked Arrow `Result<T>` access in the runtime (#53, runtime half)

`.ValueOrDie()` aborts the process on error. Replace it in the **hand-written
runtime** with a checked access that throws per H-INV-2:

- `arrow-bridge/src/codec.cpp` — **9 sites** (lines 96, 139, 153, 225, 238, 281,
  291, 306, 307).
- `arrow-bridge/include/fletcher/arrow_bridge/arrow_row_view.hpp` — **4 sites**
  (lines 74, 108, 145, 238).

Use `.status()`/`.ok()` (or a small `ValueOrThrow` helper) → throw
`std::invalid_argument` with a message naming the operation. Prefer one shared
helper over 13 ad-hoc guards (feeds HARD-7's dedup discipline).

**Out of scope here:** the 25 `.ValueOrDie()` patterns the **generator emits**
into generated code (`protoc/src/generator.cpp`) — GEN round (H-INV-5).

**Acceptance.** A runtime path that previously aborted on a bad
`Result<T>` now throws `std::invalid_argument`; a negative test drives at least
one such path (e.g. a builder/finish or `GetScalar` failure) and asserts the
throw. No happy-path behaviour changes.

### HARD-3 — Surface discarded / swallowed errors (#54 + #60)

Two independent silent-failure sites, same discipline (a failed operation must
leave a diagnostic):

- **#54 — `OwnedSchema::DeepCopy` discards its status.**
  [owned_schema.hpp:53-57](../pubsub/include/fletcher/pubsub/owned_schema.hpp#L53-L57)
  calls `ArrowSchemaDeepCopy(src, copy.get())` and ignores the returned
  `ArrowErrorCode`. On failure the schema is left released/empty and
  `MakeSharedSchema` later returns `nullptr` with no diagnostic — a failed copy
  is indistinguishable from success. Fix: capture the code; throw
  (`std::invalid_argument` / `std::runtime_error`; the header already includes
  `<stdexcept>`). **Fix in place** — the file move (#21) is a deferred round
  (locked decision H-8).
- **#60 — `catch(...)` in `FletcherTopicType::serialize` swallows everything.**
  [fast_dds_pubsub_provider.cpp:177-180](../fastdds-pubsub-provider/src/fast_dds_pubsub_provider.cpp#L177-L180)
  zeroes `payload.length` and returns `false` with no trace of the cause. Fix:
  add a `catch (const std::exception& e)` that captures `e.what()` into a
  logged/stored diagnostic *before* falling through to the `false` return; keep
  the catch-all as a last resort. **Must not rethrow** out of `serialize`
  (H-INV-3 — DDS calls this on its own path).

**Acceptance.** `DeepCopy` of a schema whose copy fails throws with a message
(negative test); a `serialize` that fails surfaces a diagnostic
(`what()`-derived) and still returns `false` without propagating.

### HARD-4 — Provider lifetime & callback re-entrancy (#63 + #62 residual)

- **#63 — FastDDS destructor iterates topics without the lock.**
  [fast_dds_pubsub_provider.cpp:441-460](../fastdds-pubsub-provider/src/fast_dds_pubsub_provider.cpp#L441-L460):
  the `for (auto& [name, ts] : impl_->topics)` loop (`:444`) deletes DDS entities
  (`:447-452`) with no `lock_guard(impl_->mu)` held, while `Publish` holds `mu`
  (`:538`) and calls `ts.writer->write()` (`:559`). Fix: the **primary contract**
  is a documented "no concurrent API calls in flight during destruction"
  precondition (a lock alone cannot cure use-*during*-destruction); add the lock
  as defence-in-depth **only if it cannot deadlock** with the callback-outside-
  lock discipline (H-INV-3, locked decision H-4).
- **#62 residual — re-entrant `Unsubscribe` UAF in XRCE `OnTopic`.** The reported
  deadlock is already resolved (recursive mutex; issue closed). Residual: `OnTopic`
  holds `auto& ts = tit->second` ([xrce_dds_pubsub_provider.cpp:216](../xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp#L216))
  and invokes the user callback (`:235` data path; `:201-207` schema-flush path,
  which then touches `ts.pending` at `:207`). A callback that `Unsubscribe`s its
  own delivering topic erases that `TopicState`, dangling `ts` → use-after-free.
  Fix (narrow): copy the fields needed after the callback (or otherwise avoid
  holding a `TopicState&` across the callback invocation) so a re-entrant erase
  is safe. **Do NOT** re-architect XRCE to run callbacks outside the lock — the
  deadlock is gone and that is a larger change (locked decision H-10).

**Acceptance.** #63: the destructor's contract is documented and any added lock
is shown deadlock-free; a test exercises destruction with the documented
precondition. #62-residual: a subscriber whose callback unsubscribes its own
topic mid-delivery does not use-after-free (test drives re-entrant `Unsubscribe`
from within a delivery and completes cleanly).

### HARD-5 — Document "last callback after Unsubscribe" (#65)

**Docs-only; verified by review, not test-gated.** `Subscriber`'s fan-out
snapshots `(id, callback)` pairs under `mu`, releases the lock, then invokes them
([subscriber.cpp:52-74](../pubsub/src/subscriber.cpp#L52-L74)); a subscriber that
unsubscribes between the snapshot and the call receives one final message. This
is intentional. Document it on the `Unsubscribe` doc comment
([subscriber.hpp:60-62](../pubsub/include/fletcher/pubsub/subscriber.hpp#L60-L62)).

**Acceptance.** The `Unsubscribe` doc comment states the one-final-message
guarantee and why (copy-then-release-then-call). No code change.

### HARD-6 — `[[nodiscard]]` on public API (#56)

Zero `[[nodiscard]]` annotations exist today. Silently discarding these return
values is always a bug. Annotate the **current** declarations (the issue's
target list is remapped: `PubSub`/`Driver`/`HasTopic` no longer exist):

| Function | Current header:line |
|---|---|
| `PositionalReader::IsNull(int)` | `core/.../positional_io.hpp:267` |
| `Subscriber::Subscribe(...)` | `pubsub/.../subscriber.hpp:58` |
| `PubSubProvider::Subscribe(...)` | `pubsub/.../provider.hpp:103` |
| `Publisher::ListTopics()` | `pubsub/.../publisher.hpp:42` |
| `Codec::EncodeRow(...)` | `arrow-bridge/.../codec.hpp:48` |
| `Codec::DecodeRow(...)` (both overloads) | `arrow-bridge/.../codec.hpp:50-51` |
| `SerializeEnvelope(...)` | `core/.../envelope.hpp:39` |
| `DeserializeEnvelope(...)` (both overloads) | `core/.../envelope.hpp:74, 117` |
| `OwnedSchema::DeepCopy(...)` | `pubsub/.../owned_schema.hpp:53` |

**Acceptance.** A dedicated translation unit that discards each annotated
return value fails to compile under the suite's warning-as-error setting (or an
equivalent forcing check); all components still build. Additive only (H-INV-4).

### HARD-7 — Consolidate duplicated helpers (#57)

Refactor — **behaviour-preserving, output byte-identical** (H-INV-1). Three
helpers, three states:

1. **`JoinSegments` — partly extracted.** The shared helper exists at
   `pubsub/include/fletcher/pubsub/internal/segments.hpp:17` and is used by
   pubsub core + the FastDDS provider. Byte-identical copies remain in
   `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp:55-63`,
   `pubsub-arrow/src/publisher_arrow.cpp:80-90`, and
   `pubsub-arrow/src/subscriber_arrow.cpp:444`. Both those components already
   depend on pubsub → delete the copies, include the header. Test/gateway mock
   `Join` (`test_publisher_subscriber.cpp:75`, `test_pubsub_arrow.cpp:76`,
   `gateway/src/main.cpp:127`) point at the shared helper too.
2. **`AppendFixed<T>` — byte-identical dup.**
   `arrow-bridge/src/codec.cpp:26-29` and
   `arrow-bridge/src/scalar_codec.cpp:18-22` (different anonymous namespaces,
   intra-component). Move into a shared arrow-bridge `detail` header
   (e.g. `scalar_codec.hpp`).
3. **`BitfieldBytes` — divergent dup.**
   `arrow-bridge/src/codec.cpp:34` takes `int64_t` (deliberate overflow-safety,
   per its comment); `core/.../positional_io.hpp:140` and `:418` take `size_t`
   (two identical copies within one file). Consolidate to a single `core`
   definition, **preserving the `int64_t` overflow-safe intent** on the codec
   path (do not silently narrow) and collapsing the two positional_io copies.

**Acceptance.** Each helper has one definition; all duplicate copies deleted; the
full suite (including integration) stays green; no generated/wire output changes.

---

## Sequencing & coupling

Linear; each item's forcing test 🟢 before the next. Order isolates the memory
bug first and puts the refactor last so it dedups the *final* code:

```
HARD-1 (scalar_codec) → HARD-2 (ValueOrDie runtime) → HARD-3 (silent errors) →
HARD-4 (provider lifetime) → HARD-5 (docs) → HARD-6 (nodiscard) → HARD-7 (dedup)
```

- HARD-1 and HARD-2 both touch arrow-bridge but different functions.
- HARD-3 (`owned_schema.hpp` DeepCopy) and HARD-6 (annotates `DeepCopy`) touch
  the same declaration — HARD-3 first (body), HARD-6 second (annotation).
- HARD-7 touches `codec.cpp`/`scalar_codec.cpp` (AppendFixed, BitfieldBytes) —
  after HARD-1/HARD-2 so it consolidates settled code.

## Out of scope

- **Generator / generated code** (H-INV-5): the 25 emitted `.ValueOrDie()`
  sites, #55 (silent `// TODO` for unsupported types), #59 (GeoArrow CRS) → GEN
  round.
- **#75** (emit C++ enum symbols) — dedicated feature round.
- **#21** (move `OwnedSchema` out of pubsub) — dedicated architecture round;
  `owned_schema.hpp` stays in pubsub this round (locked decision H-8).
- Re-architecting XRCE dispatch to invoke callbacks outside the lock (H-10);
  the deadlock (#62) is already resolved.
- Reader-bounds / oversized-length hardening against untrusted wire bytes — that
  is the separate `feature/robustness_improvements` Phase-1 work-stream (see
  [docs/robustness-plan.md](robustness-plan.md)); this round does not duplicate
  or depend on it.

## Closed during triage (verified fixed, not part of this round)

- **#64** (TOCTOU in `CreateTopic`) — fixed by the `Publisher` split
  (`publisher.cpp:44-75` claims the key under the lock).
- **#61** (XRCE session data race) — fixed by the single coarse
  `std::recursive_mutex` the run-loop and all API methods share.
- **#62** (deadlock) — resolved by the recursive mutex + single-thread pump; only
  the narrow UAF residual (folded into HARD-4) remained.
