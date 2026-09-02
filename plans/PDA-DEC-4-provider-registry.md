# PDA-DEC-4 — Provider registry: name-or-path selector, typed core + opaque document

Oracle: [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md) §4, §4.1, §4.2
(§0.1(1)(2), §5.1, §6, §9, §11 constraining). Decisions 3, 8, 14; also 2, 4.
Rulings 2026-08-31 (linkage, configuration shape, discovery), 2026-09-01 (the bindings
do not depend on the driver ABI; what PDA-DEC does to the existing protocol code).

## Summary

One creation call turns a *selector* plus a *configuration* into a
`shared_ptr<PubSubProvider>`. The selector is a single string from configuration that
**only the seam** classifies as a built-in name or a driver path — no caller can ask
which, because the type exposes no way to ask. Configuration is the §4.1 typed core plus
an opaque document Fletcher forwards and never reads. Built-ins are registered by an
explicit call on an ordinary object; there is no global table and no loader.

## Design

### 1. The three types (all in `pubsub/include/fletcher/pubsub/provider_registry.hpp`)

```cpp
/// WHAT to select. Deliberately not "how it is provided": there is no accessor for
/// the kind, so nothing above the registry can branch on built-in vs loaded (§0.1(2)).
class ProviderSelector {
 public:
  static ProviderSelector Parse(std::string text);   // the ONLY way to make one
 private:
  friend class ProviderRegistry;
  std::string text_;  bool is_name_;
};

/// §4.1 — a small typed core plus bytes Fletcher transports and does not read.
struct ProviderConfig {
  uint32_t max_payload_bytes = 0;   // 0 == unset: the provider's own default applies
  uint32_t domain_id = 0;
  std::string document;             // opaque; format is the PROVIDER's (§4.2)
};

class ProviderRegistry {
 public:
  using Factory     = std::function<std::shared_ptr<PubSubProvider>(const ProviderConfig&)>;
  using PathResolver = std::function<std::shared_ptr<PubSubProvider>(
                           const std::string& path, const ProviderConfig&)>;

  void Register(std::string name, Factory factory);   // a BUILT-IN, explicitly
  void SetPathResolver(PathResolver resolver);        // the seat PDA-ABI fills; empty here
  [[nodiscard]] std::shared_ptr<PubSubProvider> Create(
      const ProviderSelector& selector, const ProviderConfig& config) const;   // FROZEN
};
```

### 2. Classification is the seam's job, not the caller's (the load-bearing decision)

An operator writes one string — `provider = fastdds` or
`provider = /opt/fletcher/libzenoh_driver.so`. If the *caller* decided which of the two
it had, the caller would be branching on built-in vs loaded, which §0.1(2) and decision 3
forbid. So the caller passes the string through and `Parse` classifies it, by one total
rule stated in the header:

> A **name** is a non-empty string of `[A-Za-z0-9_-]` and nothing else. Every other
> non-empty string is a **path**. The empty string is refused. There is no trimming, no
> case folding, no normalisation, and the rule does not consult the registry — so a given
> string means the same thing in every build, whether or not the built-in is linked.

Lookup of a name is exact and case-sensitive; `Register` validates against the *same*
predicate, so a registered name is always selectable.

### 3. How PDA-ABI admits a path without widening anything

`Create`'s signature is frozen here (§4 clause 2). PDA-ABI writes a loader in **its own**
component — `dlopen` never enters `fletcher-pubsub` — wraps it in a `PathResolver` and
calls `SetPathResolver`. The path branch inside `Create` is already present and already
routed; today it finds no resolver and refuses typed. That is what "PDA-ABI adds a
resolver, not a second API" means, and it is **executable in this round**: the suite
installs a stand-in resolver and proves a path selector resolves through the identical
`Create` call, from the identical caller helper, as a name does (test 2 below).

The registry does not *assume* a loader: the seat is empty, nothing calls it, no build
depends on one, and `Create` for a name never touches it.

### 4. Registration: explicit, per-object, no globals

`fletcher-pubsub` cannot depend on `fastdds-pubsub-provider` — the component graph runs
the other way — so a built-in is registered by whoever links it, with one call. This is
the static half of the 2026-08-31 linkage ruling ("MCU targets register the same driver
statically at link time"), done without static-initialiser magic: this tree has already
measured that a linker drops such objects out of a static archive
(`integration-tests/pubsub-conformance/CMakeLists.txt`, the OBJECT-library comment). The
result is that *availability* is a link-time fact stated in code, while *selection* stays
a runtime string — and moving a protocol from built-in to loaded is a config edit, not a
caller edit.

`Create` never caches: each call constructs a fresh instance, and the registry holds no
reference to what it made, so it may be destroyed while providers live (§4 clause 3; the
property PDA-DEC-8 then measures rather than establishes).

### 5. Configuration, and what Fletcher does with the document

Nothing. `document` is copied and handed to the factory; the seam has no parser, no
format, no dependency (decision 8, §4.2). Whether Fast DDS reads it as inline XML or as
the name of an XML profile file is **the provider's** decision, in PDA-DEC-6. Its C form
is §3.5's: a pointer and a length borrowed for the duration of the call, length
authoritative (the bytes may contain NUL); a provider that keeps it copies it. The typed
core is append-only — a later field never changes `Create`.

A built-in's own knobs arrive the same way: `InProcessPubSubProvider::SchemaCarriage`
becomes a document key when PDA-DEC-5 registers it, never a second construction entry
point (that header's own forward note requires this).

### 6. Failure and threading

`Create`, `Register` and `SetPathResolver` are seam entry points, so §5.1 applies
unchanged: they throw `PubSubError` with a stable `PubSubStatus` and nothing else escapes
— an exception from a caller-supplied factory is translated by the same rule (a
`std::overflow_error` becomes `kPayloadTooLarge`, everything untyped becomes `kInternal`
carrying the original `what()`). No new status value is needed.
`Create` is `const` and safe to call concurrently; the two mutators are not. The
ownership rule in the header is **populate, then share** — one construction phase, then
the object is read-only. No lock, so an MCU build pays nothing.

## Corner cases forbidden

**Rung 1 — unrepresentable**

1. *A caller that branches on built-in vs loaded.* `ProviderSelector` has one
   constructor and **no kind accessor**; `Create` returns the same type either way and no
   derived type is nameable from above. There is no second creation call to branch into.
2. *A name that can be registered but never selected* (and its inverse). `Register` and
   `Parse` share one predicate, so the two vocabularies cannot drift.
3. *Selection that depends on link order or on a static initialiser surviving an
   archive.* Registration is an explicit call on an object the caller owns; there is no
   global registry, and none may be added.
4. *A cached or shared provider instance.* `Create` never memoizes and stores nothing, so
   "two selections silently share one transport" is not a state this type can reach.
5. *Fletcher learning a protocol's vocabulary.* The only operations on `document` are copy
   and forward; no parser, no dependency, and the registry binary links `fletcher-pubsub`
   and **no transport SDK** — the compiler enforces that no DDS or XRCE vocabulary is
   reachable from it. (Not "no provider header": `in_process_provider.hpp` is *inside*
   `fletcher-pubsub` and PDA-DEC-5 links this very binary against it. DEBT-7.)
6. *A string that is ambiguously a name and a path.* The rule is total and disjoint, and
   independent of what is registered.

**Rung 2 — refused typed at the door**

7. Empty selector string → `kInvalidArgument`.
8. Unknown name → `kInvalidArgument`, message listing the registered names.
9. Path selector with no resolver installed → `kNotSupported`. Distinct from 8 on
   purpose: "you asked for something that does not exist here" and "this build cannot load
   drivers" are different operator actions.
10. Registering a name twice → `kInvalidArgument`. No overwrite, no last-wins: silently
    swapping which transport a name means is not a state this type can enter.
11. An empty `Factory`, an empty `PathResolver`, or an ill-formed name at registration →
    `kInvalidArgument`, at registration rather than at use.

**Handled residue** — each with why it could not be forbidden

12. *A factory that returns a null provider* → `kInternal`, named. **Why not forbidden:**
    the factory is caller-supplied through `std::function`; no C++ type stops it returning
    null, and the value does not exist until after the call, so there is no door to refuse
    it at.
13. *`max_payload_bytes == 0` meaning "unset".* **Why not forbidden:** the seam does not
    know any provider's valid bounds or its default, so it cannot demand a value it could
    check. It is safe because `IsPayloadBound(0)` is false everywhere in this tree, so
    "unset" can never be mistaken for a real bound by the provider that reads it.
14. *An exception escaping a caller-supplied factory.* **Why not forbidden:** constructing
    a transport can fail for any reason; §5.1 requires only that what leaves the seam is a
    `PubSubError`.

## Premises and stop conditions

- **P1 — `fletcher-pubsub` may not depend on any provider component.** Built-ins are
  therefore registered by explicit call. **STOP-AND-ASK** if review requires
  self-registration via static initialisers: that is global state (§4 clause 3) against a
  hazard this tree has already measured.
- **P2 — ANSWERED at review (§A); the stop-and-ask does NOT fire and must not be raised.**
  Clause 2 is satisfied under *both* readings, because `SetPathResolver` lands and is
  exercised in this item: PDA-ABI adds no registry method, it calls one that already
  exists. As landed, the spec amendment freezes the **whole** registry surface —
  `Create`, `Register`, `SetPathResolver` — not just `Create`'s signature (DEBT-2).
- **P3 — the document is bytes Fletcher never interprets**, including whether it is
  content or a filename. **STOP-AND-ASK** if any provider migration needs Fletcher to
  resolve it (open a file, expand a variable): that is a config dependency, decision 8.
- **P4 — `PubSubStatus` already carries every cause this item needs**
  (`kInvalidArgument`, `kNotSupported`, `kInternal`). **STOP-AND-ASK** rather than
  appending an enumerator to a `static_assert`-pinned enum for a nicer message.
- **P5 — the typed core is exactly `{max_payload_bytes, domain_id}`** (§4.1).
  **STOP-AND-ASK** if PDA-DEC-6/7 finds a setting Fletcher itself must reason about; do
  not widen the core silently. (The 2026-08-31 ruling's "domain/endpoint identity" is
  illustrative — §4.1 fixes the two fields, and the spec outranks the gloss. Raised as
  Brief decision 3 because it narrows what an XRCE operator configures at the seam.)

## Forcing-test mapping

New suite `Registry` — `integration-tests/pubsub-conformance/src/registry.cpp`, its own
binary `conformance_registry`, linking `fletcher-pubsub` and **no provider SDK** (that
link line is itself the machine check for forbidden case 5). PDA-DEC-5 and PDA-DEC-8 add
to this suite rather than opening another home.

| Test | Turned green by | Red for the right reason before / mutation that must redden it |
|---|---|---|
| **`Registry.SelectsByNameWithoutCallerKnowingTheProvider`** (forcing) | §1's `Create` + §4's table. Two factories, `alpha` and `beta`, each producing a probe that records a delivered row under **its own tag** into a journal the test owns. The caller is one helper `MakeProvider(const ProviderRegistry&, const std::string&, const ProviderConfig&)` whose body names no concrete provider type and contains exactly one `Create` | Does not compile today — no such header. **It must not be greenable by a non-null check**: it passes only when a row published through the returned provider surfaces under the tag the *name* maps to. M1 swap the two registrations → red; M2 make `Create` ignore the name and return the first factory → red; M3 make the probe not record → red |
| `Registry.PathSelectorResolvesThroughTheSameCall` | §3's resolver seat: a stand-in `PathResolver` returns a third probe tagged `loaded` and records the string it was handed. **The same helper**, a different config string | M4 make `Parse` classify everything as a name → red (resolver never called; refusal instead). M5 have the resolver ignore its argument and the assertion on the recorded path string → red |
| `Registry.PathSelectorWithoutResolverIsRefusedAsUnsupported` | §3's typed refusal. Asserts the **status is `kNotSupported`**, not merely that something threw | The live negative control for the test above: if the seat were wired to anything, this goes red. M6 return a default-constructed provider instead of refusing → red |
| `Registry.UnknownNameIsRefusedWithTheAvailableNames` | forbidden case 8 | Asserts `kInvalidArgument` **and** that the message names a registered provider — a bare `EXPECT_THROW` is one of this round's vacuous shapes and is not acceptable here |
| `Registry.DuplicateRegistrationIsRefused` / `Registry.EachCreateReturnsAnIndependentInstance` | forbidden cases 10 and 4 | M7 make `Register` overwrite → red; M8 memoize in `Create` → the second red (two selections yielding one pointer) |

Inner loop: `ctest ... -R 'Registry\.'` — the **whole** suite, not the forcing test alone,
because the negative control is what stops a dead instrument greening it (PDA-DEC-2's
convention). Forcing test alone:
`-R 'Registry\.SelectsByNameWithoutCallerKnowingTheProvider'`.
*Machine checks, so nothing is hand-composed:* the compiler and the registry binary's link
line (no provider SDK reachable); a `static_assert` that `Create` still returns exactly
`std::shared_ptr<PubSubProvider>`; `ctest -N` for the entry list.

## Risks / Unknowns

- **The defining risk — a signature PDA-ABI must widen.** Discharged three ways: the
  selector has no kind accessor (so nothing above can be widened *into*), `Create` is
  frozen and `static_assert`ed, and the path half is **proved in-round with a stand-in
  resolver** rather than promised in prose. If P2 is rejected, the claim reverts to prose
  and that is a stop-and-ask, not a redesign.
- **Public surface 5** — `ProviderSelector`, `ProviderConfig`, `ProviderRegistry`,
  `ProviderRegistry::Factory`, `ProviderRegistry::PathResolver` (the last two are nested
  aliases of `std::function` shapes, not new concepts). Nothing of this item's own is
  retired to make room: the retirements this registry causes (`FastDDSProviderOptions`,
  `XrceConfig`, the gateway's `if (args.provider == "fastdds")`) belong by plan to
  PDA-DEC-5/6/7 and absorbing them here would swallow three items. Waiver requested; not
  argued.
- **No product caller lands here.** The gateway's hardcoded selection lives one item
  longer — that is the plan's own split (PDA-DEC-5 is deliberately small), not a bridge
  this design builds, and nothing here is scheduled for deletion later. Merging
  PDA-DEC-4+5 is a PM call; the split keeps the registry reviewable on its own.
- **False-green traps.** (1) The harness resolves `fletcher-pubsub` **from the Conan
  cache**, so the new header is invisible to it until `conan create pubsub` runs — and
  `-o run_tests=True` is a no-op on a cached package, so *"Already installed!" is not a
  pass*. (2) The full-suite run must pass `-DFLETCHER_CONFORMANCE_XRCE=ON` explicitly; a
  cached `OFF` silently drops two subjects. (3) Neither gateway harness builds its own C++
  binary — this item touches no gateway code, so there is no exposure, but the config's
  fixed commands still apply.
- **Vacuity.** Four guards in this round asserted nothing. The rule for this item is
  written into the table above: no test may pass on "non-null" or on "it threw"; every one
  asserts a *delivered row under a tag* or a *specific status*, and each has a named
  mutation the implementer must run and report.
- **Assumed, unverified:** no out-of-tree consumer constructs providers in a way this
  registry would have to accommodate. *Record correction (DEBT-10a): §10 counts four files
  consuming the protocol-typed config, not four construction sites, and it predates
  PDA-DEC-1/2/3 — the conformance harness alone now adds ~8. Harmless for an add-only item;
  PDA-DEC-6/7 must re-measure rather than trust that table.*

## Files-to-touch

**New:** `pubsub/include/fletcher/pubsub/provider_registry.hpp` ·
`pubsub/src/provider_registry.cpp` ·
`integration-tests/pubsub-conformance/src/registry.cpp`.

**Changed:** `pubsub/CMakeLists.txt` (one source) ·
`integration-tests/pubsub-conformance/CMakeLists.txt` (the `conformance_registry` binary +
`gtest_discover_tests`, no `RESOURCE_LOCK` — no domain, no port, no child) ·
`integration-tests/pubsub-conformance/README.md` (what the `Registry` suite claims, and
that it claims nothing about any transport) · `docs/pubsub-interface-spec.md` §4/§4.1
(record the landed classification rule and the two refusal statuses; §4 clause 2 restated
as "`Create` is frozen; a resolver is installed, not added to it") ·
`.claude/runbook.PDA-DEC.config.md` (`inner_loop_cmd`'s `-R` scope becomes `Registry\.`; its
`conan create` loop already builds `pubsub`).

## Files-to-delete

**None — and that is deliberate, not an oversight.** This item is add-only in product
code: every construct it makes obsolete has a named owner one to three items later
(`FastDDSProviderOptions` → PDA-DEC-6, `XrceConfig` → PDA-DEC-7, the gateway's
`if`-chain and `--provider` validation → PDA-DEC-5), and deleting them here would absorb
items the plan deliberately keeps separate. No shim, no re-export, no deprecated alias and
no coexistence window is created by this item, so there is nothing awaiting a later
deletion either.

## Numbers

Declared net lines **+640 / −25**. New public surface **5** (waiver requested; nothing
retired here — see Risks). Design cycles used: 1/2.
