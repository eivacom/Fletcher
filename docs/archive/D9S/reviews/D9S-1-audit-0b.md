# D9S-1 — step 0b audit (new-test mode)

**Verdict: CLEAR.** Date 2026-11-13. Proof-of-read: `plans/RIX-rulings.md`, 393 lines, last heading "2026-11-13 — D9S pre-round: a failed handoff is reported; corrections approved"; d9 §3 L1–L18 read.

**Scope (uncommitted diff, `git diff HEAD -- c-abi/tests`):** `c-abi/tests/test_binding_abi.cpp:48` `BindingAbi.TheVariantIsAnsweredBeforeAnyOtherCall`; `:62` `BindingAbi.TheVariantAndVersionAnswerAlikeFromManyThreads`; `c-abi/tests/CMakeLists.txt:148-161` (file compiled into `c_abi_probe_tests`, `TEST_PREFIX "probe."`). Control: dotnet `NativeLoaderTests` (existing, T2).

**Checked against:** d9 L16, L12, L13, L6 (D27), L5, L7; d9 §2 "Variant"; rulings 2026-11-12 "one shim in two variants", 2026-11-13 "Wrong build" (D23) and "Version: one version" (D27).

- **Law (a), both rows LAWFUL.** Probe answers *edge*, which matches d9 §2: the probe shim is "a third artifact from the same sources, not a variant". Nothing pins it as a separate variant. `EXPECT_NE(variant, 0)` forbids 0 as a build, which agrees with the brief. Pinning the literals 1 and 2 follows L6's append-only rule. Declaring `FL_VARIANT_SERVER` in the edge header is one declared vocabulary, and L12 allows it ("both builds ... declare one surface"). It is not a new entry point.
- **Law (b): yes.** Both rows call only `fl_binding_variant` and `fl_binding_abi_version` from `binding.h`, and each executable links one shim artefact, as the ladder's ground rules require.
- **First call: observed by construction, and the test says so.** The row's first statement is the query. ctest gives each discovered row its own process, and `test_probe_shim.cpp` has no static-init calls into the shim. In a plain all-rows gtest run it is not the first call, and the row's comment says so. Acceptable.
- **Flakiness: none.** No timing assertions. The release barrier only spins (`ready`/`go`). Results are counted per thread and asserted after `join`.
- **Point (1), L6 "never silent": no rung needed.** No c-abi test sees 7→8, but the handshake that refuses a mismatched minor is already `[built]` with T2, and the brief's control exercises it. "Every change bumps" stays `[built as a property; no test]`. That gap is older than this stage, and pinning the literal 8 would only pin a number. This is not a new VALUE subject.
- **Scheduled restoration:** none. **Tier-1 disagreement:** none.
- **Laws without a test after this stage (gaps, not findings):** L8, L6's append-only/bump rules, and L3's OUT-untouched clause. These are unchanged. L16 becomes edge-tested here, and its server half is D9S-2's.
