#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# D9S-3 (d9 L8; Q4; register BC-11): the edge build carries no Arrow C++, shown by NAME.
# The size ceilings stay as the coarse check; this is the check by name, beside them.
#
#   check_edge_arrow_free.sh <recipe dir> <profile> <edge shim>
#
# GRAPH: `conan graph info` on the recipe with variant=edge and the shipped defaults (no
#   tests, no probe). Fails, naming each, if a host node is arrow, fletcher-arrow-bridge or
#   fletcher-pubsub-arrow. Refused (nothing was read) if the graph has no fletcher-c-abi root.
# SYMBOLS: Linux (.so, or any file without .dll/.exe): `nm -C`, all symbols (a shared Arrow
#   shows as undefined arrow:: references); fails on arrow::, and the listing must hold
#   fletcher::. Windows (.dll/.exe): the type names and decorated import names the image
#   carries (a Release DLL keeps no full symbol table); fails on @arrow@@, and the listing
#   must hold @fletcher@@. Neither pattern can match nanoarrow, the edge codec.
# CONTROLS, on every run, each must go red by naming Arrow or this step fails: the same graph
#   with run_tests=True, judged by the graph half's own test, must name both
#   fletcher-arrow-bridge and arrow, and a c_abi_tests binary from the
#   Conan cache (it links Arrow) must yield an Arrow name.
set -euo pipefail
export LC_ALL=C

RECIPE=${1:?usage: check_edge_arrow_free.sh <recipe dir> <profile> <edge shim>}
PROFILE=${2:?usage: check_edge_arrow_free.sh <recipe dir> <profile> <edge shim>}
EDGE=${3:?usage: check_edge_arrow_free.sh <recipe dir> <profile> <edge shim>}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
ARROW_PKG='arrow|fletcher-arrow-bridge|fletcher-pubsub-arrow'
PY=
for c in python python3; do "$c" -I -c 'import json' >/dev/null 2>&1 && { PY=$c; break; }; done
[ -n "$PY" ] || { echo "::error::no working python or python3 to read the Conan graph"; exit 1; }

# graph_hosts [extra conan option args]: the host node names of the edge graph, one per line.
graph_hosts() {
  conan graph info "$RECIPE" -pr:a="$PROFILE" -o "&:variant=edge" "$@" --format=json \
    >"$TMP/g.json" 2>"$TMP/g.err" || { echo "::error::conan graph info failed:" >&2; cat "$TMP/g.err" >&2; return 1; }
  "$PY" -I -c 'import json,sys
n=json.load(sys.stdin)["graph"]["nodes"]
print("root", n.get("0",{}).get("name",""))
[print("host", x["name"]) for x in n.values() if x.get("context")=="host" and x.get("name")]' \
    <"$TMP/g.json" | tr -d '\r' >"$TMP/g.txt"
  grep -qx 'root fletcher-c-abi' "$TMP/g.txt" || { echo "::error::the graph of $RECIPE has no fletcher-c-abi root: nothing was read, so nothing was checked" >&2; return 1; }
  sed -n 's/^host //p' "$TMP/g.txt"
}

# scan <file>: lists the file's names into $TMP/list; sets HITS (the Arrow ones, sorted, unique)
# and HAS_OURS (1 when the listing holds a Fletcher name, 0 when it read nothing of ours).
scan() {
  [ -f "$1" ] || { echo "::error::no file at $1"; exit 1; }
  case "$1" in
    *.dll|*.exe) FL='@fletcher@@'; AR='@arrow@@'
      grep -aoE '[A-Za-z0-9_?$@]*@(arrow|fletcher)@@' "$1" >"$TMP/list" || true ;;
    *) FL='fletcher::'; AR='(^|[^A-Za-z0-9_])arrow::'
      command -v nm >/dev/null || { echo "::error::nm is not installed"; exit 1; }
      nm -C "$1" >"$TMP/list" || true ;;
  esac
  grep -E "$AR" "$TMP/list" | sort -u >"$TMP/hits" || true
  HITS=$(head -n 5 "$TMP/hits"); NHITS=$(wc -l <"$TMP/hits" | tr -d ' ')
  HAS_OURS=0; grep -qF "$FL" "$TMP/list" && HAS_OURS=1 || true
}

fail=0
# 1. The edge graph holds no Arrow package.
hosts=$(graph_hosts) || exit 1
if bad=$(grep -xE "$ARROW_PKG" <<<"$hosts"); then
  echo "::error::the edge build's dependency graph holds Arrow C++ (d9 L8): $(echo $bad)"; fail=1
else echo "graph: no Arrow package among the edge build's $(wc -l <<<"$hosts" | tr -d ' ') host nodes"; fi
# 2. The edge shim holds no Arrow name, and the listing is proven to have been read.
scan "$EDGE"
if [ "$HAS_OURS" -eq 0 ]; then
  echo "::error::the listing of $EDGE holds no Fletcher name ($FL): it was stripped or unreadable, so nothing was checked"; exit 1
elif [ -n "$HITS" ]; then
  echo "::error::the edge shim carries $NHITS Arrow C++ name(s) (d9 L8), e.g.:"; echo "$HITS" | sed 's/^/  /'; fail=1
else echo "symbols: no Arrow name in $EDGE (it carries $FL names)"; fi
# 3. Control: the test graph must name the Arrow packages the shipped graph must not.
tests=$(graph_hosts -o "&:run_tests=True") || exit 1
ctl=$(grep -xE "$ARROW_PKG" <<<"$tests" || true)   # step 1's own test, applied to the control graph
if ! grep -qx fletcher-arrow-bridge <<<"$ctl" || ! grep -qx arrow <<<"$ctl"; then
  echo "::error::graph control: the run_tests=True graph must name both fletcher-arrow-bridge and arrow by step 1's test, and names: '$(echo $ctl)'; the graph half cannot be trusted"; fail=1
else echo "control (graph) is red, as required, naming: $(echo $ctl)"; fi
# 4. Control: a test binary that links Arrow must yield an Arrow name.
bin=$(find "${CONAN_HOME:-$HOME/.conan2}/p/b" \( -name c_abi_tests -o -name c_abi_tests.exe \) -type f -print -quit)
if [ -z "$bin" ]; then
  echo "::error::symbol control: no c_abi_tests binary under the Conan cache, so the symbol half cannot be trusted"; fail=1
else
  scan "$bin"
  if [ -z "$HITS" ]; then echo "::error::symbol control: $bin links Arrow but yields no Arrow name, so the symbol half cannot be trusted"; fail=1
  else echo "control (symbols) is red, as required: $NHITS Arrow name(s) in $bin, e.g. $(head -n 1 "$TMP/hits")"; fi
fi
[ "$fail" -eq 0 ] || exit 1
echo "the edge build carries no Arrow C++, and both controls went red by name"
