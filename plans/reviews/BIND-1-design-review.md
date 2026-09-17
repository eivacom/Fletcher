# BIND-1 — specification review of `binding.h`

**Subject:** `c-abi/include/fletcher/abi/binding.h`, 852 lines, as committed at `696a42f`
and unchanged since.
**Reviewed at:** `0811091` (BIND-2c landed; the header itself has not been edited).
**Why a spec review and not a code review:** BIND-1's deliverable is a *specification*.
The header is append-only within a major version, it is the surface C# binds now and
Rust binds next round, and its ownership wording is the part the plan calls "the
expensive thing to get wrong". A rule that is wrong here is wrong in two languages and
cannot be quietly corrected later.

**Outcome: 3 BLOCKERs, 4 DEBT items, 3 reviewed-and-kept, 2 NITs. All three BLOCKERs
were closed in cycle 2 (below); the DEBT and NITs are carried forward.** None of the
BLOCKERs is a design error — all three are places where the header states something
that is no longer true, or states a rule it does not finish. That is the failure mode a
spec review exists to catch, and it is why this one is worth doing before BIND-3 binds
against the file.

---

## What I verified (claim → result)

| Claim the header makes about itself | Result |
|---|---|
| Pure C99, self-contained, no Fletcher C++ header | ✅ `BindingAbi.CompilesAsC99AndIsSelfContained` green on both platforms; `c99_probe.c` includes it and nothing else |
| Every declaration derived from the seam spec, derivation named in the comment | ✅ spot-checked 9 of them against `docs/pubsub-interface-spec.md`; each names its §. The two additions (`fl_error.origin`, the codec's three steps) say so and say why |
| Nothing crossing is a `std::shared_future` (D-BIND-22) | ✅ `fl_schema_arrival` is a handle with a polling `wait`; no future, promise or fire-once callback anywhere |
| Not shared with, not defined in terms of, PDA-ABI's `driver.h` (D-BIND-2′) | ✅ no include, no shared tag, no layout claim |
| Three ownership words used precisely, unqualified pointers are BORROWED | ✅ with one exception — see **B1** |
| Append-only within a major; status numbers never renumbered | ✅ stated, and consistent with `core/README.md`'s own rule |
| "the shim's export table is exactly the functions declared below" | ❌ **B3** |
| "Only `fl_binding_abi_version()` is defined by the shim today" | ❌ **B2** |

---

## BLOCKERs

> The C snippets in this section quote the header **as cycle 1 found it**. All three
> passages were rewritten by the fixes recorded in cycle 2, so grepping the current
> header for them will not find them. That is the point of a review record.

### B1 — `fl_error`'s ownership rule does not cover the direction the header itself opens

This is the finding BIND-2c raised, and reviewing the whole header makes it worse than
it looked: the contradiction is not a corner, it is the ownership model meeting a
callback and not being extended to it.

The struct states one direction:

```c
 * Zero-initialise before the call (`fl_error err = {0};`). On FL_OK the callee
 * leaves it untouched. On failure the caller owns `message` and releases it with
 * fl_error_dispose, which is why every binding's throw site is a `finally`. */
typedef struct fl_error {
    int32_t status;     /* an fl_status value */
    int32_t origin;     /* an fl_origin value */
    uint8_t* message;   /* OWNED by the caller; NULL when status == FL_OK */
    size_t message_len; /* length in bytes; 0 when message is NULL */
} fl_error;
```

…and, 260 lines later, hands an `fl_error*` to a function the CALLER wrote:

```c
typedef fl_status (*fl_grow_fn)(fl_write_window* window, size_t min_bytes, fl_error* err);
```

Both cannot hold. Under the struct's rule the bytes are allocated by the shim and
released by `fl_error_dispose`; under `fl_grow_fn` they are allocated by the binding
and released by the shim's `delete[]`. Those are the same heap only while both sides
share a CRT — and BIND-3's acceptance says the shim links the **MSVC CRT statically**,
which is exactly when they are not.

It is not hypothetical: BIND-2c's first implementation called `fl_error_dispose` on a
callback-filled error, which is heap corruption under the configuration BIND-3 intends
to ship. It now copies the message and never frees it, which leaks at worst under one
reading rather than corrupting under the other, with the open question recorded at the
call site.

**Recommendation.** State the borrow discipline the rest of the ABI already uses: a
`grow` callback that sets `message` keeps those bytes alive until it returns, and the
shim copies what it needs and frees nothing. That needs no new exports and matches
`fl_str`, which is borrowed everywhere it appears. The alternatives both widen the
surface — a caller-supplied disposer, or shim-exported `fl_alloc`/`fl_free`.

**Also in scope:** `fl_grow_fn` is the only callback taking an `fl_error*` today, so
whatever is decided should be stated on `fl_error` itself rather than on `fl_grow_fn`,
or the next callback that takes one reopens this.

> **ANSWERED 2026-09-17 — D-BIND-32.** The maintainer ruled the borrow reading, and
> ruled it for every binding rather than for C# alone: a callback that sets `message`
> keeps those bytes alive until it returns; the shim copies what it needs and frees
> nothing. Stated on `fl_error` as this finding asked, with a cross-reference from
> `fl_grow_fn`. No signature, struct or enumerator changed, so no version bump.
> `WindowBuffer::Grow` already behaved this way and its "open question" comment is now a
> statement of the rule.
>
> The performance question raised against the ruling resolves in its favour: the copy is
> on the failure path, beside a C++ exception, publishing nothing. The borrow-and-copy
> cost that is on the *hot* path is unrelated — `ToSegments` per `fl_publisher_publish_row`
> — and is risk B-2, which BIND-3's per-row benchmark settles with numbers.
>
> **B1 closed. B2 and B3 remain open, so BIND-1 is still 🔴.**

### B2 — "Status of the surface" is stale, and it is the paragraph a binding author trusts

```c
// ── Status of the surface ───────────────────────────────────────────────────
// BIND-1 SPECIFIES this header; it does not implement it. Only
// `fl_binding_abi_version()` is defined by the shim today. Every other
// declaration is the specification BIND-2 (the codec), BIND-3 (the interop
// tier) and BIND-4 (pub/sub) implement, and BIND-Rust consumes next round.
// Calling an unimplemented entry point of a BIND-1-era shim is a link error, not
// a run-time surprise: the symbol is not exported until its item lands.
```

Since BIND-2c, roughly fifteen of these are implemented and exported. The paragraph now
tells a binding author something false about the artifact in their hands.

The last sentence is the valuable part and it is worth keeping — "not exported until its
item lands" is a real, checkable property, and it is why BIND-2c implemented nothing as
a stub returning `FL_NOT_SUPPORTED`. But the enumeration in front of it will go stale
again at BIND-4 and at BIND-9.

**Recommendation.** Keep the property, drop the inventory: say that a declaration is
exported only once its item lands, that a binding therefore discovers an unimplemented
entry point at link time, and that `fl_binding_abi_version()` is the runtime handshake.
Then no item boundary has to remember to edit this paragraph. If an inventory is wanted,
it belongs in the tracker, which is already maintained.

### B3 — The export-table claim names a mechanism that does not produce it

```c
/* Symbol visibility. FLETCHER_C_ABI_BUILDING is defined by the shim's own
 * build only (CMake PRIVATE define), so a consumer gets the import form on
 * Windows and a plain declaration elsewhere. Everything not marked with this
 * macro is hidden: the CMake target sets visibility to hidden on gcc/clang and
 * Windows exports nothing without __declspec(dllexport), so the shim's export
 * table is exactly the functions declared below. */
```

The conclusion is true today. The stated reason for it is not, and BIND-2c's first CI
run is the proof: with exactly those visibility settings in place, the Linux shim
exported six libstdc++ `shared_ptr` internals —

```
_ZTISt11_Mutex_baseILN9__gnu_cxx12_Lock_policyE2EE
_ZTISt16_Sp_counted_baseILN9__gnu_cxx12_Lock_policyE2EE
_ZTSSt11_Mutex_baseILN9__gnu_cxx12_Lock_policyE2EE
_ZTSSt16_Sp_counted_baseILN9__gnu_cxx12_Lock_policyE2EE
_ZTSSt19_Sp_make_shared_tag
_ZZNSt19_Sp_make_shared_tag5_S_tiEvE5__tag
```

— because libstdc++ marks them *explicit* default-visibility so RTTI works across a
shared object, and an explicit attribute beats a `-fvisibility` default. One
`std::make_shared` in a shim translation unit was enough. The property now holds because
`c-abi/cmake/exports.map` says so at the link, not because of the two mechanisms this
comment names.

**Recommendation.** Point the comment at the version script, or stop claiming the
mechanism and claim only the property. A header comment that explains *why* something is
true is more dangerous than one that does not, when the why is wrong: it is exactly what
stops the next reader from checking.

---

## DEBT — for whoever edits the header next; none of these loops the design

### D1 — The OUT convention says "on any failure", but two non-failures also leave OUT untouched

```c
//   OUT        Written only on success. On any failure the callee leaves the
//              object untouched, so a caller may reuse a variable across calls
//              without clearing it.
```

`fl_schema_arrival_wait` has five outcomes, and two of them — `FL_PENDING` and
`FL_SUBSCRIPTION_ENDED` — are documented as *values rather than failures*, yet both
leave `*out` untouched. Under the convention as written they are neither "success" (so
OUT is not written) nor "failure" (so the untouched guarantee does not apply to them).
The behaviour is right and is spelled out locally; the global convention is what is
imprecise. Read it as "written only on `FL_OK`; on anything else the callee leaves it
untouched" and the whole header becomes consistent.

### D2 — `fl_string_list_at` leaves out-of-range unspecified, and the implementation had to invent it

The parallel accessor one section away specifies it:

```c
/* The key at `index`, BORROWED from the set. Out of range yields {NULL, 0}. */
FL_ABI_EXPORT fl_str fl_attachments_key_at(const fl_attachments* atts, size_t index);
```

The string-list one does not:

```c
FL_ABI_EXPORT size_t fl_string_list_size(const fl_string_list* list);
FL_ABI_EXPORT fl_str fl_string_list_at(const fl_string_list* list, size_t index);
FL_ABI_EXPORT void fl_string_list_dispose(fl_string_list* list);
```

BIND-2c chose `{NULL, 0}` to match its neighbour. That was the right guess, but a spec
that requires a guess has a hole in it. Two accessors of identical shape, one specified
and one not, is also the kind of asymmetry that reads as significant when it is not.

A rider worth stating while it is open: for `fl_publisher_list_topics` the sentinel is
unambiguous only because a topic name can never be empty — the six topic rules refuse an
empty segment list and an empty segment, so the joined name of any valid topic has at
least one byte. If `fl_string_list` is ever reused for something that may contain empty
strings, `{NULL, 0}` stops distinguishing "absent" from "present and empty".

### D3 — `FL_NOT_SUPPORTED` carries two different "not yet"s, and its own text fits neither well

```c
    /* This provider does not implement the requested behaviour. Distinct from
     * FL_REENTRANT_CALL, which says "not from there". */
    FL_NOT_SUPPORTED = 6,
```

Two entry points answer with it for reasons the sentence does not describe:

- `fl_provider_create` with a **path** selector — well-formed, names a driver this build
  cannot load, and there is no provider yet for anything to be unimplemented *by*;
- `fl_subscriber_subscribe_schema` — the seam has not grown the pair yet (D-BIND-29).

Both are defensible mappings and neither needs a new number. The status's prose is what
should widen: "this build does not implement the requested behaviour" covers all three
cases without inventing anything.

### D4 — `fl_attachments_find`'s complexity is unstated, and the set's ordering invariant is what makes it cheap

```c
/* Find by key. Returns 1 and writes `out` when present, 0 when absent; `out` is
 * BORROWED exactly as value_at's result is. Absence is not a failure and carries
 * no error. */
FL_ABI_EXPORT int fl_attachments_find(const fl_attachments* atts, fl_str key, fl_blob* out);
```

The set is sorted in ascending unsigned-byte key order and the header says so twice, so
a binary search is available and is presumably intended. Nothing says whether `find` is
one. This matters for a binding deciding whether to call `find` in a loop over a
delivery's hot path or to build its own index once. One clause settles it.

---

## Reviewed and kept — recorded so they are not reopened

These are the three the plan named as the expensive wording. All three are right. The
reasoning is written down here so the next reader does not have to re-derive it.

### K1 — `fl_schema`'s never-call-Arrow's-release warning: prose is the only available guard, and that is not a defect

```c
 * A binding must NEVER call the Arrow C Data Interface's own `release` callback
 * on this structure. That callback destroys the schema for every other holder,
 * including the provider that is still delivering on it, and the failure appears
 * later and elsewhere. This is the single most expensive mistake available at
 * this boundary, which is why the type carries an owner rather than being a bare
 * `ArrowSchema*`.
```

The obvious objection is that the struct still hands out the pointer, so the mistake is
one keystroke away:

```c
typedef struct fl_schema {
    void* owner;                      /* opaque; NULL when schema is NULL */
    const struct ArrowSchema* schema; /* BORROWED from the owner; never released directly */
} fl_schema;
```

It has to. The sanctioned use — "a binding that wants an Arrow schema of its own imports
a COPY" — requires the pointer to import from. Hiding it behind an accessor would move
the misuse from `schema->release(schema)` to
`fl_schema_borrow(&s)->release(...)`, which is not a meaningful barrier for one more
export. The `const` qualifier already makes the mistake require a cast in C++ and is the
cheapest real friction available.

**Kept as written.** The warning is placed correctly (on the type, not on the release
function), it says what breaks and where the symptom appears, and it explains why the
type has an owner at all.

### K2 — The write window's disclosed residue: disclosure beats zeroing, and the alternative defeats the type

```c
 * DISCLOSED RESIDUE: the count is trusted against `room`, not against what was
 * actually touched, so bytes in [written, reported) are committed and published
 * having been written by nobody. That is inherent in handing over real memory,
 * and it is disclosed rather than checked away.
```

Zeroing the window before the lend would cost O(room) on exactly the path that exists to
avoid a copy, which is self-defeating; `WriteBuffer`'s own comment in the tree makes the
same point. Trusting the count against what was touched is not implementable without
either shadow state or page protection.

Worth noting the residue window narrowed in practice: BIND-2c's zero-return rule (a
writer reporting 0 bytes fails the publish) means the disclosed range is
`[written, reported)` for `reported > 0` only.

**Kept as written**, and the word "disclosed" is doing real work — it tells a binding
author this is a contract rather than a bug they found.

### K3 — Attachments' never-sort rule: correctly placed and correctly justified

```c
 * A2 - THE ORDER IS PART OF THE VALUE: ascending unsigned-byte order of the key
 * bytes, which is the order the entries are written to the wire in. A BINDING
 * NEVER SORTS. It reads the sequence Fletcher publishes. This matters more than
 * it looks: C#'s ordinal string comparison is UTF-16 code-unit order, which
 * disagrees with byte order (U+FF5E sorts before U+10000 in one and after it in
 * the other), so a binding that "helpfully" re-sorted would publish a different
 * wire order than the C++ side for the same value.
```

The rule names the exact trap, in the exact language that has it, with a concrete pair
of code points. The builder is the sanctioned and only sort site, and it is stated as
such. The surface supports the rule structurally: enumeration is positional, there is no
map API, and a published set cannot be inserted into.

**Kept as written.** This is the best-specified section of the header.

---

## NITs

- **N1** — `fl_blob`'s `owner` is "NULL only when `size == 0`", and `fl_blob_release` is
  "a no-op on an empty blob". Consistent, but "empty blob" is never defined as
  `size == 0` in so many words. One clause.
- **N2** — The delivery callback's `subscription_id` is a bare `uint64_t` here while
  BIND-4 wraps it in a managed `Subscription` handle (D-BIND-20). Correct and
  deliberate; a sentence saying the C form is the raw id and a binding is expected to
  wrap it would stop the next reader wondering whether they disagree.

---

## Numbers

| | |
|---|---|
| Lines reviewed | 852 |
| Declarations (`FL_ABI_EXPORT`) | 40 |
| Implemented and exported as of `0811091` | ~15 (BIND-2c) |
| BLOCKERs | 3 |
| DEBT | 4 |
| Reviewed and kept | 3 |
| NITs | 2 |

**None of the three BLOCKERs requires a signature change**, so none of them is blocked
by the append-only rule, and all three are comment edits plus, for B1, one sentence of
new rule. B1 is the only one with code riding on it — BIND-2c's `WindowBuffer::Grow`
takes the survivable side and says so at the call site, and will follow whatever is
decided here.

**This review does not close BIND-1.** It is cycle 1; the item goes 🟢 when the three
BLOCKERs are answered and the header is re-read against the answers. **B1 was answered
on 2026-09-17 (D-BIND-32) and the header updated; B2 and B3 are open.**

---

# Cycle 2 (final) — re-read at `f73cfb4` + the B2/B3 fixes

The header is now 913 lines (was 852). Every change since cycle 1 is a comment: no
signature, struct, enumerator or macro moved, so nothing in this cycle touches the
append-only rule or needs a version bump.

## BLOCKER 1 — the `fl_error` borrow rule: **closed**

Ruled as D-BIND-32 and stated on `fl_error` itself, which is where the finding asked
for it:

```c
 *   A CALLBACK THAT SETS `message` KEEPS THOSE BYTES ALIVE UNTIL IT RETURNS.
 *   The shim copies what it needs before the callback's frame is gone and frees
 *   NOTHING. fl_error_dispose is for errors the shim filled; a callback never
 *   calls it on one of its own.
```

Checked against the finding's three asks: it is on the type rather than on
`fl_grow_fn` ✅, so the next callback taking an `fl_error*` inherits it; `fl_grow_fn`
carries a cross-reference rather than a second copy of the rule ✅; and the rejected
alternatives are named with their reason ✅. `WindowBuffer::Grow` already behaved this
way, so nothing in the shim changed.

## BLOCKER 2 — "Status of the surface": **closed, and made unable to restale**

The replacement keeps the property cycle 1 wanted kept and drops the inventory that
caused the staleness:

```c
//   A DECLARATION IS EXPORTED ONLY ONCE ITS ITEM LANDS. Nothing here is ever
//   stubbed out to return FL_NOT_SUPPORTED because it has not been written yet -
//   that status means "this build cannot do the thing", which is a different and
//   much more confusing statement than "not linked yet".
```

The test that matters for this one is whether a future item boundary has to remember
to edit it. It does not: there is no longer a count, a list, or an item name that
lands later. The paragraph now points at the tracker as the one maintained home for
"what has landed", and says out loud that it went stale before — which is the part
that stops someone helpfully re-adding an inventory.

## BLOCKER 3 — the export-table claim: **closed, and cycle 1 understated the problem**

Worth recording as a correction rather than quietly fixing, because cycle 1 got this
half right. It said *"The conclusion is true today. The stated reason for it is not."*
The first half was true only of **Linux**. On **Windows** the conclusion was never
true: ConanCenter's static `fast-dds` is built with `EPROSIMA_USER_DLL_EXPORT` and its
objects carry /EXPORT directives the linker honours, so the shim's table has ~3531
names and always did. The header claimed the property unconditionally, so it was
wrong on Windows from the day it was written, and BIND-2c's six libstdc++ symbols were
a second, separate breach on the platform where it *could* hold.

The replacement states both platforms separately, names the mechanism that actually
produces the property on each, and says plainly that it does not hold on Windows:

```c
 *   LINUX - the property holds, and it holds because `c-abi/cmake/exports.map`
 *   says so at the link. Hidden visibility governs what our own translation units
 *   emit BY DEFAULT and `--exclude-libs ALL` localises what arrives from a static
```

```c
 *   WINDOWS - the property does NOT hold, and cannot be made to from here.
```

It also keeps the reassurance a binding author needs on reading that — imports bind
per module, so nothing resolves incorrectly — and ends with the claim that *is*
unconditional and is the one a caller depends on: every callable function is marked,
and nothing unmarked is callable.

## The cycle-1 claims table, re-checked

| Claim | Cycle 1 | Now |
|---|---|---|
| "the shim's export table is exactly the functions declared below" | ❌ B3 | ✅ stated per platform, with the real mechanism |
| "Only `fl_binding_abi_version()` is defined by the shim today" | ❌ B2 | ✅ claim removed; the property it guarded is kept |
| Three ownership words used precisely; unqualified pointers BORROWED | ✅ *with one exception — B1* | ✅ no exception remaining |
| Pure C99, self-contained | ✅ | ✅ re-verified after the edits: `BindingAbi.CompilesAsC99AndIsSelfContained` green, 23/23, zero warnings |

## DEBT and NITs: unchanged and still open

D1–D4 and N1–N2 are untouched, deliberately. None of them blocks the item — by this
repo's review convention DEBT does not loop the design — and each is a one-clause edit
whoever next opens the header can take. They are listed above and stay listed.

The two most worth taking early, because both cost an implementer time rather than
just reading time: **D1** (the OUT convention says "on any failure" while two
documented non-failures also leave OUT untouched) and **D2** (`fl_string_list_at`
leaves out-of-range unspecified, where its twin specifies `{NULL, 0}` — BIND-2c had to
guess).

## Outcome

**All three BLOCKERs closed; the header re-read against the answers.** That is BIND-1's
last acceptance bullet, and with it the item's acceptance is met in full.

| | |
|---|---|
| BLOCKERs opened / closed | 3 / 3 |
| Of those needing a signature change | 0 |
| DEBT carried forward | 4 |
| NITs carried forward | 2 |
| Header lines, cycle 1 → cycle 2 | 852 → 913 |
| Behaviour changed in the shim | none — every edit is a comment |
