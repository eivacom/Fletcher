# GIR-4 Conformance Review
Date: 2026-07-10
Target: feature/generator-ir-rewrite branch, working-tree changes vs base 78a0a0a
Design: plans/GIR-4-edge-decode-on-ir.md
Locked Decisions: plans/GIR-locked-decisions.md

VERDICT: CONFORMS

All six pressure-test points pass. Migration is clean, byte/value-identical, properly scoped, and well-tested.

PRESSURE-TEST ANALYSIS

1. DECISION #1 — Language-Neutral IR: PASS
   - ir.hpp: No diff; IR node definitions unchanged
   - cpp_backend_decode_visitor.cpp L35-36: ReadMethod() calls LookupScalar().positional_read (backend lookup)
   - cpp_backend_type_table.hpp L36: CppScalarInfo::positional_read field exists
   - StorageType() helper also uses backend lookup; IR remains language-neutral

2. DECISION #2 — Byte/Value Identity: PASS
   - No golden files modified (git diff shows zero *.bin/*.ipc changes)
   - CoverageHarness extended with ReconstructAndCheck() helper (L236-252)
   - Encodes fixture → decodes via edge constructor → asserts field equality (nullable presence + values)
   - Asserts decode→encode byte-identity fixpoint
   - Loads committed goldens, reconstructs, asserts field equality and byte identity
   - 11 fixtures + goldens round-trip: MakeScalars, MakeComposite, alternates, Branch, Leaf, NestedEnums, FlattenedPoint, FieldFlattenedPosition, ServiceRequest, ServiceReply
   - Field equality substantive: ScalarCoverage 34+ fields, Branch nested structs, CompositeCoverage maps/nested-lists

3. DECISION #4 — Scope (Decode Only): PASS
   - EmitFieldDecode removed entirely (183 lines deleted)
   - Both constructors route to cpp_backend::EmitFieldDecodeFromIr
   - PositionalReadCall removed; logic moved to cpp_backend_type_table.cpp backend lookup
   - Schema/IPC/view/TS/RBA emitters untouched (no diffs)
   - FieldKind projection unchanged

4. DECISION #3/#4 — Bridge Accounting: PASS
   - Encode on IR (GIR-3 baseline)
   - Decode on IR (GIR-4 this round)
   - Bridge still active: FieldMapping consumed by schema/IPC/view/TS/RBA
   - FieldKind not deleted

5. Dead-Code Removal: PASS
   - EmitFieldDecode deleted cleanly (was called in 2 places, both now routed to IR visitor)
   - PositionalReadCall deleted cleanly (logic replicated via backend lookup)
   - No forward declarations, no extern, no orphaned stubs
   - CMakeLists.txt updated: cpp_backend_decode_visitor.cpp added

6. Tests Substantive: PASS

Unit Test (test_ir.cpp L458-627):
   - Constructs proto schema (Inner, StructListWrapper, Host)
   - Builds IR nodes, asserts actual generated C++ code via DecodeOf()
   - (a) Non-nullable scalar: "i32_ = r.ReadInt32();\n" + backend lookup
   - (b) Nullable scalar: null gate if (!r.IsNull(...))
   - (c) String copy: std::string(r.ReadString())
   - (d) Binary copy: auto [p,n] = r.ReadBinary(); by_.emplace(reinterpret_cast<const char*>(p), n)
   - (e) Non-nullable struct: { auto sr = r.ReadStruct(...); inner_.emplace(sr); }
   - (f) Nullable struct: if (!r.IsNull(...)) { auto sr = r.ReadStruct(...); oi_.emplace(sr); }
   - (g) Repeated struct (single-level): ReadListHeader, clear, reserve, loop with emplace_back(sr); NO nullable gate
   - (h) Map: key-first → ReadMapValueBitfield(count) CRITICAL → value-second; moved key
   - (i) Nested list: fresh loop vars (lh_0, i_0, lh_1, i_1); resize + indexed assign [i_0][i_1]; no push_back; struct innermost
   - (j) Unsupported: diagnostic comment, not silent decode body
   All assertions exact strings or positional checks; not vacuous

Coverage Harness (test_coverage_harness.cpp L84-252):
   - ExpectEquals() overloads for all message types
   - ScalarCoverage: 34+ field assertions
   - Branch: nested struct, optional struct, repeated structs
   - CompositeCoverage: maps (scalar/struct value), nested lists (depth 2/3), flattened lists
   - ReconstructAndCheck(): encode → decode via edge constructor → field equality → byte-identity fixpoint
   - Repeats for golden bytes
   - 11 fixtures + committed goldens

LOCKED-DECISION REGRESSION CHECK

1. Language-neutral IR (#1): PASS — No C++ strings in IR; backend lookup
2. Wire bytes unchanged (#2): PASS — No golden files; round-trip guarded
3. RBA read-only (#3): PASS — Emitter untouched; FieldKind unchanged
4. Emitter-by-emitter (#4): PASS — Decode oracle in place
5. Schema/IPC unified (#5): PASS — Out of scope; bridge untouched
6. Fixes into rewrite (#6): PASS — No wire corruption fix
7. Enum/Dictionary (#7): PASS — No model change
8. Edge codec strategy (#8): PASS — Bespoke edge kept; descriptor-driven not foreclosed
9. Red-first tests (#9): PASS — Forcing test red-first
10. Scope (#10): PASS — Generator + tests + harness
11. Base after HARD (#11): PASS — 78a0a0a is post-HARD

SUMMARY

GIR-4 implementation:
✓ Migrates edge C++ decode to IR-driven visitor (EmitFieldDecodeFromIr)
✓ IR language-neutral; read-method from C++ backend lookup only
✓ Deletes EmitFieldDecode (183 lines) and PositionalReadCall cleanly
✓ Routes both constructors to IR visitor; no dangling references
✓ Preserves FieldMapping bridge for schema/IPC/view/TS/RBA
✓ Guards byte/value identity with extended CoverageHarness (11 fixtures + goldens)
✓ Tests substantive: shape assertions + full round-trip equality
✓ No golden files modified; no scope creep
✓ Conforms with all locked decisions

This implementation faithfully implements GIR-4 design and passes all conformance gates.
