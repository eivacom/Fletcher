// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

// BIND-7: the C# accessor and view, written under `csharp_accessor` beside the model
// (D-BIND-99). Per message, `<stem>.fletcher.accessor.cs` holds `<Msg>Accessor`, a
// column-oriented reader that validates and casts every column once (TryMake, D-BIND-94),
// and `<stem>.fletcher.arrow.cs` holds `<Msg>View`, a readonly struct of (accessor, row)
// that is also the accessor's row type (D-BIND-97). The accessor has no per-field members
// and the view nothing but the fields, named and typed as on the model class, so no field
// name can collide with a generated member (D-BIND-98).
//
// Both walk the model's own field list and classification (csharp_backend_columns), so a
// column reads here as the model's FromArrow reads it, and every expected Arrow type is read
// from the model class's Schema.

#include <google/protobuf/descriptor.h>

#include <optional>
#include <string>

#include "csharp_backend_columns.hpp"

namespace fletcher::csharp_backend {

// D-BIND-100: the reason `file` cannot be generated with `csharp_accessor`, or nullopt. No
// `<Msg>View` or `<Msg>Accessor` of this file or of an imported file of the namespace (B2 of
// BIND-7's review) may share its name with a type the model declares in the same C# namespace
// (`message Track` beside `message TrackView` is CS0101), and a view's property may not share the
// view's name (`Track.track_view` is CS0542). Only the file and the files it imports are visible
// here, so a clash with a file of the same package that `file` does not import is left to the
// compiler.
std::optional<std::string> CsAccessorNameConflict(const google::protobuf::FileDescriptor* file);

class CsAccessorVisitor {
   public:
    explicit CsAccessorVisitor(const google::protobuf::FileDescriptor* file);

    // The whole <stem>.fletcher.accessor.cs: one `<Msg>Accessor` per generated message.
    std::string GenerateAccessorFile();

    // The whole <stem>.fletcher.arrow.cs: one `<Msg>View` per generated message.
    std::string GenerateViewFile();

   private:
    const google::protobuf::FileDescriptor* file_;
    CrossFile cross_file_;
};

}  // namespace fletcher::csharp_backend
