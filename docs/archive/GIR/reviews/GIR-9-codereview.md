# Codex Review: GIR-9 (C++ enum symbols + typed accessors)

**Date:** 2026-07-11  
**Target:** branch diff against 9b6e36eb9588e9c7e56f03cd2a3f7f4444220de7  
**Scope:** Enum-class emission, CppEnumName, typed accessors, topological ordering, include collection, test coverage

---

## Summary

The enum accessor support misses imported enums reached through field-level flattening and can also recurse indefinitely on cyclic nested-enum owner references. These are **correctness issues** in generated-code production for valid schema shapes.

---

## Findings

### P2 (Should-Fix) — Include enum headers for flattened imported fields

**Location:** `protoc/src/generator.cpp:131-133`

**Issue:** When a field-level flatten inlines fields from an imported message, `GatherFieldsImpl` can emit typed enum accessors for the imported inner enum, but the include-collection pass only checks the top-level field itself. In that scenario the top-level field is a message type, so no enum header is added. The generated row header then references an enum from the imported `.fletcher.pb.h` without including it, causing compile failures for flattened imported enum fields.

**Impact:** Schemas with flattened imported enum fields will fail to compile.

---

### P2 (Should-Fix) — Break cycles in enum-owner ordering

**Location:** `protoc/src/generator.cpp:187-187`

**Issue:** For schemas where two messages each have a field of the other's nested enum, neither message is marked emitted before recursing through enum-owner dependencies. Thus `TopologicalVisit(A)` calls `TopologicalVisit(B)` which calls back into `TopologicalVisit(A)` until stack overflow. Even if that schema is unsupported, generation should detect the cycle or fail cleanly rather than recursing indefinitely.

**Impact:** Certain cyclic schema shapes cause stack overflow / infinite recursion during code generation.

---

## Review Notes

Codex encountered policy-blocked tool invocations during deeper cross-file analysis (grep, file walking). The two P2 findings above represent high-confidence issues identified from available diff surface and generator structure. Additional validation across the full enum-owner graph and header-include closure would benefit from manual inspection of the topological-visit and include-gathering implementations.

