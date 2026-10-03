# BIND-3 — conformance against the acceptance

**Subject:** BIND-3's acceptance bullets in `BIND-csharp-bindings.md`, each checked
against the tree rather than against a commit message.
**Reviewed at:** `a5fabed`. Code review: `BIND-3-codereview.md` (0 BLOCKERs,
4 DEBT, 3 NITs).
**Why separate from the code review:** that pass asks what the code does; this one
asks whether the bullets are met. Where a bullet was *changed* during the item —
three were — this pass states what it says now and who changed it, because a
conformance pass that grades against a bullet the author rewrote is worth nothing
unless the rewrite is visible.

**Outcome: CONFORMS on 9 of 9 live bullets.** Four clauses were moved to other
items by maintainer ruling and are not graded here. Three bullets were rewritten
mid-item, all three by the maintainer, all three recorded in four places.

---

## The live bullets

| # | Bullet | Verdict |
|---|---|---|
| 1 | `Eiva.Fletcher.Interop`: P/Invoke declarations, a `SafeHandle` per native handle, `SetDllImportResolver` naming the missing RID, the ABI version handshake at load | ✅ 21 declarations, all exported by `binding.h`; a handle type per opaque ABI handle; the resolver names the RID and the native rows FAIL rather than skip when the shim is absent; the handshake is an EXACT match in a type initializer, not `>=` — `binding.h` says there is no compatibility guarantee before 1.0 |
| 2 | Error handling per D-BIND-19: one `ThrowIfFailed`, `FletcherException {Status, Origin, Message}`, `FletcherFormatException` when the origin is the codec, a captured managed exception rethrown as itself, and a reflection test for N-1 | ✅ one throw site; both origins asserted (the second is what stops the first being vacuous); rule 3 checked FIRST; the dispose is an unconditional `finally` with a row asserting the struct is zeroed on the rethrow path; `ThunkDisciplineTests` requires the OUTERMOST clause to be a catch-all |
| 3 | `FletcherCodec(Schema)`, `Bind → BoundRows` (dispose unbinds **then** releases), `Encode`, `Decode`, `DecodeBatch`, no method returning bytes | ✅ and the ordering is structural — the export is reference-counted by the bound-rows handle, so it survives a caller who never disposes |
| 4 | C Data Interface export/import via `Apache.Arrow.C` | ✅ export at bind and at open, import at decode. The receiving half (N-5) is **moved**, see below |
| 5 | Arrow IPC schema parse with metadata preserved, and the IPC parity test on fields, types, nullability and metadata key order | ✅ over all ten checked-in goldens: the spec's own keys (`proto_package`, `proto_message`, `field_number` on every field) plus a round trip comparing fields, types, nullability and KEY ORDER, with a vacuity guard. Key order is checked because Arrow metadata is an ordered list, not a map |
| 6 | Dictionary handling per spec §"Dictionary Types" | ✅ **after the bullet was found unmet.** The shim refused every dictionary; D-BIND-39 ruled the bullet right and the implementation wrong. Now: planned as the value type, non-scalar value types refused BY FIELD NAME, index resolved at encode, and a decoded dictionary arrives as its value type |
| 7 | The UTF-16↔UTF-8 boundary stated in the XML docs | ✅ on `FletcherCodec`: wire lengths count UTF-8 BYTES, and the transcode happens inside `Apache.Arrow` on the way into an array rather than at this boundary, which is why the copy accounting does not count it. The oracle's scope-note half is BIND-5's |
| 8 | Every type the PROTO MAPPING produces round-trips through the binding (was "Bucket 1 green in C#") | ✅ 17 cases over 14 distinct Arrow types, every one with a null row, with a coverage guard against the mapping table |
| 9 | `integration-tests/binding-abi-conformance` round-trips every corpus scenario with the values C++ produces | ✅ 5 fixtures, byte identity between managed and native Arrow through one codec, green on both platforms in CI |

---

## The three bullets that changed, and who changed them

A conformance pass that grades against a rewritten bullet is circular unless the
rewrite is visible. All three were **ruled by the maintainer**, not by the author.

1. **"Bucket 1 green in C#" → "every PROTO-MAPPING type round-trips" (D-BIND-39).**
   Raised because bucket 1 is `arrow-bridge`'s **Arrow-native** suite: roughly half
   of it exercises unions, decimals, intervals, half-float, the view types and
   fixed-size binary, none of which any `.proto` can produce, plus `test_envelope`
   (BIND-8 by §2.6) and the `WriteBufferInPlace` writer cases (BIND-4 by surface).
   Porting it would have asked the binding to implement a tier it does not serve.
2. **The dictionary bullet: confirmed, not relaxed (D-BIND-39).** The temptation
   was to soften it to match the code. The maintainer ruled the other way and the
   shim changed. Worth recording as the healthier direction of travel.
3. **The glibc floor moved to BIND-9 (D-BIND-41)** and became a MEASUREMENT rather
   than a statement: `ci.c-abi.yml` prints the floor on every Linux build, so
   BIND-9 states a measured number. **Not yet observed** — the step ships in
   `afddc09` and prints on its first `c-abi / build-linux` run.

## Clauses moved out of BIND-3 and not graded here

| Clause | Moved to | Ruling |
|---|---|---|
| N-5's native deep copy of a RECEIVED schema; `SchemaHandle.ToArrowSchema()` | BIND-4 | nothing produces an `fl_schema` until the subscriber exists, so the receiving direction is unreachable |
| The per-row publish benchmark (B-2) | BIND-4 | D-BIND-37 — a publish benchmark needs a managed publisher |
| The MSVC CRT, static or dynamic | BIND-9 | D-BIND-38 — a packaging question |
| The glibc floor stated | BIND-9 | D-BIND-41 — a statement about a package, and BIND-3 packages nothing |

**BIND-3 therefore carries no performance evidence, deliberately.**

---

## Carried forward

- **D1 from the code review is the one that matters:** the managed and native
  dictionary-rewrite derivations are proven to agree for a top-level and a
  struct-nested dictionary only. Owed to BIND-4/5.
- The remaining DEBT and NITs do not loop the item (the round's convention).
- **This pass and the code review share an author with the code.** #129 is a draft
  until BIND-10, so no outside eyes have seen 3a–3d except the maintainer's three
  rulings and Copilot's review of the earlier items. Weight the mechanical checks
  accordingly.
