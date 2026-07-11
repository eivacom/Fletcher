# Codex Review: GIR-10 Codec Edge/Boundary + Scalar-Leaf Nested-List Enablement

**Target:** branch diff against 2dc579fb0e1e23ae3baeab590c95770c43d771b7

## Summary

The patch exposes scalar-leaf nested lists through the shared FieldMapping path without updating or guarding the accessor/Rust emitters that still assume struct leaves. This can break code generation for users who enable those outputs.

## Findings by Severity

### BLOCKING (P1)

1. **Reject scalar-leaf nested lists from accessor generation**
   - **File:** `protoc/src/type_mapper.cpp:227-231`
   - **Issue:** When a user runs `protoc` with `--fletcher_opt=accessor` or `rust` on a proto containing `repeated` flattened scalar wrappers, this new mapping makes the field a supported `NESTED_LIST` but leaves `nested_class`/`nested_msg` empty. The recordbatch accessor emitter still assumes every supported nested list has a struct leaf and only checks depth, so it will generate invalid accessor references instead of skipping or handling the scalar leaf; the new test fixture avoids this only by omitting those options.

## Recommendation

The scalar-leaf nested-list support must be gated or the accessor/Rust emitter must be hardened to detect and reject/skip scalar leaves before generating code.
