#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# BIND-9a-0 (D-BIND-13, D-BIND-134, D-BIND-140): checks on the PACKED packages,
# because the packages are what a tester installs.
#
#   check_dotnet_packages.sh <folder with the .nupkg files> [<commit>]
#
#   * all four packages are there;
#   * Eiva.Fletcher.Interop carries exactly the two shims, under
#     runtimes/linux-x64/native/ and runtimes/win-x64/native/, and no probe shim;
#   * no other package carries a runtimes/ entry, and Eiva.Fletcher.GatewayClient
#     depends on no package that would bring one (D-BIND-13);
#   * every package carries LICENSE and THIRD-PARTY-LICENSES.txt at its root,
#     and the notice is not empty;
#   * BIND-9a-2 (D-BIND-140, D-BIND-141): every package carries its README.md at its
#     root, named by the nuspec, with no placeholder left and the platform floors and
#     the relinking route in it; given a <commit>, the README names it.
set -euo pipefail

DIR=${1:?usage: check_dotnet_packages.sh <folder> [<commit>]}
COMMIT=${2:-}
fail=0
err() { echo "::error::$*"; fail=1; }

# Listing and reading go through Python's zipfile: present on the runners and in the
# devcontainer, where unzip is not.
entries() { python3 -c 'import sys, zipfile; print(chr(10).join(zipfile.ZipFile(sys.argv[1]).namelist()))' "$1"; }
member() { python3 -c 'import sys, zipfile; sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read(sys.argv[2]))' "$1" "$2"; }
nuspec() { python3 -c 'import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); sys.stdout.write(z.read([n for n in z.namelist() if n.endswith(".nuspec")][0]).decode())' "$1"; }

pkg() {
  find "$DIR" -maxdepth 1 -name "$1.[0-9]*.nupkg" ! -name '*.snupkg' -print -quit
}

for id in Eiva.Fletcher Eiva.Fletcher.Interop Eiva.Fletcher.Model Eiva.Fletcher.GatewayClient; do
  PKG=$(pkg "$id")
  if [ -z "$PKG" ]; then err "$id was not packed"; continue; fi
  ENTRIES=$(entries "$PKG")
  echo "$ENTRIES" | grep -q '^lib/net' || err "$PKG has no lib/ assembly"
  for f in LICENSE THIRD-PARTY-LICENSES.txt; do
    echo "$ENTRIES" | grep -qx "$f" || err "$PKG does not carry $f at its root (D-BIND-140)"
  done
  if echo "$ENTRIES" | grep -qx THIRD-PARTY-LICENSES.txt; then
    [ "$(member "$PKG" THIRD-PARTY-LICENSES.txt | wc -c)" -gt 200 ] || err "$PKG: THIRD-PARTY-LICENSES.txt is empty"
  fi
  if echo "$ENTRIES" | grep -qx README.md; then
    README=$(member "$PKG" README.md)
    nuspec "$PKG" | grep -q "<readme>README.md</readme>" || err "$PKG: the nuspec does not name README.md as its readme"
    if echo "$README" | grep -qE '@VERSION@|@COMMIT@'; then err "$PKG: README.md still has a placeholder"; fi
    for fact in "GLIBCXX_3.4.32" "glibc 2.38" "Visual C++ 2015-2022" "NativeAOT" "git checkout"; do
      echo "$README" | grep -qF "$fact" || err "$PKG: README.md does not state $fact (D-BIND-140, D-BIND-141)"
    done
    if [ -n "$COMMIT" ]; then
      echo "$README" | grep -qF "git checkout $COMMIT" || err "$PKG: README.md does not name commit $COMMIT"
    fi
  else
    err "$PKG does not carry README.md at its root"
  fi
  RUNTIMES=$(echo "$ENTRIES" | grep '^runtimes/' || true)
  if [ "$id" = Eiva.Fletcher.Interop ]; then
    EXPECTED=$'runtimes/linux-x64/native/libfletcher-c-abi.so\nruntimes/win-x64/native/fletcher-c-abi.dll'
    if [ "$(echo "$RUNTIMES" | sort)" != "$EXPECTED" ]; then
      err "$PKG must carry exactly the two shims (D-BIND-134); it carries: ${RUNTIMES:-nothing}"
    fi
  elif [ -n "$RUNTIMES" ]; then
    err "$PKG carries native assets, which only Eiva.Fletcher.Interop may (D-BIND-13): $RUNTIMES"
  fi
  if echo "$ENTRIES" | grep -qi 'probe'; then
    err "$PKG carries the test-only probe shim"
  fi
  if [ "$id" = Eiva.Fletcher.GatewayClient ] && nuspec "$PKG" | grep -qE 'id="Eiva\.Fletcher(\.Interop)?"'; then
    err "$PKG depends on a package that carries native assets (D-BIND-13)"
  fi
  echo "$PKG: checked"
done
exit $fail
