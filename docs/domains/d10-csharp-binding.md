# Domain 10: The C# binding

> **`specified`: owner-confirmed 2026-11-15** ("Confirm (Rec.)", round D10C, the C# switch round, first
> deliverable). Survey mode for what exists in `dotnet/` (verified at `fb0f5bf`); round mode for the switch.
> **D10C pre-round rulings (2026-11-15) applied:** ledger :536 (per-row Arrow on the native per-row
> path, on both packages; distinct file names, the server package depends on the base one, the
> loader prefers the server shim; domain 7's option refusal passed through and the cap dropped; the
> .NET edge/server split kept) and ledger :551 (the batched API on every package, refusing by name on
> edge; dictionaries re-folded as domain 7 delivers them; a failed hand-over in `BatchStatus` with
> origin `Handover` (4); D-BIND-64, D-BIND-59 (3) and the schema-less latch revised by :203). They
> became D20-D27 and closed former Q1-Q6 and contradictions 1-8; BIND annotations written the same
> day. **D10C pre-round audit rulings (2026-11-15, ledger :575) applied:** R-1 to R-6 and B1 of
> [plans/D10C-pre-round-audit.md](../../plans/D10C-pre-round-audit.md) became D28-D33; the PM's
> `BatchReason` renumbering under L2 is D34; the audit's F1 (L12) and F8 (§8) corrections applied.

**Mode: survey for everything that exists in `dotnet/`, verified against the tree at `fb0f5bf`**
(branch `feature/csharp-switch`). The code is ground truth. The BIND plan
([BIND-csharp-bindings.md](../../plans/BIND-csharp-bindings.md)), its digest
([BIND-locked-decisions.md](../../plans/BIND-locked-decisions.md)), code comments and READMEs are
claims checked against it. **Round mode for what the switch will make true:** written from the
rulings in [RIX-rulings.md](../../plans/RIX-rulings.md) (cited `ledger :N`, the line of the ruling's
heading) and from d7 and d9, and marked `[designed-not-built]`. Citations: `SA:N` is
[`dotnet/src/Fletcher/SubscriberArrow.cs`](../../dotnet/src/Fletcher/SubscriberArrow.cs), `NL:N`
is [`dotnet/src/Fletcher.Interop/NativeLoader.cs`](../../dotnet/src/Fletcher.Interop/NativeLoader.cs),
`NM:N` is [`dotnet/src/Fletcher.Interop/NativeMethods.cs`](../../dotnet/src/Fletcher.Interop/NativeMethods.cs),
`FE:N` is [`dotnet/src/Fletcher/FletcherException.cs`](../../dotnet/src/Fletcher/FletcherException.cs);
other files are named. `h:N` is `c-abi/include/fletcher/abi/binding.h`, as in d9. C# tests are in
`dotnet/tests/`, named `Class.Method`; d7 tests are named without their fixture prefix, as in d7.

**What the C# binding stands on is Windows-only evidence.** Every server-build mark in d9 reads
"Windows, by hand; CI and Linux unrun" until a green CI run on the owner's draft PR covers it
(ledger :408 (2)). A domain-10 `[built]` mark that depends on the server build inherits that
qualifier.

## 1. Intent

**Owns:** the managed packages through which a .NET application reaches the C ABI shim, and every
C# idiom laid over it (domain-map row 10; owner ruling 2026-11-12, "Shim only", ledger :246 (1)):
- **`Eiva.Fletcher.Interop`**: the P/Invoke mirror of `binding.h` (`NativeMethods`, `AbiTypes`,
  the `SafeHandle`s in `Handles.cs`), the loader and the ABI handshake (`NativeLoader`), and the
  native asset it packs (the edge shim, D-BIND-13 as revised by d9 D22).
- **`Eiva.Fletcher`**: the managed tier: `Publisher`, `Subscriber`, `SubscriberArrow`,
  `FletcherCodec`, `BoundRows`, `SchemaArrival`, `ProviderRegistry`, `TopicPath`, the exception
  taxonomy (`FletcherException`, `FletcherStatus`, `FletcherOrigin`) and its one throw site
  (`Errors.ThrowIfFailed`), the delivery thunks and their lifetime bookkeeping (D-BIND-18, -50,
  -51).
- **`Eiva.Fletcher.Model`**: the lossless `Timestamp`/`Duration` and the collection views that
  generated code uses (D-BIND-74); domain 10 owns the package, domain 1 owns what is generated
  against it.
- **The server package**, `Eiva.Fletcher.Server` (ledger :605 (1)) `[built for both shim packages (D10C-5; Linux devcontainer and Windows by hand, CI unrun)]`: the separate .NET package
  carrying the server build of the shim (d9 D22) under its own file name, depending on the base
  package; the loader prefers the server shim when it is present (ledger :536 (2), D21; settles d9 Q5a and register
  BC-10's deferred half).
- **Mapping each C ABI answer onto a C# idiom at one site**: a status to an exception, an origin
  to a typed exception, `Timeout.InfiniteTimeSpan` to `INT64_MAX`, a borrowed C structure to a
  managed owner, a managed refusal before a call native would answer with termination.

**Refuses:**
- **Batch assembly** (register BC-9; d7 D16). Turning rows into batches is domain 7's. The managed
  `Batcher` (`SA:592-1066`) is a copy that the switch deletes (L4). Until then it stays exactly as
  it is: no interim fixes (ledger :203 (3), :278).
- **Answering a question a lower layer owns** (L3). Batch options are domain 7's (BC-8, d7 L15);
  topic rules, re-entrancy and unsubscribe semantics are domain 3's, forwarded by the shim (d9 L4);
  row bytes are domain 2's (D-BIND-1). The binding forwards their refusals. The exceptions are
  ruled ones only (L3).
- **A managed codec on the native path** (D-BIND-1). The positional codec runs in the shim.
- **The C# gateway client**, `Eiva.Fletcher.GatewayClient`, with its managed codec and its own
  `GatewayBatcher`: domain 5's (domain-map row 10; BC-9 "the `GatewayBatcher` has none").
- **Generated C#** (the protoc plugin's output, `<Msg>`, `<Msg>Accessor`, the generated
  publisher/subscriber pair): domain 1's.
- **A provider authored in C#** (D-BIND-24): providers are opaque handles; no `Register`, no
  resolver.
- **Shipping C#'s batched subscribe to `main` before the switch lands.** PR #129 does not merge with
  the managed batcher in it; if shipping becomes urgent the question returns to the owner
  (ledger :278; domain-map planned order 6).

## 2. Ubiquitous language

| Term | Definition (as the code behaves) |
|---|---|
| **Binding** | This domain: the managed packages over the shim. `binding.h` calls the C surface "the binding ABI"; that surface is domain 9's (d9 §2 "Binding"). |
| **Base package** | `Eiva.Fletcher.Interop`, which packs the edge shim under `runtimes/<rid>/native/` for `win-x64` and `linux-x64` (`Fletcher.Interop.csproj:52-55`, D-BIND-27, -134). `Eiva.Fletcher` references it. |
| **Server package** | `Eiva.Fletcher.Server` (ledger :605 (1)) `[built for both shim packages (D10C-5; Linux devcontainer and Windows by hand, CI unrun)]`. The separate .NET package carrying the server shim (d9 D22) under its own file name, and nothing else (no assembly), depending on the base package (ledger :536 (2), D21). |
| **Edge shim / server shim** | The two builds of the one shim (d9 L12): `fletcher-c-abi` and `fletcher-c-abi-server` (ruling 2026-11-13, ledger :395). C# loads the server file when it is present in the app's folders, else the edge file (L5, D21, D28; `NativeLoader.CandidatePaths`, D10C-4). |
| **Loader** | `NativeLoader` (`NL:27-340`): installs the per-assembly resolver, probes beside the assembly then the application (every server candidate before every edge candidate, D10C-4), and explains a failure by RID, by the refused file and its build, or by platform floor (D-BIND-172). |
| **Handshake** | `NativeMethods`' type initializer: install the resolver, then compare `fl_binding_abi_version()` with `HeaderVersion` before any other import can run (`NM:72-79`, `NL:320-339`). |
| **Variant check** | `[built]` (D10C-7; Windows, by hand; CI and Linux unrun) The binding asking the loaded shim `fl_binding_variant` (d9 L16). One call site, `Subscriber.SubscribeBatches`, asked only after the shim refused a batched subscribe, to word the refusal (L3, L6); `NativeLoaderTests.NoNativeLoadOutsideTheResolver` holds it to exactly one (BC-10). |
| **Thunk** | An `[UnmanagedCallersOnly]` static method native calls: the delivery thunk (`Subscriber.cs`, `DeliveryThunk`), the batch thunk (`SubscriberArrow.BatchThunk`, D10C-7), the writer and grow thunks. One containment site per direction (D-BIND-19 rule 2). |
| **Absorbed failure** | A managed handler exception caught at a thunk, counted in `AbsorbedCallbackFailures` and `Diagnostics.AbsorbedTotal`, and raised as `HandlerFaulted` (D-BIND-18, -19 rule 5). |
| **Managed batcher** | `SubscriberArrow.Batcher`: C#'s former copy of batch assembly. ~~Deleted by the switch (d7 D16).~~ Deleted by D10C-7 (L4). |
| **Window (C#)** | In the former managed batcher (deleted by D10C-7), the raw rows, their end offsets and attachments since the last flush. D-BIND-68's "framed window". Since the switch the word means domain 7's window only. |
| **Batch handler** | `RecordBatchHandler(RecordBatch? batch, IReadOnlyList<AttachmentsBuilder> attachments, BatchStatus status)` (`SA:114`). |
| **Per-row Arrow subscribe** | `SubscriberArrow.Subscribe`: the native per-row path, no batcher (`RowDelivery`, D10C-3; ledger :536 (1), D20); on the edge shim now, on the server package from rung 4. Distinct from `Subscriber.Subscribe`, which hands the borrowed raw row to a `RowHandler`. |
| **Switch** | This round's change: `SubscriberArrow`'s batches come from domain 7 through the server shim's `fl_subscriber_subscribe_batches`, and the managed batcher is deleted. |

**Naming findings:**
- **"Per-row subscribe"** has two C# meanings: `Subscriber.Subscribe` (raw borrowed row) and
  `SubscriberArrow.Subscribe` (a one-row `RecordBatch`). The 2026-11-13 consequence "C#'s per-row
  subscribe must work on the edge build without a batcher" (ledger :363 (4)) did not say which;
  ledger :536 (1) settles it as `SubscriberArrow.Subscribe` (D20).
- **"Window"** has the three meanings d9 lists, plus C#'s framed window.
- ~~**"Interop is the only package with native assets"** is still stated in code
  (`Fletcher.Interop.csproj:6-10`) and checked (`scripts/check_dotnet_packages.sh:12-13`, `:108`);
  D22 made it false by ruling.~~ **Done (D10C-5):** the comment says L15 and the check is one rule list
  over five packages.
- **`FletcherStatus.NotSupported`** is documented as "This provider does not implement the requested
  behaviour" (`FE:38`); D28 widened the meaning to "this provider or this build cannot do this".
  **`FletcherOrigin.Seam`** reads "a PubSubError or a std::exception" (`FE:67`); `h:265` now adds
  "or the shim's own refusal" (d9 Q12).

## 3. Invariants and laws

**L1. The binding never calls a shim it has not verified.** Before any entry point is reachable,
the loaded shim's ABI version is compared with the version the binding was built against; before
1.0 the minor must match exactly, and a mismatch refuses with a message naming both versions.
`[built]`
- Holds at: `NM:72-79` (the type initializer installs the resolver, then verifies, before any
  other import resolves), `NL:320-339` (`VerifyAbiVersion`), `NL:33-39` (`HeaderVersionMajor` 0,
  `HeaderVersionMinor` 8: the one line D9S edited under d9 D27, ledger :363 (2)).
- Both builds share one version (d9 L6, D27), so the handshake cannot tell them apart; L6 does.
- Tests: `NativeLoaderTests.TheShimLoadsAndReportsTheAbiVersionThisBindingWasBuiltAgainst`,
  `.TheHandshakeAcceptsItsOwnVersion`, `.TheHandshakeRefusesAnyOtherVersion`.

**L2. The binding mirrors the C surface; it authors none of it.** `NativeMethods` and `AbiTypes`
are a mechanical mirror of `binding.h`: same names, same parameters, blittable layouts. Every
number the shim hands out (`fl_status`, `fl_origin`, `FL_BATCH_*`, `fl_variant`) has a C# value
equal to it, never renumbered.
`[built]` (D10C-2): `fl_status` 0-10, `fl_origin` 0-4, `FL_BATCH_*` 1-3 and `fl_variant` 1-2 each equal their C# mirror, by test.
- Holds at: `NM:1-54` (the mirror rule), `AbiTypes.cs:1-193` (blittable by construction;
  `LibraryImport` refuses otherwise), `FE:18-52` (`FletcherStatus` 0-10), `FE:62-78`
  (`FletcherOrigin` 0-4), `SA:117-130` (`BatchReason` 1-3), `NM:37-47` (`FlVariant`).
- **The violation (closed by D10C-2):** `h:264-269` declares `FL_ORIGIN_HANDOVER = 4` (d9 D32, ABI 0.8); C# stops at
  3. It is unreachable today, because C# never calls a batched entry point, and `Errors.ThrowIfFailed`
  would cast 4 to an unnamed enum value (`Errors.cs:65-67`). The switch adds
  `FletcherOrigin.Handover = 4` (d9 D32, "Forward note (domain 10)"; ledger :551 (3), D26).
- **The second violation (closed by D10C-2):** `BatchReason {RowLimit, Timeout, Closing}` is 0-based (`SA:117-130`);
  `FL_BATCH_ROW_LIMIT/TIMEOUT/CLOSING` are 1-3 (`h:1013-1015`). **Decided (D34, PM decision under this
  law, 2026-11-15):** the switch renumbers `BatchReason` to 1-3, equal to the C values. A visible change
  (a cast or persisted value shifts by one; `default(BatchReason)` is no longer a named member),
  accepted because the batched API has not reached `main` (D4). `[built]` (D10C-2).
- The mirror imports what the binding calls, not every export. Since D10C-7 it imports
  `fl_subscriber_subscribe_batches`, `fl_subscriber_unsubscribe_batches` and the whole `fl_batch`
  (`failure` included), and no longer imports `fl_decode_rows_framed`, which the
  shim keeps (Q8 default). That is not a violation.
- Tests: `AbiNumberTableTests.EveryNumberTheShimHandsOutHasTheSameValueInCSharp` (one row per table, both
  directions; closes Q11), `ErrorTests.OriginFourIsHandover`, `AbiNumberTableTests.BindingVariantAnswersADefinedMember`.
  While `FlVariant` is internal, its row is the sole proof of the variant's numbers; rung 4 re-proves it from
  outside if it makes the variant public.

**L3. The binding forwards a lower layer's answer; it does not answer again.** A refusal from the
shim reaches the caller with its number and verbatim message. The binding checks a rule itself only
where a ruling accepts the duplication, and then native re-checks it.
`[built for the arrival timeout, topics and selectors and the same-topic refusal (D10C-6); for the edge refusal's
sentence (D10C-7; Windows, by hand; CI and Linux unrun); for batch options, built and tested (D10C-9: passed as given; the clamp, the managed refusal and
`BatchOptions.MaxTimeout` deleted; `BatchOptionsTests.EveryBatchOptionReachesDomain7AsGiven`, BC-8's 6 x 7 grid, and the source scan
`NoBatchOptionIsCheckedOrClampedInCSharp`; Windows, by hand; CI and Linux unrun)]`.
- Holds at: `Errors.cs:52-92` (one throw site, `ThrowIfFailed`, over one decoder, `ToException`; number and message cross, D-BIND-19 rules 1-3).
- **Accepted duplications, by ruling:** topic segments and the provider selector are checked in
  managed code in UTF-8 bytes and re-checked natively (Q17 of 2026-09-11, D-BIND-19 rule 4,
  D-BIND-20; `TopicPath.cs`, `ProviderSelector.cs`).
- **The violation, closed by D10C-7 (register BC-8 copies; the lines cited are the pre-switch tree):** `SA:258-265` refuses a negative timeout and a finite
  timeout above `BatchOptions.MaxTimeout` (~49.7 days, `SA:161`), and `SA:267` clamps `MaxRows`
  into `[1, int.MaxValue]`. Domain 7 is the one site (d7 L15, BC-8); its binding clause ("a binding
  passes the refusal through") is `[designed-not-built]`, owned by this round (d7 L15 mark). Ruling
  2026-11-12, "Refuse both" (ledger :235): "C# callers passing MaxRows < 1 now get an exception
  instead of 1". **Settled 2026-11-15 (ledger :536 (3), D22):** the exception is domain 7's refusal
  passed through, `FletcherException(InvalidArgument)` with domain 7's message; the `MaxTimeout`
  cap goes (deleted by D10C-9); D-BIND-19 rule 4's and D-BIND-20's managed pre-check no longer applies to batch options
  (it still applies to topics and selectors). **Confirmed 2026-11-15 (ledger :575 (4), D31):** the type
  is `FletcherException(InvalidArgument)`, not an `ArgumentException`; :235's "C# throws as it does
  today" is revised. Visible change: code catching `ArgumentOutOfRangeException` stops catching it.
- **One binding sentence (ledger :575 (2), D29).** "Verbatim" means the shim's message reaches the
  caller unaltered. At one site, the batched entry point's wrapper, the binding may append exactly one
  sentence of its own after it; the only such sentence is the edge refusal's, naming the server package
  (L6). The number and origin are never changed. `[built]` (D10C-7; Windows, by hand; CI and Linux unrun): `Subscriber.SubscribeBatches`
  passes it to `Errors.ThrowIfFailed`'s one optional sentence, only when the build answers Edge.
- **Ruled refusal: the same-topic mix (ledger :575 (6), D33).** A `SubscriberArrow` refuses a row
  subscription and a batched subscription to the same topic, in managed code, with a named exception,
  whichever comes second. It is this object's own rule: its two kinds share one `fl_subscriber` and one
  provider, where the transport would let the later silently replace the earlier (d9 D33). It revises
  :448 (1)'s "documented, not refused" for C#; the transport's rule between separate subscribers stays
  domain 3/4's. `[built]` (D10C-6: `SubscriberArrow.cs`, `Claim` at the two subscribe entry points and
  the release in `Unsubscribe`; `SubscriberArrowTests.ARowAndABatchedSubscriptionToOneTopicAreRefusedWhicheverComesSecond`).
- Tests: `SchemaArrivalTests.*` (negative refused, infinite accepted);
  `SubscriberArrowTests.ATimeoutPastTheTimersMaximumIsRefused`, which pinned the BC-8 copy, is deleted, not
  re-anchored (§8 rule 6; amended by ledger :650 (1); deleted by D10C-7); the edge refusal's sentence:
  `SubscriberArrowTests.ABatchedSubscribeOnTheEdgeShimIsRefusedNamingTheServerPackage` and the edge leg of
  every batched test (`EdgeRefuses`); the same-topic refusal (D10C-6): `SubscriberArrowTests.ARowAndABatchedSubscriptionToOneTopicAreRefusedWhicheverComesSecond`,
  with two guards, `TheSameKindTwiceAndDifferentTopicsAreNotRefused` and
  `TwoArrowSubscribersOnOneProviderDoNotRefuseEachOther` (guards: they fail if the rule is widened past the mix or the object).

**L4. There is one batch assembler, and it is not in C#.** `SubscriberArrow`'s batches are domain
7's, reached through the server shim. The binding holds no window, row count, deadline, timer or
batch thread of its own.
`[built]` (D10C-7; Windows, by hand; CI and Linux unrun): the managed batcher, its window ceiling and its framed decode are deleted
(ruling :660); `SubscriberArrow`'s batches come from `fl_subscriber_subscribe_batches`.
- Source: ruling 2026-11-12, "one shim, two variants" (ledger :203 (1)); d7 D16; register BC-9.
- The managed batcher is deleted only after its tests are checked for cases domain 7's suite lacks
  (domain-map planned order 6; ledger :203, "I don't want to just delete the C# batcher if it
  contains functionality that should be ported over"). §8 lists the check.
- Structural tests (BC-9's shape, widened to the whole binding by design review C2-1):
  `ManagedBatcherGoneTests.TheBindingSourceHoldsNoTimerClockOrRowBuffer` (no timer type, no `Stopwatch` outside
  `SchemaArrival.cs`, no held row buffer or framed decode in `dotnet/src/Fletcher`) and
  `.TheManagedBatcherIsGoneFromTheAssemblies` (by metadata: no timer referenced in `Fletcher.dll`, `Stopwatch` used only
  by `SchemaArrival`, no `fl_decode_rows_framed` import in `Fletcher.Interop.dll`; on the build output, and in
  `ci.dotnet.yml`'s pack job on the packed assemblies through `FLETCHER_PACKED_ASSEMBLIES`). Behaviourally:
  `SubscriberArrowTests.EveryBatchArrivesOnOneThreadInWindowOrder` (L13).

**L5. A C# application loads the server shim when it is present beside the application (the server
package brings it); otherwise the edge shim; never both, and never the edge shim in place of a present
server shim that will not load.** Which file loads is decided at one site in the binding, from what is
present, not from what the application calls (revised 2026-11-15 by ledger :575 (1), D28; it read
"only if it references the server package").
`[built]` (D10C-4; Windows, by hand; CI and Linux unrun): `NativeLoader.Resolve` is the one site; every
server candidate before every edge candidate, the first present loaded once, no by-name probing
(`LoaderLayoutTests`, `NativeLoaderTests.NoNativeLoadOutsideTheResolver`).
- Source: ruling 2026-11-13, "C# loads: Depends on package" (ledger :363 (4); d9 D29, revising
  D19); d9 L5, L17 (one shim per process; a second loaded is refused by the marker); ruling
  2026-11-13, "the server build is its own file" (ledger :395): "the C# switch round teaches C#
  that name or renames it when packing".
- **Settled 2026-11-15 (ledger :536 (2), D21):** the two shims keep distinct file names
  (`fletcher-c-abi`, `fletcher-c-abi-server`), so both may sit in one application's output; the
  server package depends on the base package; `NativeLoader` prefers the server shim when it is
  present, else loads the edge shim. That resolver is the single site (BC-10's deferred half; d9
  Q5a settled). "References the server package" therefore means "the server shim is in the
  application's output".
- **Settled 2026-11-15 (ledger :575 (1), D28):** a server shim present beside the application is
  loaded; one present but unloadable (for example below its platform floor) stops the load with an
  exception naming the file and the operating system's reason; **there is no fallback to the edge
  shim.** Presence decides in every route the runtime takes, its default probing included (the
  by-name probe is deleted, D10C-4). Revises :363 (4)'s "references";
  refines :536 (2). `[built]` (D10C-4; Windows, by hand; CI and Linux unrun)
- CI's staging of the server shim under the edge file name (ledger :525) retires when the loader
  lands (ledger :536 (2)). Retired (D10C-4): both lanes stage the server file under its own name.
- **From packages** (D10C-11; local bundle, by hand: `run.sh` on the Linux devcontainer, whose .NET 8 builds ran rolled forward on .NET 10, and on Windows; `minimal.sh` through a Git Bash docker wrapper; CI unrun): the preview-bundle consumer (T46 and its shim check,
  `integration-tests/preview-bundle-consumer`), in a configuration that adds `Eiva.Fletcher.Server`:
  exactly one shim is loaded, `fletcher-c-abi-server` from `runtimes/<rid>/native/`, and never the
  edge shim beside it; and `minimal.sh`'s server arm below the platform floor: refused naming the
  server file, never the edge one.

**L6. A C# caller learns its build by asking, never from a missing symbol.** A batched call on the
edge build fails with a named not-supported exception that names the server package; it never
surfaces as `EntryPointNotFoundException` or a crash.
`[built]` (D10C-7; Windows, by hand; CI and Linux unrun): `SubscriberArrowTests.ABatchedSubscribeOnTheEdgeShimIsRefusedNamingTheServerPackage` and every
batched test's edge leg; the transport lane's edge pass (`TopicOptionsOverFastDdsTests`).
From packages (D10C-11; local bundle, by hand: `run.sh` on the Linux devcontainer, whose .NET 8 builds ran rolled forward on .NET 10, and on Windows; `minimal.sh` through a Git Bash docker wrapper; CI unrun): the preview-bundle consumer's base configuration (T46) and
`minimal.sh`'s at-floor base runs, which require `NotSupported`, origin `Seam`, the shim's text naming
`fletcher-c-abi-server` and the sentence naming `Eiva.Fletcher.Server`, with no rows arm.
- Source: ruling 2026-11-13, "Wrong build: Clear 'not supported'" (d9 D23, L12, L16); d9 D28
  ("this provider or this build cannot do this"). The shim's edge refusal is `[built]` (d9 L12),
  `FL_NOT_SUPPORTED` with `FL_ORIGIN_SEAM` and a message naming `fletcher-c-abi-server`
  (`h:1098-1101`).
- **Settled 2026-11-15 (ledger :551 (1), D25):** the batched API exists in `Eiva.Fletcher` on
  every package; on the edge shim it throws a named NotSupported exception naming the server
  package. **How it is named (ledger :575 (2), D29):** the shim's own message verbatim, then one
  binding sentence naming the server package (L3). C# does not refuse from the variant query itself;
  it forwards the shim's `FL_NOT_SUPPORTED` (register BC-10). Nothing is a compile error, and the generated subscriber (domain 1) is unaffected.
  Whether C# also exposes the loaded variant as a public property is the round's design.

**L7. Everything but the batched subscribe works on the edge shim.** A C# application that does not
reference the server package publishes, subscribes per row and decodes as it does today.
`[built]` (D10C-11, from packages; local bundle, by hand: Linux devcontainer with the .NET 8 builds rolled
forward on .NET 10, and Windows; `minimal.sh` through a Git Bash docker wrapper; CI unrun): the preview-bundle consumer's base
configuration (`Eiva.Fletcher` only) publishes, subscribes per row and decodes on the edge shim, and no
`*fletcher-c-abi-server*` file is anywhere in its output.
- Source: ruling 2026-11-13 (4), consequence: "C#'s per-row subscribe must work on the edge build
  without a batcher" (ledger :363; d9 D29, §6 R-3).
- Holds today at: `Subscriber.cs` (`fl_subscriber_subscribe`), `FletcherCodec.cs:260`
  (`fl_decode_rows`). `SubscriberArrow.Subscribe` and its `RowDelivery` (D10C-3), no batcher.
- **Settled 2026-11-15 (ledger :536 (1), D20):** `SubscriberArrow.Subscribe` moves to the native
  per-row path (`fl_subscriber_subscribe`, then the shim's per-row decode) and works on both
  packages. This arbitrates :246 (2) (whose "per-row is batched with `MaxRows = 1`" premise is
  amended) against :363 (4). The per-row path decodes each row natively and adds no window, count
  or timer (L4). **What it reports for a row it cannot decode is ruled (ledger :575 (3), D30):** a
  null batch with `RowsDropped = 1` (L9; register BC-16). `[built on the edge shim]` (D10C-3;
  `SubscriberArrowTests.PerRowDeliversEachRowAloneOrCountsItAsOneDrop`); the server leg is rung 4's.

**L8. A batch handed to the binding is borrowed for its callback; the binding keeps it only by
moving it.** Inside the batch callback the binding takes the array and schema by the C Data
Interface's move and gives the handler a managed `RecordBatch` it owns; it never keeps a pointer
into a borrowed batch, never releases what it did not move, and never disposes a batch's `failure`.
`[built]` (D10C-7; Windows, by hand; CI and Linux unrun): `SubscriberArrow.BatchDelivery.Deliver` moves (copies each struct and NULLs the source's
`release`, found with `Marshal.OffsetOf` on Apache.Arrow's own struct, whose field is internal), then imports; a failure
anywhere in the take-over still delivers the window, as a null batch with its rows counted.
Tests: the guard `SubscriberArrowTests.AKeptBatchReadsIntactAfterClosing` (server leg) and the leg
`.TheBatchThunkMovesTheBatchOutOfTheBorrowedStructs` (P2). The `failure` clause `[built]` (D10C-10; Windows, by hand; CI and
Linux unrun): the thunk copies it with `Errors.ToException`, which disposes nothing; leg `.TheBatchThunkNeverDisposesTheBorrowedFailure`
(the struct the shim lent is unchanged, and a source scan for the struct-copy case, review DEBT-1).
- Source: ruling 2026-11-13, "Batch owner: Borrowed; keep by moving" (ledger :363 (1); d9 D26,
  L14); d9 D32 and L2's borrowed clause (ledger :512): `failure` is copied to keep, never disposed.
- Keeps D-BIND-59 (3)'s ownership half for the handler ("the handler OWNS the batch and disposes
  it") by moving before the handler runs; its null-batch half is revised (ledger :551 (4), L14). Attachments: each row's `fl_attachments` is borrowed (d9 L14), so the
  binding copies into owned `AttachmentsBuilder`s, as today (D-BIND-59 (2)).

**L9. Nothing is lost or absorbed silently.** Every managed handler failure is caught at its thunk,
counted and raised; every row domain 7 or the shim reports lost reaches the handler in
`RowsDropped`, unchanged; a failed hand-over reaches the application with its status, origin and
message.
`[built for handler failures on the row path and in the batch thunk (D10C-7); built for the hand-over's failure in
BatchStatus (D10C-10); Windows, by hand; CI and Linux unrun]`. A window C# itself cannot take over arrives as a
null batch with its rows added to `RowsDropped` and no error (D10C-7 design, residue (a); ruling :669); so does a
failed hand-over whose failure C# cannot copy (out of memory): the window arrives, reason kept, without its failure (D10C-10).
Tests: `Fletcher.Handover.Tests.HandoverTests.AWindowTheShimCannotHandOverReachesTheHandlerWithItsFailure` (d9's
fault shim; a row-limit window and the Closing), its control `UnarmedNoWindowCarriesAFailure`, and the legs
`SubscriberArrowTests.TheBatchThunkForwardsAFailureAsGiven`, `.ACopiedFailureThatCannotBeDecodedStillDeliversTheWindow`.
- Holds at: `Subscriber.cs:824-877` (catch, `ReportAbsorbed`), `SA:453-468`, `SA:543-550` (per row)
  and `SubscriberArrow.BatchDelivery.Deliver` (batch handler failures counted and raised; D10C-7), `Diagnostics.cs`.
- Source: D-BIND-18, D-BIND-19 rule 5; ruling 2026-11-12, "Shim only" (2) (ledger :246): BC-7 is
  not a prerequisite because "a managed handler's exception is caught, and counted, in C#'s own
  trampoline"; ruling 2026-11-13, "Lost handoff: Report it as lost" (ledger :383; d9 L18); ruling
  2026-11-14, "(b) Keep the promise" (ledger :512; d9 D32): "an operator can tell a shim failure
  from a batcher failure". **Settled 2026-11-15 (ledger :551 (3), D26):** the handler gets a null
  batch, the window's rows in `RowsDropped`, its reason, and the failure in `BatchStatus`: status,
  origin `Handover` (4) and a copy of the message.
- **Per-row Arrow (ledger :575 (3), D30):** a row `SubscriberArrow.Subscribe` cannot decode (no schema
  yet, a schema that cannot be opened, malformed bytes) reaches the handler as a null batch with
  `RowsDropped = 1`, from one classify-and-count site (register BC-16); never skipped silently, as
  C++'s per-row `Subscribe` does (BC-2's registered gap). `[built on the edge shim]` on the native
  per-row path (D10C-3, `RowDelivery.OnRow`); the server leg is rung 4's.
- Tests: `SubscriberArrowTests.PerRowDeliversEachRowAloneOrCountsItAsOneDrop`,
  `SubscriberArrowTests.AHandlerThatThrowsIsAbsorbedCountedAndRaised`, its batched twin
  `.ABatchedHandlerThatThrowsIsAbsorbedCountedAndRaised` (D10C-7),
  `ProcessWideTests.AbsorbedTotalMovesByExactlyWhatTheSubscribersAbsorbed`,
  `ThunkDisciplineTests.EveryUnmanagedCallersOnlyMethodIsWrapped`.

**L10. No managed exception crosses into native; one containment site per direction.** Every
thunk catches everything, including failures of its own bookkeeping. A managed exception that
caused a native failure is rethrown as itself.
`[built]`
- Holds at: `Subscriber.cs:824-877`, `Errors.cs:52-71` (rule 3 first: `captured?.Throw()`).
- Source: D-BIND-19 rules 2 and 3; d9 L1 ("a caller's callback must not throw into the shim";
  `fl_batch_fn` "must not throw", `h:1058-1060`). The batch thunk (`SubscriberArrow.BatchThunk`, D10C-7)
  is in the scope of the reflection test, which passes on it.
- Tests: `ThunkDisciplineTests.EveryUnmanagedCallersOnlyMethodIsWrapped`,
  `.EveryThunkIsStaticAndBlittable`, `ErrorTests.ACapturedManagedExceptionIsRethrownAsItself`.

**L11. What native would answer by terminating, the binding refuses first, in managed code; what
native serves, the binding does not refuse.** Disposing a `Subscriber` or `SubscriberArrow` from a
delivery on the same provider is refused with a managed exception; closing or disposing a batched
subscription from inside its own batch callback is served.
`[built for row deliveries; built for batch callbacks (D10C-9: no new code, the guard is the test; Windows, by hand; CI and Linux unrun)]`
- Holds at: `Subscriber.cs:704-712` (provider-keyed frame stack, D-BIND-51), `SA:376-393`
  (`SubscriberArrow.Dispose`), `Subscriber.cs:395-411` (re-entrant `Subscribe`, naming
  `DispatchAfterDelivery`).
- Source: D-BIND-18, -50, -51; d9 L1 (the row subscriber's termination is the only one); d7 L9,
  D12, D13 (ledger :174: destruction inside the batch callback is lawful; an `Unsubscribe` from a
  row-delivery thread returns at once); destroying a `SubscriberArrow` from a row-delivery thread
  stays forbidden (2026-09-05).
- After the switch a batch callback runs on domain 7's batch thread, where no delivery frame is
  pushed (BC-6), so the existing refusal does not fire there: the served case needs no new code,
  only a test (V14).
- Tests: `ReentrancyTests.*`, `CallerTierArmTests.*`, for a `Subscriber`;
  `SubscriberArrowTests.DisposingInsideAPerRowDeliveryIsRefusedAndNothingIsTornDown` for
  `SubscriberArrow.Dispose`'s refusal (D10C-3); the served batch-callback case (D10C-9, the row-limit
  and timeout windows): `SubscriberArrowTests.DisposingTheSubscriberInsideABatchHandlerIsServed`, whose
  control is `TheDisposeReturnedFlagCanFail`.

**L12. A C# idiom is translated at one site, before the call.** `Timeout.InfiniteTimeSpan` (−1)
becomes `INT64_MAX`; any other negative is refused; bounds are UTF-8 bytes, never `string.Length`.
For a batched subscription `Timeout.InfiniteTimeSpan` means no timeout and crosses as
`kNoTimeout`. **Who refuses a negative:** for batch options, the callee (domain 7), its refusal passed
through (L3, D22, D31); batch options do not reuse the managed refusal. For the arrival, managed code
refuses and native re-checks (D-BIND-20). **A negative value never crosses as zero or as a positive
value:** a sub-millisecond negative is not rounded toward zero. (Corrected 2026-11-15 from the D10C
audit, F1.)
`[built for the arrival; for batch options the one site is built (D10C-7; Windows, by hand; CI and Linux unrun)]`:
`SchemaArrival.ToNativeMilliseconds` (infinite as INT64_MAX; truncated when non-negative, FLOORED when
negative, so −1 tick crosses as −1), reached by the arrival behind its managed refusal of negatives and
by `SubscribeBatched` as given. Tested (D10C-9): the −1-tick arm in the BC-8 grid (−1 tick crosses as −1, the
literal "got -1 ms"; mutating that literal turns the grid red), and BC-14's scan
`BatchOptionsTests.EveryTicksToMillisecondsConversionIsOnePlace` (every ticks-to-milliseconds expression is inside
`ToNativeMilliseconds`, which `SubscribeBatched` calls).
- Holds at: `SchemaArrival.ToNativeMilliseconds` (the one site; `ToMilliseconds` keeps the arrival's managed
  refusal of negatives in front of it), `TopicPath.cs` (byte bounds).
- Source: D-BIND-20; register BC-14 ("each binding translates its own idiom at one site (C#:
  `SchemaArrival.ToMilliseconds`)"); d7 §4 and §6 (C#'s −1 ms is refused by L15, so the binding
  translates it); ruling 2026-11-12, "Add 'never' to C++" (ledger :226): "C# keeps its current
  behaviour", i.e. `Timeout.InfiniteTimeSpan` stays C#'s spelling of no timeout.
- ~~Today the batch path has its own conversion (`SA:751-762`, `TotalSeconds * Frequency`), bounded
  by the `MaxTimeout` cap.~~ Deleted with the managed batcher (D10C-7). The cap goes (ledger :536 (3),
  D22): a finite timeout crosses as given and domain 7 decides (d7 L5, L15); `MaxTimeout` itself is
  deleted (D10C-9).

**L13. Batches reach a C# handler one at a time, in window order, on domain 7's batch thread.**
The binding adds no thread, queue, reordering or lock held across the handler.
`[built]` (D10C-7; Windows, by hand; CI and Linux unrun): the batch thunk runs on the thread it is called on and holds no lock;
`SubscriberArrowTests.EveryBatchArrivesOnOneThreadInWindowOrder` (d7's
`EveryCallbackRunsOnTheBatchThreadInWindowOrder`, replayed from C#). (Was violated before the switch,
D-BIND-64.)
- Source: d7 L12, D10; d9 L11, L15 (the shim forwards on that thread). Register BC-9: "Until the
  switch, the C# copy violates L9 and L12; accepted only because C# does not ship to `main`
  first". D-BIND-64 is false of C++ since D7F; it is revised by :203 (ledger :551 (4), D27) and
  annotated.

**L14. A C# batched subscription has domain 7's semantics, unaltered.** The reasons, the loss
count, what a null batch means, when windows close, closing (exactly one `Closing` for every
accepted subscription, empty when idle; nothing after it), a refused subscribe (nothing ever
delivered), a late schema (losses reported, assembly once it arrives) and out-of-range options
(refused) are domain 7's, mapped one to one.
`[built for the reasons, loss counts, null batches, closing, late schemas, dictionaries and order (D10C-7; Windows, by hand; CI and Linux unrun);
built and tested for out-of-range options (V5-V8: D10C-9's grid); built for the hand-over's
failure in BatchStatus (D10C-10, L9's tests; Windows and the local Linux devcontainer, by hand; CI unrun)]`. C# forwards domain 7's batches, reasons and counts as given: the closing
(`AnIdleCloseDeliversOneEmptyClosing`, T25, `ClosingASiblingFromABatchHandlerWaitsForItsClosing`), late schemas
(T12, T13), null batches (T24) and dictionaries (T21, T36) are tested on the server leg.
- Source: d7 L1-L9, L12, L13, L15; ruling 2026-11-12 "Late schema in C#: Follow domain 7" (ledger
  :258 (3)); "Refuse both" (ledger :235); 2026-11-14 "every close ends with one closing delivery"
  (ledger :484; d7 D18); 2026-11-14 "a refused batched subscribe never delivers" (ledger :500; d7
  D19); 2026-11-11 Decisions 1 and 2 (ledger :174; d7 D12, D13); 2026-11-15 (ledger :551 (2),
  (4); D24, D27): dictionaries re-folded as domain 7 delivers them, and D-BIND-64, D-BIND-59 (3) and
  the schema-less latch revised by :203.
- **Divergences of the managed batcher (survey, `fb0f5bf`), each a visible change at the switch, which
  landed in D10C-7 (the C# column is the pre-switch behaviour; `SA:` lines cite that tree):**

| # | Question | C# today | Domain 7 | Authority |
|---|---|---|---|---|
| V1 | Threads and order | three threads, concurrent, out of order (`SA:60-78`) | one batch thread, in order | d7 L12, D10; ledger :551 (4) |
| V2 | `Unsubscribe` inside the handler | closing flush runs nested, inside the handler (`SA:362-364`) | after the handler returns, queued windows first | d7 L9, D8 |
| V3 | Idle close | nothing delivered (`SA:874-877`) | one empty `Closing` | d7 L6, L9, D18 |
| V4 | First row with no schema | latches "undecodable for life" (`SA:730-746`) | an absent schema decides nothing; assembly starts at the first decodable row | d7 L8, L13; ledger :258 (3), :551 (4) |
| V5 | `MaxRows < 1` | clamped to 1 (`SA:267`) | refused | d7 L15; ledger :235 |
| V6 | `MaxRows > int.MaxValue` | clamped (`SA:267`) | used as given | d7 L15, D7O-3 |
| V7 | Finite timeout > ~49.7 days | refused (`SA:258-265`) | accepted; unrepresentable means no deadline | d7 L5, L15, D17; BC-8; the cap goes, ledger :536 (3) |
| V8 | Negative timeout | `ArgumentOutOfRangeException`, managed | domain 7's invalid-argument refusal, passed through as `FletcherException(InvalidArgument)` | d7 L15; ledger :536 (3), D22 |
| V9 | Window bound | raw-byte ceiling, `Array.MaxLength` (D-BIND-63) | per-column 32-bit offset budget | d7 L4 |
| V10 | Whole-window decode failure | zero-row batch, rows counted (`SA:956-965`, `1055-1065`) | null batch ("finishing the batch failed") | d7 L7, D9 |
| V11 | Null batch means | the schema could not be opened (`SA:105-111`) | no batch could be built: no schema yet, rejected, finish failed; plus a failed hand-over | d7 L7; d9 L18; ledger :551 (3), (4) |
| V12 | Dictionary column | value type (D-BIND-8, -59) | re-folded by `BatchDecoder` (domain 2); per-row stays the value type (D32) | d9 Q11; ledger :551 (2), :575 (5); D24, D32 |
| V13 | Handler exception | counted and raised | swallowed uncounted by d7 | C# keeps its count (L9; ledger :246 (2)); BC-7 |
| V14 | `Dispose` inside a batch handler | refused when the flush runs in a row delivery (RowLimit), served on the timer thread | lawful from the batch callback | d7 L9, D12; L11 |

- The deadline rule is the same on both sides (anchored to the previous flush, 1 ms floor;
  `SA:748-763`, d7 L5); so is "only good rows count toward `max_rows`" (D-BIND-68 B3,
  `SA:946-953`).

**L15. Native code ships only in shim packages, one shim per package, and each package's limits
are checked when it is packed.** Every other managed package is asset-free.
`[built for both shim packages (D10C-5; Linux devcontainer and Windows by hand, CI unrun)]`
- Holds at: `Fletcher.Interop.csproj:52-63` (both RIDs or none), `Fletcher.Server.csproj:30-56` (both
  server shims or no pack; no assembly), `scripts/check_dotnet_packages.sh:118-203` (one rule list,
  BC-17: the exact `runtimes/` entries and Fletcher dependencies per target-framework group of each
  package, the server package's entry allow-list, the notices' `arrow`; the ceilings `:49`, `:56`:
  12 MB on Interop, D-BIND-157, and 18 MB on the server package, Q7), and its fourteen controls in
  `scripts/test_check_dotnet_packages.py` (the server notice's one-for-both-platforms rule, ruling
  2026-11-16, is `scripts/check_notice_covers.sh`),
  `scripts/check_shim_dependencies.sh` (the exact DLL allow-list, D-BIND-148; the linux-x64 floors,
  D-BIND-149), `NL:226-233` (the floors stated where a load failure is explained, D-BIND-172).
- Source: D-BIND-13 as revised by ruling 2026-11-13 "Packaging: Separate server package" (ledger
  :337 (1); d9 D22): "two packages, each a full shim"; D-BIND-157 stays on the base package; the
  package's id is ruled (ledger :605 (1)).
- D-BIND-148/-149 apply to the server shim as they stand (Q7's default, measured in D10C-5: the same
  DLL list and floors); its ceiling is the first measured pack plus 4.3 MB, rounded up (Q7).

## 4. The interface

**Public surface (survey).** `Eiva.Fletcher`: `ProviderRegistry.Create`, `PubSubProviderHandle`,
`Publisher` (declare, publish `BoundRows`, `PublishRaw`, `RowWriter`), `Subscriber` (`Subscribe`
with `RowHandler`, `SubscribeSchema`, `Unsubscribe`, `DispatchAfterDelivery`,
`AbsorbedCallbackFailures`, `HandlerFaulted`), `SubscriberArrow` (`Subscribe`, `SubscribeBatched`,
`Unsubscribe`, `SubscribeSchema`, `Dispose`), `BatchOptions {MaxRows = 8000, Timeout = 1 min}`, `BatchStatus {Reason, RowsDropped}`, `BatchReason {RowLimit = 1, Timeout = 2, Closing = 3}`
(the C values, `h:1013-1015`; D10C-2), `RecordBatchHandler`, `FletcherCodec`,
`BoundRows`, `SchemaArrival`, `TopicPath`, `TopicOptions`, `AttachmentsBuilder`/`AttachmentsView`,
`FletcherException`/`FletcherFormatException`, `FletcherStatus`, `FletcherOrigin`, `Diagnostics`.
`Eiva.Fletcher.Interop` is internal to `Eiva.Fletcher` (`Fletcher.Interop.csproj:41-47`).

**C# entry points into the shim (survey).** Every import resolves the one name
`NativeMethods.LibraryName = "fletcher-c-abi"` (`NM:90`), so every one loads `fletcher-c-abi.dll`
(`win-x64`) or `libfletcher-c-abi.so` (`linux-x64`): in a package the base package's edge shim; in
a source build whatever is staged beside the tests (CI's dotnet lane stages the edge build,
`.github/workflows/ci.dotnet.yml`, `locate_shim.sh fletcher-c-abi variant=edge`). 47 entry points:
- Version and marker: `fl_binding_abi_version` (the handshake), `fl_single_copy_marker`.
- Errors and lists: `fl_error_dispose`, `fl_string_list_size/at/dispose`.
- Codec: `fl_codec_open/close`, `fl_rows_bind` (in `BoundRowsHandle`), `fl_rows_unbind`,
  `fl_encode_row`, `fl_decode_rows`. (`fl_decode_rows_framed`, the managed batcher's, is no longer
  imported: D10C-7.)
- Blobs and attachments: `fl_blob_create/retain/release`, `fl_attachments_builder_*`,
  `fl_attachments_size/key_at/value_at/find/dispose` (the borrowed read side imported a second time
  under `_raw` names).
- Provider, publisher: `fl_provider_create/destroy`, `fl_publisher_create` (in `PublisherHandle`),
  `_destroy`, `_create_topic(_with_options)`, `_list_topics`, `_publish_raw/row/rows`.
- Schema and arrival: `fl_schema_retain/release/copy`, `fl_schema_arrival_wait/dispose`.
- Subscriber: `fl_subscriber_create` (in `SubscriberHandle`), `_destroy`, `_subscribe(_with_options)`,
  `_subscribe_schema`, `_unsubscribe_schema`, `_unsubscribe`.
- Batches (D10C-7): `fl_subscriber_subscribe_batches`, `fl_subscriber_unsubscribe_batches`, and
  `fl_binding_variant` (imported since D10C-2, called since D10C-7).

**What the switch adds** `[built]` (D10C-7; Windows, by hand; CI and Linux unrun) except where marked:
- Imports of the three server entry points and of `fl_batch`/`fl_batch_fn`, against the server shim
  (and answered `FL_NOT_SUPPORTED` by the edge shim, L6).
- ~~`FletcherOrigin.Handover = 4` (L2)~~ `[built]` (D10C-2); and the failure in `BatchStatus` (L9, D26) `[built]` (D10C-10) (Windows and the local Linux devcontainer, by hand; CI unrun).
- `SubscriberArrow.SubscribeBatched` over `fl_subscriber_subscribe_batches`: `MaxRows` as `int64`
  unchanged, `Timeout` through the one translation site (L12), refusals passed through as
  `FletcherException(InvalidArgument)` (L3, D22), `BatchOptions.MaxTimeout` removed (D10C-9); a batch
  thunk that moves the batch (L8) and calls the handler on domain 7's batch thread (L13). Closing by
  `fl_subscriber_unsubscribe_batches` (bit-63 ids, `h:1121-1127`). On the edge shim it throws a
  named NotSupported (L6, D25). Dictionary columns arrive re-folded (D24).
- `SubscriberArrow.Subscribe` on the native per-row path, on both packages (L7, D20): ~~the edge
  package~~ `[built]` (D10C-3); the server package, rung 4.
- ~~One `fl_subscriber` holds both a `SubscriberArrow`'s row and batched subscriptions; a same-topic
  mix through one provider collides (d9 D33, ledger :448 (1)), so `SubscriberArrow` refuses it (L3,
  D33, ledger :575 (6))~~ `[built]` (D10C-6).
- ~~`BatchReason` renumbered to 1-3 (L2, D34)~~ `[built]` (D10C-2).
- ~~The loader's preference for the server shim, failing loudly with no fallback (L5, D21, D28)~~
  `[built]` (D10C-4; Windows, by hand; CI and Linux unrun); the server package `[built]` (D10C-5, L15).

**Who calls, and on which thread (after the switch):** a row delivery on the transport's thread
(unchanged); a batch on domain 7's batch thread, one at a time, in window order (L13);
`SubscriberArrow.Unsubscribe` from outside a batch handler blocks until the closing batch has been
delivered; from inside its own batch handler, or from a row delivery on the same provider, it does
not wait (d7 L9, d9 §4).

## 5. Decisions of record

| # | Decision | Source |
|---|---|---|
| D1 | **Domain 10 is the C# binding**: `Eiva.Fletcher`, `.Interop`, `.Model`; the gateway client is domain 5's, generated C# domain 1's; each language binding is its own domain. | Owner rulings 2026-11-12, "New bindings domain" and "Shim only" (ledger :258 (1), :246 (1)); domain-map row 10 |
| D2 | **One batch assembler, domain 7's; C#'s managed batcher is deleted once C# runs on it,** after its tests are checked for cases domain 7 lacks. Supersedes D-BIND-25's batcher half (annotation owed by this round). | Ledger :203 (1); d7 D16; domain-map planned order 6 |
| D3 | **No interim fixes to the C# batcher.** It stays as it is, non-compliant (L4, L13, L14), until deleted. | Ledger :203 (3); :278 revised :258 (2) ("keeping it compliant") in its favour |
| D4 | **C#'s batched subscribe does not ship to `main` (PR #129) before the switch lands.** If shipping becomes urgent the question is re-opened, not assumed. | Ledger :278, "No fixes; don't ship it first" |
| D5 | **C# keeps its no-timeout spelling**, `Timeout.InfiniteTimeSpan`; after the switch it crosses as `kNoTimeout` (`INT64_MAX` ms). | Ledger :226, "C# keeps its current behaviour"; d7 D14, §6; BC-14 |
| D6 | **A C# `MaxRows < 1` throws after the switch** ("an exception instead of 1"); the refusal is domain 7's, passed through. | Ledger :235, "Refuse both"; d7 L15, D15; BC-8 (exception type: D22) |
| D7 | **A C# subscription whose first row arrives before its schema follows domain 7:** losses reported, assembly once the schema arrives; not undecodable for life. | Ledger :258 (3), "Follow domain 7" |
| D8 | **BC-2's per-row gap and BC-7 are not prerequisites of the switch;** a managed handler's exception stays caught and counted in C#'s trampoline. Its premise ("C#'s per-row `Subscribe` is a batched one") was amended by :536 (1); C#'s per-row path does not inherit BC-2's gap: it counts its own drops (D30, BC-16). | Ledger :246 (2); :536 (1); :575 (3) |
| D9 | **A C# app loads the server shim only if it references the server package;** otherwise the edge shim ("references" revised to "present" by D28). C#'s per-row subscribe must work on the edge shim without a batcher (which subscribe: D20). | Ledger :363 (4); d9 D29 (revising D19) |
| D10 | **The server shim ships in its own .NET package,** a full shim; D-BIND-157's 12 MB ceiling stays on the base package; D-BIND-13 and -17 revised (two packages, one shim per process). | Ledger :337 (1); d9 D22 |
| D11 | **The server shim's file is `fletcher-c-abi-server`;** this round teaches C# that name or renames it when packing. | Ledger :395; d9 Q5a (shim half) |
| D12 | **A batched call on the edge shim is a named not-supported refusal,** never a missing symbol; both builds answer the variant query. | Ledger :337 (2); d9 D23, D28 |
| D13 | **A batch is borrowed for its callback and kept only by moving it.** | Ledger :363 (1); d9 D26 |
| D14 | **One ABI version for both builds; `HeaderVersionMinor` moved with the bump** (the one domain-10 line D9S edited, now 8). | Ledger :363 (2); d9 D27 |
| D15 | **A failed hand-over carries its status, `FL_ORIGIN_HANDOVER` (4) and message, borrowed;** C# mirrors code 4 and never disposes `failure`. | Ledger :383 (1), :512; d9 D30, D32 |
| D16 | ~~**CI proves C# against the server shim by staging a copy under the edge file name** (test staging only; decides nothing about packing). Retires when D21's loader lands.~~ Retired (D10C-4): the server file is staged under its own name. | Ledger :525; ledger :536 (2) |
| D17 | **Every close of an accepted batched subscription ends with exactly one closing delivery; a refused one delivers nothing,** so a C# caller may free handler state as soon as a refused call throws. | Ledger :484, :500; d7 D18, D19 |
| D18 | Inherited BIND decisions this domain keeps: one codec, native (D-BIND-1); values as C Data (D-BIND-1a); errors with origin, five rules (D-BIND-1c, -19, -54); thunk lifetime bookkeeping and managed refusals (D-BIND-18, -50, -51); timeouts and byte bounds (D-BIND-20); no managed provider (D-BIND-24); RIDs (D-BIND-27); handler owns the batch, one handler shape (D-BIND-59); dynamic MSVC CRT (D-BIND-155); AOT-compatible (D-BIND-81); the platform floors (D-BIND-149, -169, -172). | `plans/BIND-locked-decisions.md` |
| D19 | **Decisions the switch retires:** D-BIND-25's batcher half; D-BIND-63 (the raw-byte ceiling); D-BIND-64 (unserialised handlers, nested closing; revised, D27); D-BIND-66 (the intake's own counting, for the batched path); D-BIND-68's purpose (Q8 for the entry point). D-BIND-64 and -59 are annotated (2026-11-15); the D-BIND-25, -63, -66 and -68 annotations are still owed by the round. | d7 D16; BC-9; d9 Findings (d)6; ledger :551 (4) |
| D20 | **C#'s per-row Arrow subscribe runs on the native per-row path and works on both packages.** Arbitrates :246 (2) (its "batched with `MaxRows = 1`" premise amended) against :363 (4). Resolves former Q1 and d9 R-3 (L7). | Ledger :536 (1) |
| D21 | **Distinct native file names; the server package depends on the base package; `NativeLoader` prefers the server shim when present.** Settles d9 Q5a and BC-10's deferred half; CI's renamed-copy staging (:525, D16) retires when it lands. Resolves former Q2 (L5). | Ledger :536 (2) |
| D22 | **An out-of-range batch option raises domain 7's refusal, passed through as `FletcherException(InvalidArgument)`; the 49.7-day `MaxTimeout` cap goes.** Supersedes D-BIND-19 rule 4 / D-BIND-20's pre-check for batch options only. Resolves former Q4 and d9 R-4; :235's "as it does today" is revised by :575 (4) (D31) (L3, L12, L14 V5-V8). | Ledger :536 (3); :575 (4) |
| D23 | **C# keeps the edge/server split;** D-BIND-1a rider i ("a .NET process is never the microcontroller") is revised by :363 (4). Resolves former Q12 and d9 Findings (d)5. | Ledger :536 (4) |
| D24 | **A batched dictionary column reaches C# re-folded, as domain 7 delivers it;** D-BIND-8 / D-BIND-59's "value type" is revised for batches. Resolves former Q5 and, with domain 2, d9 Q11 (L14 V12). | Ledger :551 (2) |
| D25 | **C#'s batched subscribe exists on every package; on the edge shim it throws a named NotSupported naming the server package.** Resolves former Q3 (L6). | Ledger :551 (1) |
| D26 | **A failed hand-over reaches a C# handler as a null batch, its rows counted, with the failure's status, origin `Handover` (4) and message in `BatchStatus`;** `FletcherOrigin` gains code 4. Resolves former Q6 (L2, L9). Built: `BatchStatus.Failure` (`FletcherException?`; D10C-2, D10C-10) (Windows and the local Linux devcontainer, by hand; CI unrun). | Ledger :551 (3), :669 |
| D27 | **D-BIND-64 (concurrent, out-of-order handlers), D-BIND-59 (3) (null batch = schema not opened only) and C#'s schema-less first-row latch are revised by :203;** C# follows domain 7 (d7 L7, L9, L12) (L13; L14 V1, V2, V4, V11). | Ledger :551 (4) |
| D28 | **A server shim present beside the app is loaded; one present but unloadable stops the load loudly, naming the file; no fallback to the edge shim.** Revises :363 (4)'s "references"; refines :536 (2) (L5). | Ledger :575 (1) |
| D29 | **The edge refusal is the shim's message verbatim plus one binding sentence naming the server package,** appended at the batched wrapper; number and origin unchanged (L3, L6). | Ledger :575 (2) |
| D30 | **An undecodable per-row Arrow row arrives as a null batch with `RowsDropped = 1`,** from one site (L7, L9; BC-16). | Ledger :575 (3) |
| D31 | **A bad batch option raises `FletcherException(InvalidArgument)`** with domain 7's message; :235's "throws as it does today" is revised (L3, D22). | Ledger :575 (4) |
| D32 | **Per-row and batched may deliver different Arrow types for one dictionary column** (the value type per row, a `DictionaryArray` batched); C# converts nothing (L3, L14 V12, D24). | Ledger :575 (5) |
| D33 | **`SubscriberArrow` refuses a row and a batched subscription to the same topic,** a ruled exception under L3; revises :448 (1)'s "documented, not refused" for C#. | Ledger :575 (6) |
| D34 | **`BatchReason` is renumbered to the C values 1-3** (`RowLimit = 1`, `Timeout = 2`, `Closing = 3`); a visible change, accepted because the batched API is not on `main` (D4). | PM decision 2026-11-15, under L2, which already governs it |

## 6. Relationships

- **Domain 9 (the C ABI shim).** The only native code this domain calls. Obligations d9 places here:
  the handshake's managed half (L1), `Timeout.Infinite` mapping (L12), `SafeHandle`s, managed
  refusals (L11), deleting the managed batcher (L4), mirroring origin 4 and never disposing
  `failure` (L2, L8, D32), choosing which shim to load (L5; BC-10's deferred half), and the d9 §6
  domain-10 list (D21, Q7). Collisions d9 registered for this round, both ruled 2026-11-15:
  **R-3** (per-row `SubscriberArrow` on the edge build; D20) and **R-4** (managed pre-checks vs
  pass-through; D22). d9 Q5a is settled (D21) and d9 Q11 answered (D24); d9 Q9
  (`fl_decode_rows_framed` after the switch) carries a default (Q8). A change to `binding.h` or `c-abi/` is domain 9's; this round edits none unless the owner
  grants a named exception (as d9 D27 granted one line the other way).
- **Domain 7 (batch assembly).** Reached only through the shim's server build. Domain 7 owns every
  batching answer (BC-1, -2, -3, -4, -8); this domain maps them (L14) and answers none (L3, L4).
  d7 L15's binding clause and d7 §6's two obligations (translate −1 ms; `MaxTimeout` dropped, D22)
  are this round's. d7's suite is the target of the managed-batcher test check (§8).
- **Domain 2 (row encoding).** Owns the bytes; the shim hosts its codec (d9 D24). Per-row decode
  (`fl_decode_rows`) gives a dictionary's value type; a batch from `BatchDecoder` arrives re-folded,
  and C# delivers each as it comes (V12, D24, D32).
- **Domain 3 (provider interface).** Its rules reach C# through the shim unchanged: topic rules,
  re-entrancy, the unsubscribe barrier (D-BIND-50), schema delivery (D-BIND-146).
- **Domain 1 (generated C#).** Generated classes, accessors and the generated publisher/subscriber
  pair call `Eiva.Fletcher`'s public surface and use `Eiva.Fletcher.Model`'s types. Domain 1 owns
  what is generated; this domain owns the surface it is generated against. D25 keeps
  `SubscriberArrow`'s public shape on every package, so the generated subscriber (D-BIND-79) needs
  no change; any later change to that shape needs domain 1's agreement.
- **Domain 5 (gateway client).** `Eiva.Fletcher.GatewayClient` takes no reference on the native
  packages (D-BIND-120) and batches on its own (`GatewayBatcher`, BC-9, D-BIND-133). Not this
  domain's. Whether its batching must one day reach domain 7 is the owner's question for when
  domain 5 is specified (BC-9).

## 7. Open questions

Former Q1-Q6 and Q12 were answered by the owner on 2026-11-15 (ledger :536, :551) and are now D20-D27;
former Q10 (doc comments lagging D28) is work the round owes (Findings (a)2). Numbers are kept.

| # | Question | Owner |
|---|---|---|
| Q7 | Do D-BIND-148's allow-list and D-BIND-149's floors apply to the server shim as they stand, and what packed-size ceiling does the server package get (D-BIND-157 stays on the base package)? Proposed default: the same lists and floors, failing by name if the server shim needs more; a ceiling of its own set from the first measured pack plus the same margin D-BIND-157 left. Owner only if the measured shim breaks a list or floor. **Default stands unless the owner objects.** **Measured (D10C-5, 2026-11-16):** the server shim passes the same list and floors; the first pack is 12,848,400 bytes, so the ceiling is 18,000,000 (12.85 + 4.3 MB, rounded up). | domain 10 (packaging stage), then owner on a breach |
| Q8 | Does `fl_decode_rows_framed` stay after the switch (d9 Q9)? Its one consumer is deleted. Proposed default: it stays; removing an entry point is domain 9's, at a later d9 stage, recorded as d9 debt. Owner only to remove it in this round. **Default stands unless the owner objects.** | domain 9, after this round |
| Q9 | Which managed-batcher tests carry cases domain 7's suite lacks (§8)? **Answered by the D10C pre-round audit (part C): four cases, listed in §8 rule 6;** the test-authority step re-checks before the deletion stage; each is ported by a domain-7 round or dropped by the owner. | D10C, stage before deletion |
| Q11 | ~~No C# test compares `FletcherStatus`/`FletcherOrigin`/`BatchReason`/the variant with `binding.h`'s numbers (L2; d9 Findings a7). Proposed: the switch adds one.~~ **Closed (D10C-2):** `AbiNumberTableTests.EveryNumberTheShimHandsOutHasTheSameValueInCSharp`. | D10C |

## 8. How to extend this domain

1. **Mirror, then wrap.** A new entry point enters `NativeMethods` exactly as `binding.h` declares
   it, every new number enters its C# enum equal to the C value, and the managed tier gives it a
   lifetime and an exception one layer up (L2).
2. **Forward, do not re-answer.** Check in managed code only what a ruling lets you duplicate; pass
   every other refusal through `Errors.ThrowIfFailed` with its number and message (L3).
3. **One translation site per idiom** (L12); one throw site (L10); every thunk inside the catch the
   reflection test enforces (L10).
4. **Borrowed means borrowed.** Copy or move inside the callback; never keep a pointer; never
   dispose what the shim lends (L8).
5. **Build-dependent features ask the build** (L6), exist on every package and refuse by name on
   the edge shim (D25); everything else works on the edge shim (L7).
6. **Retire a test that asserts what a law forbids,** through the test-authority step, before the
   code that satisfies it is deleted. For the switch (L4), the managed-batcher tests sort into
   (corrected 2026-11-15 from the D10C audit, F8; full table in `plans/D10C-pre-round-audit.md` part C):
   - **Kept, re-anchored on domain 7 and the server shim** (lawful; each has a d7 mirror or holds under
     d7): `FlushesAtTheRowLimit`, `FlushesAtTheTimeout`, `UnsubscribingFlushesThePartialWindow`,
     `AttachmentsAlignWithTheirRows`, `ADroppedRowIsReportedAndItsAttachmentDiscarded`,
     `OnlyDroppedRowsStillDeliverAnEmptyBatch`, `ACorruptRowIsDroppedAndTheBatchStaysAligned`,
     `ACorruptRowDoesNotCountTowardMaxRows`, `AFixedSizeListArrivesNonNull`,
     `ASchemaTheCodecCannotOpenDropsEveryRow`, `UnsubscribingFromInsideATimeoutFlushIsSafe`,
     `UnsubscribeWaitsForATimerHandlerStillRunning`, `AWindowWithAVeryLongTimeoutWaits` (was
     `AWindowAtTheMaximumTimeoutWaits`; 100 years and `TimeSpan.MaxValue`, D10C-9), `OnACarryingTransportASubscriberThatSubscribedFirstStillDecodes`,
     `OnTheDefaultInProcessLoopbackASubscriberThatSubscribedFirstDropsAndCountsEveryRow` (its assertions
     are d7's `SchemaDeclaredAfterSubscribeDropsReportedAtUnsubscribe` outcome),
     `ATimerHandlerThatCancelsDuringADeliveryDoesNotDeadlock` (holds under d7; may be dropped as a
     duplicate of d7's `InCallbackUnsubscribeWhilePublishingDoesNotDeadlock`),
     `RowsSplitAcrossMessagesAreDroppedNotMisaligned`, `WindowsFollowOneAnother`. The per-row tests
     (`Subscribe`, among them `AHandlerThatThrowsIsAbsorbedCountedAndRaised` and
     `ARowWithAnEmptyAttachmentKeyArrives`) re-anchor onto the native per-row path.
   - **Lawful subject, a forbidden assertion to rewrite:** `ADictionaryColumnKeepsItsNulls` and
     `BatchesAreWellFormed` (each reads the dictionary column as `StringArray`; D24),
     `UnsubscribingFromTheHandlerDuringARowLimitFlushStopsDelivery` (expects no closing delivery; D17).
   - **Passes by tolerance; re-anchor:** `TopicOptionsReachTheProviderThroughBothForms` (its batched half
     asserts `NotSupported`, which the edge shim answers for the build whatever the options do).
   - **Subject forbidden; delete** (deleted by D10C-7)**:** `ATimeoutFlushAndARowLimitFlushMayOverlap` (L13, V1),
     `ATimeoutPastTheTimersMaximumIsRefused` (V7, D22, D31), `ADictionaryColumnArrivesAsItsValueType`
     (V12, D24), and the window-ceiling tests `ARowThatWouldPassTheCeilingFlushesTheWindowFirst`,
     `ARowLargerThanTheCeilingIsDroppedAndCounted`, `TheDefaultCeilingIsTheMostOneManagedBufferHolds` (L4).
   - **Cases domain 7's suite lacks (Q9), to port:** `RowsSplitAcrossMessagesAreDroppedNotMisaligned`
     (d7 has the "buffer not fully consumed" check but no batch-level test),
     `ARowWithAnEmptyAttachmentKeyArrives` (the batched path), a callback that throws (d7's half: the
     next batch still arrives; d7 L10 has no test), and a small-size offset-budget split (d7's is
     `heavy` only, with no test-only budget). `AFixedSizeListArrivesNonNull` is not one: d7 has
     `FixedSizeListWithNamedItemArrivesNonNull`.
   A case domain 7 lacks is ported to domain 7 by a domain-7 round, or the owner rules it dropped;
   it is never kept alive in C# (ledger :203).
7. **Record the change.** Write any change to a law here before the code, with its build state.

## Findings (reported, not fixed)

**(a) Documents that disagree with the code or with a later ruling**
1. ~~**Owed by the round (tier 3: code and checks behind a ruling).** `Fletcher.Interop.csproj:6-10`
   states, and `scripts/check_dotnet_packages.sh:12-13` and `:108` enforce, "Interop is the only
   package that carries native assets"; ruling 2026-11-13 (ledger :337 (1), d9 D22) made it false,
   and D21 adds the server package beside it.~~ **Done (D10C-5):** the comment says L15, and the check
   is one rule list over five packages.
2. ~~**Owed by the round (tier 3).** `FE:38` (`FletcherStatus.NotSupported`) and `FE:67`
   (`FletcherOrigin.Seam`) lag d9 D28 and `h:265`.~~ **Done (D10C-2):** `FE:38` now reads "This provider or
   this build cannot do this" and `FE:67` includes the library's own refusal.
3. `SA:4-93` and `SA:105-111` describe D-BIND-64's concurrency and "NULL only when the schema could
   not be opened"; both become false at the switch (V1, V11).
4. D-BIND-25, -63, -66 and -68 carry no annotation yet (D19); D-BIND-1a rider i, -8, -19, -20,
   -59 and -64 were annotated 2026-11-15.

**(b) Rules answered in two places**
1. Batch options: `SA:258-267` and d7's constructor (BC-8). Removed by the switch (L3).
2. Turning rows into batches: `SA:592-1066` and domain 7 (BC-9). Removed by the switch (L4).
3. Batched decode: `fl_decode_rows_framed` (nanoarrow, at flush) and `BatchDecoder` (Arrow C++, per
   row), with different dictionary answers (V12) and different whole-window failure answers (V10).

**(c) Behaviour with no test**
- ~~`FletcherStatus`/`FletcherOrigin`/`BatchReason` against `binding.h` (L2).~~ Tested (D10C-2).
- ~~`SubscriberArrow.Dispose`'s refusal inside a row delivery (L11).~~ Tested (D10C-3,
  `SubscriberArrowTests.DisposingInsideAPerRowDeliveryIsRefusedAndNothingIsTornDown`).
- ~~Origin 4 reaching C# (L2), unreachable today.~~ Origin 4 maps to `Handover` (D10C-2, `ErrorTests.OriginFourIsHandover`); reaching it from a real batch is tested (D10C-10, `HandoverTests`) (Windows and the local Linux devcontainer, by hand; CI unrun).
- A C# handler disposing its `SubscriberArrow` from a batch callback (L11), until the switch.
