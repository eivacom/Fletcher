# PDA-DEC-4 — architecture review (cycle 1 of 2)

Design: `plans/PDA-DEC-4-provider-registry.md` @ `07c8d40` (275 lines) · Brief: `plans/PDA-DEC-4-brief.md` (60).
Oracles: spec §4/§4.1/§4.2/§1/§9/§11, locked decisions 2/3/4/8/14, the 25-entry rulings ledger.

**Verdict: APPROVE-WITH-DEBT(10). No BLOCKERs. No stop-and-ask.**

The two rulings the PM asked for are in §A and §B; they are answered here so that neither
needs to reach the owner. §C–§F are the pressure tests. §G is the debt register.

---

## A. RULING on premise P2 — settled in the design's favour, and moot either way

§4 clause 2: *"PDA-ABI adds a resolver, not a second API. The registry's signature is fixed
here."* §9's table: PDA-ABI *"Adds to §4 — a resolver for path selectors"*, *"May change the
seam — no."*

The design reads "the registry's signature" as `Create`'s. That is the natural reading, but
**the ruling does not depend on it**, because this design lands `SetPathResolver` *here*:
it is declared in §1's class, exercised in-round by `Registry.PathSelectorResolvesThroughTheSameCall`,
and its empty state is a live negative control. So under the *strict* reading — clause 2
forbids any new registry method in PDA-ABI — the design already complies: PDA-ABI adds no
registry method at all, it **calls one that already exists**. The alternative reading's stated
cost ("the resolver seat must be complete and callable here") **is already paid by this design.**

Consequence: **P2's stop-and-ask does not fire, and must not be raised by the implementer.**
The only residue is that the design's own §4-clause-2 restatement (`Files-to-touch`) is narrower
than what it should say — see DEBT-2.

I concur with **Brief decision 1(a)**: (b) would leave §4 clause 1 ("a selector is a name or a
path") unimplemented and would move the path branch's *behaviour* into PDA-ABI, which is the
change clause 2 exists to prevent. (a) is the only option that discharges the round's purpose
with an executable proof rather than prose.

## B. RULING on "no kind accessor" — the substance holds; the word is overclaimed

Decision 3 and the 2026-09-01 ruling require that built-in-versus-loaded be **invisible above
the seam** and that a protocol be movable built-in → loaded **without touching a caller**.
Both hold under this design: the caller passes a config string through one `Create`, and the
move is a configuration edit (`fastdds` → `/opt/libfastdds_driver.so`). No caller changes.
There is no second creation call, no kind enum, no `Create` overload set.

But rung-1 item 1 claims a caller that branches on built-in-vs-loaded is **unrepresentable**,
and that is not true. The classification rule is (necessarily) public and is a *pure function
of the string that never consults the registry* — so any caller can re-derive it in one line:

```cpp
const bool is_a_name = s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos;
```

and, under this design, name ⟺ built-in and path ⟺ loaded exactly. `typeid(*p).name()` on the
returned provider is a second one-liner, and `dynamic_cast<InProcessPubSubProvider*>` works
today for any caller linking `fletcher-pubsub`. None of this is fixable — you cannot stop a
caller inspecting a string it already holds, or RTTI on a polymorphic base — and none of it
matters, because such a branch **breaks nothing when the configuration changes**. The accurate
claim is *"the seam offers no way to ask, and no caller has cause to"*. DEBT-6.

## C. The signature claim — attacked, and it holds

`Parse`'s rule (`[A-Za-z0-9_-]+` is a name; every other non-empty string is a path; empty
refused) is **total and disjoint** by construction, and independent of what is registered. I
pushed the realistic inputs through it:

| Input | Class | Right answer? |
|---|---|---|
| `/opt/fletcher/libzenoh.so`, `./x.so`, `../x.so` | path (`/`, `.`) | yes |
| `C:\drivers\zenoh.dll`, `drivers\z.dll`, `\\host\share\z.dll` | path (`:`, `\`) | yes |
| `libzenoh.so`, `zenoh.dll`, `z.dylib` (bare, relative) | path (`.`) | yes — every shared-library spelling on every target carries a dot |
| non-ASCII / UTF-8 path | path | yes |
| `fastdds`, `in-process`, `xrce_dds` | name | yes |
| `fastdds.v2` | path — and `Register` refuses it symmetrically | consistent: the two vocabularies cannot drift |
| `""` | refused `kInvalidArgument` | yes |
| `myDriver` (relative file, no dot, no separator) | **name** → `kInvalidArgument` "unknown name, available: …" | a misclassification, but loud, and the operator's fix is `./myDriver` — **no signature change** |
| `"fastdds "` / `"fastdds\r\n"` (config whitespace) | **path** → `kNotSupported` "this build cannot load drivers" | loud but confusingly worded — DEBT-8 |

No input misclassifies **silently**, and no misclassification is repaired by adding a
disambiguator, an overload or a flag — every one is repaired by the operator editing one
string. The design is also right that the rule must not consult the registry: a
registry-consulting rule would make `zenoh` mean different things in two builds, which is the
drift decision 3 forbids.

The claim survives three further attacks the design does not name:

- **Static registration of an ABI driver on MCU** (linkage ruling's static half): PDA-ABI wraps
  the C entry point in a `Factory` lambda in its own component and calls `Register(name, …)`.
  No new method.
- **A loaded driver reachable by *name*** (so one config works on MCU and desktop): same
  mechanism — `Register("zenoh", factory_that_dlopens)`. The seat is strictly more general than
  the path resolver alone, so all four (built-in|loaded) × (name|path) combinations are
  expressible through the frozen surface.
- **Version negotiation, host-callback tables, module caches**: all live inside PDA-ABI's
  resolver closure; none appears in `Create`'s signature.

The one thing PDA-ABI genuinely needs and does not get is a **lifetime rule for the resolver's
product** — see DEBT-1, the most substantive item below.

Decision 14 is honoured: `PathResolver` is a `std::function` over C++ types, nothing calls it,
no build depends on a loader, and no `extern "C"` / C header / vtable / negotiation appears. It
is a *seat*, not an ABI shape: it says nothing about what a loader looks like beyond "given a
path and a config, produce a provider".

## D. Falsifiability — the three named tests can go red

- `SelectsByNameWithoutCallerKnowingTheProvider` asserts *a delivered row under the tag the
  name maps to*, not non-null. M1 (swap registrations) and M3 (probe stops recording) redden
  it. **M2 (return the first factory) reddens only if the test asserts both directions** —
  `alpha`→alpha *and* `beta`→beta. The design implies this ("two factories"); the implementer
  must confirm it when running M2, or M2 is a vacuous mutation.
- `PathSelectorResolvesThroughTheSameCall` asserts the resolver was reached *and* the recorded
  path string. M4 and M5 redden it.
- `PathSelectorWithoutResolverIsRefusedAsUnsupported` is a **genuinely independent** control:
  it fails exactly in the state that would let test 2 pass for the wrong reason (the seat wired
  to something, or the path branch defaulting), and it asserts a specific status rather than
  "it threw". M6 reddens it. Running the whole `Registry.` suite in the inner loop, not the
  forcing test alone, is the right call and matches PDA-DEC-2's convention.

The vacuity rule ("no test may pass on non-null or on it-threw") is written into the table with
a named mutation per row. This is the strongest part of the design.

The third leg of the frozen-signature defence — the `static_assert` — is under-specified: see
DEBT-3.

## E. Scope, budget, buildability

- **Nothing is designed that PDA-DEC-5/6/7 must undo.** No shim, no re-export, no deprecated
  alias, no coexistence bridge; `Files-to-delete: none` is honest because this item is add-only
  and every construct it obsoletes has a named later owner. `SchemaCarriage` arriving as a
  document key (not a second construction API) matches the forward note in
  `in_process_provider.hpp:41-43` **verbatim** and closes carried debt C2-9.
- **Budget:** design 275/300, brief 60/60 ✓. Surface 5 against a budget of 3 — the round config
  already anticipates this item as the exception (`new_public_surface: 3  # PDA-DEC-4's registry
  is the one item expected to need more`). For the PM's waiver decision, not a finding:
  `ProviderConfig` and `ProviderRegistry` are unavoidable; `Factory` and `PathResolver` are
  public *names* but not new concepts — they must be nameable because the methods taking them
  are public, and removing them only means spelling `std::function<…>` inline for the same
  surface and worse documentation. `ProviderSelector` is the one genuinely optional type
  (`Create(const std::string&, const ProviderConfig&)` would carry the identical
  no-kind-accessor property at surface 4); it earns its place by refusing the empty string at
  configuration-read time and giving the classification rule one home, but the PM should know
  the waiver is really "4 + 1 by choice".
- **+640/−25** is plausible-but-tight for a commented header + impl + a six-test suite with two
  probe providers; this round has under-costed twice (PDA-DEC-1: +108%). Not a finding.
- **Buildability verified:** `pubsub/conanfile.py` packages headers by glob, so no conanfile
  change is needed and `Files-to-touch` is complete. A provider-SDK-free binary in this harness
  has two precedents (`conformance_copy_accounting`, `conformance_seam_vocabulary`), both with
  the same "no RESOURCE_LOCK — no domain, no port, no child" shape the design copies. No hidden
  cross-cutting change; no CI path change (the binary lands in an existing harness).

## F. Tree claims spot-checked

| Claim | Verdict |
|---|---|
| Linker drops static-initialiser objects out of a static archive, measured in this tree | **TRUE** — `integration-tests/pubsub-conformance/CMakeLists.txt:98-100` |
| `in_process_provider.hpp`'s forward note requires `SchemaCarriage` to arrive as config, not a second construction API | **TRUE** — lines 41-43, verbatim |
| `IsPayloadBound(0)` is false everywhere in this tree | **TRUE** — `payload_bound.hpp:36-38`; asserted at `fastdds-pubsub-provider/tests/test_fletcher_sample_pub_sub_type.cpp:160` |
| `PubSubStatus` already carries `kInvalidArgument` / `kNotSupported` / `kInternal` (P4) | **TRUE** — `core/include/fletcher/core/status.hpp:67-83`; `TranslateSeamFailure` (lines 132-150) already implements the `overflow_error → kPayloadTooLarge`, else-`kInternal` rule the design cites |
| `-o run_tests=True` is a no-op on a cached package; `-DFLETCHER_CONFORMANCE_XRCE=ON` must be explicit | **TRUE** — config `known_accepted_failures` and `full_suite_cmd:73-77` |
| `pubsub/CMakeLists.txt` needs one source line | **TRUE** — lines 11-16 |
| "Fletcher's providers are constructed at four sites in tree (§10)" | **FALSE as stated** — §10 counts four *files consuming the protocol-typed config*, and predates PDA-DEC-1/2/3; the conformance harness alone now adds ~8 construction sites. Harmless here (add-only item); see DEBT-10 |
| "the registry TU links `fletcher-pubsub` only — a provider header is unreachable from it" | **Overstated** — `in_process_provider.hpp` lives *inside* `fletcher-pubsub` and is reachable; PDA-DEC-5 will use it in this very suite. What the link line actually checks is that no **transport SDK** is reachable. DEBT-7 |

No hand-composed post-change ledger, survival table or fixpoint walk appears in the design; the
machine checks named (compiler, link line, `static_assert`, `ctest -N`) are the right instruments.

---

## G. DEBT register — 10 items, none loops the design

**DEBT-1 (the substantive one) — the resolver seat has no lifetime rule.** The design states
that the registry "holds no reference to what it made, so it may be destroyed while providers
live", and endorses *populate, then share* — so discarding the registry after startup is a
sanctioned, natural pattern. In PDA-ABI the module handle will most naturally live in the
resolver closure (a module cache), which the registry owns. Destroying the registry then unloads
a `.so` under a still-running provider: use-after-unload, and not reliably loud. One normative
sentence beside `SetPathResolver` closes it for good: *a resolver must ensure everything the
returned provider depends on — including a loaded module — stays alive as long as that provider
does, independently of the registry's and the resolver's own lifetime.* This is locked
decision 5's idiom applied to the one seat PDA-ABI fills.

**DEBT-2 — freeze the whole registry surface in the §4 clause 2 amendment, and strike P2's stop
condition.** The design's planned restatement ("`Create` is frozen; a resolver is installed, not
added to it") leaves the narrow reading available to PDA-ABI. Say instead: *the registry's
public surface is fixed here — `Create`, `Register`, `SetPathResolver`; PDA-ABI adds no registry
method, it calls `SetPathResolver`.* Then rewrite P2 to record the ruling in §A rather than a
stop-and-ask, so the implementer does not escalate an answered question.

**DEBT-3 — pin the whole signature, not the return type.** The design's second leg is "a
`static_assert` that `Create` still returns exactly `std::shared_ptr<PubSubProvider>`". A
return-type assert cannot see a defaulted third parameter or a dropped `const` — precisely the
widening this item exists to prevent. Assert the member-pointer type instead:
`static_assert(std::is_same_v<decltype(&ProviderRegistry::Create), std::shared_ptr<PubSubProvider> (ProviderRegistry::*)(const ProviderSelector&, const ProviderConfig&) const>);`

**DEBT-4 — a second `SetPathResolver` call silently replaces the first.** The design's own
forbidden case 10 argues that "silently swapping which transport a name means is not a state
this type can enter"; the identical argument applies to swapping which loader every path means,
and the case is reachable (a host and a bootstrap library both installing one, or a test double
overwritten by a real loader). Refuse the second call with `kInvalidArgument` — rung 2, one
line — or state why replacement is legal.

**DEBT-5 — state the selector's C form, and refuse an embedded NUL at `Parse`.** The design
gives `document` its §3.5 C form (pointer + length, borrowed, length authoritative) but leaves
the selector's unstated, and §4 selection is *binding-visible* surface (§9), so a binding must
not have to invent it. While stating it: a length-carrying binding can pass `"fastdds\0…"`,
which classifies as a path and would reach PDA-ABI's `dlopen(path.c_str())` truncated —
loading a different library with no signal. Forbidding an embedded NUL in `Parse` is cheaper
than every future resolver remembering to check.

**DEBT-6 — soften rung-1 item 1.** See §B. Substance holds; replace "unrepresentable" with "the
seam offers no way to ask, and no caller has cause to", and note that what decision 3 actually
guarantees is that *moving a protocol built-in → loaded is a configuration edit, never a caller
edit*.

**DEBT-7 — restate forbidden case 5's machine check as "no transport SDK is reachable".**
`in_process_provider.hpp` is inside `fletcher-pubsub` and therefore reachable from the registry
binary; PDA-DEC-5 will link exactly that. The check still does its job (no DDS/XRCE vocabulary
can enter), but the sentence as written is falsified by the next item.

**DEBT-8 — say why a selector was treated as a path.** The realistic misclassification is
trailing whitespace or a CRLF from a config file, and today it surfaces as `kNotSupported`
"this build cannot load drivers", which reads as an infrastructure problem rather than a typo.
Have that message name the classification and the first offending character/offset.

**DEBT-9 — forward note for PDA-DEC-7: `domain_id` narrows.** The seam's core is `uint32_t`
(matching `FastDDSProviderOptions::domain_id`), while `XrceConfig::domain_id` is `uint16_t`
(`xrce_dds_pubsub_provider.hpp:56`). A silent truncation puts the client on the wrong DDS domain
with no error — a wrong answer, not a failure. PDA-DEC-7 must **refuse** out-of-range, never
narrow.

**DEBT-10 — two record corrections.** (a) The Risks line "providers are constructed at four
sites in tree (§10)" is wrong (see §F); §10's blast-radius table predates PDA-DEC-1/2/3 and
should be re-measured by PDA-DEC-6/7 rather than trusted. (b) `provider.hpp:68-70` still says
"Provider-specific configuration … is supplied at provider construction time via a
provider-specific Options struct", which the registry replaces conceptually here and factually
in PDA-DEC-6/7 — update it here (one line) or book it explicitly against those items.

---

## H. What I checked and did not find

No spec, ruling or locked-decision violation. Decision 8 holds (the only operations on
`document` are copy and forward; no parser, no dependency, no format). Decision 14 holds. §4
clause 3 holds (no global state, no cache, multiple instances ordinary, registry destructible
independently — modulo DEBT-1). The configuration-shape ruling's "domain/endpoint identity"
gloss versus §4.1's fixed two fields is **not** silently resolved: it is raised as Brief
decision 3 with the spec correctly outranking the gloss, which is the right handling and the
right escalation. Premises P1, P3, P4, P5 are real premises with real stop conditions, and P4/P5
check out against the tree. `std::function`/`std::string` in the public surface introduce no new
substrate premise — `provider.hpp` already includes `<functional>` and uses both.

*NIT fixed silently in the design doc: `Files-to-touch` claimed `inner_loop_cmd` "gains … the
`conan create pubsub` step"; the existing loop (`runbook.PDA-DEC.config.md:52`) already builds
`pubsub`, so only the `-R` scope changes.*
