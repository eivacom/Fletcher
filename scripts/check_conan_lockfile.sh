#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# BIND-9b-7 (review L14, D-BIND-173): the shim's Conan lockfile, which ci.dotnet writes off the
# graph it built the shim from and a release attaches, so that the relinking route in
# dotnet/README.package.md rebuilds the third-party packages the release was built with
# instead of ConanCenter's newest recipe revisions.
#
#   check_conan_lockfile.sh <lockfile> <profile>
#
#   * it locks third-party packages only: no fletcher-* (the commit pins those, and their
#     recipe revisions hash the checkout, line endings included);
#   * in an empty Conan home holding only this checkout's five fletcher recipes, the shim's
#     graph, resolved from ConanCenter with the lockfile passed as the README passes it
#     (--lockfile-partial), has exactly the locked third-party packages, recipe revisions
#     included: none resolved that the lockfile leaves open, none locked that the graph lacks.
# A graph, not a build: ci.dotnet's own build is the shim's build test (D-BIND-171).
set -euo pipefail

LOCK=${1:?usage: check_conan_lockfile.sh <lockfile> <profile>}
PROFILE=${2:?usage: check_conan_lockfile.sh <lockfile> <profile>}
[ -s "$LOCK" ] || { echo "::error::$LOCK is missing or empty"; exit 1; }
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# The first Python that runs (on Windows, python3 may be the Store's stub).
PY=
for candidate in python3 python; do
  if "$candidate" -c "import json" 2> /dev/null; then PY=$candidate; break; fi
done
[ -n "$PY" ] || { echo "::error::no Python to read the graph with"; exit 1; }

# A native path for Conan on Windows (Git Bash passes environment variables unconverted).
native() { (cd "$1" && { pwd -W 2>/dev/null || pwd; }); }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir "$WORK/home"
export CONAN_HOME
CONAN_HOME=$(native "$WORK/home")

for component in core pubsub fastdds-pubsub-provider xrcedds-pubsub-provider c-abi; do
  conan export "$ROOT/$component" > /dev/null 2>> "$WORK/conan.log" \
    || { cat "$WORK/conan.log"; echo "::error::conan export $component failed"; exit 1; }
done
conan graph info "$ROOT/c-abi" --lockfile="$LOCK" --lockfile-partial -pr:a="$PROFILE" \
  --format=json > "$WORK/graph.json" 2>> "$WORK/conan.log" \
  || { cat "$WORK/conan.log"; echo "::error::the shim's graph does not resolve with $LOCK"; exit 1; }

"$PY" - "$LOCK" "$WORK/graph.json" <<'EOF'
import json, sys

lock = json.load(open(sys.argv[1]))
graph = json.load(open(sys.argv[2]))
locked = {r.split("%")[0] for key in ("requires", "build_requires", "python_requires")
          for r in lock.get(key, [])}
resolved = {n["ref"] for n in graph["graph"]["nodes"].values()
            if n.get("ref") and n.get("recipe") != "Consumer"}

fail = False
for ref in sorted(r for r in locked if r.startswith("fletcher-")):
    print(f"::error::{sys.argv[1]} locks {ref}: it must lock third-party packages only")
    fail = True
third_party = {r for r in resolved if not r.startswith("fletcher-")}
for ref in sorted(third_party - locked):
    print(f"::error::the shim's graph resolved {ref}, which {sys.argv[1]} does not lock")
    fail = True
for ref in sorted(r for r in locked - third_party if not r.startswith("fletcher-")):
    print(f"::error::{sys.argv[1]} locks {ref}, which the shim's graph does not have")
    fail = True
if not any("#" in r for r in locked):
    print(f"::error::{sys.argv[1]} locks no recipe revisions")
    fail = True
if fail:
    sys.exit(1)
print(f"{sys.argv[1]}: {len(locked)} third-party packages locked, exactly the shim's graph:")
for ref in sorted(locked):
    print(f"  {ref}")
EOF
