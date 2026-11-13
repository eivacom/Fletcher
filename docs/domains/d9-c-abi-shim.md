# Domain 9: The C ABI shim

> **Specified.** The owner confirmed these laws on 2026-11-13 ([RIX-rulings.md](../../plans/RIX-rulings.md),
> "Laws: Confirm, adjusted by answers"), as amended by the same day's three rulings: the server
> variant ships in its own .NET package (D22), both builds answer a variant query and the edge build
> refuses a batched subscribe by name (D23; L12, L16), and the shim's row codec is domain 2's (D24;
> L10 moved). A change to a law needs an owner ruling, recorded in a decision of record. L12-L18
> describe the server-tier variant and stay `[designed-not-built]` until the shim round builds them.
> **D9S pre-round rulings (2026-11-13) applied:** a batch is borrowed for its callback and kept only
> by moving it (D26; L14 amended, audit B1 resolved); one ABI version for both builds, both bump
> (D27; L6); `FL_NOT_SUPPORTED` means "this provider or this build cannot do this" (D28; L2, L12);
> a C# app loads the server build only if it references the server package (D29, revising D19); a
> batch the shim cannot hand over is reported (D30; new law L18); the audit's tier-2 corrections
> (D31). Audit: [D9S-pre-round-audit.md](../../plans/D9S-pre-round-audit.md).

**Mode: survey for everything that exists, verified against the tree at `180b2d7`; D9S-1 (`fcb7be6`) added 15 header lines, so `h:N` citations after `h:172` are about 15 low; D9S-2 changed `CMakeLists.txt`, `conanfile.py` and the workflows, so citations into those predate it** (branch
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
ABI version and its compatibility rule; the export table; the one-copy-per-process check; the
built-in provider registry the shim carries; and **which variant a build is**, how a caller asks,
and what each variant may link.

**Hosts, owned by domain 2:** the binding-tier row codec, `NanoarrowCodec`
(`c-abi/src/nanoarrow_codec.*`), behind `fl_codec_*`, `fl_rows_*`, `fl_encode_row` and
`fl_decode_rows*` (ruling 2026-11-13, "Codec owner: Domain 2"). What bytes it writes and accepts is
domain 2's law (d2 L2, L8); this domain owns only the entry points, their handles and their
containment.

- **Edge variant** (exists; today's shim): as today, plus only the variant query and named
  not-supported answers from the batched entry points (L12, D23), and the ABI version bump both
  builds share (L6, D27). The query and the bump are `[built]` (D9S-1); the refusals are
  `[designed-not-built]`. Never
  carries Arrow C++, because "it needs to be able to run on embedded devices" (owner, 2026-11-12).
  Ships in the base package, `Eiva.Fletcher.Interop` (D22).
- **Server-tier variant** `[designed-not-built]`: the edge surface plus `pubsub-arrow`'s batched
  subscribe (domain 7). Ships in its own .NET package, a full shim (D22). A C# app loads it only if
  it references the server package; otherwise it loads the edge build (D29, revising in place
  D19's "C# on desktop loads the server variant"; domain 10, next round).

**Refuses:**
- **Answering the seam's questions again.** Topic validation, options checks, re-entrancy,
  unsubscribe semantics and timeout meaning are domain 3's; the shim forwards them and forwards
  their refusals unchanged (`cpp:28-39`, `cpp:96-102`, `cpp:871-877`, `cpp:941-946`). Two exceptions
  are recorded (L4): the bound-rows publish check, and the edge build's refusal of a batched entry
  point (L12), which no seam call stands behind (audit F3, D31).
- **Batching** (register BC-9). The shim has no window, no count, no timer, no thread. The server
  variant exposes domain 7's assembler; it builds none of its own and decides nothing about when a
  window closes.
- **Validating batch options** (register BC-8). Domain 7's L15 is the one answer; the server
  variant passes the refusal through and does not check `max_rows` or the timeout itself.
- **Encoding and decoding rules.** What bytes a row becomes, which bytes are refused, which schemas
  a batch can be assembled from and the dictionary re-fold are all domain 2's, including for the
  codec this domain hosts (D24). That codec decodes a dictionary as its value type and stops there
  (`nanoarrow_codec.hpp:163-179`, D-BIND-39).
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
| **Variant** | One of the two builds of the shim from one source: `edge` and `server`, chosen by the Conan option `variant` (default `edge`, in the package ID; D9S-2). The test-only probe shim (`CMakeLists.txt:164-211`) is a third artifact from the same sources, always edge, not a variant. |
| **Edge variant** | Today's shim: no Arrow C++, codec on nanoarrow. Shipped in the base package. |
| **Server(-tier) variant** | `[built]` (Windows, by hand; CI and Linux unrun) as an artefact (D9S-2: `fletcher-c-abi-server`, the edge sources answering "server", no Arrow yet); the batched subscribe and the Arrow link are `[built]` for 4a (D9S-4a: `pubsub-arrow` is linked only under `variant=server`; Windows, by hand; CI and Linux unrun); each row's attachments are `[built]` for 4b (D9S-4b; same marks); the batched close (stage 5) is `[designed-not-built]`. Shipped in a package of its own (D22). |
| **Variant query** | `[built]` for all three artefacts (D9S-1 edge and probe; D9S-2 server, (Windows, by hand; CI and Linux unrun)). `fl_binding_variant`: the entry point both builds export that answers which build was loaded (L16, D23). |
| **Base package / server package** | `Eiva.Fletcher.Interop`, carrying the edge variant, and the separate .NET package carrying the server variant; each holds a full shim (D22). |
| **Entry point** | An exported `fl_*` function. 48 today (`h`, `grep -c FL_ABI_EXPORT`). |
| **Handle** | An opaque `fl_*` struct pointer the caller holds and cannot read (`handles.hpp:6-11`). |
| **Owner handle** | The `owner` of an `fl_blob` or `fl_schema`: a refcounted block reached only through retain/release (`h:398-512`, `handles.hpp:43-59`). |
| **Borrowed / Owned / OUT** | The header's three ownership words (`h:29-42`). Unqualified pointers are borrowed. |
| **Status** | An `fl_status` number: the seam's published taxonomy, number for number (`h:176-234`). |
| **Not supported** | `FL_NOT_SUPPORTED`: "this provider or this build cannot do this" (D28, 2026-11-13; widened from "this provider does not implement the requested behaviour"). The edge build's refusal of a batched entry point reuses it; no new status number. |
| **Hand-over** | `[designed-not-built]` The shim's turning one batch domain 7 delivered into its C form and calling the binding's batch callback with it (L14). A **failed hand-over** is one the shim cannot complete; it reaches the binding as a "no batch could be built" delivery (L18). |
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
- **That exception is the row subscriber's only** `[designed-not-built]` (audit F4, D31). A batched
  subscription (server variant) follows domain 7's L9 and D12: closing or destroying it from inside
  its own batch callback is lawful, and the queued batches, then the closing batch, are still
  delivered. The batch thread pushes no `DeliveryScope` (BC-6), so the shim neither terminates nor
  refuses that call; it forwards it (BC-5, L15).
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
- **`FL_NOT_SUPPORTED` means "this provider or this build cannot do this"** (D28, 2026-11-13). The
  edge build's refusal of a batched entry point (L12) reuses it with a message naming the edge build
  and the server package; the shim allocates no new number. That refusal carries `FL_ORIGIN_SEAM`
  (Q12, edge half, resolved by D9S-4a; audit F5).
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
`[built]`, with **one answer of its own**: the bound-rows publish check; a **second** is designed,
the edge build's refusal of the batched subscribe (L12) `[built]` (D9S-4a).
- Holds at: `cpp:28-39` (the six topic rules are `RequireSegments`'), `cpp:96-102` (topic options),
  `cpp:871-877` (unsubscribe's no-op and no-wait cases), `cpp:941-946` (the arrival timeout is
  forwarded; "negative means forever" belongs to C#, D-BIND-20).
- **The exception:** `RequireRowsMatchTopic` (`cpp:182-222`, D-BIND-60, -65, -67) refuses bound rows
  for a topic this publisher did not declare, or whose wire layout differs. The seam's `Publisher`
  only sees bytes, so it cannot ask; C++ `PublisherArrow` answers the same question for C++ callers.
  That is one question with two sites (Findings b2).
- **The second exception** `[built]` (D9S-4a; audit F3, D31): on the edge build, a batched
  entry point answers `FL_NOT_SUPPORTED` itself (L12, D28), with no seam call behind it. It is the
  answer to "can this build do this?", which only the shim can know (BC-10).
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
- **Two packages, one process** (D22, 2026-11-13): an application may reference both the base and
  the server package, so two full shims can reach one application's output. The law still holds:
  only one may load per process, and a second loaded is refused by this check (L17). How the two
  files coexist on disk, and which one a binding loads, is the shim and C# switch rounds' design
  (Q5a).
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
  `dotnet/src/Fletcher.Interop/NativeLoader.cs:253` (`VerifyAbiVersion`). D-BIND-42, -43, -46, -52,
  -57, -68 each bumped the minor (0.7; D-BIND-52 added by audit F6, D31); D9S-1 made it 0.8.
- **Both builds share one ABI version, and both bump** (D27, 2026-11-13; resolves Q8 and audit
  LC-1) `[built]` for the bump to 0.8 and the query (D9S-1, with the C# loader's one line);
  the batched subscribe is `[built]` (D9S-4a) and its close `[designed-not-built]` (stage 5). The variant query and the batched entry
  points are appended to the one header, so the edge build's minor moves with the server build's. The builds are told apart by
  the variant query (L16), never by the version.
- **Named scope exception** (D27, audit OQ-1): the C# loader's expected minor,
  `HeaderVersionMinor` in `dotnet/src/Fletcher.Interop/NativeLoader.cs` (line 34 at `0fab68e`), is
  domain 10's file. The shim round edits that one line, to the new minor, in the same commit as the
  bump; otherwise every dotnet lane loads the new shim and refuses it. Nothing else in domain 10
  changes in this round.
- An edge-build refusal under L12 is implemented behaviour ("this build cannot", D28), not a stub.
- "An unimplemented entry point is a LINK ERROR" (`h:81`) holds for a static link. Through P/Invoke
  it is a run-time `EntryPointNotFoundException` at first call. This matters for the variants (L16).
  The header says so since 2026-11-13 (audit F22, D31).
- Tests: `BindingAbi.VersionMatchesHeader`, `BindingAbi.VersionIsPackedMajorMinor`, `test_package/`.

**L7. The surface is pure C and self-contained, and the export table is exactly the surface.** No
consumer needs a Fletcher, Arrow or eProsima header; Arrow structures are forward-declared.
`[built]` for the header; the export-table clause `[built for Linux; violated on Windows]` (accepted,
D-BIND-157). Since D9S-2 `scripts/check_shim_exports.sh` compares both builds' `fl_` sets with `binding.h` on both platforms, by design; proven on Windows only, by hand (the whole-table clause stays Linux-only)
- Holds at: `h:17-18`, `h:97-108`, `CMakeLists.txt:99-104`, `:138-147` (hidden visibility,
  `--exclude-libs ALL`, the version script `cmake/exports.map:46-51`), `:154-162` (private links),
  `conanfile.py:58-63` (no transitive headers).
- Windows: ConanCenter's static Fast DDS carries `/EXPORT` directives; 3531 names exported
  (`CMakeLists.txt:116-127`, README:49-60).
- Tests: `BindingAbi.CompilesAsC99AndIsSelfContained` (C99 with `-pedantic-errors`; C11 on MSVC),
  `test_package/`. CI, not ctest: `scripts/check_shim_exports.sh` (`ci.c-abi.yml`, both legs, D9S-2) refuses
  any Linux export outside `fl_*` and the linker's own names and compares each build's `fl_` set with
  `binding.h`'s `FL_ABI_EXPORT` set, so edge = server = header; its control puts the probe shim in the
  server's place and must fail by name.

**L8. The edge variant links no Arrow C++.** Its codec is nanoarrow; Arrow C++ and `arrow-bridge`
may appear only in test binaries.
`[built]` (D9S-3; Windows, by hand, for the graph half and the name scan of the edge DLL; the Linux
`nm -C` half and CI are unrun until the owner's draft PR runs them): the check by name now has a code
owner.
- Holds at: `conanfile.py:95-107` (Arrow and `arrow-bridge` are required only under `run_tests`; with tests off no Arrow package, test-only or not, may be in the graph — ruling 2026-11-13, "Narrow the docs";
  and `run_tests` is deleted from the package ID — *record (D9S-2/3 reviews): false; the tests-on package lists `arrow` and `fletcher-arrow-bridge` in its `conaninfo.txt`, so it has a different ID*), `CMakeLists.txt:154-162` (the shim's links),
  `nanoarrow_codec.hpp:20-24`, `tests/CMakeLists.txt:6-15`.
- **How it is enforced:** `scripts/check_edge_arrow_free.sh` (`ci.c-abi.yml`, both legs, before the
  size ceilings so a large Arrow link is named) fails by name if the edge configuration's Conan host
  graph holds `arrow`, `fletcher-arrow-bridge` or `fletcher-pubsub-arrow`, or if the edge shim
  carries an Arrow name (`nm -C` on Linux; on Windows only the class and import names the DLL itself
  carries, a Release DLL having no symbol table). Two controls run every time and must each go red
  naming Arrow (the `run_tests=True` graph; a `c_abi_tests` binary). The size ceilings stay as the
  coarse check (L12): 12 MiB on win-x64 and 20 MiB on linux-x64, whose message names this mistake
  (D-BIND-1a rider ii). D-BIND-148's DLL allow-list would catch a shared Arrow, not a static one.
- Tests: the CI step "The edge build carries no Arrow C++ (d9 L8)" (D9S-3, ladder rung 3), with its
  controls. Windows parity with Linux's full symbol table would need a link map or PDB, a later-round
  question (not needed given Linux and the graph).

**L9. A row is written once, into the transport's window.** No encode entry point returns bytes;
values cross as Arrow arrays, never as per-field C arguments; a bound array serves many publishes.
`[built]` for the row payload; attachments cross by retained owner handle.
- Holds at: `cpp:701-735` (the codec runs inside `Publish`'s encoder), `h:979-985`,
  `write_window.hpp:6-18` (one crossing per refill), `cpp:286-307` (`fl_blob_create` copies, by
  design, D-BIND-42: attachments are sidecar). Zero-copy stops at managed scalars (D-BIND-1b).
- Tests: `BindingEntryPoints.TheFusedPublishRunsOverTheWholeChain`; the probe shim's
  `ProbeFixture.TheFusedPublishAppendsThePayloadFromTheCallersBuffer`,
  `.AStagedRowPushedThroughARawPublishIsACopy` (the negative control), `.ALoanedAttachmentIsDeliveredByAddress`.

**L10. Moved to domain 2** (ruling 2026-11-13, "Codec owner: Domain 2"; D24). The hosted codec's
byte identity is now [d2 L2](d2-row-encoding.md) (encode) and d2 L8 (decode agreement).

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

**The server-tier variant.** Confirmed 2026-11-13 (D25). D9S-2 built the second build (no batching, no Arrow yet); *(D9S-2 evidence: Windows, run by hand at `0a43e99`; the CI workflows and every Linux leg are unrun — CI runs on pull requests only; owed before the marks read fully `[built]`.)*

**L12. One source, two variants; the edge variant gains only the variant query and named refusals.**
Both builds come from the same sources and declare one surface. The edge build exports every entry
point the server build does: the variant query answers truthfully, and each batched entry point
refuses with a named not-supported error, having done nothing. Nothing else in the edge build
changes, beyond the version bump L6 requires (D27): no other entry point, no link (L8), no size
check, no behaviour.
`[built]` on Windows (by hand; CI and Linux unrun) for one source, one recipe and the two builds (D9S-2); the edge refusal of the batched subscribe is `[built]` (D9S-4a: `Batches.TheBuildAnswersABatchedSubscribeAsItAnswersTheQuery`,
in edge, probe and server; (Windows, by hand; CI and Linux unrun)); the batched close is `[designed-not-built]` (stage 5). Source: 2026-11-12, "one shim in two variants", "the edge variant exactly as
today"; amended 2026-11-13, "Wrong build: Clear 'not supported'" ("the edge surface gains only that
query and those refusals"; D23). Precedent: the probe shim is a second artifact from the same object
library and entry points (`CMakeLists.txt:164-211`, `conanfile.py:30-34`, D-BIND-62).
- **Read as exported, not merely declared.** The ruling says "a batched subscribe on the edge build
  fails with a named 'not supported' error rather than a missing-symbol crash"; a missing-symbol
  crash is what an unexported entry point gives a P/Invoke caller (L6), so the batched entry points
  exist on the edge build. That refusal is not a stub in the sense of the header's own no-stub rule
  (`h:69-85`, confirmed as the header's rule by D-BIND-52; audit F9 corrected the earlier citation of
  D-BIND-31): `FL_NOT_SUPPORTED` means "this provider or this build cannot do this" (D28).
- **"Named" resolved 2026-11-13 (D28; audit LC-2):** the existing `FL_NOT_SUPPORTED`, with a message
  naming the edge build and the server package. No new status number. This refusal is L4's second
  exception (the shim's own answer). Its `fl_origin` is `FL_ORIGIN_SEAM`, as every refusal the shim
  makes itself (Q12, edge half, resolved by D9S-4a).

**L13. The server variant only adds.** It adds entry points; it never removes an edge entry point,
changes one's signature, or changes what one does. A caller written against the edge surface
behaves identically on the server variant. The only entry points whose answers differ between the
builds are the variant query and the batched ones (L12).
`[built]` on Windows for the edge surface on the server artefact (D9S-2: the whole edge-surface suite runs against it; Linux unrun);
the added batched subscribe is `[built]` (D9S-4a, with each row's attachments D9S-4b; its close is stage 5, `[designed-not-built]`). Source: as L12.

**L14. A batch crosses as an Arrow C Data Interface array and schema, borrowed for the batch
callback; a binding keeps it only by moving it.** *(Amended 2026-11-13, D26.)* The batch callback is
handed the array and schema for that call, as L11 says of everything a callback is handed. A binding
that keeps them takes them by the C Data Interface's move (it copies the structs and marks the
sources released by setting their `release` to null), and from then on owns and releases them the
Arrow way. What the binding has not moved when the callback returns, the shim releases then. The
shim keeps nothing of a batch after its callback returns. A window's loss count and reason cross
with it, and a "no batch could be built" (domain 7 L7) is expressible. **Attachments:** each row's
attachments cross beside that row as one `fl_attachments` per row, in row order, borrowed for the
call and kept only by `fl_blob_retain`; a row with none gets an empty set, never NULL, and the list
is NULL exactly when the batch has no rows. The shim builds one set per entry of domain 7's
row-aligned list (d7 L3) and checks none of it (L15).
`[built]` **in part**: the attachments clause is `[built]` (D9S-4b, Windows, by hand; CI and Linux
unrun: `Batches.EachBatchRowCarriesItsOwnAttachments`). The rest (D9S-4a, same marks): the callback is handed the array, the schema, the reason and the
loss count, and a window that could not be built arrives as no batch (`Batches.EachClosedWindow...`,
`Batches.AWindowThatCouldNotBeBuilt...`); the release of what the binding did not move is one scope
guard in `batches_server.cpp`, with **no test yet** (ladder rung 5, `[designed-not-built]`); a failed
hand-over is L18 (D9S-6). Source: BIND Decision 4 / D-BIND-1a (values cross as C Data); ruling
2026-11-13, "Batch owner: Borrowed; keep by moving" (D26), which **resolves audit B1**: the earlier
text ("the receiver owns") contradicted L11 and D5 / D-BIND-2a for a batch handed to a callback.
L11 holds as written. Precedent for an owned array: `fl_decode_rows` (`h:1180-1183`: a fresh array
the caller imports and owns). Contrast `fl_schema`, which is shared and must never be released the
Arrow way (`h:455-473`). How attachments (`L3` of domain 7, row-aligned) cross is settled by 4b (Q6). A batch
the shim cannot hand over is L18's.

**L15. Domain 7's answers pass through; the shim answers none of them again.** Options are checked
only by domain 7 (BC-8): the shim forwards `max_rows` and the timeout as given, `kNoTimeout` as
`INT64_MAX` ms untranslated (a narrower field maps its own maximum onto it), and L15's refusal as
the seam's `FL_INVALID_ARGUMENT` with domain 7's message. The shim adds no window, count, timer,
ordering or thread (BC-9); the batch callback runs on domain 7's batch thread, one batch at a time,
in window order (domain 7 L12, D10), and closing follows domain 7's L9.
`[built]` **in part** (D9S-4a, Windows, by hand; CI and Linux unrun): the shim adds no window, count, timer or thread
(`ShimBatchesNothing.SourcesHoldNoThreadClockOrTimer`), and the callback runs on one batch thread in
window order for one subscription (`Batches.EachClosedWindow...`). **Not yet tested:** the options
pass-through and refusal (rung 7, BC-8, BC-14) and closing as domain 7's L9 (rung 6), both
`[designed-not-built]` as tests though the shim forwards both. Source: d7 §4 and §6, D-D7O-1, register BC-8 and BC-9, 2026-11-12 "Refuse
both" ("the shim passes the refusal through"). `subscriber_arrow.cpp:39-46` throws
`PubSubError(kInvalidArgument)`, which `Capture` already forwards as `FL_INVALID_ARGUMENT`
(`containment.cpp:62-66`); a `std::invalid_argument` at a seam origin would become `FL_INTERNAL`
(`containment.cpp:76-82`), so the pass-through depends on domain 7 keeping that type.

**L16. A caller can ask the loaded shim which variant it is.** Both builds export one variant
query, callable before anything else and from any thread, as `fl_binding_abi_version` is. A caller
that asks never has to learn its build from a missing symbol; one that does not ask gets L12's named
refusal, never a crash.
`[built]` for all three artefacts (D9S-1 edge and probe; D9S-2 server (Windows, by hand; CI and Linux unrun): `fl_binding_variant`, `fl_variant`). Source: ruling 2026-11-13, "Wrong build: Clear 'not supported'" ("the program
can ask the shim which build it has … via a query that both builds export"; D23). Today
`fl_binding_abi_version` cannot tell two builds of one version apart. The query's name, its answer's
type, and whether it changes the ABI version (Q8) are the shim round's design.

**L17. One shim per process holds across variants and packages.** A process loads the edge variant
or the server variant, never both, even when the application references both the base and the
server package. Both builds export the marker, so L5's check refuses the later one.
`[designed-not-built]`. Source: the 2026-11-12 rejection of a second native library; D-BIND-17 as
revised 2026-11-13 ("two packages, each a full shim; one shim per process still holds"; D22).

**L18. A batch the shim cannot hand over is reported, never lost silently.** When the shim cannot
turn a batch domain 7 delivered into its C form (an export or allocation failure in its batch
thunk), the binding's batch callback still receives that window, as a "no batch could be built"
delivery (L14; domain 7 L7): no array, the batch's rows counted as dropped in that window's loss
count, and its reason kept. The thunk never throws back into domain 7. One site in the shim answers
this (register BC-12). Domain 7 is untouched: to it, the window was delivered.
`[designed-not-built]`. Source: ruling 2026-11-13, "Lost handoff: Report it as lost" (D30); audit
census C5 and A37 / F12. Without it, a throw from the thunk is swallowed by domain 7 uncounted
(d7 L10, BC-7) and the C caller loses rows domain 7 counted as delivered, which d7 L1 forbids.
- **A law, not an L14 clause:** L14 says what a batch is and who releases it; L18 answers a
  different question ("is a failed hand-over reported?") with its own single site and its own
  structural test (BC-12: Σ`num_rows` + Σ dropped seen by the C caller equals the rows offered, with
  the export failing on a chosen window). Kept apart so each law has one test.
- **Ambiguous in the ruling:** "its reason kept" is read as the window's own closing reason (domain
  7's `BatchStatus`) still reaching the binding, with the hand-over failure's message crossing
  beside it. How both cross is the stage's design (Q6).

## 4. The interface

Surface, `h` sections in order (ABI 0.8, 49 entry points, every one implemented):
- **Version** (`h:158-172`): `fl_binding_abi_version` and `fl_binding_variant` (with `fl_variant`,
  edge = 1, server = 2), each callable first, from any thread.
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
- **Batched subscribe** (D9S-4a `[built]`, Windows, by hand; CI and Linux unrun): `fl_subscriber_subscribe_batches` over domain 7's
  `SubscriberArrow::Subscribe(segments, RecordBatchCallback, BatchOptions, TopicOptions)`, with `fl_batch`
  (reason `FL_BATCH_ROW_LIMIT`/`TIMEOUT`/`CLOSING` = 1/2/3, `rows_dropped`, `num_rows`, array and schema,
  both NULL for a window that could not be built), `attachments` (one `fl_attachments` per row, an
  empty set for a row with none; D9S-4b `[built]`, same marks) and `fl_batch_fn`. The array and schema are borrowed
  for the call; a binding keeps them by moving them (L14, D26). A batched id has bit 63 set. **Not yet
  built:** the batched close (stage 5) and `failure` (D9S-6, L18).
- **Both builds**: the variant query (L16) `[built]` for edge and probe (D9S-1; its single site is
  `binding_version.c`, BC-10). The edge build exports the batched entry points too, answering a
  named not-supported refusal (L12) `[built]` for the batched subscribe (D9S-4a).

**Who calls, and on which thread:**
- Any entry point from any thread; `fl_codec` and `fl_rows` are immutable and shared without locks
  (`h:994-995`, `h:1013-1014`).
- `fl_delivery_fn` on the transport's thread; `fl_writer_fn` and `fl_grow_fn` on the calling thread,
  inside the call. Server variant: the batch callback on domain 7's batch thread (L15).
- `fl_subscriber_unsubscribe` outside a delivery blocks until that id's running delivery ends; inside
  one it does not wait (`h:906-924`, D-BIND-50).
- Destroying a subscriber from inside its own delivery terminates the process (L1); a binding
  refuses it first. That is the row subscriber's; a batched subscription closed or destroyed from
  inside its own batch callback follows domain 7's L9 (L1).

**Builds** (`conanfile.py`): the shipped shim, `edge` or `server` by the `variant` option (default edge; in the package ID; the server build's file is `fletcher-c-abi-server`; `variant=server` with `with_probe_shim` is refused); `with_probe_shim` adds the test-only
`fletcher-c-abi-probe` (in the package ID, so it cannot pass for a release); `run_tests` adds the
gtest suites and the test-only Arrow C++ oracle (not in the package ID). The server variant ships
in its own .NET package (D22); the NuGet-level name stays domain 10's (Q5a).

## 5. Decisions of record

| # | Decision | Source |
|---|---|---|
| D1 | One codec format; the binding wraps it and never reimplements it in a managed language. The shim adds a C++ driver of `positional_io` over nanoarrow. | BIND Decision 1, D-BIND-1 (maintainer, 2026-08-31), sharpened D-BIND-23 |
| D2 | Values cross as the Arrow C Data Interface; a bespoke row-builder ABI is rejected. | BIND Decision 4, D-BIND-1a (2026-09-11) |
| D3 | The native artifact links nanoarrow only, never Arrow C++; checked by a size ceiling in CI. **Partially superseded 2026-11-12** for the server variant only (domain-map row 9; D7O audit R-4). Annotating Decision 6 is owed by the shim round; **annotated 2026-11-13** (D9S pre-round, audit F17) beside plan Decision 6 and D-BIND-1a rider ii. | BIND Decision 6, D-BIND-1a rider ii |
| D4 | The binding ABI is derived from the seam spec alone and shares no header with the driver ABI. | D-BIND-2′ (2026-09-05) |
| D5 | Everything a delivery receives is borrowed; one callback at a time per subscription; no thread affinity. | D-BIND-2a |
| D6 | The shim statically links and registers `inprocess`, `fastdds`, `xrce`; a path selector is `kNotSupported`; a binding never registers a provider or resolver. | D-BIND-3 (amended 2026-09-11), D-BIND-24 |
| D7 | Zero-copy for rows and attachments, proven by a copy oracle; the oracle runs through a test-only probe shim. | D-BIND-4, D-BIND-58, -61, -62 |
| D8 | Native assets live in one package, `Eiva.Fletcher.Interop`. **Revised 2026-11-13 (D22):** two packages carry native assets, each a full shim. Annotating D-BIND-13 is owed; **annotated 2026-11-13** (audit F18). | D-BIND-13 |
| D9 | Exactly one copy of Fletcher per process, checked at load. **Revised 2026-11-13 (D22):** two packages each ship a shim; still only one may load per process (L5, L17). Annotating D-BIND-17 is owed; **annotated 2026-11-13** (audit F19). | D-BIND-17 (2026-09-11, Q11) |
| D10 | Errors: one numbered type per side, number and message always cross in a caller-owned `fl_error`; one containment site per direction; outcomes are values. | D-BIND-1c, D-BIND-19, D-BIND-54 |
| D11 | A callback's `fl_error` message is borrowed to the shim; the shim copies and frees nothing. | D-BIND-32 |
| D12 | Timeouts: `INT64_MAX` is the unbounded form, negative refused; byte bounds, never characters. | D-BIND-20; for batches D-D7O-1 |
| D13 | No `std::shared_future` crosses; the arrival is the one waiting mechanism. | D-BIND-22 |
| D14 | An entry point is exported only when implemented; no stubbed `FL_NOT_SUPPORTED`. | The header's own rule, `h:69-85`, confirmed by D-BIND-52 (audit F9 corrected the earlier "D-BIND-31; `h:69-90`") |
| D15 | The shim's codec carries the proto mapping; dictionaries encode and decode as their value type. | D-BIND-39, D-BIND-36 |
| D16 | Bound rows publish only to a topic this publisher declared, with the same wire layout. | D-BIND-60, -65, -67 |
| D17 | ABI 0.7 adds `fl_decode_rows_framed`, which serves C#'s managed batcher. | D-BIND-68 |
| D18 | The MSVC runtime stays dynamic; the Windows shim's DLL imports are an exact allow-list; a 12 MB packed ceiling on `Eiva.Fletcher.Interop`, Windows exports accepted. **2026-11-13 (D22):** the 12 MB ceiling stays on the base package; the server package's ceiling is not ruled. | D-BIND-38, -155, -148, -157 |
| D19 | **The shim is built in two variants from one source**: edge (exactly as today, no Arrow C++, embedded devices) and server tier (adds the batched subscribe). C# on desktop loads the server variant. A second native library is rejected. **Revised in place 2026-11-13 (D29):** a C# app loads the server build only if it references the server package. | Owner ruling 2026-11-12, "one shim, two variants" |
| D20 | **The domain is the C ABI shim alone, both variants;** each language binding is its own domain. | Owner ruling 2026-11-12, "Shim only" |
| D21 | One batch assembler, domain 7's; the shim passes domain 7's refusal through. | Owner rulings 2026-11-12 (d7 D16, "Refuse both") |
| D22 | **The server variant ships in its own .NET package.** Apps that publish and subscribe per row stay small; apps that want batches add the second package. D-BIND-157's ceiling stays on the base package. D-BIND-13 and D-BIND-17 are revised: two packages, each a full shim; one shim per process still holds (L5, L17). | Owner ruling 2026-11-13, "Packaging: Separate server package" ([RIX-rulings.md](../../plans/RIX-rulings.md)) |
| D23 | **Both builds export a variant query; the batched entry points on the edge build answer a named not-supported status**, never a missing symbol. The edge surface gains only that query and those refusals (L12, L16). | Owner ruling 2026-11-13, "Wrong build: Clear 'not supported'" |
| D24 | **The shim's row codec is domain 2's;** domain 9 hosts it. Former L10 moves to domain 2 (d2 L2, L8). Resolves Q2. | Owner ruling 2026-11-13, "Codec owner: Domain 2" |
| D25 | **Domain 9's laws L1-L17 are confirmed** as amended by D22-D24; domain 9 is `specified` and stage design may start. | Owner ruling 2026-11-13, "Laws: Confirm, adjusted by answers" |
| D26 | **A batch handed to a callback is borrowed for the call; a binding keeps it only by moving it** (Arrow's C Data Interface move); what is not moved the shim releases when the callback returns. L11 holds; L14 amended. Resolves audit B1. | Owner ruling 2026-11-13, "Batch owner: Borrowed; keep by moving" ([RIX-rulings.md](../../plans/RIX-rulings.md), "D9S pre-round: batch ownership, version, error code, what C# loads") |
| D27 | **Both builds share one ABI version, and both bump** with the shim round. Recorded scope exception: the round edits one line of domain 10, the C# loader's expected version (`HeaderVersionMinor`, `dotnet/src/Fletcher.Interop/NativeLoader.cs`), in the same commit as the bump. Resolves Q8 and audit LC-1 / OQ-1 (L6, L12). | Owner ruling 2026-11-13, "Version: One version; edit the line" |
| D28 | **`FL_NOT_SUPPORTED` is reused; its meaning widens to "this provider or this build cannot do this"**; no new status number. Resolves Q3's residue and audit LC-2 (L2, L12, §2). | Owner ruling 2026-11-13, "Error code: Reuse, widen meaning" |
| D29 | **Revision in place of D19's "C# on desktop loads the server variant":** a C# app loads the server build only if it references the server package, otherwise the edge build. Consequence owned by the C# switch round: C#'s per-row subscribe must work on the edge build, without a batcher (§6). Resolves audit R-2 / F2. | Owner ruling 2026-11-13, "C# loads: Depends on package" |
| D30 | **A batch the shim cannot hand over is reported, never lost silently:** a "no batch could be built" delivery for that window, its rows counted as dropped, its reason kept; one site in the shim (register BC-12); domain 7 untouched. Recorded as law L18. Resolves audit census C5 / F12. | Owner ruling 2026-11-13, "Lost handoff: Report it as lost" ("D9S pre-round: a failed handoff is reported; corrections approved") |
| D31 | **The audit's tier-2 text corrections are approved** and applied before the round: F1 (L12), F3 (L4, §1), F4 (L1), F6 (L6), F7 (L7), F9 (L12, D14), F15 (§8); F17-F19 (BIND annotations); F10, F22, F23 (`binding.h` comments); F24-F26 (`c-abi/README.md`); F27 (ledger notes). | Owner ruling 2026-11-13, "Corrections: Approve all as listed" |

## 6. Relationships

- **Domain 3 (provider interface).** The shim is the C form of domain 3's `Publisher`, `Subscriber`,
  `SchemaArrival`, `Attachments`, `Blob`, `ProviderRegistry` and status taxonomy, and forwards their
  rules (L4). Re-entrancy (`FL_REENTRANT_CALL`), the unsubscribe barrier and the termination on a
  destroy inside a delivery are domain 3's answers.
- **Domain 6 (driver ABI).** The mirror image below the seam; no shared header (D4). The path selector
  waits for PDA-ABI's resolver seat.
- **Domain 2 (row encoding).** Owns `NanoarrowCodec`, which this domain hosts (D24): what it writes
  and refuses is domain 2's law (d2 L2, L8). This domain owns its entry points, handles and
  containment. Domain 2's `BatchDecoder` will also run inside the server variant (via domain 7).
- **Domain 4 (transports).** Three providers are linked into the shim as built-ins; their limits
  (XRCE's `domain_id` ≤ 65535, no schema channel outside `fastdds`) reach callers unchanged.
- **Domain 7 (batch assembly).** The server variant exposes it (L15). Obligations from d7 §4 and §6:
  `kNoTimeout` as `INT64_MAX` ms untranslated; a narrower field maps its own maximum onto it; L15's
  refusal passed through (BC-8); no batching of its own (BC-9). Domain 7's L9 exceptions (closing from
  inside a batch callback does not wait) and L12 reach C callers through the shim. A batch the shim
  cannot hand over is reported by the shim (L18, D30); domain 7 is untouched by that rule.
- **Domain 10 (C# binding), anticipated.** Loads the shim; owns the handshake's managed half,
  `Timeout.Infinite` mapping, `SafeHandle`s, managed refusals (D-BIND-18, -51), and the deletion of
  the managed batcher once it reaches domain 7 through the server variant.
  - **What a C# app loads** (D29, 2026-11-13, revising D19): the server build only if the app
    references the server package; otherwise the edge build. **Consequence, owned by the C# switch
    round:** C#'s per-row subscribe must then work on the edge build, without a batcher.
  - **The one line this round edits in domain 10** (D27): `HeaderVersionMinor` in
    `dotnet/src/Fletcher.Interop/NativeLoader.cs`, in the bump's commit (L6).
  - **Collisions registered for the C# switch round** (audit, not ruled here):
    **R-3** (F21): D-BIND-59 (1) and the 2026-11-12 "Shim only" (2) make C#'s per-row
    `SubscriberArrow.Subscribe` a batched one with `MaxRows = 1`; on the base package (edge build)
    there is no batcher once the managed one is deleted (d7 D16). Either it moves to
    `fl_subscriber_subscribe` + `fl_decode_rows`, or per-row `SubscriberArrow` needs the server
    package. D29's consequence above points at the first.
    **R-4** (F20): D-BIND-19 rule 4 and D-BIND-20 (managed pre-checks, "native re-checking the same
    rules") against 2026-11-12 "Refuse both", d7 L15 and BC-8 (a binding passes domain 7's refusal
    through). "C# throws as it does today" reads either way; BC-8 and d7 L15 take "C# throws from
    domain 7's refusal". Translating -1 to `INT64_MAX` is lawful either way.
  - **Domain-10 questions** (moved from §7 Q5a, with audit R-9 / F14): the two packages' native file
    names (distinct, so both can sit in one app's output?), whether the server package depends on
    the base one, how a binding chooses which to load (census C10, BC-10's deferred half), whether
    D-BIND-148's exact DLL allow-list and D-BIND-149's floors apply to the server shim as they stand
    or with lists and floors of its own, and whether the server package gets a size ceiling of its
    own (D-BIND-157 stays on the base package, D22).
- **Domain 5 (gateway).** Not a consumer: `Eiva.Fletcher.GatewayClient` has its own managed codec
  and batcher (D-BIND-1's one exception; BC-9).

## 7. Open questions

| # | Question | Owner |
|---|---|---|
| Q1 | Resolved 2026-11-13 (D23, L16): a variant query both builds export. Its name and answer type are the shim round's design. | — |
| Q2 | Resolved 2026-11-13 (D24): domain 2 owns `NanoarrowCodec`; former L10 is d2 L2 and L8. | — |
| Q3 | Resolved 2026-11-13 (D23, L12): one surface; the edge build exports the batched entry points and refuses them by name. Residue resolved 2026-11-13 (D28, audit LC-2): "named" is `FL_NOT_SUPPORTED` plus a message; its meaning widens to "this provider or this build cannot do this"; no new number. | — |
| Q4 | Resolved by D9S-3 (`52a0103`): `scripts/check_edge_arrow_free.sh` checks the edge build's graph and the edge shim's names for Arrow C++ by name (BC-11); Windows by hand, Linux and CI unrun. | — |
| Q5 | Resolved 2026-11-13 (D22): the server variant ships in its own .NET package; D-BIND-157 stays on the base package; D-BIND-13 and -17 revised. | — |
| Q5a | Packaging residue the ruling does not settle: do the two packages' native files have distinct names (so both can sit in one app's output), does the server package depend on the base one, how does a binding choose which to load, and does the server package get a size ceiling of its own? **The shim-side half is resolved (owner ruling 2026-11-13, D9S-2 Decision 1): the server build's file is `fletcher-c-abi-server`.** **Registered 2026-11-13 as domain-10 questions in §6**, together with R-9 (D-BIND-148 / -149 for the server shim). | domain 10 (C# switch round), with the shim round for the native file name |
| Q6 | How do a batch's row-aligned attachments, `BatchStatus` and a null batch cross? Per-row `fl_attachments` handles, a flattened form, or a C Data column? **Resolved for `BatchStatus` and the null batch (D9S-4a):** `int32_t reason`, `int64_t rows_dropped`, and `array == schema == NULL` for no batch (a zero-row batch has an array). **Attachments resolved (D9S-4b, `[built]`):** one `fl_attachments` per row, an empty set rather than a NULL entry, the list NULL exactly when the batch has no rows. | shim round (4b), then owner if it widens the seam's vocabulary |
| Q7 | How are `kNoTimeout` and L15's refusal expressed in the C signature: an `int64_t timeout_ms` with `INT64_MAX` as no timeout (as `fl_schema_arrival_wait` does), and `FL_INVALID_ARGUMENT` with domain 7's message? D-D7O-1 says a plain value; confirm the field width. **Resolved (D9S-4a):** `int64_t max_rows` and `int64_t timeout_ms`, `INT64_MAX` is `kNoTimeout` untranslated; the refusal is domain 7's, passed through (its test is rung 7, D9S-5). | — |
| Q8 | Resolved 2026-11-13 (D27, audit LC-1): one `FL_BINDING_ABI_VERSION` for both builds, and both bump; the C# loader's expected minor moves in the same commit (the named scope exception, L6). | — |
| Q9 | After the C# switch, `fl_decode_rows_framed` exists only for the deleted managed batcher. Does it stay (L13: the edge surface is unchanged) or go (a 0.x minor may remove it)? Also carries Findings (d)6. | C# switch round (domain 10) |
| Q10 | Which consumer loads the edge variant once C# desktop moves? "Embedded devices" has no RID today (D-BIND-27: `win-x64`, `linux-x64`; `linux-arm64` first addition). Partly answered 2026-11-13 (D29): a C# app that does not reference the server package loads the edge build. The embedded-device consumer remains open. | owner |
| Q11 | A batched dictionary column arrives re-folded (domain 2's `BatchDecoder`), while `fl_decode_rows` returns its value type (D-BIND-39, D-BIND-59). Is that difference intended for a server-variant caller? | domain 2, with domain 10 |
| Q12 | Which `fl_origin` does the edge build's `FL_NOT_SUPPORTED` refusal carry, and which a failed hand-over on domain 7's batch thread (L18)? `FL_ORIGIN_SEAM` is "a `PubSubError` from the seam". A new enumerator is the shim's own (append, bump, L6), not a seam stop-and-ask (audit F5). **Edge half resolved (D9S-4a): `FL_ORIGIN_SEAM`**, as every refusal the shim makes itself; the comment on `FL_ORIGIN_SEAM` widens to "...or the shim's own refusal of an argument or of a call this build cannot serve" and D-BIND-54 switches only on (`FL_INVALID_ARGUMENT`, `FL_ORIGIN_CODEC`), so it is unaffected. **Hand-over half open**, owed to D9S-6. | shim round (D9S-6, hand-over half) |
| LC-1 | Audit owner confirmation: does the edge build's minor bump, with one version for both builds? **Resolved 2026-11-13 (D27).** | — |
| LC-2 | Audit owner confirmation: may the edge refusal reuse `FL_NOT_SUPPORTED`? **Resolved 2026-11-13 (D28):** yes, its meaning widened. Residue: the seam's own published table (`core/README.md:37`, domain 3's text) still reads "This provider does not implement the requested behaviour"; the origin is Q12. | domain 3, for the table's wording |

## 8. How to extend this domain

1. **Forward, do not answer.** A new entry point calls the layer that owns the rule and lets its
   refusal through `Contain`; the shim checks only what that layer cannot see, and records it here as
   L4 does its exceptions.
2. **One containment site.** Every fallible entry point is a `Contain` call; no `try` of its own, no
   status number of its own (L1, L2).
3. **State ownership in the header's three words,** name the one release function, and make the
   handle tolerate any release order (L3).
4. **Bump and record.** Append to the header, bump the minor, record the ruling; never stub (L6).
5. **Variant discipline:** an addition that needs Arrow C++ is implemented in the server variant only;
   the edge build exports it as a named not-supported refusal (L12), and its links and size check are
   re-run unchanged (L8, L12; audit F15). A change to the codec's bytes is domain 2's (D24).
6. **Record the change.** Write any change to a law here before writing the code, with its build state.

## Findings (reported, not fixed)

**(a) Documents that disagree with the code**
1. **The marker's text.** `single_copy.cpp:119` returns "ABI 0.1"; the shim is 0.7 (`h:163-164`). The
   header promises it "carries the component and its ABI version" (`single_copy.hpp:42-43`).
2. **The schema watch.** `cpp:11-18` says `fl_subscriber_subscribe_schema` answers `FL_NOT_SUPPORTED`
   until the seam grows the pair; it is implemented (`cpp:889-915`, D-BIND-52).
3. **Windows exports.** `h:135-136` and `CMakeLists.txt:127` say "Revisited at BIND-9"; D-BIND-157
   accepted them (README:59-60 is current).
4. **Counts.** `CMakeLists.txt:92-93` says twenty entry points are exported; there are 47.
   `cpp:104-106` says no exported function constructs an `fl_attachments`; the builder does (`cpp:486-497`).
5. **Single-copy wording.** `h:382-385` says the shim "REFUSES TO INITIALISE"; only the copy loaded
   second refuses, and it still loads (L5).
6. **The link-error claim.** `h:79` held for static linking only; a P/Invoke caller sees a run-time
   error (L6). **Fixed 2026-11-13** (audit F22): `h:81-82` now says so.
7. **The status taxonomy test.** `h:180-183` names a C# port of `test_status_taxonomy`; none was found
   under `dotnet/tests` (searched by name and by `FletcherStatus`). Unsure: it may live under another
   name.

**(b) Rules answered in two places**
1. **Encoding the wire format:** `NanoarrowCodec` and `arrow-bridge`'s `Codec`, held together by byte
   identity (D-BIND-39). Accepted by decision; since 2026-11-13 both are domain 2's (d2 L2; D24).
2. **"May these rows be published on this topic?"** `RequireRowsMatchTopic` (`cpp:182-222`) and C++
   `PublisherArrow`'s per-topic codec. Same answer by design (D-BIND-67), two sites, no test that
   compares them.
3. **"Are these bytes a valid row?"** `NanoarrowCodec::DecodeRows`/`DecodeRowsFramed` and domain 2's
   `BatchDecoder`. Today in different binaries; in the server variant, in one. No parity test.
   Domain 2's question since 2026-11-13 (d2 L8).
4. **Range checks on a timeout:** `SchemaArrival::Wait` refuses a negative; the shim forwards it
   (`cpp:941-946`). Not a second answer, recorded so the server variant follows the same pattern
   (L15).

**(b2) Same topic, one provider (D9S-4a, owner ruling 2026-11-13, "one subscriber object").** A row
subscription and a batched one to the same topic through one provider collide, because batched
subscriptions run on a second subscriber of the provider (seam §7 clause 4: one callback per topic per
instance). The providers answer it differently: on `inprocess` the later silently replaces the earlier
(the earlier stops with no signal; a still-pending arrival answers `FL_SUBSCRIPTION_ENDED`), on `fastdds`
the later is refused with `FL_INVALID_ARGUMENT` ("already subscribed"). The shim forwards the rule and
documents it (`binding.h`); it does not refuse the mix (L4). **Reported to domains 3 and 4:** two
providers, two answers to one seam rule.

**(c) Behaviour with no test**
- That every void entry point cannot throw (L1).
- That the C `fl_status` numbers equal `PubSubStatus` (L2); a `static_assert` per enumerator would do it.
- That a failed call leaves OUT parameters untouched (L3).
- The append-only rule within a major (L6).
- ~~That the edge shim contains no Arrow C++ (L8): only the size ceiling.~~ Checked by name since
  D9S-3 (`scripts/check_edge_arrow_free.sh`; Windows, by hand; Linux and CI unrun).
- The Windows half of the export-table property: violated, accepted (L7).

**(d) Tier-1 collisions for the owner** (between a BIND decision, a 2026-11-12 ruling and the code).
Items 2-4 resolved by the 2026-11-13 packaging ruling (D22); item 6 handed to the C# switch round.
1. **BIND Decision 6 / D-BIND-1a rider ii vs "one shim, two variants".** Known and accepted as a
   partial supersession (R-4); the annotation is owed by the shim round. **Annotated 2026-11-13.**
2. **D-BIND-157 (12 MB packed ceiling on `Eiva.Fletcher.Interop`) vs C# desktop loading a server
   variant that carries Arrow C++.** If the server variant ships in Interop, the ceiling (and CI's
   12 MiB / 20 MiB shim ceilings, whose message says they exist to catch Arrow C++) will trip; the
   2026-11-12 ruling records the Arrow C++ cost, not the ceiling. **Resolved 2026-11-13 (D22):** the
   server variant ships in its own package; the ceiling stays on the base package.
3. **D-BIND-13 (native assets in ONE package) vs a server variant in its own package.** Shipping it
   separately breaks D-BIND-13 ("Adding native assets to a second package → STOP-AND-ASK").
   **Resolved 2026-11-13 (D22):** D-BIND-13 revised; annotated 2026-11-13.
4. **D-BIND-17 ("no other binary the package ships carries Fletcher code") vs one package carrying
   both variants per RID.** Shipping both in Interop means two Fletcher binaries in one package.
   Items 2-4 together meant every packaging choice met one locked decision. **Resolved 2026-11-13
   (D22):** D-BIND-17 revised to two packages each carrying a full shim, one loaded per process
   (L5, L17); annotated 2026-11-13.
5. **D-BIND-1a rider i ("a .NET process is never the microcontroller", so the edge/server split "may
   not need mirroring in C#") vs the variant split now being mirrored in what C# loads.** Probably
   compatible (desktop C# loads server); listed because the rider argued the opposite direction.
   Since D29 a C# app chooses its build by package; still unruled (audit R-10, low).
6. **D-BIND-25's managed Arrow tier and D-BIND-68's `fl_decode_rows_framed`** exist for the batcher the
   2026-11-12 ruling deletes (d7 D16 records D-BIND-25's supersession; D-BIND-68 is not mentioned).
   **Owned by the C# switch round (domain 10)**, with Q9.
