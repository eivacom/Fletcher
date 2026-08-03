// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "option_metadata.hpp"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fletcher {

namespace {

using google::protobuf::Descriptor;
using google::protobuf::DescriptorPool;
using google::protobuf::EnumValueDescriptor;
using google::protobuf::FieldDescriptor;
using google::protobuf::Message;
using google::protobuf::Reflection;

constexpr const char* kFieldOptions = "google.protobuf.FieldOptions";
constexpr const char* kMessageOptions = "google.protobuf.MessageOptions";
constexpr const char* kEnumValueOptions = "google.protobuf.EnumValueOptions";

constexpr const char* kRuleToken = "metadata_from_option=";

// The four keys the generator always emits. A rule may not target them: the
// issue requires the mapped pairs to be appended, never to replace these, and
// ArrowMetadataBuilderAppend does not de-duplicate.
bool IsReservedKey(const std::string& key) {
    return key == "proto_package" || key == "proto_message" || key == "field_number" ||
           key == "field_id";
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::string cur;
    for (const char c : s) {
        if (c == sep) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    return parts;
}

// A compiled step: the extension to read, then a path of sub-fields within it.
// `terminal` is the field the step's value (or enum hop) comes from — the last
// path entry, or the extension itself when the extension is not message-typed.
struct CompiledStep {
    const FieldDescriptor* ext = nullptr;
    std::vector<const FieldDescriptor*> path;  // non-terminal entries are singular messages
    const FieldDescriptor* terminal = nullptr;
};

struct CompiledRule {
    MetadataRule::Scope scope = MetadataRule::Scope::kField;
    std::string arrow_key;
    std::vector<CompiledStep> steps;
};

const char* RequiredExtendee(MetadataRule::Scope scope, size_t step_index) {
    if (step_index > 0) return kEnumValueOptions;
    return scope == MetadataRule::Scope::kField ? kFieldOptions : kMessageOptions;
}

// Render a singular value. Returns false for a type Fletcher refuses to
// stringify; the caller turns that into a hard error at compile time.
bool RenderSingular(const Message& msg, const FieldDescriptor* f, std::string* out) {
    const Reflection* refl = msg.GetReflection();
    switch (f->cpp_type()) {
        case FieldDescriptor::CPPTYPE_STRING:
            *out = refl->GetString(msg, f);
            return true;
        case FieldDescriptor::CPPTYPE_BOOL:
            *out = refl->GetBool(msg, f) ? "true" : "false";
            return true;
        case FieldDescriptor::CPPTYPE_INT32:
            *out = std::to_string(refl->GetInt32(msg, f));
            return true;
        case FieldDescriptor::CPPTYPE_INT64:
            *out = std::to_string(refl->GetInt64(msg, f));
            return true;
        case FieldDescriptor::CPPTYPE_UINT32:
            *out = std::to_string(refl->GetUInt32(msg, f));
            return true;
        case FieldDescriptor::CPPTYPE_UINT64:
            *out = std::to_string(refl->GetUInt64(msg, f));
            return true;
        case FieldDescriptor::CPPTYPE_ENUM: {
            // D4: the enum value's proto name, verbatim. Changing this to the
            // number or a transformed form is a change to THIS line only.
            const int number = refl->GetEnumValue(msg, f);
            const EnumValueDescriptor* evd = f->enum_type()->FindValueByNumber(number);
            *out = evd ? evd->name() : std::string();
            return true;
        }
        default:
            return false;  // message / float / double
    }
}

bool RenderRepeatedElement(const Message& msg, const FieldDescriptor* f, int index,
                           std::string* out) {
    const Reflection* refl = msg.GetReflection();
    switch (f->cpp_type()) {
        case FieldDescriptor::CPPTYPE_STRING:
            *out = refl->GetRepeatedString(msg, f, index);
            return true;
        case FieldDescriptor::CPPTYPE_BOOL:
            *out = refl->GetRepeatedBool(msg, f, index) ? "true" : "false";
            return true;
        case FieldDescriptor::CPPTYPE_INT32:
            *out = std::to_string(refl->GetRepeatedInt32(msg, f, index));
            return true;
        case FieldDescriptor::CPPTYPE_INT64:
            *out = std::to_string(refl->GetRepeatedInt64(msg, f, index));
            return true;
        case FieldDescriptor::CPPTYPE_UINT32:
            *out = std::to_string(refl->GetRepeatedUInt32(msg, f, index));
            return true;
        case FieldDescriptor::CPPTYPE_UINT64:
            *out = std::to_string(refl->GetRepeatedUInt64(msg, f, index));
            return true;
        case FieldDescriptor::CPPTYPE_ENUM: {
            const int number = refl->GetRepeatedEnumValue(msg, f, index);
            const EnumValueDescriptor* evd = f->enum_type()->FindValueByNumber(number);
            *out = evd ? evd->name() : std::string();
            return true;
        }
        default:
            return false;
    }
}

bool IsRenderableTerminal(const FieldDescriptor* f) {
    switch (f->cpp_type()) {
        case FieldDescriptor::CPPTYPE_STRING:
        case FieldDescriptor::CPPTYPE_BOOL:
        case FieldDescriptor::CPPTYPE_INT32:
        case FieldDescriptor::CPPTYPE_INT64:
        case FieldDescriptor::CPPTYPE_UINT32:
        case FieldDescriptor::CPPTYPE_UINT64:
        case FieldDescriptor::CPPTYPE_ENUM:
            return true;
        default:
            // Float/double are refused deliberately: their textual form is
            // locale- and formatting-dependent, which would break the
            // byte-identity contract between the generated C++ and the .ipc
            // bytes across platforms. Message terminals mean the path is
            // incomplete.
            return false;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Rule parsing
// ---------------------------------------------------------------------------

bool ParseMetadataRules(const std::string& parameter, std::vector<MetadataRule>* out,
                        std::string* error) {
    for (const std::string& token : Split(parameter, ',')) {
        if (token.rfind(kRuleToken, 0) != 0) continue;  // not ours
        const std::string body = token.substr(std::string(kRuleToken).size());

        // <scope>:<expr>:<arrow_key> — split on the FIRST TWO colons only, so the
        // key keeps any colons of its own (ARROW:extension:name).
        const size_t c1 = body.find(':');
        const size_t c2 = (c1 == std::string::npos) ? std::string::npos : body.find(':', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) {
            *error = "invalid metadata_from_option '" + token +
                     "': expected <scope>:<option-path>:<arrow-key>";
            return false;
        }

        MetadataRule rule;
        rule.source = token;
        const std::string scope = body.substr(0, c1);
        if (scope == "field") {
            rule.scope = MetadataRule::Scope::kField;
        } else if (scope == "field_type") {
            rule.scope = MetadataRule::Scope::kFieldType;
        } else if (scope == "message") {
            rule.scope = MetadataRule::Scope::kMessage;
        } else {
            *error = "invalid metadata_from_option '" + token + "': unknown scope '" + scope +
                     "' (expected field, field_type or message)";
            return false;
        }

        const std::string expr = body.substr(c1 + 1, c2 - c1 - 1);
        rule.arrow_key = body.substr(c2 + 1);
        if (expr.empty()) {
            *error = "invalid metadata_from_option '" + token + "': empty option path";
            return false;
        }
        if (rule.arrow_key.empty()) {
            *error = "invalid metadata_from_option '" + token + "': empty Arrow metadata key";
            return false;
        }
        if (IsReservedKey(rule.arrow_key)) {
            *error = "invalid metadata_from_option '" + token + "': '" + rule.arrow_key +
                     "' is emitted by the generator and cannot be overwritten";
            return false;
        }

        for (const std::string& step : Split(expr, '/')) {
            if (step.empty()) {
                *error = "invalid metadata_from_option '" + token + "': empty step in option path";
                return false;
            }
            rule.steps.push_back(step);
        }
        out->push_back(std::move(rule));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Resolver
// ---------------------------------------------------------------------------

class OptionMetadataResolver::Impl {
   public:
    std::vector<CompiledRule> rules;
    // GetPrototype/GetMessage need a factory; the DEFAULT constructor is the
    // correct one here (the DescriptorPool-taking overload is deprecated in
    // protobuf 3.21). It resolves extensions against the pool in which the
    // extendee is defined, which is the pool we compiled the rules against.
    mutable google::protobuf::DynamicMessageFactory factory;
    // Parsed options keyed on the address of the source options message.
    // Descriptors are immutable for the pool's lifetime, and every
    // options-less descriptor shares one default instance that parses to empty.
    mutable std::map<const Message*, std::unique_ptr<Message>> cache;

    const Message* ParsedOptions(const Message& opts, const Descriptor* pool_opts_desc) const {
        auto it = cache.find(&opts);
        if (it != cache.end()) return it->second.get();

        // The linked-in options message carries the third-party extension in its
        // UnknownFieldSet (the plugin does not link the declaring .proto).
        // Serializing round-trips those bytes; the DynamicMessage re-parses them
        // as a real extension because its pool knows the extension.
        std::unique_ptr<Message> dyn(factory.GetPrototype(pool_opts_desc)->New());
        if (!dyn->ParseFromString(opts.SerializeAsString())) return nullptr;
        const Message* raw = dyn.get();
        cache.emplace(&opts, std::move(dyn));
        return raw;
    }

    // Evaluate one rule against one options message. Returns false when the rule
    // does not apply (option absent, sub-field unset, value empty) — never an
    // error: absence is normal and falls through to the next candidate/rule.
    bool Evaluate(const CompiledRule& rule, const Message& root_opts, std::string* value) const {
        const Message* cur_opts = ParsedOptions(root_opts, rule.steps[0].ext->containing_type());
        if (!cur_opts) return false;

        for (size_t si = 0; si < rule.steps.size(); ++si) {
            const CompiledStep& st = rule.steps[si];
            const Reflection* refl = cur_opts->GetReflection();
            if (!refl->HasField(*cur_opts, st.ext)) return false;

            // Descend to the message holding the terminal field.
            const Message* holder = cur_opts;
            if (st.ext->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
                holder = &refl->GetMessage(*cur_opts, st.ext, &factory);
                for (size_t pi = 0; pi + 1 < st.path.size(); ++pi) {
                    const FieldDescriptor* f = st.path[pi];
                    if (!holder->GetReflection()->HasField(*holder, f)) return false;
                    holder = &holder->GetReflection()->GetMessage(*holder, f, &factory);
                }
            }

            const bool last_step = (si + 1 == rule.steps.size());
            if (!last_step) {
                // Enum hop: switch context to the enum value's EnumValueOptions.
                const Reflection* hrefl = holder->GetReflection();
                if (!hrefl->HasField(*holder, st.terminal)) return false;
                const int number = hrefl->GetEnumValue(*holder, st.terminal);
                const EnumValueDescriptor* evd =
                    st.terminal->enum_type()->FindValueByNumber(number);
                if (!evd) return false;  // open proto3 enum, number not declared
                cur_opts = ParsedOptions(evd->options(), rule.steps[si + 1].ext->containing_type());
                if (!cur_opts) return false;
                continue;
            }

            // Terminal read.
            const Reflection* hrefl = holder->GetReflection();
            if (st.terminal->is_repeated()) {
                const int n = hrefl->FieldSize(*holder, st.terminal);
                if (n == 0) return false;
                std::string joined;
                for (int i = 0; i < n; ++i) {
                    std::string element;
                    if (!RenderRepeatedElement(*holder, st.terminal, i, &element)) return false;
                    if (i > 0) joined += ",";
                    joined += element;
                }
                if (joined.empty()) return false;
                *value = joined;
                return true;
            }
            if (!hrefl->HasField(*holder, st.terminal)) return false;
            std::string rendered;
            if (!RenderSingular(*holder, st.terminal, &rendered)) return false;
            // An empty rendered value counts as absent. This is what makes
            // fallback chains work under proto3 implicit presence.
            if (rendered.empty()) return false;
            *value = rendered;
            return true;
        }
        return false;
    }
};

OptionMetadataResolver::OptionMetadataResolver(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
OptionMetadataResolver::~OptionMetadataResolver() = default;

std::unique_ptr<OptionMetadataResolver> OptionMetadataResolver::Create(
    std::vector<MetadataRule> rules, const DescriptorPool* pool, std::string* error) {
    auto impl = std::make_unique<Impl>();

    for (const MetadataRule& rule : rules) {
        CompiledRule compiled;
        compiled.scope = rule.scope;
        compiled.arrow_key = rule.arrow_key;

        bool droppable = false;  // extension simply absent from this pool
        for (size_t si = 0; si < rule.steps.size() && !droppable; ++si) {
            const char* extendee = RequiredExtendee(rule.scope, si);
            const std::vector<std::string> parts = Split(rule.steps[si], '.');

            // Shortest-prefix-first: the extension's fully-qualified name is the
            // longest leading run of components that resolves to an extension of
            // the required extendee. Everything after it is the sub-field path.
            const FieldDescriptor* ext = nullptr;
            size_t consumed = 0;
            bool wrong_extendee = false;
            for (size_t k = 1; k <= parts.size(); ++k) {
                std::string prefix = parts[0];
                for (size_t j = 1; j < k; ++j) prefix += "." + parts[j];
                const FieldDescriptor* candidate = pool->FindExtensionByName(prefix);
                if (candidate == nullptr) continue;
                if (candidate->containing_type()->full_name() == extendee) {
                    ext = candidate;
                    consumed = k;
                    break;
                }
                wrong_extendee = true;
            }
            if (ext == nullptr) {
                if (wrong_extendee) {
                    *error = "invalid metadata_from_option '" + rule.source + "': step '" +
                             rule.steps[si] + "' resolves to an extension of a different type (" +
                             std::string(extendee) + " required)";
                    return nullptr;
                }
                // Not in this pool at all: no file in this protoc invocation
                // imports the declaring .proto. Drop the rule silently — one flag
                // list is routinely applied to a whole corpus.
                droppable = true;
                break;
            }

            CompiledStep step;
            step.ext = ext;
            const Descriptor* cur = (ext->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE)
                                        ? ext->message_type()
                                        : nullptr;
            if (consumed == parts.size()) {
                if (cur != nullptr) {
                    *error = "invalid metadata_from_option '" + rule.source + "': step '" +
                             rule.steps[si] + "' ends on a message; name a sub-field of " +
                             cur->full_name();
                    return nullptr;
                }
                step.terminal = ext;
            } else {
                for (size_t pi = consumed; pi < parts.size(); ++pi) {
                    if (cur == nullptr) {
                        *error = "invalid metadata_from_option '" + rule.source + "': step '" +
                                 rule.steps[si] + "' descends into a non-message field";
                        return nullptr;
                    }
                    const FieldDescriptor* f = cur->FindFieldByName(parts[pi]);
                    if (f == nullptr) {
                        *error = "invalid metadata_from_option '" + rule.source + "': '" +
                                 parts[pi] + "' is not a field of " + cur->full_name();
                        return nullptr;
                    }
                    const bool is_last = (pi + 1 == parts.size());
                    if (!is_last) {
                        if (f->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE || f->is_repeated()) {
                            *error = "invalid metadata_from_option '" + rule.source + "': '" +
                                     parts[pi] + "' is not a singular message field";
                            return nullptr;
                        }
                        cur = f->message_type();
                    } else {
                        step.terminal = f;
                    }
                    step.path.push_back(f);
                }
            }

            const bool last_step = (si + 1 == rule.steps.size());
            if (!last_step) {
                if (step.terminal->cpp_type() != FieldDescriptor::CPPTYPE_ENUM ||
                    step.terminal->is_repeated()) {
                    *error = "invalid metadata_from_option '" + rule.source +
                             "': '/' continues from an enum value's options, but '" +
                             step.terminal->full_name() + "' is not a singular enum field";
                    return nullptr;
                }
            } else if (!IsRenderableTerminal(step.terminal)) {
                *error = "invalid metadata_from_option '" + rule.source + "': '" +
                         step.terminal->full_name() +
                         "' has a type Fletcher will not render (message, float or double)";
                return nullptr;
            }
            compiled.steps.push_back(std::move(step));
        }

        if (!droppable) impl->rules.push_back(std::move(compiled));
    }

    return std::unique_ptr<OptionMetadataResolver>(new OptionMetadataResolver(std::move(impl)));
}

namespace {

// Append `key`=`value` with last-wins semantics, keeping the key at the position
// of its FIRST appearance so emission order stays deterministic.
void Upsert(std::vector<std::pair<std::string, std::string>>* pairs, const std::string& key,
            std::string value) {
    for (auto& kv : *pairs) {
        if (kv.first == key) {
            kv.second = std::move(value);
            return;
        }
    }
    pairs->emplace_back(key, std::move(value));
}

}  // namespace

std::vector<std::pair<std::string, std::string>> OptionMetadataResolver::ForMessage(
    const Descriptor* msg) const {
    std::vector<std::pair<std::string, std::string>> pairs;
    for (const CompiledRule& rule : impl_->rules) {
        if (rule.scope != MetadataRule::Scope::kMessage) continue;
        std::string value;
        if (impl_->Evaluate(rule, msg->options(), &value)) {
            Upsert(&pairs, rule.arrow_key, std::move(value));
        }
    }
    return pairs;
}

std::vector<std::pair<std::string, std::string>> OptionMetadataResolver::ForField(
    const FieldDescriptor* leaf,
    const std::vector<const FieldDescriptor*>& flatten_chain) const {
    std::vector<std::pair<std::string, std::string>> pairs;
    if (leaf == nullptr) return pairs;

    // Leaf first, then the (fletcher.flatten_field) wrappers it was inlined
    // through, innermost→outermost: the declaration closest to the Arrow column
    // wins. Within a rule the first candidate that yields a value stops the scan.
    std::vector<const FieldDescriptor*> candidates;
    candidates.push_back(leaf);
    for (auto it = flatten_chain.rbegin(); it != flatten_chain.rend(); ++it) {
        candidates.push_back(*it);
    }

    for (const CompiledRule& rule : impl_->rules) {
        if (rule.scope == MetadataRule::Scope::kMessage) continue;
        for (const FieldDescriptor* cand : candidates) {
            const Message* opts = nullptr;
            if (rule.scope == MetadataRule::Scope::kField) {
                opts = &cand->options();
            } else {  // kFieldType
                if (cand->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) continue;
                // A map field's message type is the synthetic MapEntry, which
                // carries no user options and is never a metadata carrier.
                if (cand->is_map()) continue;
                opts = &cand->message_type()->options();
            }
            std::string value;
            if (impl_->Evaluate(rule, *opts, &value)) {
                Upsert(&pairs, rule.arrow_key, std::move(value));
                break;
            }
        }
    }
    return pairs;
}

// ---------------------------------------------------------------------------
// C++ string-literal escaping
// ---------------------------------------------------------------------------

std::string EscapeCppStringLiteral(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const unsigned char c : s) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20 || c >= 0x7F) {
                    char buf[5];
                    std::snprintf(buf, sizeof buf, "\\%03o", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

}  // namespace fletcher
