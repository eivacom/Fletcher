#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# D9S-2 (d9 L7, L12, BC-10; audit F7): both builds export exactly the entry points
# binding.h declares, no more and no fewer, so a name exported by one build only is
# a defect whichever build has it.
#
#   check_shim_exports.sh <binding.h> <edge shim> <server shim> [dumpbin.exe]
#
# DECLARED: every name declared FL_ABI_EXPORT, the identifier before the first "(" of
#   its line. Both version entry points must be in it, or the header was not read.
# PER SHIM: Linux (*.so) uses `nm -D --defined-only` and refuses anything outside fl_*
#   and the linker's own five names. Windows (*.dll, with dumpbin.exe) judges only the
#   fl_ subset: the static Fast DDS archive adds thousands of exports (D-BIND-157).
#   A shim whose list parses to no fl_ name fails: nothing was read.
# EQUALITY: edge == declared and server == declared, every difference printed by name.
# CONTROL: CI also runs this with the probe shim (two extra fl_test_* exports) in the
#   server's place; that run MUST fail, naming fl_test_probe_loan.
set -euo pipefail
export LC_ALL=C

HEADER=${1:?usage: check_shim_exports.sh <binding.h> <edge shim> <server shim> [dumpbin]}
EDGE=${2:?usage: check_shim_exports.sh <binding.h> <edge shim> <server shim> [dumpbin]}
SERVER=${3:?usage: check_shim_exports.sh <binding.h> <edge shim> <server shim> [dumpbin]}
DUMPBIN=${4:-}
for f in "$HEADER" "$EDGE" "$SERVER"; do
  [ -f "$f" ] || { echo "::error::no file at $f"; exit 1; }
done

fail=0

declared=$(grep -E '^FL_ABI_EXPORT' "$HEADER" | sed -E 's/\(.*//' \
  | grep -oE '[A-Za-z_][A-Za-z0-9_]*[[:space:]]*$' | tr -d ' \t\r' | sort -u || true)
for must in fl_binding_abi_version fl_binding_variant; do
  if ! echo "$declared" | grep -qx "$must"; then
    echo "::error::$must is not among the names declared FL_ABI_EXPORT in $HEADER: the header was not read, so nothing was compared"
    exit 1
  fi
done
echo "binding.h declares $(echo "$declared" | wc -l | tr -d ' ') exported entry points"

# table_of <shim>: every name the shim exports, one per line, sorted. On Windows the
# table is dumpbin's, which carries thousands of Fast DDS names (D-BIND-157), and
# only the fl_ subset is judged below.
table_of() {
  local shim=$1
  case "$shim" in
    *.so) nm -D --defined-only "$shim" | awk '{print $3}' | sort -u ;;
    *.dll)
      [ -n "$DUMPBIN" ] || { echo "::error::a .dll needs the path to dumpbin.exe as the fourth argument" >&2; exit 1; }
      "$DUMPBIN" //nologo //exports "$shim" | tr -d '\r' | awk '$1 ~ /^[0-9]+$/ && NF >= 4 {print $4}' | sort -u
      ;;
    *) echo "::error::unknown shim type: $shim" >&2; exit 1 ;;
  esac
}

# compare <label> <shim>: the shim's fl_ set against the declared set.
compare() {
  local label=$1 shim=$2 table got other extra missing
  table=$(table_of "$shim")
  got=$(echo "$table" | grep -E '^fl_[A-Za-z0-9_]*$' || true)
  if [ -z "$got" ]; then
    echo "::error::the export list of the $label shim ($shim) holds no fl_ name: it was not read, so nothing was compared"
    fail=1
    return
  fi
  case "$shim" in
    *.so)
      other=$(echo "$table" | grep -v -E '^(fl_[A-Za-z0-9_]*|_init|_fini|_edata|_end|__bss_start)$' || true)
      if [ -n "$other" ]; then
        echo "::error::the $label shim exports symbols outside fl_ and the linker's own:"
        echo "$other" | sed 's/^/  /'
        fail=1
      fi
      ;;
  esac
  extra=$(comm -13 <(echo "$declared") <(echo "$got"))
  missing=$(comm -23 <(echo "$declared") <(echo "$got"))
  if [ -n "$extra" ]; then
    echo "::error::the $label shim exports what binding.h does not declare:"
    echo "$extra" | sed 's/^/  /'
    fail=1
  fi
  if [ -n "$missing" ]; then
    echo "::error::the $label shim does not export what binding.h declares:"
    echo "$missing" | sed 's/^/  /'
    fail=1
  fi
  [ -n "$extra$missing" ] || echo "the $label shim exports exactly the declared set ($(echo "$got" | wc -l | tr -d ' ') names): $shim"
}

compare edge "$EDGE"
compare server "$SERVER"
[ "$fail" -eq 0 ] || exit 1
echo "edge = server = binding.h"
