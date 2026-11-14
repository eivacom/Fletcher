# D9S pre-round audit — consistency (Gate 4) and bounded-context census (Gate 4b)

Written by `round-designer` on 2026-11-13 for the shim round **D9S** (domain 9, the C ABI shim, gains
a server-tier variant that exposes domain 7's batched subscribe), after the owner rulings of
2026-11-12 and 2026-11-13 were recorded ([RIX-rulings.md](RIX-rulings.md)) and domain 9 was
specified. Tree: `0fab68e` (branch `feature/shim-server-variant`), no edits.

**Round purpose (owner's terms):** C# and any other binding get columnar batches straight from
Fletcher's one batcher through the shim, while the build that runs on embedded devices stays exactly
as small and Arrow-free as today ("we should only have one and it should be the C++ one"; "Arrow C++
should never be in the native library, because it needs to be able to run on embedded devices";
"Let's build one shim in two variants").

**Rulings ledger:** `plans/RIX-rulings.md`, **357 lines**; last heading (line 334) **"## 2026-11-13 —
Domain 9 (C ABI shim): packaging, wrong build, codec owner, laws confirmed"**. The 2026-11-12 and
2026-11-13 entries govern. Read in full.

**Corpus audited:** `docs/domains/d9-c-abi-shim.md` (§1, §2, §3 L1–L17, §4–§8); domain 7's obligations
on the shim (`docs/domains/d7-batch-assembly.md` §4, §6, L9, L10, L12, L15; register BC-8, BC-9);
domain 2's L2, L8 as they cover the hosted codec; the shim-governing parts of
`plans/BIND-csharp-bindings.md` (Decision table rows 4 and 6, Part 2 packages, value transfer) and
`plans/BIND-locked-decisions.md` (D-BIND-1a and riders, -2a, -3, -13, -17, -19, -20, -31, -59, -68,
-148, -149, -157); `c-abi/include/fletcher/abi/binding.h` doc comments and `c-abi/README.md` (tier 2,
survey); the test corpus and CI checks that pin shim behaviour (part C).

**Totals:** **115 rows** (A: 92, B: 23): **66 OK, 46 FINDING, 3 BLOCKER**. The three BLOCKER rows
are one blocker, **B1** (L11 and D-BIND-2a against L14), seen from the law, the ruling and the
law-vs-law pair. Distinct findings F1–F27. Two owner confirmations (LC-1, LC-2). Six implicit
collisions for the owner (R-2, R-3, R-4, R-9, R-10, plus census C5). Test corpus (part C): 19 rows,
**0 DELETE, 4 RE-ANCHOR**, the rest keep or extend. Census (part D): 14 contexts, 6 new register rows
proposed (BC-10 to BC-15).

Verified while auditing: 47 entry points (`grep -c FL_ABI_EXPORT binding.h` is 51, four of them the
macro's own definitions); `subscriber_arrow.cpp:39-46` throws `PubSubError(kInvalidArgument)`, which
`Capture` keeps as `FL_INVALID_ARGUMENT` (`containment.cpp:62-67`); d9's `cpp:N` citations spot-checked
at 28, 96, 168, 182, 789, 871, 941, all current.

## A. Clauses against the laws

Laws are d9 L1–L17 unless prefixed (`d7`, `d2`). "Same" means the row repeats a finding already
numbered.

### d9 §1–§2

| # | Clause (source) | Governing law | Verdict |
|---|---|---|---|
| A1 | §1 Owns: "two variants built from one source … which variant a build is, how a caller asks, and what each variant may link" | L12, L16, L8 | OK |
| A2 | §1 Hosts `NanoarrowCodec` for domain 2; "this domain owns only the entry points, their handles and their containment" | d2 L2, d2 L8 (D24) | OK |
| A3 | §1 Edge variant: "as today, plus only the variant query and named not-supported answers" | L12, L6 | **FINDING F1**: L6 makes every change bump the version, so the edge build's `fl_binding_abi_version` changes too. "As today" omits that. See LL-2 and LC-1. |
| A4 | §1 Server variant: "C# on desktop loads it" (also §5 D19) | L17, D22 | **FINDING F2**: since D22, a C# desktop app that publishes and subscribes per row references only the base package and loads the **edge** variant. Implicit ruling collision R-2, for the owner. |
| A5 | §1 Refuses "Answering the seam's questions again … One exception is recorded: the bound-rows publish check (L4)" | L4, L12 | **FINDING F3**: L12's edge refusal is a second answer that the shim gives itself, with no seam call behind it. Suggest that L4 and §1 record it as a second exception ("the edge build's refusal of a batched entry point, L12"). This is a tier-2 text change to a law, so it needs the owner's approval. |
| A6 | §1 Refuses Batching (BC-9): "no window, no count, no timer, no thread" | L15, d7 L12 | OK |
| A7 | §1 Refuses Validating batch options (BC-8) | L15, d7 L15 | OK |
| A8 | §1 Refuses encoding rules; the hosted codec decodes a dictionary as its value type | d2 L7, d2 L8, D24 | OK. The server variant also delivers `BatchDecoder`'s re-folded dictionaries (Q11). That is domain 2's difference, and the shim converts neither form (census C13). |
| A9 | §1 Refuses Arrow C++ in the edge variant, ever | L8 | OK |
| A10 | §1 Refuses provider registration; a path selector answers `FL_NOT_SUPPORTED` | L4, D6 | OK |
| A11 | §1 Refuses a second native library | L5, L17 | OK |
| A12 | §1 Refuses language idioms (`Timeout.Infinite` → `INT64_MAX` is each binding's) | L4, L15 | OK |
| A13 | §2 Shim: "statically links pub/sub, the two DDS providers and nanoarrow" | L8, L12 | OK. This surveys today's shim. At close, the server variant's entry should add `pubsub-arrow` and Arrow C++. |
| A14 | §2 Entry point: "47 today" | L6 | OK (verified) |
| A15 | §2 Variant, Edge, Server, Variant query, Base/server package | L12, L16, D22 | OK |
| A16 | §2 Naming: "Window" will mean `fl_write_window` and domain 7's window in one header | — | OK. Design note: the C batch surface should not use "window" for anything that is not domain 7's window. |

### d9 §3 (the laws' own clauses)

| # | Clause (source) | Governing law | Verdict |
|---|---|---|---|
| A17 | L1 "The designed exception: `fl_subscriber_destroy` from inside a delivery … terminates the process" | L1, d7 L9 (D12) | **FINDING F4**: this exception belongs to the row subscriber. Domain 7 makes destroying a batched subscriber from inside its own batch callback lawful: queued batches, then `kClosing`, are delivered (D12). The batch thread pushes no `DeliveryScope` (BC-6). So the server variant's batched teardown must neither terminate nor be refused. State this in L1 or L15 when the stage builds it. |
| A18 | L2 "The shim invents no status number; the only field it adds is `origin`" | L2 | **FINDING F5**: `fl_origin` (`h:236-253`) has no arm for a refusal the shim originates (the edge refusal) or for a failure on domain 7's batch thread. `FL_ORIGIN_SEAM` is "a PubSubError … from the seam". The design picks an arm. A new `fl_origin` enumerator belongs to the shim (append it and bump the version, L6). It is not a seam stop-and-ask. |
| A19 | L3 every argument borrowed; OUT on success only | L3, L14 | OK. L3 governs arguments; the batch's ownership is L14's (the collision is with L11, A28). |
| A20 | L4 "`[built]`, with **one answer of its own**" | L4, L12 | Same as F3 |
| A21 | L5 "Void entry points and `fl_binding_abi_version` still answer in a poisoned shim" | L5, L16 | OK. L16's "as `fl_binding_abi_version` is" carries this over. Design note: the variant query is not a `Contain` call. |
| A22 | L5 "Two packages, one process" | L17, D22 | OK |
| A23 | L6 Holds at: "D-BIND-42, -43, -46, -57, -68 each bumped the minor (now 0.7)" | L6 | **FINDING F6**: D-BIND-52 is missing (0.4 → 0.5, the schema-watch pair; `BIND-locked-decisions.md:1583-1585`). The list has five bumps; there are six. |
| A24 | L6 "never stubbed to answer `FL_NOT_SUPPORTED`" vs the edge build's refusals | L6, L12 | OK. The refusal is implemented behaviour ("this build cannot"), not "not written yet". |
| A25 | L7 "the export set is asserted by a CI step" (`ci.c-abi.yml:231-247`) | L7, L12 | **FINDING F7**: the step only checks that every exported name matches `^fl_`. It does not compare the export set with `binding.h`. L12 ("the edge build exports every entry point the server build does") has no structural check today. Suggest an exact-set comparison: the `FL_ABI_EXPORT` declarations equal the export set of each shipped artefact, and the edge set equals the server set. Census C1. |
| A26 | L8 "How it is enforced today: a size ceiling in CI"; "the edge build needs a check of its own (Q4)" | L8, L12 | **FINDING F8**: both CI legs choose the shim with `find … -print -quit` by file name (`ci.c-abi.yml:89`, `ci.dotnet.yml:137, 285`). Once a second artefact from the same recipe sits in the Conan cache, which file gets measured is arbitrary. A server shim would trip a ceiling whose message says "Arrow C++ was linked", or an edge regression would be hidden. Census C2; part C T3, T5. |
| A27 | L9 a row is written once | L9 | OK |
| A28 | L11 "Everything a callback is handed is borrowed for that call" | L11 vs L14 | **BLOCKER B1**: L14 says the batch crosses as C Data "the receiver owns", and L15 and §4 say it is handed to a **batch callback**. Both laws are confirmed (D25). One callback cannot be handed something borrowed for the call and also own it. See LL-1. |
| A29 | L12 "Nothing else in the edge build changes: no other entry point, no link (L8), no size check, no behaviour" | L12 vs L6 | Same as F1 (LL-2) |
| A30 | L12 bullet: "That refusal is not a stub in D-BIND-31's sense" | — | **FINDING F9**: wrong citation. D-BIND-31 (`BIND-locked-decisions.md:696-720`) is the BIND-2c/BIND-4 scope ruling and says nothing about stubs. The no-stub rule is the header's own (`h:69-83`), confirmed as "the header's own rule" by D-BIND-52 (`:1573-1575`). The same miscitation appears in §5 D14 (A46). |
| A31 | L12 bullet: "`FL_NOT_SUPPORTED` is the status `h:75-77` reserves for 'this build cannot do the thing'" | L2, L12 | **FINDING F10**: the seam's published meaning is narrower. `core/README.md:37` and `h:213-215` both say "This provider does not implement the requested behaviour". Only `h:75-77` says "this build". The existing path-selector refusal (`h:747-749`) is the seam registry's own answer, so it is no precedent for a refusal the shim originates. Owner confirmation LC-2 (Q3's residue). |
| A32 | L13 "The only entry points whose answers differ between the builds are the variant query and the batched ones" | L13, L5 | OK. Design note: if `fl_single_copy_marker`'s text names the variant, it becomes a third entry point whose answer differs. Keep the text identical or list it in L13. (Its text is already stale, "ABI 0.1": d9 Findings (a)1.) |
| A33 | L14 batch and schema are owned by the receiver; "no batch could be built" is expressible | L14, d7 L7 | OK as a statement (but see B1) |
| A34 | L14 "How attachments (L3 of domain 7, row-aligned) cross is open (Q6)" | d7 L3 | **FINDING F11**: d7 L3 must survive the crossing. If the C surface leaves attachments out, a server-variant caller loses side data without being told. A precedent exists: D-BIND-59 (2), "a row's attachments arrive as owned `AttachmentsBuilder` copies". Q6 must be answered at the design of the stage that first crosses a batch. It goes to the owner only if it widens the seam's vocabulary. |
| A35 | L15 forwarding: `max_rows` and timeout as given, `kNoTimeout` as `INT64_MAX`, the refusal as `FL_INVALID_ARGUMENT` with domain 7's message; depends on domain 7 keeping `PubSubError` | L15, d7 L15 | OK. Design note: an `int64_t` row count and an `int64_t` ms timeout carry every value domain 7 can refuse, so the shim needs no "narrower field" mapping. |
| A36 | L15 "the batch callback runs on domain 7's batch thread" vs L11 "the shim adds no thread" | L11 | OK: the thread is domain 7's |
| A37 | L15 with d7 L10: to domain 7, the shim's batch thunk is "the callback". If it throws (an export or allocation failure), domain 7 swallows the throw and does not count it (`subscriber_arrow.cpp` loop, BC-7) | d7 L1, d7 L2, L2 | **FINDING F12**: domain 7 counted those rows as delivered, but the C caller never sees them. That is a silent loss, which d7 L1 forbids. Whether a hand-over failure is reported, and where, is census C5 and an owner question. |
| A38 | L16 variant query | L16 | OK |
| A39 | L17 one shim per process across packages | L17 | OK; Q5a open (census C10) |

### d9 §4–§8

| # | Clause (source) | Governing law | Verdict |
|---|---|---|---|
| A40 | §4 surface "ABI 0.7, 47 entry points, every one implemented" | L6 | OK |
| A41 | §4 Server variant: "Its C shape is the shim round's design, within L12-L17" | L12–L17 | OK |
| A42 | §4 Who calls: "Server variant: the batch callback on domain 7's batch thread" | d7 D10, L15 | OK |
| A43 | §4 Builds: "its build switch and native file name are the shim round's design (Q5a)" | L8 | **FINDING F13**: whichever switch selects the server variant (a Conan option, say) must stay in the package ID, as `with_probe_shim` does (`conanfile.py:30-34`), and not be deleted from it as `run_tests` is (`:81-82`). Otherwise a package carrying Arrow C++ can pass for the edge one. Census C2. |
| A44 | §5 D3 "Annotating Decision 6 is owed by the shim round" | L8 | OK: owed by D9S (R-7) |
| A45 | §5 D8, D9 annotations of D-BIND-13, -17 owed | L5, L17 | OK: owed (R-5, R-6) |
| A46 | §5 D14 source "D-BIND-31; `h:69-90`" | L6 | Same as F9: should be `h:69-83`, D-BIND-52 |
| A47 | §5 D18 "the Windows shim's DLL imports are an exact allow-list … the server package's ceiling is not ruled" | L8, L12 | **FINDING F14**: D-BIND-148 (exact DLL allow-list; "a new DLL … needs a ruling") and D-BIND-149 (the linux floors must *equal* `shim_floors.env`) are enforced on whichever shim CI finds. Arrow C++ may add imports on Windows and is likely to raise `GLIBCXX`. No ruling says whether the server shim gets its own lists and floors. R-9, owner. |
| A48 | §5 D19 "C# on desktop loads the server variant" | D22 | Same as F2 |
| A49 | §6 Domain 7 obligations (`kNoTimeout` untranslated; refusal passed through; no batching) | L15 | OK |
| A50 | §7 Q8 "Does the server variant version separately? … owner" | L6 | Same as F1 (LC-1) |
| A51 | §8 rule 5 "its links and size check are re-run unchanged (L8, L13)" | L12 | **FINDING F15**: "no size check changes" is L12's text. L13 is about the server variant only adding. Cite L8 and L12. |
| A52 | §8 rules 1–4, 6 | L1–L6 | OK |

### Domain 7's obligations on the shim (d7 §4, §6, laws)

| # | Clause (source) | Governing law | Verdict |
|---|---|---|---|
| A53 | d7 §4 "Across a C ABI, `kNoTimeout` is INT64_MAX … untranslated (D-D7O-1); C#'s 'infinite' (−1 ms) is refused here, so a binding translates it" | d9 L15, L4 | OK |
| A54 | d7 §6 Domain 9: "a narrower shim field maps its own maximum onto `kNoTimeout`" | d9 L15 | OK (A35: with `int64_t` no narrower field arises) |
| A55 | d7 L15 "a binding passes the refusal through"; register BC-8 | d9 L15 | OK |
| A56 | d7 L9 exceptions (in-callback `Unsubscribe`/destruction, row-thread `Unsubscribe` do not wait) reach C callers | d9 L15 | OK; see F4 |
| A57 | d7 L12 / D10: one at a time, in window order, on the batch thread | d9 L15 | OK |

### BIND plan and locked decisions

| # | Clause (source) | Governing law | Verdict |
|---|---|---|---|
| A58 | D-BIND-1a (values cross as C Data; batch encode/decode) and plan Decision 4 | L14 | OK |
| A59 | D-BIND-1a rider i: "a .NET process is never the microcontroller — so the two-tier split may not need mirroring in C# at all" | D22 | **FINDING F16**: C# now mirrors the split through two packages. The rider argued the other way; no ruling names it. R-10. |
| A60 | D-BIND-1a rider ii ("The native artifact links nanoarrow only, not Arrow C++ … verified by a packed-size check in CI") and plan Decision 6 ("Native artifact links nanoarrow only … (package size)") | L8, D19 | **FINDING F17**: partially superseded for the server variant (D7O audit R-4). Neither text is annotated. D9S owes the annotation (tier 2, owner approval). |
| A61 | D-BIND-2a (d9 D5): "Everything a delivery receives is borrowed" | L14 | **BLOCKER B1** (same): if a batch is a "delivery", this locked decision forbids L14's owned batch |
| A62 | D-BIND-3: built-ins `inprocess`, `fastdds`, `xrce` statically linked; "Dropping a built-in from the shim → STOP-AND-ASK" | L13 | OK: the server variant is a full shim (D22) |
| A63 | D-BIND-13 "Native assets are isolated to ONE package … Adding native assets to a second package → STOP-AND-ASK"; plan Part 2 table and prose ("`Interop` is the *only* package carrying native assets") | D22 | **FINDING F18**: revised by the 2026-11-13 ruling, which names it. Not annotated (R-5). `scripts/check_dotnet_packages.sh:112-113` still enforces the old text (T6). |
| A64 | D-BIND-17 "no other binary the package ships carries Fletcher code … **refuses to initialise**" | L5, L17 | **FINDING F19**: revised 2026-11-13 (two packages, each a full shim). Not annotated (R-6). "Refuses to initialise" overstates: only the copy loaded second refuses, and it still loads (d9 Findings (a)5; `h:382-385` says the same). |
| A65 | D-BIND-19 rules 1, 2, 5 (number and message cross; one containment site per direction; absorbed failures counted) | L1, L2 | OK (naming: `Translate()` is `Contain`/`Capture`) |
| A66 | D-BIND-19 rule 4 ("refusals the caller can fix are .NET argument exceptions thrown in managed code before the call, native re-checking the same rules") and D-BIND-20 ("any other negative is an `ArgumentOutOfRangeException` raised in managed code") | d7 L15, BC-8, d9 L15 | **FINDING F20**: for batch options, a managed pre-check gives BC-8 a second answer, which d7 L15 and the "Refuse both" ruling forbid ("a binding passes the refusal through"). Implicit collision R-4, owner; owned by the C# switch round. |
| A67 | D-BIND-31 (BIND-2c takes the codec surface and publisher chain) | — | OK as a ruling; it holds no shim law (miscited, F9) |
| A68 | D-BIND-59 (1): "Per-row is `SubscribeBatched` with `MaxRows = 1`" | D22, D16 | **FINDING F21**: a C# app on the base package alone has no batched subscribe, so C#'s per-row `SubscriberArrow.Subscribe` cannot be "a batch of one" there. Implicit collision R-3, owner; owned by the C# switch round. |
| A69 | D-BIND-68 (`fl_decode_rows_framed`, for the managed batcher) | L13, Q9 | OK: Q9 handed to the C# switch round |
| A70 | D-BIND-157 (12 MB packed ceiling on `Eiva.Fletcher.Interop`) | D22 | OK: stays on the base package |
| A71 | D-BIND-148, D-BIND-149 (exact import allow-list; floors equal) | L8, L12 | Same as F14 (R-9) |

### `binding.h` doc comments (tier 2, survey)

| # | Clause (source) | Governing law | Verdict |
|---|---|---|---|
| A72 | `h:29-42` ownership, stated once | L3 | OK |
| A73 | `h:44-67` versioning: append-only within a major; pre-1.0 no guarantee; every change bumped | L6 | OK |
| A74 | `h:69-83` "A DECLARATION IS EXPORTED ONLY ONCE ITS ITEM LANDS … an unimplemented entry point is a LINK ERROR" | L6, L12, L16 | **FINDING F22**: under L12 the edge build exports declarations it implements as refusals. A reader can take this paragraph to forbid that. Suggest one sentence: "a build that cannot do a thing exports it and refuses with `FL_NOT_SUPPORTED`; ask the variant query (L16)". `h:79` holds for static linking only (d9 Findings (a)6). |
| A75 | `h:85-90` "Which items have landed … one maintained home — the round's tracker, `plans/BIND-csharp-bindings.md`" | L12, L16 | **FINDING F23** (minor): the BIND round is closed. "Which build implements what" now has a law home (d9 L12, L16) and a run-time answer (the query). Re-point it at d9. |
| A76 | `h:160-172` version, "Callable before anything else and from any thread" | L16 | OK: the template for the variant query |
| A77 | `h:176-186` status taxonomy; "Adding one is a stop-and-ask against the seam spec" | L2 | OK: no new number is planned (Q3) |
| A78 | `h:213-215` `FL_NOT_SUPPORTED`: "This provider does not implement the requested behaviour" | L12 | Same as F10 (LC-2) |
| A79 | `h:236-253` `fl_origin` (seam, codec, callback) | L2 | Same as F5 |
| A80 | `h:255-316` `fl_error` caller-owned; the callback direction borrowed | L2 | OK |
| A81 | `h:373-394` marker: "REFUSES TO INITIALISE if it finds one" | L5, L17 | Same as F19 |
| A82 | `h:455-473` `fl_schema` is shared and is never released the Arrow way | L14 | OK. Design note: a batch's schema crosses as an owned `ArrowSchema` (L14), never as an `fl_schema` (census C6). |
| A83 | `h:548-555` timeout: negative refused, `INT64_MAX` unbounded, a binding maps −1 before the call | L15 | OK |
| A84 | `h:851-876` delivery: every pointer borrowed | L11 | OK (row delivery) |
| A85 | `h:1061-1066` decode: "a fresh struct array … the caller IMPORTS AND OWNS" | L14 | OK: the precedent L14 cites |

### `c-abi/README.md` (tier 2, survey)

| # | Clause (source) | Governing law | Verdict |
|---|---|---|---|
| A86 | README:3-6 "shipped as **one shared library per RID**. It is what `Eiva.Fletcher.Interop` carries" | D22, L12 | **FINDING F24**: two shims per RID, one per package |
| A87 | README:36-42 built-ins "registered by the shim itself (D-BIND-16)" | — | **FINDING F25**: wrong citation. D-BIND-16 is "CI/CD reuses existing machinery". The built-ins are D-BIND-3 as amended 2026-09-11 (d9 D6 cites it correctly). |
| A88 | README:44-55 exports; D-BIND-157 | L7 | OK |
| A89 | README:57-92 building; probe shim | L8 | OK |
| A90 | README:94-111 surface, versioning | L6 | OK (will change with the bump) |
| A91 | README:113-155 ownership | L3, L11 | OK (L14's batch ownership to be added when built) |
| A92 | README:185-211 "Load one shim per process … Handshake before anything else" | L16, L17 | **FINDING F26**: silent on choosing between the base and server shims (Q5a), and the handshake step does not mention the variant query (L16) |

## B. Laws against each other, laws against rulings, and rulings against each other (4b, 4c)

| # | Pair | Verdict |
|---|---|---|
| LL-1 | **L11** ("Everything a callback is handed is borrowed for that call"; D5 / D-BIND-2a "Everything a delivery receives is borrowed") vs **L14** ("A batch crosses as an Arrow C Data Interface array and schema the receiver owns"), with **L15**/§4 putting that batch in a **callback** | **BLOCKER B1.** Both laws were confirmed on 2026-11-13 (D25). Three resolutions, each a law change, so the owner must rule: (a) L11 gains an exception: "except a batch (L14), which the batch callback receives owned and must release" (closest to C Data's move semantics and to `fl_decode_rows`); (b) L14 becomes "borrowed for the call; a receiver that keeps it moves it" (Arrow's documented move); (c) no callback: the caller pulls batches with an OUT parameter, which conflicts with L15's "the batch callback runs on domain 7's batch thread". Stage design of the batch crossing cannot start until this is ruled. |
| LL-2 | **L12** ("Nothing else in the edge build changes") vs **L6** ("every change bumps the version"; a C# binding refuses a different minor) | **FINDING F1** → **LC-1**. Read together, the edge build's version moves because its surface grows (D23). L12's "nothing else" should read "nothing else, beyond the version bump L6 requires". Q8 (one version for both builds, or not) is open and listed as the owner's. L13 plus the variant query make one shared version sufficient. |
| LL-3 | **L4** ("one answer of its own") vs **L12** (the edge refusal is answered by the shim itself) | **FINDING F3**: compatible once L4 records the second exception |
| LL-4 | L7 (export table is exactly the surface) vs L12 (both builds export the same set) | OK: L12 strengthens L7. F7 asks for the test. |
| LL-5 | L8 (edge links no Arrow C++) vs L13 (server only adds) | OK |
| LL-6 | L5 / L17 vs D22 (two packages) | OK: L17 states the joint rule |
| LL-7 | d9 L15 vs d7 L15 / BC-8 / BC-9 | OK: d9 L15 is d7's obligation restated |
| LL-8 | L13 vs L16 (the query's answer differs by build) | OK: L13 names it |
| R-1 | 2026-11-12 "the edge variant exactly as today" vs 2026-11-13 "Wrong build: Clear 'not supported'" (the edge build gains a query and refusals) | OK: **explicit supersession**. The chosen option's text said "the query would be added to the edge too", and d9 L12 records "amended 2026-11-13". |
| R-2 | 2026-11-12 "C# on desktop loads the server variant" vs 2026-11-13 "Separate server package: apps that only publish/subscribe per row stay small" | **FINDING F2: implicit collision, for the owner.** Read literally, the earlier ruling means every C# desktop app loads the server shim. The later one means only apps that reference the server package do. Neither names the other. This also changes Q10 ("which consumer loads the edge variant?"): C# desktop per-row apps would. Not resolved here. |
| R-3 | D-BIND-59 (1) (C# per-row = `SubscribeBatched` with `MaxRows = 1`), restated in 2026-11-12 "Shim only" (2) ("C#'s per-row `Subscribe` is a batched one with `MaxRows = 1`"), vs 2026-11-13 Packaging | **FINDING F21: implicit collision, for the owner** (owned by the C# switch round). Once the managed batcher is deleted (D16), C#'s per-row `SubscriberArrow.Subscribe` on the base package has no batcher to be a batch of one. Either it moves to `fl_subscriber_subscribe` + `fl_decode_rows`, or per-row `SubscriberArrow` needs the server package. |
| R-4 | D-BIND-19 rule 4 and D-BIND-20 (managed pre-checks, "native re-checking the same rules") vs 2026-11-12 "Refuse both" ("the shim passes the refusal through and C# throws as it does today") and d7 L15's binding clause / BC-8 | **FINDING F20: implicit collision, for the owner** (owned by the C# switch round). "C# throws as it does today" can be read as "C# keeps its own check" (`SubscriberArrow.cs:228-237`) or as "C# still throws, now from domain 7's refusal". BC-8 and d7 L15 take the second reading. D-BIND-19 rule 4 mandates the first for every caller-fixable refusal. Translating −1 to `INT64_MAX` is lawful either way (an idiom, d9 §1). |
| R-5 | D-BIND-13 vs 2026-11-13 Packaging | **FINDING F18**: explicit (the ruling names D-BIND-13). Annotation owed by D9S (tier 2). |
| R-6 | D-BIND-17 vs 2026-11-13 Packaging | **FINDING F19**: explicit, annotation owed by D9S |
| R-7 | D-BIND-1a rider ii / Decision 6 vs 2026-11-12 "one shim, two variants" | **FINDING F17**: explicit partial supersession (D7O audit R-4; the owner was told the cost). Annotation owed by D9S. |
| R-8 | D-BIND-157 vs 2026-11-13 Packaging | OK: explicit, "keeps D-BIND-157 for the base package" |
| R-9 | D-BIND-148 (exact DLL allow-list; "a new DLL … needs a ruling") and D-BIND-149 (floors equal the stated ones) vs the server variant | **FINDING F14: gap, for the owner.** No ruling says whether these apply to the server shim as they stand, or with lists and floors of its own. If Arrow C++ adds a Windows import or a higher `GLIBCXX`, D-BIND-148 already requires a ruling. Pair this with Q5a's "does the server package get a size ceiling of its own?" |
| R-10 | D-BIND-1a rider i vs 2026-11-13 Packaging (the split mirrored in C#) | **FINDING F16: implicit, for the owner (low).** Probably compatible (a .NET process is still never the microcontroller; C# desktop simply chooses), but the rider's text now reads against the rulings (d9 Findings (d)5). |
| R-11 | Header no-stub rule (`h:69-83`, confirmed by D-BIND-52) vs "Wrong build" | OK: "this build cannot do the thing" is what the rule reserves `FL_NOT_SUPPORTED` for (but see LC-2) |
| R-12 | D-BIND-68 (framed decode for the managed batcher) vs 2026-11-12 one batcher (d7 D16) | OK: handed to the C# switch round (d9 Q9, Findings (d)6) |
| R-13 | 2026-11-13 "Codec owner: Domain 2" vs D-BIND-39 (byte identity) | OK: the ruling puts both codecs under d2 L2 |
| R-14 | D-BIND-3 ("Dropping a built-in from the shim → STOP-AND-ASK") vs the server package | OK: a full shim (D22) |
| R-15 | Ledger notes: the 2026-11-13 entry has no "*Ingested …*" line, though d9, d2 and the domain map carry it; the 2026-11-12 "D7O-2" entry's *Applies to* cites `plans/D7O-pre-round-audit.md`, now `docs/archive/D7O/` | **FINDING F27**: notes, not ruling text. Add the ingestion line at D9S close and re-point the path the way archived citations were re-pointed. |

**LC-1 (owner confirmation owed).** Does the edge build's ABI minor bump when the variant query and the
batched refusals land? L6 says yes (every change bumps; the C# handshake refuses a different minor,
`NativeLoader.cs:258-259`). Does one `FL_BINDING_ABI_VERSION` serve both builds? Q8, which d9 lists
as the owner's. Recommended: one version for both, bumped once, because the builds are told apart by
the query (L16), not by the version.

**LC-2 (owner confirmation owed).** May the edge refusal use `FL_NOT_SUPPORTED`, whose published
meaning (`core/README.md:37`, `h:213-215`) is "This provider does not implement the requested
behaviour"? The alternative is a new number, a seam stop-and-ask (`h:185-186`). d9 L12 reads the
ruling's "named" as `FL_NOT_SUPPORTED` plus a message naming the edge build and the server package.
Widening the published meaning to "this provider or this build" is a text change to the seam's table
(domain 3's document). Also owed: which `fl_origin` the refusal carries (F5).

## C. Test corpus (Gate 4 step 6)

Searched: `c-abi/tests/*`, `c-abi/test_package`, `integration-tests/binding-abi-conformance`,
`dotnet/tests/**/*.cs` (sources only), `.github/workflows/ci.c-abi.yml`, `ci.dotnet.yml`, `scripts/check_*`.
I searched for exact export lists or counts (`47`, `nm` comparisons), version pins, `NotSupported`
meanings, Arrow-absence checks and native-asset rules. **No test asserts behaviour that a law or ruling
now forbids. Nothing is deleted.** No test pins the exact export list or an absolute ABI minor in a
way the bump breaks, except the C# constant (T2), whose subject is lawful.

| # | Test or check | What it pins | Subject lawful? | Disposition |
|---|---|---|---|---|
| T1 | `BindingAbi.VersionMatchesHeader`, `.VersionIsPackedMajorMinor` (`c-abi/tests/test_binding_abi.cpp`) | shim answer = header constant; packing | Yes (L6) | KEEP. Relative, so the bump does not break it. Run it on both artefacts. |
| T2 | `NativeLoaderTests.TheShimLoadsAndReportsTheAbiVersion…`, `.TheHandshakeRefusesAnyOtherVersion` (`dotnet/tests/Fletcher.Tests/NativeLoaderTests.cs:44-120`), via `NativeLoader.HeaderVersionMinor = 7` (`dotnet/src/Fletcher.Interop/NativeLoader.cs:34`) | the exact-minor handshake | Yes (L6, D-BIND-42) | KEEP. **The bump forces a one-line edit in domain 10's file** in the same commit, as every earlier bump did. Otherwise every dotnet lane loads a 0.8 shim and refuses it. This crosses the one-domain rule: **owner/PM question OQ-1.** |
| T3 | `ci.c-abi.yml:81-110` (win 12 MiB) and `:164-196` (linux 20 MiB) size ceilings; error text "exists to catch Arrow C++ being linked" | edge shim has no Arrow C++ (proxy) | Yes (L8; L12 "no size check" changes) | **RE-ANCHOR** the artefact selection: pick the edge shim by package ID or option, not `find -print -quit` on the file name. The subject and the numbers stay. The server shim gets its own measurement and no Arrow-named ceiling unless ruled (R-9). |
| T4 | `ci.c-abi.yml:231-247` Linux export check (`^fl_` prefix plus `fl_binding_abi_version` present) | no non-`fl_` export | Yes (L7) | KEEP. It is not exact, so new entry points do not break it. EXTEND (F7): an exact-set comparison against `binding.h` and edge = server. |
| T5 | `ci.dotnet.yml:137-170, 285-313`: shim found by file name, stripped, `check_shim_dependencies.sh` | edge shim's deps and floors | Yes (D-BIND-148/149) | **RE-ANCHOR** the selection to the edge artefact, as T3. The server shim's run waits on R-9. |
| T6 | `scripts/check_dotnet_packages.sh:101-113`: Interop ≤ 12 MB, exactly the two shims, and "carries native assets, which only Eiva.Fletcher.Interop may (D-BIND-13)" | D-BIND-157; D-BIND-13 as written | Partly: the D-BIND-157 half is lawful; the "only Interop" half is retired by D22 | **RE-ANCHOR** the D-BIND-13 half to "native assets only in the base and server packages; every other package none". KEEP the D-BIND-157 half on the base package. The server package's expected `runtimes/` set waits on Q5a. |
| T7 | `scripts/check_shim_dependencies.sh`, `scripts/shim_floors.env` | exact import list; floors equal | Yes for edge | KEEP for edge; server per R-9 |
| T8 | `scripts/check_third_party_notice.sh` (`ci.dotnet.yml:443`) | notice covers what the binary carries | Yes | EXTEND: the server package's notice must list Arrow C++ and its static deps |
| T9 | `scripts/check_conan_lockfile.sh` | c-abi graph equals the lock | Yes | EXTEND: one lock per variant graph |
| T10 | `c-abi/tests/CMakeLists.txt:6-15, 57-58, 152`: test binaries link `arrow-bridge` and Arrow C++ | the oracle is test-only | Yes (L8 allows tests) | KEEP. A structural "no Arrow C++" check (C2) must inspect the shim, never a test binary. |
| T11 | `BindingEntryPoints.TheShippedShimHasNoProbe` (`test_binding_entry_points.cpp:834`) | exactly three providers | Yes (D-BIND-3, L13) | KEEP; run on both artefacts |
| T12 | `SingleCopy.*` (decoy module), `Containment.APoisonedShimRefusesWithoutRunningTheCall` | one shim per process | Yes (L5, L17) | KEEP. EXTEND with a real edge + server pair in one process (L17). |
| T13 | `NanoarrowCodec.*` incl. `ByteIdenticalToArrowBridge` | d2 L2 byte identity | Yes (domain 2's) | KEEP. Run against both artefacts (L13). The server binary also holds `BatchDecoder`, and d2 L8 parity is untested (domain 2's). |
| T14 | `integration-tests/binding-abi-conformance` (`requires("fletcher-c-abi/[*, include_prerelease]")`) | an `Apache.Arrow` export reaches the codec as Arrow C++'s does | Yes | KEEP. It resolves the default (edge). EXTEND to the server variant (L13: identical behaviour). |
| T15 | `BucketThreeOptionsTests` `:73-106, 201-206`, `SubscriberArrowTests` `:238-267`, `ProviderRegistryTests:59-65` | `FletcherStatus.NotSupported` for "this provider cannot" | Yes | KEEP. Unaffected; if LC-2 widens the meaning, they still hold. |
| T16 | Managed batcher tests (`dotnet/tests/Fletcher.Tests/SubscriberArrowTests.cs` batched cases, `Fletcher.GatewayClient.Tests/BatchedSubscriptionTests.cs`) | C#'s own batcher (BC-9 copies) | The subject is a registered, deferred copy | Not D9S's: domain 10 / domain 5. "No interim fixes" (2026-11-12). Untouched. |
| T17 | `c-abi/test_package/src/example.c` | links the package; version equals header | Yes | KEEP |
| T18 | `BindingAbi.CompilesAsC99AndIsSelfContained` (`c99_probe.c`) | header pure C, Arrow forward-declared | Yes (L7) | KEEP. It will catch a batched declaration that names a C++ type. |
| T19 | `ci.dotnet.yml:161-168` strip check ("exports unchanged") | stripping keeps the export table | Yes | **RE-ANCHOR** the selection, with T5 |

## D. Bounded-context census (Gate 4b)

Searched by operation, not by name. I looked for: compile definitions and `#if` that select behaviour
(`c-abi/CMakeLists.txt`, `conanfile.py`: only `FLETCHER_C_ABI_BUILDING` (visibility) and the probe's
`builtins` swap); range checks on a row count or timeout (`c-abi/src`: none; `fl_schema_arrival_wait`
forwards, `cpp:941-946`; C# `SubscriberArrow.cs:228-237`, `GatewayBatch.cs:126`, `SchemaArrival.cs:216-231`);
timeout translation (`SchemaArrival.ToMilliseconds`, the one C# site, saturating to `long.MaxValue`);
`catch (` outside `containment.*` (`binding.cpp:662`, a plan failure recorded as a null plan;
`nanoarrow_codec.cpp:915` (rethrow), `:1064-1066` (D-BIND-68 bad-row classification); none translates
to a status); status validation in the callback direction (`write_window.hpp:39-52`, `KnownFailure`);
threads, timers, clocks in `c-abi/src` (none); Arrow C Data release producers (`nanoarrow_codec` decode,
`fl_schema_copy`); the single-copy scan (`single_copy.cpp`); native-file selection
(`NativeLoader.cs:65, 216-230`); variant or feature detection in C# (none: the loader probes file names
only); size and dependency checks (T3–T9); batch assembly (`pubsub-arrow/src/subscriber_arrow.cpp`,
BC-1/2/4; C# copies in BC-9).

| # | Context (a question with one right answer) | Disposition | Site / copies | Structural test | Register |
|---|---|---|---|---|---|
| C1 | **Which variant is this build?** (the query's answer, and whether a batched entry point refuses) | **DEFINE** | One build-time constant, set per artefact target in `c-abi/CMakeLists.txt` and read by one function in `c-abi/src` that both the variant query and every batched entry point's refusal call. No incumbent in `c-abi/src`. C# detects no variant. The probe shim (`CMakeLists.txt:164-211`) is a third artefact from the same sources and must answer too (edge, unless designed otherwise). A second `#ifdef` deciding the refusal by itself would be a copy. | For every built artefact (edge, server, probe): query = server ⇔ the batched subscribe does not answer `FL_NOT_SUPPORTED`. The edge and server `fl_*` export sets are equal to each other and to the header's `FL_ABI_EXPORT` set (F7). | **BC-10** (new) |
| C2 | **Is this build allowed to link Arrow C++?** | **DEFINE** | The recipe's variant switch (kept in the package ID, F13) and the CMake target links. Incumbents: `conanfile.py:73-82` (Arrow only under `run_tests`), the CI size ceilings (proxy), D-BIND-148's DLL list (catches a shared Arrow only). | (a) The edge configuration's Conan host graph contains no `arrow`, `fletcher-arrow-bridge` or `fletcher-pubsub-arrow` outside test requires (`conan graph info`). (b) `nm -C` (not `-D`) on the **unstripped** edge shim finds no `arrow::` symbol. Both fail on any unlisted path that brings Arrow C++ in. The size ceiling stays as the coarse check (L12). | **BC-11** (new) |
| C3 | **Are these batch options acceptable?** | **REUSE** BC-8 | Domain 7, `RecordBatchBatcher`'s constructor. The shim checks nothing, uses `int64_t` fields, and lets the `PubSubError` reach `Capture` → `FL_INVALID_ARGUMENT`, origin seam, domain 7's message. Copies: none in `c-abi`; the C# copies are already in BC-8 (C# switch round). | BC-8's 6 x 7 grid driven **through the ABI**: `FL_INVALID_ARGUMENT` ⇔ out of range; message byte-equal to domain 7's; nothing delivered, no subscription left | BC-8: add "the shim (server variant) passes it through; the grid through the ABI" |
| C4 | **When does a window close, in what order, and is a row dropped?** | **REUSE** BC-1, BC-2, BC-4 (BC-9: the shim is not a copy) | Domain 7 alone. The shim forwards each `(batch, attachments, status)` once, in the order received, on the thread it arrives on. | A source scan of `c-abi/src` fails on `std::thread`, `std::condition_variable`, `steady_clock`/`system_clock` arithmetic or a timer (L11, BC-9). Plus d7's `EveryCallbackRunsOnTheBatchThreadInWindowOrder` replayed through the ABI. | BC-9: add "the shim: none (structural scan)" |
| C5 | **A batch domain 7 delivered that the shim cannot hand across** (export or allocation failure in the shim's batch thunk): is it reported, and where? | **DEFINE, owner ruling owed** | No answer today. A throw from the thunk is swallowed by domain 7 without counting (d7 L10; BC-7), which loses rows silently (d7 L1). Options: (a) the thunk never throws; on failure it delivers L14's "no batch could be built" with those rows added to `rows_dropped`. That is a second classify-and-count site next to BC-2's `TryAppend`, so BC-2 must list it. (b) Domain 7 learns a callback-failure outcome (a d7 change; BC-7). Both touch domain 7's laws or register, so the owner decides. | Fault injection: export fails on window k; Σ`num_rows` + Σ`rows_dropped` seen by the C caller = rows offered (BC-2's conservation property, through the ABI) | **BC-12** (new, deferred until ruled) |
| C6 | **How is a batch released, and by whom?** | **DEFINE** | One shim function turns domain 7's batch into the C form with Arrow C++'s exporter. The receiver releases array and schema through their own `release` (L14). The batch's schema is an owned `ArrowSchema`, never an `fl_schema` (`h:455-473`). Incumbents for "owned C Data": `fl_decode_rows` (nanoarrow's release), `fl_schema_copy`. "Shared": `fl_schema`, `fl_blob`. D-BIND-1c warns against conflating the two lifetime protocols. | Allocation-counting `MemoryPool`: after the receiver releases every batch, pool bytes return to baseline (the shim keeps nothing, L14). Releasing in any order and from any thread is safe. No batch callback is ever handed an `fl_schema`. | **BC-13** (new) |
| C7 | **How is a status or failure reported across the ABI?** | **REUSE** | `Contain`/`Capture` (`containment.hpp:55-77`, `containment.cpp:56-105`); callback direction `write_window.hpp` `KnownFailure`. The edge refusal is a `PubSubError(kNotSupported)` thrown inside `Contain`, not a hand-written `fl_error` (LC-2, F5). The four other `catch` sites classify and never set a status. | The existing L1/L2 tests, plus a scan: no file in `c-abi/src` other than `containment.*` writes `err->status` | none (L1, L2, D-BIND-19 rule 2 hold it) |
| C8 | **How does a timeout, or no timeout, cross the ABI?** | **REUSE** (D12 / D-D7O-1) | One convention: `int64_t` ms; `INT64_MAX` = unbounded / no deadline; a negative gets the callee's refusal. Shim sites forward (`cpp:941-946`; the batched subscribe). The binding translates once (C#: `SchemaArrival.ToMilliseconds`, `SchemaArrival.cs:216-231`). The C# batch check (`SubscriberArrow.cs:228`) is a BC-8 copy. | Through the ABI: `INT64_MAX` gives no `kTimeout` batch; −1 gives `FL_INVALID_ARGUMENT` with domain 7's message, untranslated by the shim | **BC-14** (new): "What does a timeout value mean when it crosses the binding ABI?" |
| C9 | **Is another shim already loaded in this process?** | **REUSE** (extend the test) | `single_copy.cpp:34-145` scan, latched; refusal in `Contain` (`containment.hpp:61-64`). Both variants export the marker (L17). | `SingleCopy.*` with a real edge + server pair loaded in either order: the later one refuses and names both | **BC-15** (new) |
| C10 | **Which shim file does a binding load: base or server package?** | **DEFER-BUT-REGISTER** (Q5a; shim round with the C# switch round) | `NativeLoader.cs` resolver: one library name, `fletcher-c-abi`. `check_dotnet_packages.sh:108` expects that name in Interop. If both packages put the same file name under `runtimes/<rid>/native/`, the app's output holds one, chosen by the build. Then the wrong variant loads with no error, and only L16 or L12 reveals it. | None until Q5a is ruled | Proposed as part of BC-10's text (deferred half) |
| C11 | **Can this binding use this shim's version?** | **REUSE** | `binding_version.c:10`; `NativeLoader.VerifyAbiVersion` (`:253-259`), one site per side. The variant adds no second version answer (LC-1). | T1, T2 | none (unless LC-1 splits the version) |
| C12 | **May a batched subscription be closed or destroyed from inside its callback, and does that wait?** | **REUSE** d7 L9 / BC-5 / BC-6 | Domain 7's `Stop`. The shim adds no refusal and no termination (F4). C#'s managed refusal of a destroy inside a delivery (D-BIND-18) is the row path's and is domain 10's. | d7 `UnsubscribeFromInsideACallbackNeverDeliversNested` (cases a–e) through the ABI | BC-5: note the shim forwards |
| C13 | **What Arrow type does a dictionary column arrive as?** | **REUSE** domain 2 | Two answers by domain 2's own laws: `BatchDecoder` re-folds (d2 L7), `NanoarrowCodec` gives the value type. The shim converts neither. Q11 (domain 2 with domain 10). | none here | none (domain 2's) |
| C14 | **Are these bytes a valid row?** (`NanoarrowCodec::DecodeRows*` and `BatchDecoder`, now in one binary) | **DEFER-BUT-REGISTER** (domain 2) | d9 Findings (b)3; d2 L8 `[built as a property; no test against domain 2]` | none yet (d2's parity test) | d2's to add; recorded here because the server variant is where both meet |

### Proposed register rows (text for `docs/domains/bounded-context-register.md`; not edited)

| ID | Question | Single site | Structural test | Status | Domain |
|---|---|---|---|---|---|
| BC-10 | Which variant is this shim build, and does it serve a batched subscribe? | One build-time constant per artefact (`c-abi/CMakeLists.txt`), read by one function in `c-abi/src` that both the variant query (L16) and every batched entry point's edge refusal (L12) call. No second `#if` decides either. **Deferred half:** which shim file a binding loads when both packages are referenced (Q5a), today `NativeLoader`'s single library name. | For every built artefact (edge, server, probe): query = server ⇔ the batched subscribe does not answer `FL_NOT_SUPPORTED`; the `fl_*` export sets of edge and server are equal, and equal to `binding.h`'s `FL_ABI_EXPORT` set. | `[designed-not-built]` (D9S); the file-selection half deferred (Q5a) | d9 (L12, L13, L16) |
| BC-11 | May this shim build link Arrow C++? | The recipe's variant switch, kept in the package ID, and the shim target's links. Edge: never (L8). Server: through `pubsub-arrow` only. | The edge configuration's Conan host graph has no `arrow`, `fletcher-arrow-bridge`, `fletcher-pubsub-arrow` outside test requires; `nm -C` on the unstripped edge shim finds no `arrow::` symbol. The size ceilings stay as the coarse check. | `[designed-not-built]`; today `[built as a property; no code owner]` (size proxy only) | d9 (L8) |
| BC-12 | Is a batch domain 7 delivered, but the shim could not hand across, reported? | **Owed: owner ruling** (census C5). Either the shim's batch thunk reports it as L14's "no batch could be built" with the rows in `rows_dropped` (a second listed classify-and-count site beside BC-2's `TryAppend`), or domain 7 gains a callback-failure outcome (BC-7). | Fault injection at export: rows conserved as seen by the C caller | **Deferred, registered** | d9 / d7 (d7 L1, L2; BC-2, BC-7) |
| BC-13 | Who releases an Arrow C Data structure the shim hands out, and how? | Owned C Data (a decoded array, a schema copy, a batch's array and schema) is released by the receiver through the struct's own `release`, and the shim keeps nothing (L14, `h:1061-1066`). Shared structures (`fl_schema`, `fl_blob`) are released only through their owner handle and never the Arrow way (`h:455-473`). The batch export goes through one shim function. | Allocation-counting pool returns to baseline after the receiver releases every batch; no batch callback is handed an `fl_schema` | `[built]` for decode and schema copy; batch `[designed-not-built]` | d9 (L3, L14) |
| BC-14 | What does a timeout value mean when it crosses the binding ABI? | The callee's meaning, forwarded untranslated: `int64_t` milliseconds, `INT64_MAX` = unbounded / no deadline, a negative = the callee's refusal (D12, D-D7O-1). The shim translates nothing. Each binding translates its own idiom at one site (C#: `SchemaArrival.ToMilliseconds`). | Through the ABI, for each timeout-taking entry point: `INT64_MAX` never fires; −1 is refused with the callee's own message | `[built]` for the arrival; batch `[designed-not-built]` | d9 (L4, L15) |
| BC-15 | Is another copy of the shim loaded in this process? | `single_copy.cpp` (load-time module scan for `fl_single_copy_marker`, latched once), refusal at `Contain`. Both variants export the marker. | `SingleCopy.*`, extended with a real edge + server pair in either load order | `[built]` for two copies of one build; across variants `[designed-not-built]` | d9 (L5, L17) |

## Owner-level items (one line each)

- **B1 (BLOCKER)**: L11 / D-BIND-2a ("everything a callback/delivery is handed is borrowed") vs L14 (the batch is owned by the receiver, handed to a callback per L15). Rule (a), (b) or (c) of LL-1 before the batch-crossing stage is designed.
- **LC-1**: the edge build's ABI minor bumps with the query and the refusals (L6), with one version for both builds (Q8). Confirm.
- **LC-2**: the edge refusal is `FL_NOT_SUPPORTED`, whose published meaning is "this provider…"; widen it to "this build", or allocate a number (seam stop-and-ask). Also which `fl_origin` it carries.
- **R-2**: "C# on desktop loads the server variant" (2026-11-12) vs "per-row apps stay small on the base package" (2026-11-13). Implicit collision.
- **R-3**: D-BIND-59 / "Shim only" (2) (C# per-row is a batch of one) vs Packaging (the base package has no batcher). Implicit collision, C# switch round.
- **R-4**: D-BIND-19 rule 4 / D-BIND-20 (managed pre-checks) vs "Refuse both" / d7 L15 / BC-8 (pass the refusal through); "C# throws as it does today" is ambiguous. Implicit collision, C# switch round.
- **R-9**: do D-BIND-148 (exact DLL list) and D-BIND-149 (floors) apply to the server shim, or does it get its own lists, floors and a size ceiling (Q5a)?
- **R-10 (low)**: D-BIND-1a rider i ("the split may not need mirroring in C#") now reads against the packaging ruling.
- **C5 / BC-12**: a batch the shim cannot hand across is lost silently today (d7 L10 swallows a thunk throw). Choose the reporting site (shim, or domain 7).
- **OQ-1**: the ABI bump forces `NativeLoader.HeaderVersionMinor` (domain 10's file) to move in the same commit, or every dotnet lane refuses the shim. Allow this one mechanical edit in a one-domain round, or sequence it.
- **Q5a (deferred)**: native file names of the two packages, dependency between them, how a binding chooses (C10).

## Tier-2 corrections needing approval (suggested, not applied)

- **F3**: L4 and §1 record the edge refusal as a second answer of the shim's own.
- **F1**: L12 reads "nothing else, beyond the version bump L6 requires" (after LC-1).
- **F4**: L1 or L15 states that the batched teardown follows d7 L9/D12 (no termination, no refusal).
- **F6**: L6's list of bumps adds D-BIND-52.
- **F7**: L7's holds-at says "the CI step checks the `fl_` prefix only".
- **F9**: L12 and §5 D14 cite `h:69-83` and D-BIND-52, not D-BIND-31.
- **F15**: §8 rule 5 cites L8 and L12.
- **F17, F18, F19**: annotate Decision 6 / D-BIND-1a rider ii, D-BIND-13 and D-BIND-17 (owed by D9S).
- **F22, F23, F10**: `binding.h` `h:69-90` (edge refusals are lawful exports; inventory home is d9); `h:213-215` per LC-2.
- **F24, F25, F26**: `c-abi/README.md` (two shims per RID; D-BIND-3 not -16; variant choice and query in "Consuming").
- **F27**: ledger ingestion line for 2026-11-13; re-point the D7O-2 entry's archived path.
