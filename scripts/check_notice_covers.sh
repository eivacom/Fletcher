#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# D10C-5 (ruling 2026-11-16, "one licence notice for the server package across platforms"):
# the server package ships ONE THIRD-PARTY-LICENSES.txt for both platforms, listing the union of
# the two server graphs (Linux adds libbacktrace, through Boost). The pack job ships the notice of
# one platform and checks that every library each platform's own notice lists is in it.
#
#   check_notice_covers.sh <shipped notice> <platform notice>...
#
#   * a library is a bullet between a notice's first two rules, as check_third_party_notice.sh
#     reads them, compared by name and version (the licence text after " -- " is not compared);
#   * every <platform notice> lists at least one library, and each is in the shipped notice.
set -euo pipefail

SHIPPED=${1:?usage: check_notice_covers.sh <shipped notice> <platform notice>...}
shift
[ "$#" -ge 1 ] || { echo "::error::check_notice_covers.sh needs at least one platform notice"; exit 1; }
listed() { awk '/^=+$/ {n++; next} n == 1 && /^  \* / {print}' "$1" | sed 's/ -- .*//' | sort -u; }
fail=0
for notice in "$SHIPPED" "$@"; do
  [ -s "$notice" ] || { echo "::error::$notice is missing or empty"; exit 1; }
  [ -n "$(listed "$notice")" ] || { echo "::error::$notice lists no library: the parse found no bullets between its first two rules"; exit 1; }
done
for notice in "$@"; do
  while IFS= read -r lib; do
    echo "::error::$SHIPPED does not list${lib#  \*}, which $notice lists: the notice shipped for both platforms must cover each (D10C-5, D-BIND-140)"
    fail=1
  done < <(comm -13 <(listed "$SHIPPED") <(listed "$notice"))
done
[ "$fail" -eq 0 ] || exit 1
echo "$SHIPPED covers the libraries of: $*"
