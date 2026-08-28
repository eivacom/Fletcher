// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

// DICT-1: typed reading of FLETCHER'S OWN custom options off a descriptor.
//
// Split against the neighbouring module — the two are deliberately different
// jobs sharing one mechanism:
//   * `option_metadata` copies THIRD-PARTY options into Arrow metadata as
//     STRINGS, driven by --fletcher_opt=metadata_from_option rules;
//   * `option_reader` (this file) reads FLETCHER'S own options into TYPED
//     values.
// Both share the one re-parse primitive below, so there is exactly one
// implementation of the "custom option the plugin does not link" trick.
//
// The plugin does NOT link a generated `fletcher/options.pb.cc` (locked decision
// #10): custom options therefore arrive in the linked-in options message's
// UnknownFieldSet, and are recovered by re-parsing those bytes into a
// DynamicMessage built from the DescriptorPool protoc populated from the
// CodeGeneratorRequest — which DOES know the extension.

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#include <memory>
#include <optional>

#include "ir.hpp"

namespace fletcher {

// Re-parse `opts` (a linked-in descriptor.pb options message; custom options the
// plugin does not link live in its UnknownFieldSet) as a DynamicMessage of
// `pool_options_descriptor` — the SAME options type as seen in the DescriptorPool
// protoc built from the CodeGeneratorRequest, which does know the extensions.
// Returns nullptr if the blob does not re-parse (or if either pointer is null).
//
// LIFETIME (load-bearing): the result is created from a `factory` prototype and
// MUST be destroyed BEFORE `factory`. Callers therefore declare the factory
// FIRST and the returned message (or its owner) after it.
//
// PRECONDITIONS (step-4b P2-7 / P2-9):
//   * `pool_options_descriptor` and `factory` must be non-null. Passing null is
//     a caller bug; this returns nullptr rather than crashing inside protobuf.
//   * FLAT POOL: a default-constructed DynamicMessageFactory resolves extensions
//     through `pool_options_descriptor->file()->pool()`. That must be the SAME
//     pool the caller looked the extension up in — true for protoc's own request
//     pool (one flat pool per CodeGeneratorRequest) and for the test pools, but
//     NOT automatically true for a layered pool with an underlay. With a layered
//     pool the extension would silently stay in the dynamic message's unknown
//     fields and read as absent.
std::unique_ptr<google::protobuf::Message> ReparseOptionsWithPool(
    const google::protobuf::Message& opts,
    const google::protobuf::Descriptor* pool_options_descriptor,
    google::protobuf::DynamicMessageFactory* factory);

// The decoded (fletcher.dictionary) option. Absence is expressed by the caller's
// std::optional, never by a member: presence is the trigger (locked #1), so an
// explicitly empty `= {}` yields this struct with its defaults.
struct DictionaryOption {
    ir::DictionaryIndexKind index_kind = ir::DictionaryIndexKind::INT32;
    bool ordered = false;
};

// Typed read of [(fletcher.dictionary) = {...}] on `field`. nullopt == option
// absent. Never fails: a DECLARED but unreadable option resolves to defaults
// (the reader is called from ir::BuildFieldIr, which has no error channel, and
// dropping a declared dictionary would emit a value-typed column for a field the
// author declared dictionary — the worst outcome).
std::optional<DictionaryOption> ReadFieldDictionaryOption(
    const google::protobuf::FieldDescriptor* field);

// Presence only. Equivalent to ReadFieldDictionaryOption(field).has_value().
// Prefer `node.facts.dictionary` when you already hold an IR node: this overload
// re-does the reflection read and, being descriptor-based, cannot see a
// dictionary that reached a node through flatten propagation. It exists for
// callers that have no IR node (e.g. a raw-descriptor validation walk).
bool HasFieldDictionary(const google::protobuf::FieldDescriptor* field);

}  // namespace fletcher
