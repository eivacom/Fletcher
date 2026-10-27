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
#   * every package carries LICENSE, COPYING (the GPL-3.0 text the LGPL incorporates,
#     D-BIND-152) and THIRD-PARTY-LICENSES.txt at its root, and the notice is not empty;
#   * BIND-9a-2 (D-BIND-140 to 142, D-BIND-149): every package carries its README.md at
#     its root, named by the nuspec, and it is dotnet/README.package.md with exactly the
#     version and the commit filled in (so nothing was lost or mangled on the way, and
#     the floors it states are the ones in shim_floors.env); BIND-9b-7 (D-BIND-172): the
#     loader's message for a shim the OS refuses states the same floors;
#   * BIND-9b-1 (D-BIND-157): Eiva.Fletcher.Interop is at most 12 MB packed;
#   * given a <commit> (D-BIND-145): the README names it, and so does the nuspec's
#     <repository commit>, which SourceLink and NuGet show.
set -euo pipefail

DIR=${1:?usage: check_dotnet_packages.sh <folder> [<commit>]}
COMMIT=${2:-}
HERE=$(cd "$(dirname "$0")" && pwd)
TEMPLATE="$HERE/../dotnet/README.package.md"
LOADER="$HERE/../dotnet/src/Fletcher.Interop/NativeLoader.cs"
# shellcheck source=shim_floors.env
. "$HERE/shim_floors.env"
fail=0
err() { echo "::error::$*"; fail=1; }
# 12 MB (D-BIND-157), overridable only to rehearse the check.
INTEROP_CEILING=${INTEROP_CEILING:-12000000}

# Listing and reading go through Python's zipfile: present on the runners and in the
# devcontainer, where unzip is not.
entries() { python3 -c 'import sys, zipfile; print(chr(10).join(zipfile.ZipFile(sys.argv[1]).namelist()))' "$1"; }
member() { python3 -c 'import sys, zipfile; sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read(sys.argv[2]))' "$1" "$2"; }
nuspec() { python3 -c 'import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); sys.stdout.write(z.read([n for n in z.namelist() if n.endswith(".nuspec")][0]).decode())' "$1"; }

pkg() {
  find "$DIR" -maxdepth 1 -name "$1.[0-9]*.nupkg" -print -quit
}

# True when the package's README.md is the template with exactly the version and the
# commit filled in, as dotnet/Directory.Build.targets does it (line endings and trailing
# blank lines aside); prints the first line that differs otherwise.
readme_matches() {
  python3 - "$1" "$TEMPLATE" "$2" "$3" <<'PY'
import sys, zipfile
packed = zipfile.ZipFile(sys.argv[1]).read("README.md").decode("utf-8").replace("\r", "").rstrip()
want = open(sys.argv[2], encoding="utf-8").read().replace("@VERSION@", sys.argv[3]).replace("@COMMIT@", sys.argv[4]).rstrip()
if packed != want:
    for n, (a, b) in enumerate(zip(packed.splitlines() + [""], want.splitlines() + [""]), 1):
        if a != b:
            print(f"  README.md line {n}: packed {a!r}, expected {b!r}")
            break
    sys.exit(1)
PY
}

for fact in "glibc $GLIBC_FLOOR" "GLIBCXX_$GLIBCXX_FLOOR"; do
  grep -qF "$fact" "$TEMPLATE" || err "dotnet/README.package.md does not state $fact, the floor in scripts/shim_floors.env (D-BIND-149)"
  # The loader quotes the floor when the OS refuses a shim that is there (D-BIND-172).
  grep -qF "$fact" "$LOADER" || err "dotnet/src/Fletcher.Interop/NativeLoader.cs does not state $fact, the floor in scripts/shim_floors.env (D-BIND-172)"
done

for id in Eiva.Fletcher Eiva.Fletcher.Interop Eiva.Fletcher.Model Eiva.Fletcher.GatewayClient; do
  PKG=$(pkg "$id")
  if [ -z "$PKG" ]; then err "$id was not packed"; continue; fi
  VERSION=$(basename "$PKG" .nupkg); VERSION=${VERSION#"$id".}
  ENTRIES=$(entries "$PKG")
  echo "$ENTRIES" | grep -q '^lib/net' || err "$PKG has no lib/ assembly"
  for f in LICENSE COPYING THIRD-PARTY-LICENSES.txt; do
    echo "$ENTRIES" | grep -qx "$f" || err "$PKG does not carry $f at its root (D-BIND-140, D-BIND-152)"
  done
  if echo "$ENTRIES" | grep -qx THIRD-PARTY-LICENSES.txt; then
    [ "$(member "$PKG" THIRD-PARTY-LICENSES.txt | wc -c)" -gt 200 ] || err "$PKG: THIRD-PARTY-LICENSES.txt is empty"
  fi
  if echo "$ENTRIES" | grep -qx README.md; then
    nuspec "$PKG" | grep -q "<readme>README.md</readme>" || err "$PKG: the nuspec does not name README.md as its readme"
    if [ -n "$COMMIT" ]; then
      if ! readme_matches "$PKG" "$VERSION" "$COMMIT"; then
        err "$PKG: README.md is not dotnet/README.package.md with version $VERSION and commit $COMMIT filled in"
      fi
    elif member "$PKG" README.md | grep -qE '@VERSION@|@COMMIT@'; then
      err "$PKG: README.md still has a placeholder"
    fi
  else
    err "$PKG does not carry README.md at its root"
  fi
  if [ -n "$COMMIT" ] && ! nuspec "$PKG" | grep -q "<repository [^>]*commit=\"$COMMIT\""; then
    err "$PKG: the nuspec's <repository commit> is not $COMMIT: $(nuspec "$PKG" | grep -o '<repository [^>]*>' || echo none) (D-BIND-145)"
  fi
  RUNTIMES=$(echo "$ENTRIES" | grep '^runtimes/' || true)
  if [ "$id" = Eiva.Fletcher.Interop ]; then
    # D-BIND-157: the packed-size ceiling. 8.6 MB with an unstripped Linux shim, about
    # 7.7 MB stripped; above the ceiling something grew unnoticed, so the pack fails.
    SIZE=$(wc -c < "$PKG")
    echo "$PKG: $SIZE bytes packed (ceiling $INTEROP_CEILING)"
    [ "$SIZE" -le "$INTEROP_CEILING" ] || err "$PKG is $SIZE bytes packed, above the $INTEROP_CEILING-byte ceiling (D-BIND-157)"
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
  if [ "$id" != Eiva.Fletcher ] && [ "$id" != Eiva.Fletcher.Interop ] && nuspec "$PKG" | grep -qE 'id="Eiva\.Fletcher(\.Interop)?"'; then
    err "$PKG depends on a package that carries native assets (D-BIND-13)"
  fi
  echo "$PKG: checked"
done
exit $fail
