// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

#include <google/protobuf/descriptor.h>

#include <optional>
#include <string>

namespace fletcher {

namespace ir {
struct IrNode;
}  // namespace ir

// Describes a scalar (or scalar-like) Arrow type: enough information for the
// code generator to emit setters, builders, and scalar constructors.
struct ScalarTypeInfo {
    std::string arrow_type_expr;   // e.g. "arrow::int32()"
    std::string storage_type;      // C++ type for std::optional<>/vector<> — e.g. "int32_t"
    std::string param_type;        // setter param — may differ (e.g. "std::string_view")
    std::string scalar_ctor;       // format string with {val} token
    std::string default_value;     // proto3 zero-default as C++ literal
    std::string builder_type;      // e.g. "arrow::Int32Builder"
    std::string scalar_type;       // e.g. "arrow::Int32Scalar" — for downcasting when decoding
    bool value_is_buffer = false;  // true for string/binary (value->ToString())
};

enum class FieldKind {
    SCALAR,           // Simple scalar: int32, string, bool, enum, well-known wrappers
    REPEATED_SCALAR,  // repeated int32 → list(int32)
    STRUCT,           // Nested message → struct<...>
    REPEATED_STRUCT,  // repeated Message → list(struct<...>)
    NESTED_LIST,      // List<List<...<Struct>>> — depth-parameterised nested list
    MAP,              // map<K,V>
};

struct FieldMapping {
    FieldKind kind;
    bool nullable;
    // DICT-2 (D6): NOT RENDERED ANYWHERE. Post-GIR-5 nothing reads this member
    // (nor ir::FieldFacts.warning, which feeds it) — the eleven writers are the
    // only traffic. Treat it as a dead channel: do not route a new diagnostic
    // through it (DICT-2 deliberately made its conflict rule a fatal front-end
    // error instead of inventing a warning nobody reads).
    std::string warning;

    // SCALAR kind:
    ScalarTypeInfo scalar;

    // DICT-2 (spec section 5): DERIVED projection of ir::FieldFacts.dictionary,
    // the ONE canonical carrier (locked #5). NOT a second source of truth: never
    // written from a descriptor read, never written by an emitter. `scalar` stays
    // the VALUE type (locked #7) — nothing here changes storage/setter/getter/
    // wire/TS behaviour.
    // Set ONLY when kind == FieldKind::SCALAR: locked #9 makes the option legal
    // only there, ValidateDictionaryDeclarations rejects every other kind
    // fatally before any emitter runs, and a `list(dictionary(...))` carrier is
    // explicitly out of scope (spec section 8).
    // NAMED CONSUMER — round RIR's IR-based RecordBatch accessor. Spec section
    // 5.1 specifies its type gate in EXACTLY this spelling:
    // arrow::dictionary(<dict_index_type_expr>, <scalar.arrow_type_expr>).
    // These fields CANNOT reach today's read-only RBA emitter: DICT-1.5 fails the
    // plugin for --fletcher_opt=accessor,rust on any dictionary proto (locked
    // #11), and RIR removes that guard in the same change that consumes them.
    bool is_dictionary = false;
    std::string dict_index_type_expr;  // e.g. "arrow::int16()"; empty iff !is_dictionary

    // REPEATED_SCALAR kind — describes the list element:
    ScalarTypeInfo element;

    // STRUCT / REPEATED_STRUCT / NESTED_LIST kind:
    std::string nested_class;   // C++ type reference (globally qualified when cross-file)
    std::string nested_header;  // non-empty → #include this path (cross-file dependency)
    int list_depth = 0;         // NESTED_LIST: 2 = List<List<leaf>>, 3 = List<List<List<leaf>>>
    // NESTED_LIST kind, GIR-10: when true the innermost leaf is a SCALAR (the
    // scalar type is carried in `element`, like REPEATED_SCALAR), not a message
    // struct (`nested_class`). This scalar-leaf nested-list shape is NEVER fed to
    // the read-only RBA accessor emitter (locked #3): the fixture that carries it
    // (ScalarNestedCoverage) is generated without the accessor/rust opts, so RBA
    // only ever sees struct-leaf nested lists and its behaviour is unchanged.
    bool nested_leaf_is_scalar = false;
    // Descriptor behind nested_class — the message whose schema the generated
    // code references via <nested_class>Schema(). Used by the in-process
    // schema builder (--fletcher_opt=ipc) to build the same schema directly.
    const google::protobuf::Descriptor* nested_msg = nullptr;

    // MAP kind:
    ScalarTypeInfo map_key;
    ScalarTypeInfo map_value;      // populated when value is a scalar type
    std::string map_value_class;   // C++ type reference (globally qualified when cross-file)
    std::string map_value_header;  // non-empty → #include this path (cross-file dep)
    bool map_value_is_message = false;
    // Descriptor behind map_value_class (see nested_msg above).
    const google::protobuf::Descriptor* map_value_msg = nullptr;
};

// Classify a proto field and return enough information to generate Arrow code.
// Returns nullopt for unsupported constructs (oneof, recursive, etc.).
//
// GIR-3: MapField() is now a thin bridge over the canonical IR — it is
// ProjectIrToFieldMapping(ir::BuildFieldIr(field), field->file()). There is no
// second, independent classifier: RBA / decode / schema / view / TS all consume
// FieldMapping derived from the same BuildFieldIr() source, so they cannot drift.
std::optional<FieldMapping> MapField(const google::protobuf::FieldDescriptor* field);

// Canonical projection of a language-neutral IR node onto the (temporary) flat
// FieldMapping bridge that the not-yet-migrated emitters consume. Returns nullopt
// for Unsupported nodes and for IR shapes the flat model cannot represent.
//
// Scalar-leaf nested lists (List<List<...<Scalar>>>) ARE representable as of
// GIR-10: they project onto `element` plus the additive `nested_leaf_is_scalar`
// discriminator, so this returns a mapping for them rather than nullopt. The
// struct-leaf counterpart uses `nested_class`. That shape never reaches the
// read-only RBA accessor — a front-end guard
// (ValidateBackendsSupportFields/FindScalarLeafNestedList in generator.cpp)
// rejects `--fletcher_opt=accessor,rust` for such protos until round RIR.
//
// The same front-end pass ALSO guards a (fletcher.dictionary) field (DICT-1.5,
// locked #11, ValidateBackendsSupportFields/FindDictionaryField in
// generator.cpp) for the same two backends and the same reason: the RBA
// emitters assume a value-typed column, not the dictionary(idx, val) column
// DICT-3 will emit. This comment names both guarded shapes so it does not read
// as this pass's full inventory.
//
// Edge ENCODE does NOT use this — it walks the IR.
std::optional<FieldMapping> ProjectIrToFieldMapping(
    const ir::IrNode& node, const google::protobuf::FileDescriptor* context_file);

// Human-readable explanation of why a field is unsupported (legacy boundary text).
std::string UnsupportedReason(const google::protobuf::FieldDescriptor* field);

// -----------------------------------------------------------------------
// DICT-2: (fletcher.dictionary) legality (spec sections 4/6, locked #8/#9)
// -----------------------------------------------------------------------
// These two are PURE PREDICATES with no side effects; generator.cpp's
// ValidateDictionaryDeclarations turns a non-nullopt answer into protoc's
// *error and fails the plugin BEFORE any artifact is written. Rejection is
// deliberately NOT `MapField -> nullopt` (design D0): nullopt becomes a
// `skipped_comment` and generation continues at exit 0, i.e. a SILENT DROPPED
// COLUMN, and it would drift the projection against IsSchemaRepresentable.
//
// REJECTION ONLY. Both read descriptors directly (HasFieldDictionary /
// ReadFieldDictionaryOption) because the conditions R4/R5 test are NOT visible
// on any IR node — ir.cpp keeps only the winner of a flatten-chain conflict and
// BuildFlattenedRepeated never reads the inner field at all. Do NOT copy that
// pattern into an emitter: dictionary-ness for MAPPING and EMISSION stays
// IR-derived from ir::FieldFacts.dictionary (design D8), which is what keeps
// DICT-1.5's "guard-inspected superset of emittable" property intact.

// Why a (fletcher.dictionary) declaration reachable from `field` is illegal, or
// nullopt when it is legal. `field` must be a field that would actually become a
// column of a generated message (a message's own declared field, or a field
// inlined through a (fletcher.flatten_field) wrapper — see
// FindIllegalDictionaryField).
std::optional<std::string> DictionaryUnsupportedReason(
    const google::protobuf::FieldDescriptor* field);

// First illegal declaration among `msg`'s columns, in declaration order. Descends
// through (fletcher.flatten_field) wrappers exactly as the two inlining walks do,
// AND into a singular-message child / list element / map value -- positions where
// cpp_backend_schema_visitor's DeepCopyMessageStruct emits a child message's OWN
// schema function, so those fields really are columns. It does NOT reach such a
// position when the child is behind a (fletcher.flatten) WRAPPER hop (which
// includes every nested-list struct leaf); that hole is disclosed in
// docs/dictionary-option-spec.md section 7.1.1 and is NOT fixable by descending
// into the wrapper -- see the exclusion note below.
// NEVER descends into an IsFlattenedWrapper (or IsRecursive) message: no schema
// function is generated for a wrapper, so nothing inlines its fields,
// (fletcher.flatten_field) inside one is a no-op, and its (fletcher.dictionary) is
// RESOLVED AND HONOURED -- applying rule R1 there would be a false positive on a
// working proto (pinned by ctest
// GenErrors.DictionaryLiveInsideFlattenWrapperAccepted; see
// docs/dictionary-option-spec.md section 7.1.1 for the boundary and the two
// shapes it deliberately leaves silent).
// nullopt when `msg` is clean.
std::optional<std::string> FindIllegalDictionaryField(const google::protobuf::Descriptor* msg);

// True if the field is nullable (proto3 `optional` keyword, or proto2 optional).
bool IsFieldNullable(const google::protobuf::FieldDescriptor* field);

// True if the message carries the (fletcher.flatten) message option.
bool HasMessageFlatten(const google::protobuf::Descriptor* msg);

// True if the message (directly or transitively) references itself.
bool IsRecursive(const google::protobuf::Descriptor* msg);

// True if the message has (fletcher.flatten) = true and exactly one field.
// No class should be generated for these — their representation is absorbed
// into the enclosing message's Arrow schema.
bool IsFlattenedWrapper(const google::protobuf::Descriptor* msg);

// True if the field has [(fletcher.flatten_field) = true].
bool HasFieldFlatten(const google::protobuf::FieldDescriptor* field);

// Maximum struct-nesting depth starting from msg.
// A flat message (only scalar fields) has depth 0.
int NestingDepth(const google::protobuf::Descriptor* msg);

// C++ class name for the generated row wrapper (lives in fletcher_gen:: namespace).
// Handles nested messages: Outer.Inner → "Outer_Inner".
std::string ClassName(const google::protobuf::Descriptor* msg);

// C++ class name for the generated immutable view wrapper.
// Handles nested messages: Outer.Inner → "Outer_InnerView".
std::string ViewClassName(const google::protobuf::Descriptor* msg);

// Convert a dotted proto path ("foo.bar") to a C++ nested-namespace path
// ("foo::bar"). Single source of truth for the package→namespace transform,
// shared by the row generator, the type-mapper's cross-file references, and the
// RecordBatch accessor emitter (replaces the former DotToColons / DotToColonsTM /
// PackageToNamespace copies).
std::string DotToColons(const std::string& s);

// -----------------------------------------------------------------------
// C++ wire format helpers (for EncodeTo code generation)
// -----------------------------------------------------------------------

// WireTypeId hex literal for a scalar proto field type.
// Returns "" for unsupported types.  e.g. TYPE_BOOL → "0x01", TYPE_INT32 → "0x04".
std::string CppWireTypeIdHex(google::protobuf::FieldDescriptor::Type type);

// WireTypeId hex literal for a composite field kind.
// STRUCT → "0x20", LIST → "0x21", MAP → "0x24".
std::string CppWireTypeIdHex(FieldKind kind);

// -----------------------------------------------------------------------
// TypeScript code generation helpers
// -----------------------------------------------------------------------

// TypeScript type string for a scalar proto field type.
// Returns "" for unsupported types.  e.g. TYPE_BOOL → "boolean", TYPE_INT64 → "bigint".
std::string TsScalarType(google::protobuf::FieldDescriptor::Type type);

// WireTypeId enum member name for a scalar proto field type.
// Returns "" for unsupported types.  e.g. TYPE_BOOL → "WireTypeId.BOOL".
std::string WireTypeIdName(google::protobuf::FieldDescriptor::Type type);

// TypeScript interface name for a message: Outer.Inner → "IOuter_Inner".
std::string TsInterfaceName(const google::protobuf::Descriptor* msg);

}  // namespace fletcher
