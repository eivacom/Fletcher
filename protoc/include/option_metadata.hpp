// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

// Passthrough of third-party custom proto options into Arrow schema metadata
// (--fletcher_opt=metadata_from_option=...). Fletcher attaches no meaning to any
// option or key: a rule is a byte-copy from a caller-named option path to a
// caller-named Arrow metadata key. No option is interpreted, validated or
// normalised, and no domain vocabulary is built in.
//
// See docs/fletcher-options.md for the user-facing specification.

#include <google/protobuf/descriptor.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fletcher {

// One --fletcher_opt=metadata_from_option=<scope>:<expr>:<arrow_key> mapping.
struct MetadataRule {
    enum class Scope {
        kField,      // FieldOptions of the Arrow field's proto field  -> field metadata
        kFieldType,  // MessageOptions of that field's message type    -> field metadata
        kMessage,    // MessageOptions of the message itself           -> schema metadata
    };

    Scope scope = Scope::kField;
    // '/'-separated steps. Step 0 reads an extension on the scope's options
    // message; every later step reads an extension on the EnumValueOptions of
    // the enum value the previous step resolved to.
    std::vector<std::string> steps;
    std::string arrow_key;  // verbatim; may contain ':' (e.g. ARROW:extension:name)
    std::string source;     // original token, for diagnostics
};

// Extract every `metadata_from_option=` token from the comma-separated
// --fletcher_opt parameter. Tokens the plugin does not claim are left alone.
// Returns false and sets *error on a claimed token that does not parse, or one
// targeting a reserved key (proto_package / proto_message / field_number /
// field_id) — those four are emitted by the generator and may not be replaced.
bool ParseMetadataRules(const std::string& parameter, std::vector<MetadataRule>* out,
                        std::string* error);

// Resolves rules against descriptors. Reads options by reflection over a
// DynamicMessage built from the DescriptorPool protoc populated from the
// CodeGeneratorRequest, so enum value names and enum-value options are reachable
// without the plugin linking the declaring .proto.
class OptionMetadataResolver {
   public:
    // Compiles `rules` against `pool`. Anything statically determinable is
    // validated here and reported as a hard error: unknown sub-field, a '/' hop
    // off a non-enum, a message-typed terminal, or an extension whose extendee
    // disagrees with the declared scope. An extension that is simply not present
    // in `pool` is NOT an error — one flag list is applied to a whole corpus and
    // only some files import the declaring .proto — that rule is dropped.
    // Returns nullptr and sets *error on failure.
    static std::unique_ptr<OptionMetadataResolver> Create(
        std::vector<MetadataRule> rules, const google::protobuf::DescriptorPool* pool,
        std::string* error);
    ~OptionMetadataResolver();

    OptionMetadataResolver(const OptionMetadataResolver&) = delete;
    OptionMetadataResolver& operator=(const OptionMetadataResolver&) = delete;

    // Schema-level pairs for `msg` (kMessage rules).
    std::vector<std::pair<std::string, std::string>> ForMessage(
        const google::protobuf::Descriptor* msg) const;

    // Field-level pairs (kField / kFieldType rules). `flatten_chain` is
    // FieldInfo::flatten_chain: the outer→inner (fletcher.flatten_field) wrappers
    // this leaf was inlined through. Candidates are tried leaf-first, then the
    // chain innermost→outermost, so the most specific declaration wins.
    std::vector<std::pair<std::string, std::string>> ForField(
        const google::protobuf::FieldDescriptor* leaf,
        const std::vector<const google::protobuf::FieldDescriptor*>& flatten_chain) const;

   private:
    class Impl;
    explicit OptionMetadataResolver(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Escape `s` for use as the body of a narrow C++ string literal (the surrounding
// quotes are NOT added). Output is pure ASCII: every byte below 0x20 or at/above
// 0x7F becomes a 3-digit octal escape, so the emitted header does not depend on
// the consuming compiler's source encoding.
//
// Octal, never \x: C++ hex escapes consume an unbounded run of hex digits, so a
// 0x01 byte followed by a literal 'A' would emit "\x01A" and be read back as one
// character. Octal escapes stop after three digits.
std::string EscapeCppStringLiteral(const std::string& s);

}  // namespace fletcher
