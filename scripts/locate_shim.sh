#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# D9S-2 (BC-11; audit F8): a shim is chosen by the identity of the package it is in,
# never by guessing from a file name and never as "the first match".
#
#   locate_shim.sh <file stem> <option=value>...   (e.g. fletcher-c-abi variant=edge)
#
# Prints the one file named <stem> (bin/<stem>.dll or lib/lib<stem>.so) inside the one
# Conan package whose conaninfo.txt records EVERY given option. Fails, naming what it
# found, on no match and on more than one.
set -euo pipefail

STEM=${1:?usage: locate_shim.sh <file stem> <option=value>...}
shift
[ "$#" -ge 1 ] || { echo "::error::locate_shim.sh needs at least one option=value: a shim is chosen by its package's identity" >&2; exit 1; }
ROOT=${CONAN_HOME:-$HOME/.conan2}

matches=()
found=()
while IFS= read -r file; do
  [ -n "$file" ] || continue
  found+=("$file")
  info="$(dirname "$(dirname "$file")")/conaninfo.txt"
  [ -f "$info" ] || continue
  options=$(tr -d '\r' < "$info" | awk '/^\[/ {on = ($0 == "[options]"); next} on && NF')
  ok=1
  for want in "$@"; do
    echo "$options" | grep -qxF -- "$want" || { ok=0; break; }
  done
  [ "$ok" -eq 1 ] && matches+=("$file")
done < <(find "$ROOT/p" \( -path "*/p/bin/$STEM.dll" -o -path "*/p/lib/lib$STEM.so" \) -type f | sort)

if [ "${#matches[@]}" -eq 1 ]; then
  echo "${matches[0]}"
  exit 0
fi
if [ "${#matches[@]}" -eq 0 ]; then
  echo "::error::no $STEM in the Conan cache sits in a package recorded with: $*" >&2
else
  echo "::error::${#matches[@]} files named $STEM sit in packages recorded with: $* - refusing to pick one:" >&2
  printf '  %s\n' "${matches[@]}" >&2
fi
if [ "${#found[@]}" -gt 0 ]; then
  echo "files named $STEM that the cache holds:" >&2
  printf '  %s\n' "${found[@]}" >&2
fi
exit 1
