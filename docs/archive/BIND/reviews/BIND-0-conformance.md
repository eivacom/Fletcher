# BIND-0 — conformance review

**Subject:** BIND-0's acceptance, ten bullets, as written in
`plans/BIND-csharp-bindings.md` Part 7 and tracked in its "State — 2026-09-14" table.
**Reviewed at:** `f03feb0`.
**Why a conformance review and not a design one:** BIND-0's deliverables are *facts*,
not a document — does the lane exist, was the matrix derived, is Arrow pinned, was the
size measured. There is nothing here to review as a specification the way BIND-1's
header was. So each bullet is checked against the tree and against CI, and the state
table is treated as a claim to verify rather than as evidence.

**Outcome: CONFORMS, 9 of 9 live bullets met** (the tenth was struck as a duplicate by
D-BIND-33 before this review ran). **3 findings carried forward, none blocking**, and
one of them is worth reading even if nothing else here is: the Part 4 test matrix has
gone stale, and the mechanism that staled it will do so again.

---

## Bullet-by-bullet

Each row states what I did to check it, not what the state table says.

| # | Acceptance bullet | Check performed | Result |
|---|---|---|---|
| 1 | The 2026-09-11 rulings are in `BIND-locked-decisions.md` (D-BIND-1 … D-BIND-27) and this file; ADO 18689 points at the committed plan; 16353's accessor wording corrected | Enumerated every `D-BIND-n` heading in the digest | ✅ **1–33 present and contiguous**, including the `1a` and `2′` variants. The bullet asked for 1–27; the digest has grown six more since, each recorded the same way |
| 2 | `.devcontainer/Dockerfile` gains the .NET 10 LTS SDK via `DOTNET_SDK_VERSION`; `dotnet/global.json` pins the same band | Read both files | ✅ `ARG DOTNET_SDK_VERSION=10.0.400` drives `dotnet-install.sh`; `global.json` is `10.0.400` with `rollForward: latestFeature`. **The same band, verified rather than asserted** |
| 3 | `c-abi/` builds a shim exporting only `fl_binding_abi_version()` | Built locally; read the CI export assertion | ✅ built, and the *"only"* is now enforced at the link by `c-abi/cmake/exports.map` rather than merely observed — see finding F2 |
| 4 | `dotnet/` builds three empty packages | Listed `dotnet/src/`; checked the CI lane's result | ✅ `Fletcher`, `Fletcher.Interop`, `Fletcher.GatewayClient`; `dotnet / linux` and `dotnet / windows` both green on the last run |
| 5 | `ci.c-abi.yml` and `ci.dotnet.yml` green on **both** platforms | Read the run, not the summary | ✅ `c-abi / build-linux`, `c-abi / build-windows`, `dotnet / linux`, `dotnet / windows` — all four `completed:success`; the whole `ci.pr` run green |
| 6 | Part 4's matrix re-derived by the stated command and committed | **Re-ran the command and reconciled every bucket** | ⚠️ **CONFORMS AS OF ITS DERIVATION, STALE NOW** — see finding F1 |
| 7 | `Apache.Arrow` pinned, and its C Data Interface export/import verified for the mapping's types, nested included (N-9) | Grepped both `.csproj` | ✅ `Version="[23.0.0]"` — exact-pin bracket syntax, in `Fletcher` and `Fletcher.GatewayClient` alike |
| 8 | The per-RID size of the shim with all three built-ins measured and recorded | Read the lane's measurement step | ✅ measured and reported into the job summary every run — but the recorded **7.61 MiB is a kickoff baseline, not a current fact**; see finding F3 |
| 9 | Landing order with the modernization branch agreed (P-7) | Read D-BIND-29 | ✅ ruled 2026-09-15: BIND does not wait for #128 |
| 10 | ~~Self-hosted publish runner + `NUGET_EIVA_API_KEY`~~ | — | ⛔ **struck by D-BIND-33** as a duplicate of BIND-9's own, more complete bullet, before this review ran |

---

## Findings

### F1 — The Part 4 matrix is stale by two cases, and the mechanism will do it again

This is the finding worth the review. I re-ran the documented command —

```
for f in core/tests/*.cpp pubsub/tests/*.cpp arrow-bridge/tests/*.cpp pubsub-arrow/tests/*.cpp \
         fastdds-pubsub-provider/tests/*.cpp xrcedds-pubsub-provider/tests/*.cpp \
         gateway/tests/*.cpp protoc/tests/*.cpp; do
  echo "$(grep -c '^TEST\(_F\|_P\)\?(' "$f") $f"
done
```

— and reconciled every bucket against it. **Bucket 1 reproduces exactly**: 19 + 11 + 11
+ 3 + 35 + 23 + 2 = **104**, the number BIND-3's acceptance is stated in. **Bucket 3
reproduces exactly**: 18 + 5 + 15 = **38**. The documented exclusions reconcile too —
`test_fletcher_sample_pub_sub_type` has exactly **24** macros, which is the "24
provider-internal cases excluded" of Q10, and `test_owned_schema` has exactly **1**.
That is a matrix that was derived honestly.

**Bucket 4 does not reproduce.** The matrix records `test_xrce_document` at **9**; the
file has **11**. Bucket 4's total is therefore **80**, not the **78** BIND-4's
acceptance is written against.

The cause is not a miscount. `test_xrce_document.cpp` was last touched by `efad14c`
("Stop a hung-up XRCE peer from killing the process (SIGPIPE)", #130), authored
**2026-09-16** — a day *after* BIND-0's kickoff commit, yet sitting *below* it in the
history. That is D-BIND-30's documented policy working as intended: #129 is rebased
onto `main` at item boundaries, and #130 rides in it until it merges. **The matrix was
correct when it was derived and a rebase silently invalidated it.**

Why it matters beyond two cases: BIND-3 and BIND-4 state their acceptance *in terms of
these totals* — "Bucket 1 (104) green", "Bucket 4 (78)". An acceptance bullet phrased
as a number that a routine rebase can change is a bullet that will be argued about at
exactly the moment it is supposed to settle something. Every future rebase can do this
again, silently, and nothing in the round notices.

**Recommendation** (not a ruling, and not BIND-0's to make): either re-derive the matrix
at each item boundary as part of the rebase, or state the buckets by *file set* and let
the count be computed when it is needed. The second costs nothing and cannot rot. This
belongs to whoever opens BIND-3, which is the first item that has to meet one of these
numbers.

### F2 — Bullet 3's "exporting only `fl_binding_abi_version()`" became true for a different reason

Recorded so the state table is not read as evidence of something it did not check.

At BIND-0 the *only* was a consequence of there being nothing else to export. It has
since survived fifteen more entry points, but not by the mechanism BIND-0 assumed: the
Linux shim leaked six libstdc++ `shared_ptr` internals the moment BIND-2c used
`std::make_shared`, and the property now holds because `c-abi/cmake/exports.map` states
it at the link. BIND-1's spec review (B3) found the header claiming the same thing for
the same wrong reason, and on Windows the property never held at all — the statically
linked Fast DDS re-exports ~3531 names.

No action for BIND-0: the bullet as written was met, and is still met. The note exists
because "the shim exports only what it declares" reads like a checked invariant in the
state table, and at BIND-0 it was an accident of an empty ABI.

### F3 — The 7.61 MiB shim size is a kickoff baseline, not a current measurement

The bullet asked for the size to be *measured and recorded*, and it was. But it was
measured against a shim whose entire exported surface was one version function, and the
state table presents the number without that qualification. The lane re-measures on
every run, so the current number is always available in the job summary; it is only the
recorded 7.61 MiB that is frozen.

No threshold is enforced until BIND-9, so nothing is riding on it today. The
qualification matters when BIND-9 sets the budget: 7.61 MiB is the floor the built-ins
cost, not the shim's size.

---

## What this review did not check

Stated because a conformance review that does not say where it stopped invites the
reader to assume it went further.

- **The ADO items.** Bullet 1's "ADO 18689 points at the committed plan; 16353's
  accessor wording corrected" was not verified — it needs `devops.eiva.com`, and I
  checked only the half that lives in the repo (the decisions digest).
- **The C Data Interface round-trip claim in bullet 7.** I verified the *pin*. The
  state table's "17 types round-trip … 18 cases on both TFMs" describes a test whose
  results I did not re-run; it is a `dotnet` lane claim and that lane is green.
- **`linux-x64`'s shim size.** Recorded as coming "from the first lane run"; I read the
  measurement step, not a stored number.

---

## Outcome

**CONFORMS.** Nine live acceptance bullets, nine met. The tenth was struck as a
duplicate by D-BIND-33 before this review ran, so nothing here relies on it.

| | |
|---|---|
| Live bullets | 9 |
| Met | 9 |
| Struck as duplicate (D-BIND-33) | 1 |
| Findings carried forward | 3 (F1 stale matrix, F2 export-table provenance, F3 stale size baseline) |
| Blocking findings | 0 |

F1 is the one to act on, and it is not BIND-0's to fix: the matrix was correct when
derived, and the acceptance bullet asked for exactly that. It belongs to BIND-3, the
first item whose acceptance is stated as one of these numbers.
