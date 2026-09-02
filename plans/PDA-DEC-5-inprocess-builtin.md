# PDA-DEC-5 — `InProcessProvider` registered as a built-in

Oracle: [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md) §4 (esp.
clause 4), §4.1, §4.2, §7 (§5.1 constraining). Decisions 3, 8, 14 (also 4).
Rulings 2026-09-01 (split; the bindings do not depend on the driver ABI; what
PDA-DEC does to the existing protocol code), 2026-09-02 (shape decides;
protocol-specific settings move into the document).
Predecessor contract: [PDA-DEC-4](PDA-DEC-4-provider-registry.md) and
`pubsub/include/fletcher/pubsub/provider_registry.hpp`.

## Summary

The loopback becomes selectable by the name `inprocess` through the registry
PDA-DEC-4 landed, and its one knob — which of §7 clause 1's two schema modes it
is in — stops being a constructor argument and becomes a line in that provider's
own document. The gateway stops constructing providers itself: `--provider`
becomes one `Create` call for *both* names, and its hand-rolled name validation
goes. No new mechanism is invented here; PDA-DEC-4 built all of it.

## Design

### 1. One registration function, one construction API

```cpp
// pubsub/include/fletcher/pubsub/in_process_provider.hpp
/// Make the loopback selectable as "inprocess". Idempotence is NOT offered:
/// a second call is refused by ProviderRegistry::Register (kInvalidArgument).
void RegisterInProcessProvider(ProviderRegistry& registry);

class InProcessPubSubProvider : public PubSubProvider {
   public:
    explicit InProcessPubSubProvider(const ProviderConfig& config = {});
    // ... the four PubSubProvider overrides, unchanged
};
```

`SchemaCarriage` **leaves the public header** and becomes a detail of
`in_process_provider.cpp`. After this item the class's entire public surface is
`PubSubProvider`'s method set plus construction/destruction: there is nothing a
holder of `shared_ptr<PubSubProvider>` gains by knowing it holds this type, which
is what §4's "indistinguishable to the caller" buys operationally. (RTTI still
*names* it — PDA-DEC-4's header already concedes and disposes of that; what
matters is that it confers no capability and no configuration route.)

The registered factory is the whole of the registration:
`registry.Register("inprocess", [](const ProviderConfig& c) { return std::make_shared<InProcessPubSubProvider>(c); })`.
`inprocess` is a name under `ProviderSelector::Parse`'s rule (`[A-Za-z0-9_-]+`),
so the string an operator already writes keeps meaning what it meant.

### 2. The document, which only this provider reads

Format is the provider's own (§4.2), and it is the same `key=value` idiom §4.2
assigns XRCE, so the tree has one shape rather than two:

- The document is a sequence of `\n`-separated entries. **Empty document → the
  defaults**, which are today's gateway behaviour.
- The only key is `schema_carriage`, whose values are `as_declared` (default,
  today's loopback) and `carried` (§7 schema-before-data).
- A trailing `\r` on an entry is stripped; nothing else is trimmed, no case
  folding, no comments. An empty entry (`\n\n`, a trailing newline) is skipped.
- **Anything else is refused** — unknown key, unknown value, an entry with no
  `=`, a duplicate key — with `PubSubError(kInvalidArgument)` quoting the
  offending entry. Refused in the constructor, so a misconfigured loopback never
  exists.

The typed core (`max_payload_bytes`, `domain_id`) is **ignored** by the loopback;
see handled residue H1.

Fletcher's seam still reads nothing: the registry copies and forwards the bytes,
and the only code that looks at them lives in the provider that owns them. No
library, no shared parser, no dependency (decision 8, §4.2) — the reader is ~25
lines of `std::string` slicing over one key. Flagged to the owner as Brief
decision 1 because this is the first document reader in-tree and it happens to
live inside `fletcher-pubsub`.

### 3. The gateway: one path, not two

`gateway/src/main.cpp` builds a local `ProviderRegistry`, registers **both**
built-ins **unconditionally and before the selector is looked at**, then makes
exactly one call:

```cpp
fletcher::ProviderRegistry registry;
fletcher::RegisterInProcessProvider(registry);
registry.Register("fastdds", [](const fletcher::ProviderConfig& c) {
    fletcher::FastDDSProviderOptions o;  o.domain_id = c.domain_id;
    return std::make_shared<fletcher::FastDDSPubSubProvider>(std::move(o));
});
auto provider = registry.Create(fletcher::ProviderSelector::Parse(args.provider),
                                fletcher::ProviderConfig{0, args.domain_id, ""});
```

Load-bearing details:

- Registration is **unconditional**. Making it `if (args.provider == "fastdds")`
  would put a selector branch back above the seam — the exact thing decision 3
  forbids. Registration states *availability* (a link-time fact); `Create`
  performs *selection* (a runtime string).
- The `if (args.provider == "fastdds") … else …` chain and the
  `a.provider != "inprocess" && a.provider != "fastdds"` validation are
  **deleted**. Two validations of one vocabulary is how they drift; the registry
  already refuses an unknown name listing what is registered.
- Refusal handling: `Create` is wrapped in one `catch (const PubSubError&)` that
  prints `fletcher-gateway: <what()>` to stderr and **returns 2** — the exit code
  today's validation uses, so scripts see no change. The *wording* changes, and a
  path-shaped `--provider` now reports "this build cannot load drivers" instead of
  "unknown provider" (Brief decision 2).
- The gateway does not construct `FastDDSPubSubProvider` unless `fastdds` is
  selected — the closure is not called until then, so an `inprocess` run costs
  exactly what it costs today.
- The `fastdds` factory body is **not** a coexistence bridge: there is no second
  path to it, and `FastDDSProviderOptions` is already owned for retirement by
  PDA-DEC-6, which rewrites these three lines to read the profile document.
  Nothing in this item is scheduled for deletion by a later stage except that
  closure's *body*.

### 4. What PDA-DEC-6/7/8 do that this item deliberately does not

Define the Fast DDS (XML QoS) and XRCE (`key=value`) documents; register those two
from **their own components**; retire `FastDDSProviderOptions` and `XrceConfig`;
decide whether a document is content or a filename for those providers; and prove
multi-instance isolation (PDA-DEC-8). This item defines exactly one document — the
loopback's — and touches no provider outside `pubsub/`.

## Corner cases forbidden

**Rung 1 — unrepresentable**

1. *Choosing the loopback's schema mode by any route other than the document.*
   The enum is not in a public header and the only constructor takes
   `ProviderConfig`. This is the ruling PDA-DEC-4 wrote forward ("a document key,
   NOT a second construction API") made structural rather than advisory.
2. *A misconfigured loopback instance.* The document is validated in the
   constructor; an object with an undecided mode never exists.
3. *The gateway branching on which provider it selected.* There is one `Create`
   call and one `shared_ptr<PubSubProvider>` variable; the name never re-appears
   after `Parse`. Machine check: the compiler — nothing after that line names a
   concrete provider type.
4. *The gateway's provider vocabulary drifting from the registry's.* The gateway
   holds no list of valid names; the registry's table is the only one.
5. *`inprocess` being registered but not selectable.* `Register` validates against
   `Parse`'s predicate (PDA-DEC-4), so this is already unreachable.

**Rung 2 — refused typed at the door**

6. Unknown document key / unknown value / entry with no `=` / duplicate key →
   `kInvalidArgument`, quoting the entry. Silently defaulting a typo'd
   `schema_carriage=Carried` to `as_declared` would ship a client decoding one
   stream two ways — the failure §7 clause 1 exists to prevent.
7. Registering `inprocess` twice → `kInvalidArgument` (PDA-DEC-4's `Register`; no
   new code).
8. `--provider` naming nothing registered → `kInvalidArgument`; a path-shaped
   `--provider` → `kNotSupported`. Both exit 2.

**Handled residue** — each with why it could not be forbidden

- **H1 — the loopback ignores `max_payload_bytes` and `domain_id`.** *Why not
  forbidden:* the typed core is universal by §4.1 and the gateway documents
  `--domain-id` as "ignored by the inprocess provider" today; refusing a non-zero
  value would be an observable gateway change, and this item's other half is
  "observably unchanged". Disclosed in the Brief's risk list, not asked as a
  decision: the recommendation is the status quo.
- **H2 — a trailing `\r` is stripped.** *Why not forbidden:* a document written on
  this project's primary platform is CRLF; refusing it would make the same
  document valid on Linux and invalid on Windows, and the 2026-09-02 ruling
  requires "the same rule in every build".

## Premises and stop conditions

- **P1 — a `PubSubError` thrown by a factory reaches the caller with its status
  intact** (`TranslateSeamFailure` in `provider_registry.cpp` passes typed
  failures through). **STOP-AND-ASK** if it is re-wrapped as `kInternal`: the
  bad-document refusal must arrive as `kInvalidArgument`, and dressing a
  configuration error as an internal one is a new handled state, not a message
  nit. Proved by `Registry.InProcessRefusesAnUnrecognisedDocumentEntry`.
- **P2 — `Register`/`Create`/`Parse` are sufficient as frozen.** This item adds
  no registry method and needs no signature change. **STOP-AND-ASK** if
  registering a built-in turns out to need one (spec §4 clause 2 says the seam was
  underspecified; the `static_assert` will fire first).
- **P3 — `InProcessPubSubProvider` lives inside `fletcher-pubsub`, so registering
  it there adds no component dependency.** True today. **STOP-AND-ASK** if review
  rules that a provider inside `fletcher-pubsub` may not read its own document
  (decision 8) — the answer is then to move the provider to its own component, and
  **not** to reinstate a typed constructor argument.
- **P4 — no gateway client or script parses the unknown-provider stderr text.**
  Neither TS harness asserts on it today. **STOP-AND-ASK** rather than keeping a
  second name validation alive if one turns up.

## Forcing-test mapping

### A. `Registry.InProcessResolvesAsABuiltIn` — `integration-tests/pubsub-conformance/src/registry.cpp`

Uses PDA-DEC-4's existing `MakeProvider(registry, string, config)` helper, whose
body names no concrete provider type and contains exactly one `Create`.

| Test | Green by | Red for the right reason / mutation |
|---|---|---|
| **`Registry.InProcessResolvesAsABuiltIn`** (forcing) | §1's registration. Subscribe to a topic, publish a known row through the *base-typed* handle, assert **the row arrives byte-identical** on the subscriber, and that publishing to an **undeclared** topic succeeds (pins the default mode to `as_declared` without an accessor) | Does not compile before the change (no `RegisterInProcessProvider`). **Not greenable by a non-null check.** M1: register under `"loopback"` → red (unknown name). M2: factory returns the `carried` mode → red (publish to undeclared throws `kTopicNotDeclared`). M3: drop the delivery in `Publish` → red |
| `Registry.InProcessCarriageComesFromTheDocument` | §2. Same helper, `document = "schema_carriage=carried"`. Asserts publish-to-undeclared is refused `kTopicNotDeclared`, and that after `CreateTopic` with a schema every delivery carries a non-null schema | The live control for M2 above: if the document were ignored, this goes red while the forcing test stays green — neither alone is sufficient. M4: parse the key but hard-code `as_declared` → red |
| `Registry.InProcessRefusesAnUnrecognisedDocumentEntry` | Rung-2 case 6. Asserts status is **`kInvalidArgument`** (not merely "threw") and the message quotes the offending entry, for `schema_carriage=Carried`, `nonsense=1`, and `schema_carriage` | M5: ignore unknown entries → red. Also the machine check for P1 |

Also converted, not added: `subjects/inprocess_carrying_main.cpp` builds its
subject with `ProviderConfig{.document = "schema_carriage=carried"}`, so the
**whole §7 conformance battery** runs against a document-configured instance. A
mis-parsed key reddens that battery, not one assertion.

Inner loop: `ctest -R 'Registry\.'` (the whole suite — PDA-DEC-4's negative
controls are what stop a dead instrument greening this), then the full harness
with `-DFLETCHER_CONFORMANCE_XRCE=ON`. Forcing test alone:
`-R 'Registry\.InProcessResolvesAsABuiltIn'`.

### B. The gateway half — proved, not asserted

"The gateway still works" is unfalsifiable on its own, and worse here: `npm test`
runs whatever `gateway.exe` is already in `build/`, so a stale binary passes the
whole existing battery. The proof therefore has two parts, and the second one is
a **staleness detector**:

1. **Unchanged behaviour** — `integration-tests/gateway-end-to-end` already runs
   its entire WebSocket battery once per provider via `describe.each`, including
   `--provider inprocess`. It is not modified. **Its load-bearing mutation is:
   drop the `inprocess` registration** → the gateway exits 2 at startup and every
   in-process context goes red. *(An earlier draft also claimed that registering
   `inprocess` in the `carried` mode would redden the schema/publish cases. The
   design review disproved it — every publish is preceded by `createTopic` with a
   schema, and the undeclared-subscribe case passes on a pending arrival. The
   carriage-mode proof belongs to the `Registry` suite, not this battery.)*
2. **A new `provider selection` block** (outside the per-provider contexts, three
   cases, ~45 lines): spawn with `--provider bogus` → exit code 2 **and** stderr
   naming the registered providers in the registry's wording; spawn with
   `--provider ./nope.so` → exit 2 **and** stderr saying this build cannot load
   drivers; spawn with `--provider inprocess` → `READY` on stdout. The old binary
   prints `unknown provider: … (expected inprocess|fastdds)` for both refusals, so
   **these two cases cannot pass against any pre-change `gateway.exe`.** The
   implementer must run them before the C++ change and report them red, then
   rebuild and report them green — that run is what converts "unchanged" from a
   claim into a measurement.

`integration-tests/gateway-fastdds-ts` is the regression check for the other
name, and must stay green unmodified (it is also the only harness that reproduces
the data-sharing defect — do not weaken it).

## Risks / Unknowns

- **False greens, in order of likelihood.** (1) `npm test` against a stale
  `gateway.exe` — the whole point of B.2; the gateway binary must be rebuilt and
  the two refusal cases must be seen red first. (2) `-o run_tests=True` is a no-op
  on a cached Conan package — "Already installed!" is not a pass; `pubsub` must be
  re-created for the harness to see the changed header. (3)
  `cmake --preset conan-default` does not reset a cached
  `FLETCHER_CONFORMANCE_XRCE` — pass it explicitly on the full run.
- **Public surface.** +1 (`RegisterInProcessProvider`), paid for by retiring 2:
  the public `SchemaCarriage` enum and the `SchemaCarriage` constructor. Net −1.
- **Coexistence windows: none created.** The `fastdds` closure's *body* is
  rewritten by PDA-DEC-6, but it is not a second path and nothing is scheduled for
  deletion.
- **Spec touch.** §7's parenthetical reference to
  `InProcessPubSubProvider::SchemaCarriage` becomes a reference to the document
  key, and §4 clause 4 records `inprocess` as the landed name. Editorial: §7's
  normative content (never mix, per subscription) is unchanged. If a reviewer
  reads that as amending the oracle, it is a stop-and-ask, not an edit.
- **Assumed, unverified:** no out-of-tree caller constructs
  `InProcessPubSubProvider` with an explicit `SchemaCarriage`. In-tree there is
  exactly one (`inprocess_carrying_main.cpp`), converted here; the pilot-phase
  no-back-compat rule covers the rest.

## Files-to-touch

**Changed:** `pubsub/include/fletcher/pubsub/in_process_provider.hpp` (the
registration function; constructor takes `ProviderConfig`; enum out) ·
`pubsub/src/in_process_provider.cpp` (the enum, the document reader, the factory) ·
`gateway/src/main.cpp` (registry + one `Create`; the `if`-chain and the validation
out; `--help`/comment text) ·
`integration-tests/pubsub-conformance/src/registry.cpp` (three tests) ·
`integration-tests/pubsub-conformance/subjects/inprocess_carrying_main.cpp` ·
`integration-tests/gateway-end-to-end/test/end-to-end.test.ts` (the selection
block) · `integration-tests/pubsub-conformance/README.md` ·
`docs/pubsub-interface-spec.md` (§4 clause 4, §4.1's document example, §7's
reference).

**New:** none — the loopback and the registry both already have homes.

## Files-to-delete

No whole file is retired; three constructs are, and each has a replacement:

- `InProcessPubSubProvider::SchemaCarriage` as **public** surface, and the
  `explicit InProcessPubSubProvider(SchemaCarriage)` constructor → replaced by the
  `schema_carriage` document key.
- The gateway's `if (args.provider == "fastdds") … else …` construction chain →
  replaced by `ProviderRegistry::Create`.
- The gateway's `a.provider != "inprocess" && a.provider != "fastdds"` validation
  and its message → replaced by the registry's two typed refusals. *Behaviour
  narrowed, disclosed:* the message text changes and a path-shaped value now gets
  a different message; the exit code does not.

No test is deleted: the carrying subject is converted, not retired.

## Numbers

Declared net lines **+270 / −60**. New public surface **1**, retiring **2** (net
**−1**). Design cycles used: 1/2.
