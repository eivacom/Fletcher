// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "option_reader.hpp"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/unknown_field_set.h>

#include <string>

namespace fletcher {

namespace {

using google::protobuf::Descriptor;
using google::protobuf::DescriptorPool;
using google::protobuf::DynamicMessageFactory;
using google::protobuf::EnumValueDescriptor;
using google::protobuf::FieldDescriptor;
using google::protobuf::Message;
using google::protobuf::Reflection;
using google::protobuf::UnknownField;
using google::protobuf::UnknownFieldSet;

constexpr const char* kDictExtName = "fletcher.dictionary";
constexpr int kDictExtNumber = 50001;
constexpr const char* kFieldOptions = "google.protobuf.FieldOptions";

// The (fletcher.dictionary) extension AS THE POOL KNOWS IT, or nullptr.
//
// This is the ONLY evidence the reader acts on: a bare length-delimited field
// numbered 50001 is deliberately NOT enough (50000/50001 sit in the
// collision-prone internal range, so trusting the number alone would let a
// foreign option fabricate a dictionary column nobody declared).
//
// STRICTNESS ASYMMETRY, recorded (step-4b P2-10): the neighbouring flatten
// reader (`FindBoolOption` in type_mapper.cpp) still matches
// (fletcher.flatten)/(fletcher.flatten_field) by BARE NUMBER 50000, which
// test_schema_visitor.cpp documents as deliberate. So the threat model cited
// above is mitigated for `dictionary` and NOT for `flatten`. Migrating that
// reader is explicitly out of DICT-1's scope (it would put a working, tested
// path at risk); this note exists so the inconsistency is a known decision
// rather than an accident.
//
// The gate deliberately does NOT require
// `message_type()->full_name() == "fletcher.DictionaryOptions"`: the sub-field
// decode below is by NAME, so a renamed-but-compatible option message still
// reads correctly, and demanding the name would silently downgrade it to
// defaults. Residual limit, recorded: a fork that RENUMBERS the extension away
// from 50001 is unreadable — and by locked decision #2 it is not Fletcher's
// option any more.
const FieldDescriptor* ResolveDictionaryExtension(const DescriptorPool* pool) {
    const FieldDescriptor* ext = pool->FindExtensionByName(kDictExtName);
    if (ext == nullptr) return nullptr;
    if (ext->is_repeated()) return nullptr;
    if (ext->containing_type()->full_name() != kFieldOptions) return nullptr;
    if (ext->number() != kDictExtNumber) return nullptr;
    if (ext->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) return nullptr;
    return ext;
}

// Enum values are decoded by SYMBOL NAME, not by number (precedent:
// option_metadata.cpp's RenderSingular). That is what makes the reader
// forward-compatible with the POOL's fletcher/options.proto rather than the
// plugin's: a consumer pinning a version that renumbered the enum values is
// still read correctly. An unknown symbol falls back to the default index type.
ir::DictionaryIndexKind IndexKindFromSymbol(const std::string& symbol) {
    if (symbol == "DICTIONARY_INDEX_INT8") return ir::DictionaryIndexKind::INT8;
    if (symbol == "DICTIONARY_INDEX_INT16") return ir::DictionaryIndexKind::INT16;
    if (symbol == "DICTIONARY_INDEX_INT64") return ir::DictionaryIndexKind::INT64;
    // DICTIONARY_INDEX_UNSPECIFIED resolves to int32, and so does anything this
    // build does not recognise — silently (step-4b N-18): a future
    // DICTIONARY_INDEX_UINT8 read by an older plugin becomes int32 with no
    // diagnostic. Consistent with the fail-soft contract; surfacing it belongs to
    // the item that owns the error channel.
    return ir::DictionaryIndexKind::INT32;
}

// Read the DictionaryOptions sub-fields off `d` BY NAME. Every missing,
// wrong-typed or unrecognised piece falls back to the default, so a payload the
// plugin only partly understands degrades one field at a time.
void DecodeDictionaryOptions(const Message& d, DictionaryOption* out) {
    const Descriptor* desc = d.GetDescriptor();
    const Reflection* refl = d.GetReflection();

    const FieldDescriptor* index_type = desc->FindFieldByName("index_type");
    if (index_type != nullptr && !index_type->is_repeated() &&
        index_type->cpp_type() == FieldDescriptor::CPPTYPE_ENUM) {
        // GetEnumValue yields the field's default (UNSPECIFIED) when unset, and
        // an undeclared number never lands here at all: a closed proto2 enum
        // pushes it into the submessage's UnknownFieldSet.
        const EnumValueDescriptor* evd =
            index_type->enum_type()->FindValueByNumber(refl->GetEnumValue(d, index_type));
        if (evd != nullptr) out->index_kind = IndexKindFromSymbol(evd->name());
    }

    const FieldDescriptor* ordered = desc->FindFieldByName("ordered");
    if (ordered != nullptr && !ordered->is_repeated() &&
        ordered->cpp_type() == FieldDescriptor::CPPTYPE_BOOL) {
        out->ordered = refl->GetBool(d, ordered);
    }
}

// Degraded PRESENCE PROBE — reached only when the extension resolved and passed
// the gate but its blob did not re-parse (a truncated/garbage payload). It
// decodes nothing, so it does not reconstitute the unknown-field walker this
// design retired; it only answers "was a dictionary declared here at all", so a
// declared-but-corrupt option resolves to defaults instead of silently
// disappearing.
bool HasLengthDelimitedDictionaryField(const Message& opts) {
    const UnknownFieldSet& unknown = opts.GetReflection()->GetUnknownFields(opts);
    for (int i = 0; i < unknown.field_count(); ++i) {
        const UnknownField& uf = unknown.field(i);
        if (uf.number() == kDictExtNumber && uf.type() == UnknownField::TYPE_LENGTH_DELIMITED) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::unique_ptr<google::protobuf::Message> ReparseOptionsWithPool(
    const google::protobuf::Message& opts,
    const google::protobuf::Descriptor* pool_options_descriptor,
    google::protobuf::DynamicMessageFactory* factory) {
    if (pool_options_descriptor == nullptr || factory == nullptr) return nullptr;

    // The linked-in options message carries the un-linked custom option in its
    // UnknownFieldSet. Serializing round-trips those bytes; the DynamicMessage
    // re-parses them as a real extension because ITS pool knows the extension.
    //
    // SerializeToString (checked), not SerializeAsString (step-4b P2-8): the
    // latter returns a partial/empty string on failure with no signal, and an
    // empty string then PARSES successfully into an empty message — which would
    // report "option absent" instead of routing the caller to its fail-soft path.
    std::string bytes;
    if (!opts.SerializeToString(&bytes)) return nullptr;

    std::unique_ptr<google::protobuf::Message> dyn(
        factory->GetPrototype(pool_options_descriptor)->New());
    if (!dyn->ParseFromString(bytes)) return nullptr;
    return dyn;
}

std::optional<DictionaryOption> ReadFieldDictionaryOption(
    const google::protobuf::FieldDescriptor* field) {
    if (field == nullptr) return std::nullopt;

    // Fast path: an options-less field shares protobuf's default instance, so
    // the common field pays nothing. (This also counts unknown fields, so a
    // field carrying ANY option falls through to the full path — correct, just
    // not free. ByteSizeLong writes the shared default instance's cached size;
    // benign, and the plugin is single-threaded.)
    const google::protobuf::Message& opts = field->options();
    if (opts.ByteSizeLong() == 0) return std::nullopt;

    const FieldDescriptor* ext = ResolveDictionaryExtension(field->file()->pool());
    if (ext == nullptr) return std::nullopt;

    // LIFETIME: the factory MUST be declared before — and therefore destroyed
    // after — the message it creates. Per-call, never static: a cached factory
    // would keep raw pointers into pools it does not own, and the plugin/tests
    // build and destroy many DescriptorPools.
    DynamicMessageFactory factory;
    std::unique_ptr<Message> dyn = ReparseOptionsWithPool(opts, ext->containing_type(), &factory);
    if (!dyn) {
        // GRANULARITY, recorded (step-4b SF-3): because protobuf refuses two
        // extensions of the same extendee at the same number in one pool, no
        // FOREIGN option can be declared at 50001 in a pool that declares
        // fletcher.dictionary — so the no-false-positive property of the gate is
        // defended at POOL-DECLARATION granularity, not per field. Once the pool
        // declares the extension, this probe trusts a bare
        // "50001 + LENGTH_DELIMITED" record and answers "declared, defaults".
        if (HasLengthDelimitedDictionaryField(opts)) return DictionaryOption{};
        return std::nullopt;
    }

    const Reflection* refl = dyn->GetReflection();
    // `dyn` is built from ext->containing_type(), so HasField(*dyn, ext) is safe:
    // protobuf CHECKs field->containing_type() == descriptor_, and here they are
    // the same object BY CONSTRUCTION. A "tidier" refactor that fetched the
    // FieldOptions descriptor from the pool by name could turn this into an abort.
    //
    // PRESENCE is the trigger, not truthiness (locked decision #1): `= {}` is a
    // dictionary with defaults, and a zero-length submessage still sets presence.
    if (!refl->HasField(*dyn, ext)) return std::nullopt;

    DictionaryOption out;
    DecodeDictionaryOptions(refl->GetMessage(*dyn, ext, &factory), &out);
    return out;  // every value copied out before `factory` dies
}

bool HasFieldDictionary(const google::protobuf::FieldDescriptor* field) {
    return ReadFieldDictionaryOption(field).has_value();
}

}  // namespace fletcher
