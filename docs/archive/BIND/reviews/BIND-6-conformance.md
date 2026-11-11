# BIND-6 — conformance against the acceptance

**Subject:** BIND-6's acceptance bullets in `BIND-csharp-bindings.md` ("### BIND-6 — C# row emitter"),
as amended by D-BIND-69 to D-BIND-82. Each bullet was checked against the tree, not against a commit
message or a slice note's own claim.

**Reviewed at:** `c8ae231`. Code review: `BIND-6-codereview.md` (5 BLOCKERs, 10 questions).

**Outcome: 9 of 13 bullets CONFORM as written; 3 PARTIAL; 1 conforms on Linux only. BIND-6 does not
close.** The bullets grade better than the code does, because three of the code review's blockers are
inputs no bullet names: a singular-named repeated field (B2), a message named `Schema` (B3), and a
flatten wrapper as a map value (B4). Bullet 9 ("every mapped type … is emitted") is graded PARTIAL
for that reason.

## The bullets

| # | Bullet | Verdict |
|---|---|---|
| 1 | `<stem>.fletcher.cs`: sealed class, nullable annotations, real enum, `static Schema`, `ToArrow`, `FromArrow`; no `WriteTo`/`ReadFrom` (D-BIND-1) | ✅ The goldens are pinned byte for byte (`CsVisitorPair.TheCommittedGoldensAreThePluginsOutput`, `CsGolden.ModelByteIdentical`) and contain neither member |
| 2 | Names per D-BIND-69; a case with `csharp_namespace`; one beside protoc's C# without CS0101 | ✅ `CsNames.*`, `CsVisitor.FileHeaderAndNamespaceIgnoringCsharpNamespace`, `ProtocDotnetBeside` 4/4 on both platforms in CI; D-BIND-82's refusal has two cases |
| 3 | D-BIND-73's rules, a case per rule | ✅ `test_csharp_type_table.cpp:79-361`. The rules as written leave the TYPE-name collision of B3 open |
| 4 | D-BIND-70 enum members, the edge cases, wire bytes match C++ | ✅ The enum cases, and `CoverageWireBytesTests` over all 12 goldens in both directions |
| 5 | The model layer per D-BIND-72 (amended by D-BIND-76); topic segments in C++'s form, **with a test that a C# and a C++ publisher of one method register the same topic** | 🟨 **PARTIAL.** Apache.Arrow-only and model-only are met. The C#-against-C++ topic test does not exist (code review B5). The bullet's "emitted only when requested" is stale after D-BIND-76 |
| 6 | Build integration per D-BIND-71 | ✅ `ProtocDotnet.csproj`'s `FletcherGen`; the property set from the environment only when empty |
| 7 | The pair's names and members; the handler delegate (D-BIND-79); `.native.cs` and `csharp_model_only` (D-BIND-76); the topic class (D-BIND-77); no `SubscribeInPlace` | ✅ `CsVisitorPair.*`, and `GeneratedPairTests` over the shim on net8.0 and net10.0. **But** a message named `Topic`, `Schema` and the like breaks the pair (B2, B3) |
| 8 | Batch-first; `Publish(T)` per-row with its price documented (D-BIND-78) | ✅ The golden's doc comment and `ThePublisherDeclaresItsTopicAndPublishesPerRowAndInBatches` |
| 9 | Every mapped type emitted; temporal lossless; maps as ordered pairs (D-BIND-75) | 🟨 **PARTIAL.** Every mapped TYPE is emitted and round-trips (`CoverageWireBytesTests`, `ConversionTests`). Common SHAPES of those types do not compile (B2, B4) or do not agree with C++ on the wire (B1) |
| 10 | Nullability annotations match the Arrow schema's (G-5) | 🟨 **PARTIAL.** A plain message field is `T?` over a non-nullable column, and writes nulls into it (B1). List items and map values differ from the schema too (question Q4). No test compares the two systematically |
| 11 | Reflection-free: NativeAOT publish with zero trim warnings (D-BIND-81) | ✅ **on Linux:** `Fletcher.AotSmoke` published for both TFMs with warnings as errors and run in CI at `d32b9f7`. Windows is open note 1 (Q6) |
| 12 | New cases in `test_csharp_type_table.cpp` and `test_csharp_visitor.cpp`; no-drift with the tokens absent and present | ✅ 22 and 36 cases, the matrix agrees. DEBT: no-drift does not cover `accessor` or `rust` (T-D3) |
| 13 | Cross-file references resolve; no duplicate filename; a shared helper emitted once | ✅ `CsVisitorCrossFile.*`, `ProtocDotnetShared`, the cross-assembly round trip. The `CollectCrossFileIncludes` deviation awaits a ruling (Q5) |

## Handovers into BIND-6

| Source | Handover | Status |
|---|---|---|
| D-BIND-9 | The depth cap | Discharged: the accessor's cap is BIND-7's; the row emitter supports any depth (6c-3) |
| D-BIND-49 / B-2 | Batch-first; settle `Publish(T)` | Discharged by D-BIND-78 |
| D-BIND-69 | PascalCasing of the package; collision with a member | Discharged by D-BIND-73 |
| D-BIND-69 | Collision with another field after PascalCasing | **Open** (Q7) |
| D-BIND-70 | The enum edge cases | Discharged, with tests |
| D-BIND-71, 72 | The recipe; the opt token; the topic spelling | Discharged by D-BIND-71, 76, 77 (the gateway parts are BIND-8's) |
| Dev plan G-1, G-2, G-3 | Temporal and maps | Discharged by D-BIND-26, 74, 75 |
| Dev plan G-5 | Nullability | **Partial** (B1, Q4) |
| Dev plan G-6 | Cross-file | Discharged with an unruled deviation (Q5) |
| Dev plan S-9 | The generated subscriber decodes with its own schema | Discharged by construction; never run over a schema-less transport (T-D8) |
| BIND-5's lesson | Port a mirror's parameters | Honoured: `CoverageWireBytesTests` transcribes the fixture value by value. **But** B1 is the same shape of miss: the fixture always filled the field whose absence breaks |

## The four places, per ruling

D-BIND-69 to 82 are in the digest, the tracker and the development plan. The architecture diagrams
lag behind:
- **D-BIND-76:** the native pair is still drawn `<<planned>>`, and the note "emitted only when asked
  for" contradicts the ruling.
- **D-BIND-77:** no topic class is drawn, though it is a new generated type.
- **D-BIND-74:** the box text is stale.

So D-BIND-76 and D-BIND-77 do not meet the four-places rule. The public-surface note is stale too
(T-D1).

## What closes BIND-6

1. **Fix B1 to B5**, B1 to B4 after the rulings Q1 to Q3. Each fix gets a test that fails without it:
   - B1: a C#-written row with an absent field, decoded by C++;
   - B2 to B4: the `pb1`, `pb2` and `pb4` shapes compiled in a lane;
   - B5: C# in `ExpectBackendsAgree`.
2. **Rule Q4 to Q10**, and record each ruling in the four places.
3. **Bring the diagrams and the public-surface note up to date** (T-D1, T-D2), and the stale text
   (T-D5, T-D9).
4. **The DEBT that is a real failure for a legal input** (P-D1, G-D1) is fixed with it. The rest is
   parked by name to BIND-9 or BIND-10.
5. **`ci.pr` green** on the fixing commit, first attempt.
