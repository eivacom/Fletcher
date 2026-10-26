#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# BIND-9b-7 (D-BIND-152, D-BIND-166): a THIRD-PARTY-LICENSES.txt written by
# gateway/deployers/third_party_licenses.py covers what its binary carries.
#
#   check_third_party_notice.sh <THIRD-PARTY-LICENSES.txt> <library>...
#
#   * every <library> is in the notice's list (the bullets between its first two rules), as
#     "  * <library> <version>";
#   * where nanoarrow is listed, the Apache-2.0 NOTICE files it carries are in the text too:
#     nanoarrow's own and the one of the flatcc it bundles (Apache-2.0 section 4(d)), and the
#     licences of flatcc's portable headers (the folder's LICENSE, and LICENSES.txt for the four
#     files under BSD-3-Clause and MIT).
# One script for every lane that writes a notice: the shim's (ci.dotnet), the gateway's
# (ci.gateway) and the plugin's (ci.protoc).
set -euo pipefail

NOTICE=${1:?usage: check_third_party_notice.sh <THIRD-PARTY-LICENSES.txt> <library>...}
shift
[ -s "$NOTICE" ] || { echo "::error::$NOTICE is missing or empty"; exit 1; }
fail=0
listed=$(awk '/^=+$/ {n++; next} n == 1 && /^  \* / {print}' "$NOTICE")
for lib in "$@"; do
  grep -q "^  \* $lib " <<< "$listed" || { echo "::error::$NOTICE does not cover $lib, which is compiled into its binary"; fail=1; }
done
if grep -q "^  \* nanoarrow " <<< "$listed"; then
  grep -q "^Apache Arrow nanoarrow" "$NOTICE" || { echo "::error::$NOTICE lists nanoarrow without nanoarrow's Apache NOTICE"; fail=1; }
  grep -q "^--- flatcc/NOTICE ---" "$NOTICE" || { echo "::error::$NOTICE lists nanoarrow without the NOTICE of the flatcc it bundles"; fail=1; }
  for portable in flatcc/portable/LICENSE flatcc/portable/LICENSES.txt; do
    grep -q "^--- $portable ---" "$NOTICE" || { echo "::error::$NOTICE lists nanoarrow without $portable, the licences of the flatcc portable headers"; fail=1; }
  done
fi
[ "$fail" -eq 0 ] || exit 1
echo "$NOTICE covers: $*"
