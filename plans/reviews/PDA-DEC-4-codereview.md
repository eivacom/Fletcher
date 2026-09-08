# PDA-DEC-4 (provider registry) — independent code review

Diff base `94ae4bc..aea67f0`, 9 files, +1069/−20, branch `feature/protocol-driver-abi`.
Reviewer built and ran the suite; all empirical claims below were produced by
`conan create pubsub` + rebuild + `ctest -R 'Registry\.'`, not by reading.

**Counts: 0 blocking · 3 should-fix · 6 nits.**

## Verdict on the production code

I found no correctness defect in `provider_registry.{hpp,cpp}`.

`ProviderSelector::Parse` is genuinely pure, total and disjoint. `IsNameChar` uses
explicit ranges rather than `<cctype>`, so it is locale-free and — checked — gives the
same answer whether `char` is signed or unsigned (`0xc3` fails every range on both).
`Parse` takes `std::string` by value and stores it, so there is no `string_view`
lifetime to outlive; the NUL check is `text.find('\0')` on the `std::string`, i.e.
length-authoritative, so `"fastdds\0/../evil.so"` cannot be smuggled past by a
C-string-length reading. The empty string is refused before the NUL scan, the
`is_name_` flag is set from the same predicate `Register` validates against, and the
path branch's `FirstNonNameChar(selector.text_)` cannot return `npos` because `Parse`
is the only constructor and only sets `is_name_ = false` when a non-name char exists.
`kNotSupported` (path, no resolver) and `kInvalidArgument` (unknown name) are distinct
on every path, `kInternal` is used only where no named status exists (null factory /
null resolver return), and every caller-supplied callable is wrapped in
`TranslateSeamFailure`, so `std::overflow_error` still lands as `kPayloadTooLarge`
rather than degrading to `kInternal`.

Threading is stated, not assumed: the header says `Create` is `const` and concurrent,
`Register`/`SetPathResolver` are not, and the code matches — `Create` only reads
`factories_` and `path_resolver_`, and a `std::function` is never invoked after being
replaced because replacement is refused outright. `fletcher-pubsub` gained one TU and
no link dependency; `conformance_registry` links `fletcher-pubsub` + gtest_main and
nothing else. Nothing accidental rode along in `provider.hpp`.

**M2 and M6 verified by build, as asked:**

- **M2** (`const auto entry = factories_.begin();`): 4 tests red, including the forcing
  test — and it fails **only at the second direction**, confirmed from the output
  (`journal->rows[1].tag` "alpha" vs "beta"; the `rows[0]` assertions passed). A
  one-direction forcing test would indeed have been vacuous.
- **M6** (`if (!path_resolver_) { if (!factories_.empty()) return factories_.begin()->second(config); }`):
  exactly **one** test red — `PathSelectorWithoutResolverIsRefusedAsUnsupported` — while
  `PathSelectorResolvesThroughTheSameCall` stayed green. The negative control is
  independent, not a restatement.

Fresh-instance and duplicate semantics hold up. `EachCreateReturnsAnIndependentInstance`
asserts `first.get() != second.get()` **and** `live_probes == 2`, so it is two distinct
objects, not two handles on one. `DuplicateRegistrationIsRefused` re-creates after the
refusal and asserts the tag is still `alpha`, not `impostor` — it cannot pass on "it
threw". Duplicate refusal cannot be bypassed by whitespace (a space is not a name char,
so `"alpha "` is refused at `Register` as a path-shaped name); `"Alpha"` is a different
name by the documented case-sensitive rule, not a bypass.

---

## Findings

### S1 — should-fix (high confidence, build-verified). The path branch's config forwarding is asserted by nothing, and the failure is silent.

Mutation `path_resolver_(selector.text_, ProviderConfig{})` — the resolver receives a
default-constructed config instead of the caller's — leaves the suite at **12/12 green**.
Verified by building.

Every path-branch test constructs `const ProviderConfig config;` (default), so
`domain_id == 0` and `max_payload_bytes == 0` are indistinguishable from "the config
never arrived". `ConfigurationReachesTheProviderAndIsNeverRead` covers only the *name*
branch. The consequence of the mutation is exactly the class this round calls a wrong
answer rather than a failure: a loaded driver joins **domain 0** with no payload bound
and no error anywhere.

This matters more than an ordinary coverage hole because the path branch is the seat
PDA-ABI fills **blind**, against this header. The guard that would catch a resolver
call mis-wired at that hand-off does not exist. `PathSelectorResolvesThroughTheSameCall`'s
own comment claims the path reaches the resolver "with the identical config — only the
string differs", and that clause is unasserted.

Fix is two lines inside the existing test: give the path selection a non-default config
(`max_payload_bytes`/`domain_id`/a document with an embedded NUL) and assert
`journal->last_config` after the path `Create`, the way the name branch already does.

### S2 — should-fix (high confidence, build-verified). Two thirds of the name alphabet is unasserted.

Both of these leave the suite at **12/12 green**, verified by building:

- deleting `(c >= '0' && c <= '9')` from `IsNameChar`
- deleting `(c >= 'A' && c <= 'Z')` from `IsNameChar`

No test registers or selects a name containing a digit or an uppercase letter. The names
in the file are `alpha`, `beta`, `gamma`, `in-process`, `xrce_dds`, `fastdds`,
`null-returner`, `thrower`, `typed-thrower`, `overflower` — all lowercase ASCII letters,
`-` and `_`. `fastdds.v2` contains a digit but is asserted to be *refused*, and it is
still refused for its `.` under both mutations, so it does not cover the digit range.

`[A-Za-z0-9_-]` is normative in the header and in the spec, and PDA-ABI reads it as the
contract. Under either mutation a build silently narrows the vocabulary and `FastDDS` or
`zenoh2` becomes a *path* refused with `kNotSupported` ("this build cannot load
drivers"). That refusal is loud and located, which is why this is should-fix rather than
blocking — but the header's own claim that "the two vocabularies cannot drift" is only as
strong as the alphabet the tests pin, and today they pin `[a-z_-]`.

Fix: one added registration+selection using a name like `Fast2DDS-v1_x` in
`RegistrationAndSelectionShareOneVocabulary`.

### S3 — should-fix (medium confidence). The DEBT-1 resolver lifetime rule is prose the code could enforce, in the forbidding direction.

The header spends ~15 lines making a normative rule the object does not hold anyone to:
a resolver "must keep everything the provider it returns depends on … alive for at least
as long as that provider, independently of this registry's lifetime and of the
resolver's own", with the failure mode named as "a use-after-unload and not reliably
loud". The code documents a hope. A resolver author who parks a module handle in
registry-owned state gets exactly the described crash, silently, and nothing in this seam
notices.

The registry can make that state unrepresentable for a few lines. Hold the resolver as
`std::shared_ptr<PathResolver>` and have `Create` return an aliasing `shared_ptr` that
owns a copy of that handle alongside the provider:

```cpp
auto keepalive = path_resolver_;                       // shared_ptr<PathResolver>
auto provider  = TranslateSeamFailure([&]{ return (*keepalive)(selector.text_, config); });
if (!provider) Refuse(...);
return std::shared_ptr<PubSubProvider>(
    std::shared_ptr<void>(std::move(keepalive)), provider.get());  // aliasing
```

The resolver — and therefore whatever its closure captured, including a module handle —
now outlives every provider it made, mechanically, whatever the resolver author did. The
hazard class the header describes stops being reachable, and most of the DEBT-1 paragraph
becomes a one-line note. Cost is one indirection on a path that already does a `dlopen`.
(Note the aliasing constructor keeps the *provider's* own control block alive too, so the
provider's deleter still runs first — the ordering is right.)

This is not a defect today, since no resolver exists; it is a design-economy call for the
owner, and the reason to make it now is that PDA-ABI is the component that would pay for
getting it wrong and it implements against this header without seeing this code.

---

## Volume

I counted. **The implementer's account holds** — unlike PDA-DEC-2.

| file | total | comment | blank | code |
|---|---|---|---|---|
| `provider_registry.hpp` | 251 | 182 | 21 | **48** |
| `provider_registry.cpp` | 176 | 25 | 13 | 138 |
| `registry.cpp` (tests) | 503 | 91 | 67 | 345 |

Delta over declared is 429 lines. Header prose is 182 of it, the README section is 47,
and 6 tests beyond the designed 6 are roughly 200 — the account covers the overrun with
room to spare, and there is no third, unexplained bucket. The 251-line header is 48 lines
of declarations: it is contract, not padding, and this item genuinely owes it because
PDA-ABI implements against it and nothing else.

Where volume is not carrying weight is the test file, mildly: **unknown-name →
`kInvalidArgument` is asserted three times** (in `UnknownNameIsRefusedWithTheAvailableNames`,
in `PathSelectorWithoutResolverIsRefusedAsUnsupported`, and in
`SelectorShapeDecidesAndIsRefusedWhenItCannotMeanAnything`) while the resolver's config
and two thirds of the name alphabet are asserted zero times. That is the proportionality
comment: the extra six tests restated a guard that existed rather than adding the two
that were missing. Not a reason to cut anything — a reason to spend S1 and S2.

## Nits (one line each)

1. `Quoted` escapes control bytes but not `\` or `"`, so `"a\x41b"` in a diagnostic is ambiguous between a literal backslash-x-4-1 and the escape of byte 0x41 — and every Windows path prints with raw backslashes.
2. Nothing asserts the hex byte in either refusal message; the CRLF test checks `"path"` and `"offset 7"` only, so `kHex[byte >> 4]`/`kHex[byte & 0x0f]` transposed would go unnoticed.
3. The offset in the path refusal goes through `std::ostringstream`, imbued with the global locale — a grouping `numpunct` would print `offset 1,234`; `Parse`'s `std::to_string` is not affected, so the two refusals can disagree in format.
4. No upper bound on selector length: a multi-megabyte selector is copied and then re-emitted into an error message at up to 4x expansion.
5. "`Create` is `const` and safe to call concurrently" is true of the registry but invokes caller-supplied factories concurrently too; the header does not say that obligation passes to the factory author.
6. A non-ASCII name (`cafe` with an accented e) is a second documented-rule misclassification beyond the `myDriver` case the header names — refused as `kNotSupported` "cannot load drivers" rather than "no such provider"; loud and located, so cosmetic only.

---

# Re-check — fix cycle 1 (`7f2dc5b..9bf99c4`), 2026-09-02

Focused re-check of my three should-fix findings plus the ~268 new lines. Done in an
isolated worktree (`git worktree add /c/tmp/pdadec4-recheck 9bf99c4`, removed after), and
— because a mutated `conan create pubsub` would poison the *shared Conan cache* for any
agent running in parallel, which is the same hazard one level down from the shared
checkout — the suite was built standalone from worktree sources against a read-only
`gtest/1.17.0`. `core` is header-only, so that build needs no Fletcher package at all and
no mutation of mine was ever published. Baseline in that build: **14/14 green**.

**Re-check counts: 0 blocking · 1 should-fix · 3 nits. S1, S2, S3 all closed.**

## First: my S3 snippet was wrong. I accept the correction without reservation.

The implementer is right and I was wrong. I checked it rather than concede it, and the
verdict is not close — the form I put in this review is a **heap-use-after-free on every
call**, confirmed by AddressSanitizer under MSVC:

```
SUMMARY: AddressSanitizer: heap-use-after-free in Prov::use() const
    #2 reviewer_form(std::shared_ptr<Seat>) main.cpp:13
```

The reason is a plain misreading of the aliasing constructor on my part. A `shared_ptr`
owns **exactly one** control block. `shared_ptr<T>(shared_ptr<Y>&& r, T* ptr)` shares
ownership with `r` **only** and merely stores `ptr`; it does not, and cannot, also adopt
the control block that `ptr` came from. So in my snippet the local `provider` was the sole
owner of the provider, it died at the end of `Create`, and the handle returned to the
caller pointed into freed memory while diligently keeping the resolver alive. My
parenthetical — *"the aliasing constructor keeps the provider's own control block alive
too, so the provider's deleter still runs first"* — is simply false, and it was the
load-bearing sentence of the suggestion. Had it been taken as written it would have been
far worse than the prose it replaced: prose that is ignored costs nothing, whereas that
snippet turns **every** `Create` into a dangling handle. Good catch, and the right call to
verify it rather than implement it.

**The landed `Anchor` form is correct.** Verified, same ASan build:

```
== anchor_form (landed) ==
 after return:
  Prov::use alive=YES
 releasing:
  ~Prov
  ~Seat (module unloaded)
```

- **Destruction order is genuinely guaranteed**, not incidental: `~Anchor` destroys
  non-static data members in reverse declaration order ([class.dtor]), `provider` is
  declared second, so it is released first and `seat` after. The provider's destructor
  therefore runs with its module still loaded. Confirmed by the trace above.
- **No dangling:** the returned handle owns the `Anchor`, and the `Anchor` owns a strong
  reference to the provider, so the pointed-to object is kept alive by the very control
  block the handle holds. `raw` is read into a local *before* `provider` is moved, so
  there is no evaluation-order trap.
- **No leak, no cycle:** nothing reachable from the provider or the seat points back at
  the `Anchor`. The registry holds `shared_ptr<Factory>`; the `Anchor` holds another; both
  are ordinary strong edges in a DAG.
- **One allocation per `Create`,** as claimed: `make_shared<Anchor>` is the single
  allocation, and the C++20 rvalue aliasing constructor moves rather than bumping a
  refcount. (`make_shared<Anchor>(Anchor{...})` materialises a temporary and
  move-constructs; that is a copy of two `shared_ptr`s, not an allocation. Immaterial.)

## S1 — CLOSED (re-derived, not taken on report)

Mutating to `(*resolver)(selector.text_, ProviderConfig{})` now reddens
`PathSelectorResolvesThroughTheSameCall`, exactly as reported:

```
journal->last_config.max_payload_bytes  Which is: 0   vs 65000u
journal->last_config.domain_id          Which is: 0   vs 151u
the resolver was handed domain 0, not the caller's — a loaded driver would join the wrong domain with no error
```

The document assertion fires too. Worth noting the detail that makes this non-vacuous:
the test resets `journal->last_config = ProviderConfig{}` between the name `Create` and
the path `Create`. Without that reset the name branch would have left `65000` in the
journal and all three new assertions would have passed no matter what the resolver
received. That reset is the whole guard; it was not obvious and it is right.

## S2 — CLOSED, and it exercises digits *and* capitals, as asked

`Fast2DDS-v1_x` spans capitals (`F`, `DDS`), digits (`2`, `1`), `-` and `_`. Both
mutations re-derived independently:

- drop `(c >= '0' && c <= '9')` → red: `character at offset 4 is outside [A-Za-z0-9_-]`
  (the `2`)
- drop `(c >= 'A' && c <= 'Z')` → red: `character at offset 0 is outside [A-Za-z0-9_-]`
  (the `F`)

Two different offsets from two different mutations is proof the single name covers both
ranges rather than one masking the other.

## S3 — CLOSED, taken mechanically and in a better form than I proposed

The rule is now enforced by the object instead of asked of the author, and the fix went
further than my finding did in one respect I had missed: `Register("zenoh",
factory_that_dlopens)` reaches a loaded module by **name**, so the factory seat needed the
same treatment as the resolver seat. Both got it. Both routes are covered by
`AModuleHeldOnlyByTheSeamOutlivesTheProvidersItMade`, and the test is real in both halves
— removing `KeepSeatAlive` from the name branch reddens it, and removing it from the path
branch reddens it. `AResolverThatFailsIsReportedAsATypedSeamFailure` is real too: deleting
the `TranslateSeamFailure` around the resolver call reddens it and nothing else.

---

## New finding

### R1 — should-fix (high confidence, build-verified). The `Anchor` member order is called load-bearing and nothing pins it.

Swapping the two `Anchor` members (and the corresponding aggregate initialiser) leaves the
suite at **14/14 green**. Verified by building.

```cpp
struct Anchor {
    std::shared_ptr<PubSubProvider> provider;   // swapped
    std::shared_ptr<Seat> seat;
};
```

The code comment says the order "is the load-bearing part … `provider` (declared last) is
released FIRST and the seat only afterwards. The provider's destructor therefore still
runs with its module loaded." Under the swap, the seat is destroyed first and the
provider's destructor runs **after** its module is gone — the module's code and vtable are
unmapped while `~Provider` executes in it. That is precisely the "use-after-unload, and
not reliably loud" failure this entire fix exists to prevent, and it survives every one of
the 14 entries.

This is the same defect class as S1 and S2 in cycle 1: the mechanism is correct, but the
one property that makes it correct is unasserted, so a later refactor that reorders two
struct members reintroduces the original hazard silently. It survives specifically because
`ModuleStandIn` is only ever observed from the *test body*, after both are gone, where the
order is invisible.

Fix, in the existing test: make the ordering observable from inside the provider's
destructor rather than after it.

```cpp
struct ModuleStandIn {
    explicit ModuleStandIn(bool* unloaded) : unloaded_(unloaded) {}
    ~ModuleStandIn() { *unloaded_ = true; }
    bool* unloaded_;
};

// A probe that reads its module's state AT ITS OWN DESTRUCTION.
class ModuleUserProvider : public ProbeProvider {
   public:
    ModuleUserProvider(std::string tag, std::shared_ptr<Journal> journal,
                       const bool* unloaded, bool* module_was_live_at_my_death)
        : ProbeProvider(std::move(tag), std::move(journal)),
          unloaded_(unloaded), out_(module_was_live_at_my_death) {}
    ~ModuleUserProvider() override { *out_ = !*unloaded_; }
   private:
    const bool* unloaded_;
    bool* out_;
};
```

then, after `provider.reset()`, `EXPECT_TRUE(module_was_live_at_my_death) << "the
provider's destructor ran after its module was unloaded — the Anchor's member order is
reversed"`. That reddens under the swap and stays green as landed, for both the path and
the name route.

## Nits (one line each)

1. `make_shared<Anchor>` is the first allocation on `Create`'s **success** path outside any
   `TranslateSeamFailure`, so OOM now escapes `Create` as a raw `std::bad_alloc` while the
   header still promises in bold that "nothing else escapes" — no leak and loud, but it is
   the one contract PDA-ABI's C boundary will be written against; wrapping the
   `KeepSeatAlive` call is one line.
2. A pathological factory that retains a `shared_ptr` to its own product defeats the
   ordering (the anchor's release is not the last one), but that already violates the
   fresh-instance contract `EachCreateReturnsAnIndependentInstance` pins, so it is
   unreachable by a conforming factory.
3. No `enable_shared_from_this` exists anywhere in `core` or `pubsub` today, so the
   header's `shared_from_this` caveat is correctly forward-looking rather than a live hole.

## Recommendation on the two nits the PM asked about

- **Nit 1 (`Quoted` does not escape `\` or `"`) — worth fixing, but folded into the next
  touch, not launched for on its own.** It is sharper than I first wrote it: on this
  platform the collision is concrete, not theoretical. `C:\x64\driver.dll` is an ordinary
  Windows driver path, and `\x64` is *exactly* this function's escape spelling for byte
  0x64. So the one message PDA-ABI's users read when a driver fails to load can render a
  real path ambiguously with a real escape. The fix is two lines in `Quoted` (emit `\\`
  for a backslash and `\"` for a quote) plus one assertion. Since R1 already warrants a
  cycle, this rides along free — I would not spend a launch on it alone.
- **Nit 3 (locale-imbued `ostringstream` offset) — genuinely fine to log and leave.** It
  needs two things to bite at once: someone calling `std::locale::global()` with a
  grouping locale, *and* an offset past 999, i.e. a selector where the first non-name
  character sits beyond position 999. Driver paths are short, so the second condition is
  effectively unreachable in the diagnostic that matters, and the consequence is a
  cosmetically grouped number rather than a wrong or missing fact. Log it; do not spend on
  it.

## Requested confirmations

- All 12 prior entries green and **unchanged in meaning**: the only edits to existing tests
  *added* assertions (the config triple in `PathSelectorResolvesThroughTheSameCall`, the
  `Fast2DDS-v1_x` name in `RegistrationAndSelectionShareOneVocabulary`) plus the
  `journal->last_config` reset. Nothing was weakened or deleted; the single `−1` line is a
  `const ProviderConfig config;` becoming mutable.
- `Registry.` is **14/14**.
- The item's forcing test is `Registry.SelectsByNameWithoutCallerKnowingTheProvider`, at
  `integration-tests/pubsub-conformance/src/registry.cpp:170`. The new lifetime entry is
  not it.
- **No new link dependency** in `fletcher-pubsub`: the only include added to the TU is
  `<memory>`, and neither CMakeLists is touched in this diff.
- **Public surface still 5** — `Factory`, `PathResolver`, `Register`, `SetPathResolver`,
  `Create` — plus the defaulted ctor and two deleted copy operations, all unchanged. The
  frozen-signature `static_assert` is untouched.
- Incidental improvement worth recording: `Create` now copies the `shared_ptr<Factory>`
  out of the map *before* invoking it, so it no longer dereferences a map iterator across
  caller-supplied code. `std::map` iterators were already stable and `Register` is
  documented non-concurrent, so this fixed nothing — but it is strictly tighter. Neither
  seat has a self-assignment path: `Register` refuses a duplicate before touching the map,
  and `SetPathResolver` assigns only when the slot is empty.

---

# Re-check — fix cycle 2 (`24c7f85..6ba466d`), 2026-09-02

Tightly scoped to R1 and the folded nit 1. Isolated worktree
(`git worktree add /c/tmp/pda4-cr2-reviewer 6ba466d`, removed after); suite built standalone
from worktree sources against a read-only `gtest/1.17.0`, so nothing of mine reached the
shared Conan cache. Baseline: **14/14 green**.

**Counts: 0 blocking · 0 should-fix · 0 nits. R1 closed. Nit 1 closed. One `RECORD:` line — and it is my error, not the implementer's.**

## R1 — CLOSED, re-derived by building

Swapping the two `Anchor` members reddens `AModuleHeldOnlyByTheSeamOutlivesTheProvidersItMade`
at `registry.cpp:672` and `:706`, both routes, **13/1**, nothing else moving — exactly as
reported.

```
registry.cpp(672): Value of: module_was_loaded_at_my_death   Actual: false
registry.cpp(706): Value of: module_was_loaded_at_my_death   Actual: false
```

### The new guard is not vacuous. I checked the three modes named, and a fourth.

- **Destructor never runs → fails, not passes.** The flag is initialised `false` and only
  the destructor ever writes it, so the polarity is fail-safe. Confirmed by mutation:
  emptying `~ModuleUserProvider`'s body reddens the test. Had it been initialised `true`,
  this guard would have been the fourth vacuous one in this item.
- **Read before write is impossible.** The write happens synchronously inside
  `provider.reset()` — that call drops the last handle, destroys the `Anchor`, destroys the
  anchor's `provider` member, and runs `~ModuleUserProvider` before returning. Both
  assertions follow it.
- **Reordering cannot put them out of reach.** Both are `EXPECT_TRUE`, not `ASSERT`, so
  neither can short-circuit the other, and both sit after the same `reset()`. Order is
  immaterial.
- **The fourth, and the one I expected to find a problem in: does the new assertion make
  the old `EXPECT_TRUE(unloaded)` redundant?** No — they are orthogonal, and I proved it
  both ways:

  | mutation | `unloaded` (670/705) | `module_was_loaded_at_my_death` (672/706) |
  |---|---|---|
  | `Anchor` members swapped | green | **red** |
  | seat leaked, never released | **red** | green |

  Each catches precisely what the other misses, so the pair — and only the pair — pins
  "the module was still loaded when the provider died, *and* it was unloaded afterwards".
  Dropping either would reopen a hole. That is a better-constructed guard than the one I
  proposed.

### Fresh eyes on the probe itself

It does work in a destructor, which is where this item's mechanism lives, so: the two raw
pointers target test-body locals declared in the *enclosing* scope of the `reset()` that
triggers the write, so neither can dangle; `~ModuleUserProvider` performs two dereferences
and a `bool` store and the base performs a decrement plus trivial member destruction, none
of which allocates or throws, and destructors are implicitly `noexcept`, so nothing here
can throw during unwinding; the `override` keeps the virtual destructor chain intact, so
deletion through `shared_ptr<PubSubProvider>` is correct. Nothing to report.

## Nit 1 — CLOSED. The fix is correct and complete. My rationale for it was not.

**The guard is real.** Reverting the escaping reddens
`PathSelectorWithoutResolverIsRefusedAsUnsupported` alone, and *both* halves fire — the
positive at `:318` and the `EXPECT_FALSE` at `:320`. The negative is the load-bearing one
(it is what catches "escaped somewhere but also reproduced raw") and it is not satisfiable
by an unescaped string: the needle is the literal text `"C:\\x64\\driver.dll"`, which an
unescaped renderer cannot produce.

**The escaping is complete and now injective.** I brute-forced it rather than reasoned:
encode every string of length ≤ 4 over an alphabet containing every character that
participates in an escape (`\`, `"`, `x`, hex digits, LF, 0xff) and look for two distinct
inputs sharing one rendering.

| encoder | colliding pairs | distinct renderings |
|---|---|---|
| old (pre-fix) | **2** | 16102 |
| new (as landed) | **0** | 16104 |

The old collisions are `"\x0a"` and `"\xff"` — each produced both by a literal
four-character `\x0a` and by the single byte 0x0a. So the defect was real and the fix
removes it entirely: every byte is covered (0x00–0x1f, 0x7f, 0x80–0xff as hex; 0x20–0x7e
raw except the two escaped), the output is prefix-free, and a decoder recovers the input
uniquely.

**But the example I gave for it is wrong, and it has been copied into the tree.** I claimed
`C:\x64\driver.dll` collided with `C:` + byte 0x64 + `\driver.dll`. It does not, and never
did: 0x64 is `d`, a *printable* byte, so the old encoder rendered it raw as `d` and never as
`\x64`. Measured:

```
A = C:\x64\driver.dll          OLD -> "C:\x64\driver.dll"
B = C: + byte 0x64 + \driver.dll   OLD -> "C:d\driver.dll"      identical: no
```

The real ambiguity requires `NN` to denote a **non-printable** byte, so the colliding paths
are those containing a literal `\x0a`, `\x1b`, `\x7f` or `\x80`–`\xff` — a hex-named build
directory such as `C:\build\xff3a2b\driver.dll`, not `x64`. The consequence for this diff
is nil: the change is right, and the test asserts that escaping happens and the raw form is
absent, which is true and mutation-sensitive whichever path it uses. Only the stated reason
is false, and it is false because I wrote it that way in cycle 1.

RECORD: the `Quoted` comment in `provider_registry.cpp` and the comment above the new
assertion in `registry.cpp` both say `C:\x64\driver.dll` would render its `\x64` as the
escape for byte 0x64 — it would not, since 0x64 is printable; the real collision needs a
non-printable byte (`\x0a`, `\xff`). Rationale only; code and guard are correct. Origin: my
cycle-1 nit, not the implementer.

## The method question: is the standalone mutation harness sound evidence here?

**Yes, for these two changes** — and I say so having independently used the same technique
and reproduced every reported result exactly (`:672`/`:706`, 13/1, and the `Quoted` revert).

Why it is sound here: `core` is header-only and the registry TU has no link dependency, so
a standalone binary compiles *the same two translation units* — `provider_registry.cpp` and
`registry.cpp` — with the same compiler and the same standard as the real target. There is
no packaging step in between that could change their meaning. It is also strictly safer
than mutating through `conan create`, because a mutated package in the shared cache is
visible to every other agent on this box, which is a hazard this round has already been
bitten by.

The limit worth stating plainly: it proves nothing about **build wiring**. It cannot see
whether `provider_registry.cpp` is still in `fletcher-pubsub`'s source list, whether
`conformance_registry` links what the CMakeLists claims, or whether `gtest_discover_tests`
registers 14 entries under `Registry.`. A mutation harness is blind to anything that shows
up only in packaging. For *this* diff that gap is empty — neither CMakeLists is touched and
the TU gained no include — which is exactly why it suffices. It would not suffice for a
change that touched the build files, and the unmutated green should still come from the
real Conan-built harness, as it does.

## Confirmations

- `Registry.` is **14/14** with **no entry added** (14 `TEST(Registry,` in the file, same as
  cycle 1); the two new assertions and the probe class landed inside existing entries.
- The forcing test is `Registry.SelectsByNameWithoutCallerKnowingTheProvider`, still at
  `registry.cpp:170`, green, and **untouched by this diff** — the test-file hunks are at
  `:307+` and `:600+` only, so its meaning cannot have changed.
- **Public surface still 5** — `Factory`, `PathResolver`, `Register`, `SetPathResolver`,
  `Create` — plus the defaulted constructor and two deleted copy operations. Unchanged.
- **No new link dependency:** this diff adds no `#include` to the TU at all and touches
  neither CMakeLists.
- Nit 3 correctly left logged and unfixed.
