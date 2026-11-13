# Domain 9: The C ABI shim

> **Draft, not specified.** No law below is confirmed. The owner confirms them before any stage of
> the shim round is designed (round-designer Gate 2, shape 2: "creates a new domain"). Laws marked
> **PROPOSED (owner confirmation owed)** describe the server-tier variant, which does not exist yet.
> The surveyed laws describe what the code does today; confirming them makes them law.

**Mode: survey for everything that exists, verified against the tree at `180b2d7`** (branch
`feature/shim-server-variant`). The code is ground truth. `binding.h`'s comments, `c-abi/README.md`,
the BIND plan ([BIND-csharp-bindings.md](../../plans/BIND-csharp-bindings.md)) and its digest
([BIND-locked-decisions.md](../../plans/BIND-locked-decisions.md)) are claims checked against it.
**Round mode for the server-tier variant:** written from the 2026-11-12 rulings
([RIX-rulings.md](../../plans/RIX-rulings.md)), marked `[designed-not-built]`. Citations:
`h:N` is [`c-abi/include/fletcher/abi/binding.h`](../../c-abi/include/fletcher/abi/binding.h),
`cpp:N` is [`c-abi/src/binding.cpp`](../../c-abi/src/binding.cpp); other files are named. Tests are
in `c-abi/tests/`, named `Suite.Case`.

## 1. Intent

**Owns:** the one pure C surface through which a language binding reaches Fletcher, in **two
variants built from one source** (owner rulings, 2026-11-12). That means: the C spelling of each
seam value (blob, schema, arrival, attachments, write window, topic, status); the handles, and who
releases each; turning every C++ failure into a status plus message at one containment site; the
ABI version and its compatibility rule; the export table; the one-copy-per-process check; hosting
the binding-tier codec (`NanoarrowCodec`) behind `fl_codec_*`; the built-in provider registry the
shim carries; and **which variant a build is**, and what each variant may link.

- **Edge variant** (exists; today's shim): exactly as today. Never carries Arrow C++, because "it
  needs to be able to run on embedded devices" (owner, 2026-11-12).
- **Server-tier variant** `[designed-not-built]`: the edge surface plus `pubsub-arrow`'s batched
  subscribe (domain 7). C# on desktop loads it (domain 10, next round).

**Refuses:**
- **Answering the seam's questions again.** Topic validation, options checks, re-entrancy,
  unsubscribe semantics and timeout meaning are domain 3's; the shim forwards them and forwards
  their refusals unchanged (`cpp:28-39`, `cpp:96-102`, `cpp:871-877`, `cpp:941-946`). One exception
  is recorded: the bound-rows publish check (L4).
- **Batching** (register BC-9). The shim has no window, no count, no timer, no thread. The server
  variant exposes domain 7's assembler; it builds none of its own and decides nothing about when a
  window closes.
- **Validating batch options** (register BC-8). Domain 7's L15 is the one answer; the server
  variant passes the refusal through and does not check `max_rows` or the timeout itself.
- **Decoding policy beyond its codec.** Which schemas a batch can be assembled from, and the
  dictionary re-fold, are domain 2's (`BatchDecoder`). The shim's codec decodes a dictionary as its
  value type and stops there (`nanoarrow_codec.hpp:163-179`, D-BIND-39).
- **Arrow C++ in the edge variant, ever.** (2026-11-12; earlier D-BIND-1a rider ii / BIND Decision 6.)
- **Provider registration and path resolution** from a binding (D-BIND-24): no
  `fl_registry_register`, no resolver (`h:740-749`). A path selector answers `FL_NOT_SUPPORTED`
  until PDA-ABI fills the seat (domain 6).
- **A second native library.** Rejected 2026-11-12: each would carry its own pub/sub core and
  providers, a provider made in one could not be used by the other, and one process would hold two
  DDS stacks.
- **Language idioms.** Mapping statuses to exceptions, `Timeout.Infinite` → `INT64_MAX`, `SafeHandle`
  wrappers, re-entrancy refusals in managed code are each binding's (domain 10 for C#).

## 2. Ubiquitous language

| Term | Definition (as the code behaves) |
|---|---|
| **Shim** | The shared library `fletcher-c-abi` (`CMakeLists.txt:80-83`): the only Fletcher component built as a shared library; statically links pub/sub, the two DDS providers and nanoarrow. |
| **Variant** | One of the two builds of the shim from one source. Today there is one (edge); the test-only probe shim (`CMakeLists.txt:164-211`) is a third artifact from the same sources, not a variant. |
| **Edge variant** | Today's shim: no Arrow C++, codec on nanoarrow. |
| **Server(-tier) variant** | `[designed-not-built]` The edge surface plus the batched subscribe; links Arrow C++ through `pubsub-arrow`. |
| **Entry point** | An exported `fl_*` function. 47 today (`h`, `grep -c FL_ABI_EXPORT`). |
| **Handle** | An opaque `fl_*` struct pointer the caller holds and cannot read (`handles.hpp:6-11`). |
| **Owner handle** | The `owner` of an `fl_blob` or `fl_schema`: a refcounted block reached only through retain/release (`h:398-512`, `handles.hpp:43-59`). |
| **Borrowed / Owned / OUT** | The header's three ownership words (`h:29-42`). Unqualified pointers are borrowed. |
| **Status** | An `fl_status` number: the seam's published taxonomy, number for number (`h:176-234`). |
| **Origin** | `fl_origin`: which containment arm produced a failure (seam, codec, callback); the shim's own field, not a status (`h:236-253`). |
| **Containment site** | `Contain`/`Capture` (`containment.hpp:55-77`, `containment.cpp:56-105`): the one place a C++ exception becomes a status. |
| **Arrival** | `fl_schema_arrival`: the waitable answer for a topic's schema, five outcomes (`h:516-564`). |
| **Delivery** | One call of a binding's `fl_delivery_fn` with one encoded row (`h:851-876`). |
| **Thunk** | The shim's lambda that adapts a seam delivery to `fl_delivery_fn` (`cpp:836-862`). |
| **Marker** | `fl_single_copy_marker`, the exported symbol whose second occurrence in a process poisons the shim (`h:373-394`). |
| **Poisoned** | The latched state in which every contained entry point refuses (`containment.hpp:61-64`). |
| **Handshake** | A binding comparing `fl_binding_abi_version()` to the `FL_BINDING_ABI_VERSION` it was built against (`h:158-172`). |
| **Built-ins** | `inprocess`, `fastdds`, `xrce`, registered by the shim itself (`builtins.cpp:33-42`). |

**Naming findings:**
- **"Window"** has three meanings near this domain: the shim's `fl_write_window` (an encode
  buffer, `h:636-669`); domain 7's window (rows awaiting a batch); and D-BIND-68's "framed window"
  (a C# batch's raw rows). The server variant will put the first two in one header.
- **"Edge"** means two things: GIR-8's edge tier (generated C++ with no Arrow at all, for MCUs,
  D-BIND-1) and this edge variant (nanoarrow, Fast DDS, XRCE). The edge variant is not GIR-8's tier.
- **"Binding"** names both this domain (`binding.h`, "the BINDING ABI", `h:4`) and the language
  bindings over it (domain 10). The owner's name for the domain is "the C ABI shim"; the ruling
  calls it "the native library"; the package is `fletcher-c-abi`.
- **"Copy"** names both a copy of the shim in a process (single-copy, D-BIND-17) and a byte copy
  (zero-copy, the copy oracle, D-BIND-4).
- **Release** has five verbs: `destroy` (provider, publisher, subscriber), `dispose` (error, arrival,
  attachments, builder, string list), `close` (codec), `unbind` (rows), `release` (blob, schema),
  plus the Arrow C Data Interface's own `release` callback, which a binding must never call on an
  `fl_schema` (`h:460-465`).
- **The containment site** is called `Translate()` in D-BIND-19 and D-BIND-31; the code calls it
  `Contain` and `Capture`.

## 3. Invariants and laws

**L1. No C++ exception crosses the ABI.** Every fallible entry point runs its body inside the one
containment site, which catches everything and returns a status. A caller's callback must not throw
into the shim either. `[built]`
- Holds at: `containment.hpp:59-71` (`catch (...)`), every `fl_status` entry point in `cpp:232-992`
  is a `Contain` call (`cpp:20-26` states the rule); `CMakeLists.txt:7-19` builds with `/EHs` so a
  C-linkage helper's throw is not assumed impossible.
- Void entry points (`*_destroy`, `*_dispose`, `*_retain`, `*_release`, the accessors) are not
  wrapped; they do only `delete`, atomics and bounds-checked reads. That they cannot throw is
  `[built as a property; no code owner]`.
- **The designed exception:** `fl_subscriber_destroy` from inside a delivery on the same subscriber
  terminates the process (`cpp:789-801`): the seam's refusal leaves a `noexcept` destructor. The shim
  deliberately does not catch it; a binding refuses the call first (D-BIND-18). This is termination,
  not an exception crossing.
- The other direction (`fl_writer_fn`, `fl_grow_fn`, `fl_delivery_fn`) is the caller's obligation
  (`h:702-705`, `h:858-861`); the shim cannot enforce it.
- Tests: `Containment.AnUnknownExceptionTypeIsContainedAsInternal`,
  `Containment.OverflowIsPayloadTooLargeAtEitherOrigin`, `Containment.TheReadersRefusalTypesDependOnTheOrigin`,
  `BindingEntryPoints.AGrowReturningANonStatusIsContainedAsInternal`. No test drives every entry point
  with a throwing body.

**L2. A failure is a value the caller owns: a seam status number plus its message.** It is never a
global or per-thread slot. The shim invents no status number; the only field it adds is `origin`.
Wait outcomes (`FL_PENDING`, `FL_SUBSCRIPTION_ENDED`) are returned values, never failures.
`[built]`; the C `fl_status` numbers equal the C++ `PubSubStatus` numbers `[built as a property; no test]`
- Holds at: `h:255-316` (`fl_error`, caller-owned message, `fl_error_dispose`),
  `containment.cpp:56-105` (`Capture`: a `PubSubError` keeps its status; `std::overflow_error` →
  `FL_PAYLOAD_TOO_LARGE` by seam §5.1; reader refusals → `FL_INVALID_ARGUMENT` at the codec origin
  only; everything else `FL_INTERNAL` with its `what()`), `cpp:932-985` (outcomes returned, failures
  thrown).
- `Capture` casts `PubSubStatus` to `fl_status` (`containment.cpp:66`). Nothing in `c-abi/` asserts
  the two enums agree; `h:180-183` says a C# port of `test_status_taxonomy` compares the C# enum to
  the published table, and no such test was found under `dotnet/tests` (unsure; see Findings c).
- Tests: `BindingEntryPoints.ErrorDisposeIsSafeOnAZeroedStructAndTwice`, `.SuccessLeavesTheErrorUntouched`,
  `.AGrowThatRefusesKeepsItsStatusMessageAndOrigin`, `.MalformedBytesCarryTheReadersMessageAndTheCodecOrigin`,
  `.AMalformedTopicCarriesTheSeamsRefusal`, `Containment.ANullErrorStillReturnsTheStatus`.

**L3. Every argument is borrowed; every owned thing has exactly one named release; release order
between handles never matters to the shim.** A callee that keeps something copies or retains it,
and never consumes a caller's Arrow structure. OUT parameters are written only on success.
`[built]`; "OUT untouched on failure" `[built as a property; no code owner]`
- Holds at: `h:29-42` (the three words), `handles.hpp:14-22`, `:63-68`, `:142-176` (publisher,
  subscriber and rows hold `shared_ptr`s to what they depend on, so a GC'd binding may finalise in
  any order); `cpp:309-355` (atomic refcounts, safe from any thread); `cpp:645-651` and
  `nanoarrow_codec.hpp:72-80` (schemas deep-copied, arrays borrowed and never released);
  `cpp:825-867` (the arrival is allocated before the subscription goes live, so nothing can throw
  between a live subscription and its id reaching the caller).
- Releasing twice, or releasing what was never received, is undefined (`h:35-37`) and not checked.
- The header is stricter than the code: it calls destroying a provider before its publishers
  undefined (`h:751-753`); the handles make it safe. Not a contradiction; the header states the
  caller's obligation (`handles.hpp:166-169`).
- Tests: `BindingEntryPoints.ClosingTheCodecBeforeUnbindingTheRowsIsSafe`,
  `Subscriber.ARetainedSchemaOutlivesTheDeliveryAndTheProviderItCameFrom`,
  `Attachments.ARetainedBlobOutlivesTheSet`, `Subscriber.ACopiedSchemaOutlivesTheHandleItCameFrom`,
  `Blobs.RetainAndReleaseAreSafeOnEmptyAndNull`, `NanoarrowCodec.BindBorrowsTheArrayAndNeverConsumesIt`.
  No test asserts that a failed call left an OUT parameter untouched.

**L4. The shim forwards the seam's rules; it does not answer them again.** A seam refusal reaches
the caller with the seam's number and message. A timeout crosses as the seam's value, untranslated:
`INT64_MAX` ms is the unbounded form, a negative is the seam's refusal (`h:548-555`).
`[built]`, with **one answer of its own**: the bound-rows publish check.
- Holds at: `cpp:28-39` (the six topic rules are `RequireSegments`'), `cpp:96-102` (topic options),
  `cpp:871-877` (unsubscribe's no-op and no-wait cases), `cpp:941-946` (the arrival timeout is
  forwarded; "negative means forever" belongs to C#, D-BIND-20).
- **The exception:** `RequireRowsMatchTopic` (`cpp:182-222`, D-BIND-60, -65, -67) refuses bound rows
  for a topic this publisher did not declare, or whose wire layout differs. The seam's `Publisher`
  only sees bytes, so it cannot ask; C++ `PublisherArrow` answers the same question for C++ callers.
  That is one question with two sites (Findings b2).
- Tests: `BindingEntryPoints.AMalformedTopicCarriesTheSeamsRefusal`,
  `Subscriber.NullArgumentsAreRefusedAndCancellingNothingIsNotAnError`, the `TopicOptions.*` cases,
  `Subscriber.ASchemaWatchIsRefusedFromInsideADelivery`; the exception: the five `Publisher.*` cases.

**L5. A process holds exactly one shim.** Fletcher's re-entrancy state is a `thread_local` in a
header-only target, so two copies would stop seeing each other's delivery frames. A shim that finds
another module exporting the marker refuses every fallible call, naming both modules.
`[built]` for the detectable case (two shims); the undetectable case (a C++ host that statically
links Fletcher and hosts a binding) is ruled out by fact, not by check (D-BIND-17).
- Holds at: `cpp:168-176` (load-time check), `single_copy.cpp:34-108` (module scan),
  `single_copy.cpp:127-145` (latched once), `containment.hpp:61-64` (refusal before the body runs).
- **Only the later-loaded copy is poisoned.** The check runs once, at each copy's own load; the
  first copy saw one module and stays healthy. Nothing can be created through the second, so the
  forked state is not reachable through it. The header says "REFUSES TO INITIALISE" (`h:382-385`),
  which reads as both.
- Void entry points and `fl_binding_abi_version` still answer in a poisoned shim.
- Tests: `SingleCopy.TheMarkerIsExportedAndIdentifiesTheShim`, `.TheShimItselfIsFound`,
  `.ASecondMarkerExportIsFound` (a decoy module), `.TheAnswerFollowsTheProcess`,
  `.AHealthyProcessIsNotPoisoned`, `Containment.APoisonedShimRefusesWithoutRunningTheCall`,
  `Containment.ClearingTheRefusalRestoresService`.

**L6. A change to the ABI is never silent.** The loaded shim reports its version; within a major
the header is append-only; before 1.0 there is no compatibility guarantee, so a binding refuses a
shim whose minor differs; every change bumps the version and is recorded. An entry point is
exported only once it is implemented, never stubbed to answer `FL_NOT_SUPPORTED`.
`[built]` for the handshake; the append-only and every-change-bumps rules `[built as a property; no test]`
- Holds at: `h:44-90`, `h:163-172`, `binding_version.c:10`; the C# side at
  `dotnet/src/Fletcher.Interop/NativeLoader.cs:253` (`VerifyAbiVersion`). D-BIND-42, -43, -46, -57,
  -68 each bumped the minor (now 0.7).
- "An unimplemented entry point is a LINK ERROR" (`h:79`) holds for a static link. Through P/Invoke
  it is a run-time `EntryPointNotFoundException` at first call. This matters for the variants (L16).
- Tests: `BindingAbi.VersionMatchesHeader`, `BindingAbi.VersionIsPackedMajorMinor`, `test_package/`.

**L7. The surface is pure C and self-contained, and the export table is exactly the surface.** No
consumer needs a Fletcher, Arrow or eProsima header; Arrow structures are forward-declared.
`[built]` for the header; the export-table clause `[built for Linux; violated on Windows]` (accepted,
D-BIND-157)
- Holds at: `h:17-18`, `h:97-108`, `CMakeLists.txt:99-104`, `:138-147` (hidden visibility,
  `--exclude-libs ALL`, the version script `cmake/exports.map:46-51`), `:154-162` (private links),
  `conanfile.py:58-63` (no transitive headers).
- Windows: ConanCenter's static Fast DDS carries `/EXPORT` directives; 3531 names exported
  (`CMakeLists.txt:116-127`, README:44-55).
- Tests: `BindingAbi.CompilesAsC99AndIsSelfContained` (C99 with `-pedantic-errors`; C11 on MSVC),
  `test_package/`; the export set is asserted by a CI step, not by ctest (`ci.c-abi.yml:231-247`, Linux).

**L8. The edge variant links no Arrow C++.** Its codec is nanoarrow; Arrow C++ and `arrow-bridge`
may appear only in test binaries.
`[built as a property; no code owner]`: nothing structural forbids it.
- Holds at: `conanfile.py:58-83` (Arrow and `arrow-bridge` are required only under `run_tests`,
  and `run_tests` is deleted from the package ID), `CMakeLists.txt:154-162` (the shim's links),
  `nanoarrow_codec.hpp:20-24`, `tests/CMakeLists.txt:6-15`.
- **How it is enforced today:** a size ceiling in CI, 12 MiB on win-x64 and 20 MiB on linux-x64
  (`ci.c-abi.yml:81-110`, `:164-196`), whose error message names this mistake (D-BIND-1a rider ii).
  It is a proxy: it catches Arrow C++ by its megabytes, not by name. D-BIND-148's DLL allow-list
  would catch a shared Arrow, not a static one. No test asks "is any Arrow C++ symbol in the edge
  shim?"
- Tests: no test. Once a server variant links Arrow C++ from the same `CMakeLists.txt`, the edge
  build needs a check of its own (Q4).

**L9. A row is written once, into the transport's window.** No encode entry point returns bytes;
values cross as Arrow arrays, never as per-field C arguments; a bound array serves many publishes.
`[built]` for the row payload; attachments cross by retained owner handle.
- Holds at: `cpp:701-735` (the codec runs inside `Publish`'s encoder), `h:979-985`,
  `write_window.hpp:6-18` (one crossing per refill), `cpp:286-307` (`fl_blob_create` copies, by
  design, D-BIND-42: attachments are sidecar). Zero-copy stops at managed scalars (D-BIND-1b).
- Tests: `BindingEntryPoints.TheFusedPublishRunsOverTheWholeChain`; the probe shim's
  `ProbeFixture.TheFusedPublishAppendsThePayloadFromTheCallersBuffer`,
  `.AStagedRowPushedThroughARawPublishIsACopy` (the negative control), `.ALoanedAttachmentIsDeliveredByAddress`.

**L10. The shim's codec writes the bytes domain 2's codec writes.** One wire format, two drivers
(D-BIND-39): `NanoarrowCodec` may not differ from `arrow-bridge`'s `Codec` by one byte.
`[built]` for encode; decode parity with domain 2 `[built as a property; no test]`
- Holds at: `nanoarrow_codec.hpp:6-31`; the corpus `tests/codec_corpus.hpp`.
- Decode is tested as the inverse of the shim's own encode, not against domain 2's decoders. Which
  bytes are *refused* as malformed is not compared between `NanoarrowCodec` and `BatchDecoder`. In
  the server variant both will decode rows in one binary (Findings b3).
- **Unsure whose law this is.** The rule is domain 2's (the wire format); the second driver lives
  here (Q2).
- Tests: `NanoarrowCodec.ByteIdenticalToArrowBridge`, `.DecodeIsTheInverseOfEncode`,
  `.ASlicedBatchEncodesEachRowAsTheWholeBatchDoes`; the `binding-abi-conformance` lane (an
  `Apache.Arrow` export reaches the codec as Arrow C++'s does).

**L11. The shim adds no thread, and holds no lock across a caller's callback.** A callback runs on
the thread the layer below chooses: a delivery on the transport's thread, one at a time per
subscription, with no thread affinity; a writer or grow callback synchronously on the calling
thread. Everything a callback is handed is borrowed for that call.
`[built]`
- Holds at: `h:851-876`, `h:671-706`; `cpp:836-862` (the thunk; schema and attachments views torn
  down on every exit); `cpp:193-212` (`declared_mu` released before any refusal or publish).
  Re-entry on the same provider and thread is the seam's refusal (`FL_REENTRANT_CALL`), forwarded.
- Tests: `Subscriber.TheChainDeliversThroughTheAbiInBothDirections`,
  `.AttachmentsReachTheHandlerAndARetainedBlobOutlivesTheDelivery`, `.ASchemaWatchIsRefusedFromInsideADelivery`,
  `BindingEntryPoints.ManyThreadsShareOneCodecAndOneBoundBatch`.

**The server-tier variant. PROPOSED (owner confirmation owed); none of it is built.** L12 and L13
follow from the 2026-11-12 ruling's text. L14 to L17 are round-designer's reading of the rulings,
domain 7's laws and the register.

**L12. One source, two variants; the edge variant is unchanged by the other.** Both are built from
the same sources; the edge variant's surface, links, size check and behaviour stay exactly as today.
`[designed-not-built]`. Source: 2026-11-12, "one shim in two variants"; "the edge variant exactly
as today". Precedent: the probe shim is a second artifact from the same object library and entry
points (`CMakeLists.txt:164-211`, `conanfile.py:30-34`, D-BIND-62).

**L13. The server variant only adds.** It adds entry points; it never removes an edge entry point,
changes one's signature, or changes what one does. A caller written against the edge surface
behaves identically on the server variant.
`[designed-not-built]`. Source: as L12. Open: whether the edge header declares the additions (Q3).

**L14. A batch crosses as an Arrow C Data Interface array and schema the receiver owns.** The
receiver releases it the Arrow way; the shim keeps nothing of it. A window's loss count and reason
cross with it, and a "no batch could be built" (domain 7 L7) is expressible.
`[designed-not-built]`. Source: BIND Decision 4 / D-BIND-1a (values cross as C Data); precedent
`fl_decode_rows` (`h:1061-1066`: a fresh array the caller imports and owns). Contrast `fl_schema`,
which is shared and must never be released the Arrow way (`h:455-473`). How attachments (`L3` of
domain 7, row-aligned) cross is open (Q6).

**L15. Domain 7's answers pass through; the shim answers none of them again.** Options are checked
only by domain 7 (BC-8): the shim forwards `max_rows` and the timeout as given, `kNoTimeout` as
`INT64_MAX` ms untranslated (a narrower field maps its own maximum onto it), and L15's refusal as
the seam's `FL_INVALID_ARGUMENT` with domain 7's message. The shim adds no window, count, timer,
ordering or thread (BC-9); the batch callback runs on domain 7's batch thread, one batch at a time,
in window order (domain 7 L12, D10), and closing follows domain 7's L9.
`[designed-not-built]`. Source: d7 §4 and §6, D-D7O-1, register BC-8 and BC-9, 2026-11-12 "Refuse
both" ("the shim passes the refusal through"). `subscriber_arrow.cpp:39-46` throws
`PubSubError(kInvalidArgument)`, which `Capture` already forwards as `FL_INVALID_ARGUMENT`
(`containment.cpp:62-66`); a `std::invalid_argument` at a seam origin would become `FL_INTERNAL`
(`containment.cpp:76-82`), so the pass-through depends on domain 7 keeping that type.

**L16. A caller can tell which variant it loaded before it calls a server entry point.**
`[designed-not-built]`. Source: round-designer, from L6's handshake: today `fl_binding_abi_version`
cannot distinguish two builds of one version, and a P/Invoke caller would learn of a missing entry
point only by `EntryPointNotFoundException` at first call. **Unsure** whether the owner wants this as
a law or leaves it to each binding; the mechanism is open (Q1).

**L17. One shim per process holds across variants.** A process loads the edge variant or the server
variant, never both; both export the marker, so L5's check refuses the later one.
`[designed-not-built]`. Source: the 2026-11-12 rejection of a second native library, D-BIND-17.

## 4. The interface

Surface, `h` sections in order (ABI 0.7, 47 entry points, every one implemented):
- **Version** (`h:158-172`): `fl_binding_abi_version`, callable first, from any thread.
- **Status and errors** (`h:174-316`): `fl_status` (0-10), `fl_origin`, `fl_error`, `fl_error_dispose`.
- **Strings, topics, marker** (`h:318-394`): `fl_str`, `fl_topic`, `fl_string_list_*`, `fl_single_copy_marker`.
- **Shared ownership** (`h:396-512`): `fl_blob_create/retain/release`, `fl_schema_retain/release/copy`.
- **Arrival** (`h:514-564`): `fl_schema_arrival_wait` (five outcomes; `int64_t timeout_ms`,
  `INT64_MAX` unbounded), `_dispose`.
- **Attachments** (`h:566-632`): positional read; a builder that seals into byte order.
- **Write window** (`h:634-706`): `fl_write_window`, `fl_grow_fn`, `fl_writer_fn`.
- **Provider, publisher, subscriber** (`h:708-969`): `fl_provider_create/destroy` (selector +
  `fl_provider_config`), `fl_publisher_*` (create, declare with or without `fl_topic_options`, list,
  `publish_raw`), `fl_subscriber_*` (subscribe with or without options, unsubscribe, schema watch pair).
- **Codec** (`h:971-1092`): open, bind, `publish_row(s)`, `encode_row`, `decode_rows`, `decode_rows_framed`.
- **Server variant** `[designed-not-built]`: a batched subscribe over domain 7's
  `SubscriberArrow::Subscribe(segments, RecordBatchCallback, BatchOptions, TopicOptions)`
  (`subscriber_arrow.hpp:77-160`) and its unsubscribe. Its C shape is the shim round's design, within
  L12-L17.

**Who calls, and on which thread:**
- Any entry point from any thread; `fl_codec` and `fl_rows` are immutable and shared without locks
  (`h:994-995`, `h:1013-1014`).
- `fl_delivery_fn` on the transport's thread; `fl_writer_fn` and `fl_grow_fn` on the calling thread,
  inside the call. Server variant: the batch callback on domain 7's batch thread (L15).
- `fl_subscriber_unsubscribe` outside a delivery blocks until that id's running delivery ends; inside
  one it does not wait (`h:906-924`, D-BIND-50).
- Destroying a subscriber from inside its own delivery terminates the process (L1); a binding
  refuses it first.

**Builds** (`conanfile.py:30-46`): the shipped shim; `with_probe_shim` adds the test-only
`fletcher-c-abi-probe` (in the package ID, so it cannot pass for a release); `run_tests` adds the
gtest suites and the test-only Arrow C++ oracle (not in the package ID). The server variant's build
switch, package and file name are open (Q1, Q5).

## 5. Decisions of record

| # | Decision | Source |
|---|---|---|
| D1 | One codec format; the binding wraps it and never reimplements it in a managed language. The shim adds a C++ driver of `positional_io` over nanoarrow. | BIND Decision 1, D-BIND-1 (maintainer, 2026-08-31), sharpened D-BIND-23 |
| D2 | Values cross as the Arrow C Data Interface; a bespoke row-builder ABI is rejected. | BIND Decision 4, D-BIND-1a (2026-09-11) |
| D3 | The native artifact links nanoarrow only, never Arrow C++; checked by a size ceiling in CI. **Partially superseded 2026-11-12** for the server variant only (domain-map row 9; D7O audit R-4). Annotating Decision 6 is owed by the shim round. | BIND Decision 6, D-BIND-1a rider ii |
| D4 | The binding ABI is derived from the seam spec alone and shares no header with the driver ABI. | D-BIND-2′ (2026-09-05) |
| D5 | Everything a delivery receives is borrowed; one callback at a time per subscription; no thread affinity. | D-BIND-2a |
| D6 | The shim statically links and registers `inprocess`, `fastdds`, `xrce`; a path selector is `kNotSupported`; a binding never registers a provider or resolver. | D-BIND-3 (amended 2026-09-11), D-BIND-24 |
| D7 | Zero-copy for rows and attachments, proven by a copy oracle; the oracle runs through a test-only probe shim. | D-BIND-4, D-BIND-58, -61, -62 |
| D8 | Native assets live in one package, `Eiva.Fletcher.Interop`. | D-BIND-13 |
| D9 | Exactly one copy of Fletcher per process, checked at load. | D-BIND-17 (2026-09-11, Q11) |
| D10 | Errors: one numbered type per side, number and message always cross in a caller-owned `fl_error`; one containment site per direction; outcomes are values. | D-BIND-1c, D-BIND-19, D-BIND-54 |
| D11 | A callback's `fl_error` message is borrowed to the shim; the shim copies and frees nothing. | D-BIND-32 |
| D12 | Timeouts: `INT64_MAX` is the unbounded form, negative refused; byte bounds, never characters. | D-BIND-20; for batches D-D7O-1 |
| D13 | No `std::shared_future` crosses; the arrival is the one waiting mechanism. | D-BIND-22 |
| D14 | An entry point is exported only when implemented; no stubbed `FL_NOT_SUPPORTED`. | D-BIND-31; `h:69-90` |
| D15 | The shim's codec carries the proto mapping; dictionaries encode and decode as their value type. | D-BIND-39, D-BIND-36 |
| D16 | Bound rows publish only to a topic this publisher declared, with the same wire layout. | D-BIND-60, -65, -67 |
| D17 | ABI 0.7 adds `fl_decode_rows_framed`, which serves C#'s managed batcher. | D-BIND-68 |
| D18 | The MSVC runtime stays dynamic; the Windows shim's DLL imports are an exact allow-list; a 12 MB packed ceiling on `Eiva.Fletcher.Interop`, Windows exports accepted. | D-BIND-38, -155, -148, -157 |
| D19 | **The shim is built in two variants from one source**: edge (exactly as today, no Arrow C++, embedded devices) and server tier (adds the batched subscribe). C# on desktop loads the server variant. A second native library is rejected. | Owner ruling 2026-11-12, "one shim, two variants" |
| D20 | **The domain is the C ABI shim alone, both variants;** each language binding is its own domain. | Owner ruling 2026-11-12, "Shim only" |
| D21 | One batch assembler, domain 7's; the shim passes domain 7's refusal through. | Owner rulings 2026-11-12 (d7 D16, "Refuse both") |

## 6. Relationships

- **Domain 3 (provider interface).** The shim is the C form of domain 3's `Publisher`, `Subscriber`,
  `SchemaArrival`, `Attachments`, `Blob`, `ProviderRegistry` and status taxonomy, and forwards their
  rules (L4). Re-entrancy (`FL_REENTRANT_CALL`), the unsubscribe barrier and the termination on a
  destroy inside a delivery are domain 3's answers.
- **Domain 6 (driver ABI).** The mirror image below the seam; no shared header (D4). The path selector
  waits for PDA-ABI's resolver seat.
- **Domain 2 (row encoding).** The shim hosts a second driver of domain 2's wire format, bound to it by
  byte identity (L10). Domain 2's `BatchDecoder` will run inside the server variant (via domain 7).
- **Domain 4 (transports).** Three providers are linked into the shim as built-ins; their limits
  (XRCE's `domain_id` ≤ 65535, no schema channel outside `fastdds`) reach callers unchanged.
- **Domain 7 (batch assembly).** The server variant exposes it (L15). Obligations from d7 §4 and §6:
  `kNoTimeout` as `INT64_MAX` ms untranslated; a narrower field maps its own maximum onto it; L15's
  refusal passed through (BC-8); no batching of its own (BC-9). Domain 7's L9 exceptions (closing from
  inside a batch callback does not wait) and L12 reach C callers through the shim.
- **Domain 10 (C# binding), anticipated.** Loads the shim; owns the handshake's managed half,
  `Timeout.Infinite` mapping, `SafeHandle`s, managed refusals (D-BIND-18, -51), and the deletion of
  the managed batcher once it reaches domain 7 through the server variant.
- **Domain 5 (gateway).** Not a consumer: `Eiva.Fletcher.GatewayClient` has its own managed codec
  and batcher (D-BIND-1's one exception; BC-9).

## 7. Open questions

| # | Question | Owner |
|---|---|---|
| Q1 | **How does a caller tell which variant it loaded?** Options include a distinct file name, a variant query entry point (added to the edge variant too, which changes its surface), or the presence of a symbol. L16 is proposed; the mechanism is not. | owner |
| Q2 | **Who owns `NanoarrowCodec`?** It implements domain 2's wire format, lives in this domain's artifact, and is the edge variant's only codec. Is L10 domain 9's law or domain 2's? | owner |
| Q3 | Does the edge variant's header **declare** the server entry points (unexported, a "link error" by L6, a run-time error through P/Invoke), not declare them (two headers, or a `#if`), or **export them answering `FL_NOT_SUPPORTED`**, the status `h:75-77` reserves for "this build cannot do the thing"? The last changes the edge surface, against "exactly as today". | owner |
| Q4 | How is "the edge variant links no Arrow C++" enforced once the server variant links it from the same build? The size ceiling is a proxy; a symbol or dependency-graph check would be structural. | shim round |
| Q5 | **Packaging.** Does the server variant ship in `Eiva.Fletcher.Interop` beside the edge one, in its own package, or replace it in Interop? Each option meets a BIND decision (Findings e). | owner |
| Q6 | How do a batch's row-aligned attachments, `BatchStatus` and a null batch cross? Per-row `fl_attachments` handles, a flattened form, or a C Data column? | shim round, then owner if it widens the seam's vocabulary |
| Q7 | How are `kNoTimeout` and L15's refusal expressed in the C signature: an `int64_t timeout_ms` with `INT64_MAX` as no timeout (as `fl_schema_arrival_wait` does), and `FL_INVALID_ARGUMENT` with domain 7's message? D-D7O-1 says a plain value; confirm the field width. | shim round |
| Q8 | Does the server variant **version** separately? One `FL_BINDING_ABI_VERSION` for both, or a minor bump when the server additions land (L6 says every change bumps; the edge surface does not change)? | owner |
| Q9 | After the C# switch, `fl_decode_rows_framed` exists only for the deleted managed batcher. Does it stay (L13: the edge surface is unchanged) or go (a 0.x minor may remove it)? | owner, with domain 10 |
| Q10 | Which consumer loads the edge variant once C# desktop moves? "Embedded devices" has no RID today (D-BIND-27: `win-x64`, `linux-x64`; `linux-arm64` first addition). | owner |
| Q11 | A batched dictionary column arrives re-folded (domain 2's `BatchDecoder`), while `fl_decode_rows` returns its value type (D-BIND-39, D-BIND-59). Is that difference intended for a server-variant caller? | domain 2, with domain 10 |

## 8. How to extend this domain

1. **Forward, do not answer.** A new entry point calls the layer that owns the rule and lets its
   refusal through `Contain`; the shim checks only what that layer cannot see, and records it here as
   L4 does its one exception.
2. **One containment site.** Every fallible entry point is a `Contain` call; no `try` of its own, no
   status number of its own (L1, L2).
3. **State ownership in the header's three words,** name the one release function, and make the
   handle tolerate any release order (L3).
4. **Bump and record.** Append to the header, bump the minor, record the ruling; never stub (L6).
5. **Variant discipline** (proposed): an addition that needs Arrow C++ goes in the server variant only,
   and the edge variant's build, links and size check are re-run unchanged (L8, L12, L13).
6. **Record the change.** Write any change to a law here before writing the code, with its build state.

## Findings (reported, not fixed)

**(a) Documents that disagree with the code**
1. **The marker's text.** `single_copy.cpp:119` returns "ABI 0.1"; the shim is 0.7 (`h:163-164`). The
   header promises it "carries the component and its ABI version" (`single_copy.hpp:42-43`).
2. **The schema watch.** `cpp:11-18` says `fl_subscriber_subscribe_schema` answers `FL_NOT_SUPPORTED`
   until the seam grows the pair; it is implemented (`cpp:889-915`, D-BIND-52).
3. **Windows exports.** `h:135-136` and `CMakeLists.txt:127` say "Revisited at BIND-9"; D-BIND-157
   accepted them (README:54-55 is current).
4. **Counts.** `CMakeLists.txt:92-93` says twenty entry points are exported; there are 47.
   `cpp:104-106` says no exported function constructs an `fl_attachments`; the builder does (`cpp:486-497`).
5. **Single-copy wording.** `h:382-385` says the shim "REFUSES TO INITIALISE"; only the copy loaded
   second refuses, and it still loads (L5).
6. **The link-error claim.** `h:79` holds for static linking only; a P/Invoke caller sees a run-time
   error (L6).
7. **The status taxonomy test.** `h:180-183` names a C# port of `test_status_taxonomy`; none was found
   under `dotnet/tests` (searched by name and by `FletcherStatus`). Unsure: it may live under another
   name.

**(b) Rules answered in two places**
1. **Encoding the wire format:** `NanoarrowCodec` and `arrow-bridge`'s `Codec`, held together by byte
   identity (L10, D-BIND-39). Accepted by decision; recorded.
2. **"May these rows be published on this topic?"** `RequireRowsMatchTopic` (`cpp:182-222`) and C++
   `PublisherArrow`'s per-topic codec. Same answer by design (D-BIND-67), two sites, no test that
   compares them.
3. **"Are these bytes a valid row?"** `NanoarrowCodec::DecodeRows`/`DecodeRowsFramed` and domain 2's
   `BatchDecoder`. Today in different binaries; in the server variant, in one. No parity test.
4. **Range checks on a timeout:** `SchemaArrival::Wait` refuses a negative; the shim forwards it
   (`cpp:941-946`). Not a second answer, recorded so the server variant follows the same pattern
   (L15).

**(c) Behaviour with no test**
- That every void entry point cannot throw (L1).
- That the C `fl_status` numbers equal `PubSubStatus` (L2); a `static_assert` per enumerator would do it.
- That a failed call leaves OUT parameters untouched (L3).
- The append-only rule within a major (L6).
- That the edge shim contains no Arrow C++ (L8): only the size ceiling.
- The Windows half of the export-table property: violated, accepted (L7).

**(d) Tier-1 collisions for the owner** (between a BIND decision, a 2026-11-12 ruling and the code;
listed, not resolved)
1. **BIND Decision 6 / D-BIND-1a rider ii vs "one shim, two variants".** Known and accepted as a
   partial supersession (R-4); the annotation is owed by the shim round.
2. **D-BIND-157 (12 MB packed ceiling on `Eiva.Fletcher.Interop`) vs C# desktop loading a server
   variant that carries Arrow C++.** If the server variant ships in Interop, the ceiling (and CI's
   12 MiB / 20 MiB shim ceilings, whose message says they exist to catch Arrow C++) will trip; the
   2026-11-12 ruling records the Arrow C++ cost, not the ceiling.
3. **D-BIND-13 (native assets in ONE package) vs a server variant in its own package.** Shipping it
   separately breaks D-BIND-13 ("Adding native assets to a second package → STOP-AND-ASK").
4. **D-BIND-17 ("no other binary the package ships carries Fletcher code") vs one package carrying
   both variants per RID.** Shipping both in Interop means two Fletcher binaries in one package.
   Items 2-4 together mean every packaging choice meets one locked decision (Q5).
5. **D-BIND-1a rider i ("a .NET process is never the microcontroller", so the edge/server split "may
   not need mirroring in C#") vs the variant split now being mirrored in what C# loads.** Probably
   compatible (desktop C# loads server); listed because the rider argued the opposite direction.
6. **D-BIND-25's managed Arrow tier and D-BIND-68's `fl_decode_rows_framed`** exist for the batcher the
   2026-11-12 ruling deletes (d7 D16 records D-BIND-25's supersession; D-BIND-68 is not mentioned).
