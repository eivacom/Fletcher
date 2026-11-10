#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# BIND-9b-6: verify.sh, the tag check cd.dotnet runs only on a tag, exercised without one.
# Run by ci.action-lint. Each case is a props file, a ref and whether the check must pass.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
failed=0

props() {
  printf '<Project>\n  <PropertyGroup>\n    <VersionPrefix>%s</VersionPrefix>\n    <VersionSuffix>%s</VersionSuffix>\n  </PropertyGroup>\n</Project>\n' "$1" "$2" > "$WORK/p.props"
}

expect() {
  local want=$1 ref=$2
  if bash "$HERE/verify.sh" "$WORK/p.props" dotnet-v "$ref" > "$WORK/out" 2>&1; then got=pass; else got=fail; fi
  if [ "$got" != "$want" ]; then
    echo "::error::verify.sh: expected $want for $ref with $(tr -d '\n ' < "$WORK/p.props"), got $got: $(cat "$WORK/out")"
    failed=1
  else
    echo "ok: $want  $ref"
  fi
}

props 0.6.0 alpha
expect pass refs/tags/dotnet-v0.6.0-alpha
expect fail refs/tags/dotnet-v0.6.0
expect fail refs/tags/dotnet-v0.6.1-alpha
expect fail refs/tags/protoc-v0.6.0-alpha
expect fail refs/heads/main
props 0.6.0 ''
expect pass refs/tags/dotnet-v0.6.0
expect fail refs/tags/dotnet-v0.6.0-alpha
# The repository's own props parse.
bash "$HERE/verify.sh" "$HERE/../../../dotnet/Directory.Build.props" dotnet-v refs/tags/dotnet-v0.0.0 > "$WORK/out" 2>&1
grep -q "does not match" "$WORK/out" || { echo "::error::dotnet/Directory.Build.props did not parse: $(cat "$WORK/out")"; failed=1; }

exit "$failed"
