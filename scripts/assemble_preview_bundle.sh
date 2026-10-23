#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# BIND-9a-1 (D-BIND-139): finishes the per-push preview bundle in a folder that already holds
# the artifacts ci.pr downloaded into it, and checks that nothing is missing.
#
#   assemble_preview_bundle.sh <bundle folder> <version> <commit>
#
# Expects, downloaded by ci.pr's package-bundle job:
#   nuget/     the four .nupkg (and .snupkg) at <version>, and Eiva.Fletcher.Protoc (BIND-9b-5),
#              at <version> in a preview bundle and at the protoc component's version elsewhere
#   gateway/   gateway-linux.tar.gz, gateway-windows.zip
#   protoc/    protoc-gen-fletcher-linux-x64, protoc-gen-fletcher-windows-x64.exe,
#              LICENSE, COPYING, THIRD-PARTY-LICENSES.txt
# Adds protoc/fletcher/options.proto, LICENSE, COPYING, VERSION and README-preview.md (from
# scripts/preview/README-preview.md). Run from the repository root.
set -euo pipefail

DIR=${1:?usage: assemble_preview_bundle.sh <dir> <version> <commit>}
VERSION=${2:?version}
COMMIT=${3:?commit}

mkdir -p "$DIR/protoc/fletcher"
cp protoc/include/fletcher/options.proto "$DIR/protoc/fletcher/"
cp LICENSE "$DIR/LICENSE"
cp COPYING "$DIR/COPYING"
printf 'version: %s\ncommit: %s\nbranch: %s\n' "$VERSION" "$COMMIT" "${BRANCH:-feature/csharp-bindings}" > "$DIR/VERSION"
sed -e "s/@VERSION@/$VERSION/g" -e "s/@COMMIT@/$COMMIT/g" scripts/preview/README-preview.md > "$DIR/README-preview.md"

missing=0
need() { [ -s "$DIR/$1" ] || { echo "::error::the preview bundle has no $1"; missing=1; }; }
for id in Eiva.Fletcher Eiva.Fletcher.Interop Eiva.Fletcher.Model Eiva.Fletcher.GatewayClient; do
  need "nuget/$id.$VERSION.nupkg"
done
protoc_packages=$(find "$DIR/nuget" -maxdepth 1 -name 'Eiva.Fletcher.Protoc.[0-9]*.nupkg' | wc -l)
if [ "$protoc_packages" -ne 1 ]; then
  echo "::error::the preview bundle has $protoc_packages Eiva.Fletcher.Protoc packages, not one"; missing=1
fi
# A preview bundle's five packages share its version (D-BIND-138); ci.pr sets this for one.
if [ "${BUNDLE_PROTOC_VERSION_MUST_MATCH:-0}" = 1 ]; then need "nuget/Eiva.Fletcher.Protoc.$VERSION.nupkg"; fi
need gateway/gateway-linux.tar.gz
need gateway/gateway-windows.zip
need protoc/protoc-gen-fletcher-linux-x64
need protoc/protoc-gen-fletcher-windows-x64.exe
need protoc/fletcher/options.proto
need protoc/LICENSE
need protoc/COPYING
need protoc/THIRD-PARTY-LICENSES.txt
need LICENSE
need COPYING
need VERSION
need README-preview.md
if grep -q '@VERSION@\|@COMMIT@' "$DIR/README-preview.md"; then
  echo "::error::README-preview.md still has a placeholder"; missing=1
fi
[ "$missing" -eq 0 ] || exit 1

echo "preview bundle $VERSION at $COMMIT:"
(cd "$DIR" && find . -type f | sort | sed 's/^\.\//  /')
