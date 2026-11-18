#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# BIND-9a-0 (D-BIND-13, D-BIND-134, D-BIND-140): checks on the PACKED packages,
# because the packages are what a tester installs.
#
#   check_dotnet_packages.sh <folder with the .nupkg files> [<commit>]
#
#   * all five packages are there;
#   * BC-17 (d10 L15, D10C-5), one rule list, one comparison per row and per target-framework
#     dependency group, every breakage on ONE ::error:: line:
#       Eiva.Fletcher.Interop   exactly the two edge shims under runtimes/; no Fletcher dependency
#       Eiva.Fletcher.Server    exactly the two server shims; depends on Eiva.Fletcher.Interop;
#                               no entry outside its allow-list (no assembly: lib/<tfm>/_._)
#       Eiva.Fletcher           no runtimes/; depends on Eiva.Fletcher.Interop, not on the server
#       Model, GatewayClient    no runtimes/; no Fletcher dependency
#     so nothing but the server package carries the server shim and nothing but an app's own
#     choice (a reference to it) brings it (D-BIND-13 as extended); a shim anywhere but its own
#     runtimes/ entry (contentFiles/, build/, lib/) is refused too, and a dependency outside every
#     group; no package carries the probe shim;
#   * every package carries LICENSE, COPYING (the GPL-3.0 text the LGPL incorporates,
#     D-BIND-152) and THIRD-PARTY-LICENSES.txt at its root, and the notice is not empty;
#   * D-BIND-175 (approved by EIVA legal, ruling 2026-11-17): the four managed packages carry
#     LICENSE-EXCEPTION, the Fletcher Linking Exception, at their root, the tree's text; the
#     server package, a native library the exception leaves under the LGPL alone, does not;
#   * BIND-9a-2 (D-BIND-140 to 142, D-BIND-149): every package carries its README.md at
#     its root, named by the nuspec, and it is dotnet/README.package.md with exactly the
#     version and the commit filled in (so nothing was lost or mangled on the way, and
#     the floors it states are the ones in shim_floors.env); BIND-9b-7 (D-BIND-172): the
#     loader's message for a shim the OS refuses states the same floors;
#   * BIND-9b-1 (D-BIND-157): Eiva.Fletcher.Interop is at most 12 MB packed; the server package
#     at most SERVER_CEILING (d10 Q7);
#   * D10C-5: only the server package's notice lists arrow (Arrow C++, linked by the server
#     shim alone); every other package carries the edge notice (d9 L8);
#   * given a <commit> (D-BIND-145): the README names it, and so does the nuspec's
#     <repository commit>, which SourceLink and NuGet show;
#   * BIND-9b-7 (review L14, D-BIND-173): the shim's lockfiles (the edge and the server one, per
#     platform) are beside the packages, for a release to attach; scripts/check_conan_lockfile.sh
#     checked their content.
set -euo pipefail

DIR=${1:?usage: check_dotnet_packages.sh <folder> [<commit>]}
COMMIT=${2:-}
HERE=$(cd "$(dirname "$0")" && pwd)
TEMPLATE="$HERE/../dotnet/README.package.md"
LOADER="$HERE/../dotnet/src/Fletcher.Interop/NativeLoader.cs"
EXCEPTION="$HERE/../LICENSE-EXCEPTION"
# shellcheck source=shim_floors.env
. "$HERE/shim_floors.env"
fail=0
err() { echo "::error::$*"; fail=1; }
# 12 MB (D-BIND-157), overridable only to rehearse the check.
INTEROP_CEILING=${INTEROP_CEILING:-12000000}
# D10C-5 (d10 Q7): the server package's first measured pack plus the 4.3 MB D-BIND-157 left
# (12 - 7.7), rounded up to a whole MB: 12,848,400 + 4,300,000 = 17,148,400, so 18 MB. Measured
# 2026-11-16 in the devcontainer pack (image fletcher-build:latest, 6524a5a33e83; commit
# 8fe7e86 plus this stage's changes; Linux server shim stripped to 20,523,920 bytes, Windows
# msvc194 Release shim from the Conan cache). CI re-confirms and does not gate the close: the
# constant changes only if CI's bytes round to another whole MB. Overridable, to rehearse it.
SERVER_CEILING=${SERVER_CEILING:-18000000}
SERVER_ID=Eiva.Fletcher.Server
NATIVE_IDS="Eiva.Fletcher Eiva.Fletcher.Interop $SERVER_ID"

# Listing and reading go through Python's zipfile: present on the runners and in the
# devcontainer, where unzip is not.
entries() { python3 -c 'import sys, zipfile; print(chr(10).join(zipfile.ZipFile(sys.argv[1]).namelist()))' "$1"; }
member() { python3 -c 'import sys, zipfile; sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read(sys.argv[2]))' "$1" "$2"; }
nuspec() { python3 -c 'import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); sys.stdout.write(z.read([n for n in z.namelist() if n.endswith(".nuspec")][0]).decode())' "$1"; }

# One line per target-framework dependency group: "<tfm> <the Fletcher dependency ids in it,
# sorted, comma-joined, or none>", and one for a dependency outside every group. Per group,
# never their union: a dependency is required in every group or refused in every group (BC-17).
# A nuspec with no group at all is refused (exit 1, the reason on stdout).
native_deps() {
  python3 - "$@" <<'PY'
import re, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
spec = z.read([n for n in z.namelist() if n.endswith(".nuspec")][0]).decode("utf-8-sig")
native = set(sys.argv[2:])
group = r'<group targetFramework="([^"]+)"\s*(?:/>|>(.*?)</group>)'
ids = lambda body: ",".join(sorted(set(re.findall(r'<dependency id="([^"]+)"', body)) & native)) or "none"
groups = re.findall(group, spec, re.S)
if not groups:
    print("its nuspec has no targetFramework dependency group, so BC-17 has nothing to compare per group")
    sys.exit(1)
for tfm, body in groups:
    print(f"{tfm}\t{ids(body)}")
loose = ids(re.sub(r'<group targetFramework="[^"]+"\s*(?:/>|>.*?</group>)', '', spec, flags=re.S))
if loose != "none":
    print(f"(outside any targetFramework group)\t{loose}")
PY
}

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

for id in Eiva.Fletcher Eiva.Fletcher.Interop $SERVER_ID Eiva.Fletcher.Model Eiva.Fletcher.GatewayClient; do
  PKG=$(pkg "$id")
  if [ -z "$PKG" ]; then err "$id was not packed"; continue; fi
  VERSION=$(basename "$PKG" .nupkg); VERSION=${VERSION#"$id".}
  ENTRIES=$(entries "$PKG")
  if [ "$id" = "$SERVER_ID" ]; then LIB="lib/<tfm>/_._ placeholder (it has no assembly)"; else LIB="lib/ assembly"; fi
  echo "$ENTRIES" | grep -q '^lib/net' || err "$PKG has no $LIB"
  # BC-17's row for this package: its runtimes/ entries, its Fletcher dependencies, its ceiling.
  CEILING=
  case "$id" in
    Eiva.Fletcher.Interop)
      WANT_RT="runtimes/linux-x64/native/libfletcher-c-abi.so runtimes/win-x64/native/fletcher-c-abi.dll"
      WANT_DEPS=; CEILING=$INTEROP_CEILING ;;
    "$SERVER_ID")
      WANT_RT="runtimes/linux-x64/native/libfletcher-c-abi-server.so runtimes/win-x64/native/fletcher-c-abi-server.dll"
      WANT_DEPS=Eiva.Fletcher.Interop; CEILING=$SERVER_CEILING ;;
    Eiva.Fletcher) WANT_RT=; WANT_DEPS=Eiva.Fletcher.Interop ;;
    *) WANT_RT=; WANT_DEPS= ;;
  esac
  for f in LICENSE COPYING THIRD-PARTY-LICENSES.txt; do
    echo "$ENTRIES" | grep -qx "$f" || err "$PKG does not carry $f at its root (D-BIND-140, D-BIND-152)"
  done
  if echo "$ENTRIES" | grep -qx THIRD-PARTY-LICENSES.txt; then
    [ "$(member "$PKG" THIRD-PARTY-LICENSES.txt | wc -c)" -gt 200 ] || err "$PKG: THIRD-PARTY-LICENSES.txt is empty"
  fi
  # D-BIND-175: the linking exception covers the managed assemblies, not the native libraries.
  if [ "$id" = "$SERVER_ID" ]; then
    if echo "$ENTRIES" | grep -qx LICENSE-EXCEPTION; then
      err "$PKG carries LICENSE-EXCEPTION, which D-BIND-175 does not put in a native-library package (the server shim stays under the LGPL alone)"
    fi
  elif ! echo "$ENTRIES" | grep -qx LICENSE-EXCEPTION; then
    err "$PKG does not carry LICENSE-EXCEPTION at its root (D-BIND-175)"
  elif ! member "$PKG" LICENSE-EXCEPTION | tr -d '\r' | cmp -s - <(tr -d '\r' < "$EXCEPTION"); then
    err "$PKG: its LICENSE-EXCEPTION is not the tree's (D-BIND-175)"
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
  RUNTIMES=$(echo "$ENTRIES" | grep '^runtimes/' | sort | tr '\n' ' ' | sed 's/ $//' || true)
  if [ -n "$CEILING" ]; then
    # D-BIND-157 (Interop: 8.6 MB with an unstripped Linux shim, about 7.7 MB stripped) and d10 Q7
    # (the server package): above the ceiling something grew unnoticed, so the pack fails.
    SIZE=$(wc -c < "$PKG")
    echo "$PKG: $SIZE bytes packed (ceiling $CEILING)"
    [ "$SIZE" -le "$CEILING" ] || err "$PKG is $SIZE bytes packed, above the $CEILING-byte ceiling (D-BIND-157, d10 Q7)"
  fi
  if [ "$RUNTIMES" != "$WANT_RT" ]; then
    err "$PKG: its runtimes/ entries must be exactly [${WANT_RT:-none}] (BC-17, d10 L15); it carries: ${RUNTIMES:-nothing}"
  fi
  # Captured, not piped into the loop: a failed read of the nuspec must stop the check, not
  # leave it with no comparison made.
  if DEP_LINES=$(native_deps "$PKG" $NATIVE_IDS | tr -d '\r'); then
    while IFS=$'\t' read -r tfm have; do
      [ "$have" = "${WANT_DEPS:-none}" ] || err "$PKG: its $tfm dependency group has the Fletcher dependencies {$have}; BC-17 (d10 L15) requires exactly {${WANT_DEPS:-none}}"
    done <<< "$DEP_LINES"
  else
    err "$PKG: ${DEP_LINES:-the dependency groups could not be read} (BC-17, d10 L15)"
  fi
  # A shim anywhere but its own runtimes/ entry (contentFiles/, build/, lib/ ...) would reach an
  # app's output folder, where the loader prefers the server shim (d10 L5): so only the server
  # package carries it, and only there.
  STRAY=
  for entry in $(echo "$ENTRIES" | grep -E '(^|/)(lib)?fletcher-c-abi(-server)?\.(so|dll)$' || true); do
    case " $WANT_RT " in *" $entry "*) ;; *) STRAY="$STRAY $entry" ;; esac
  done
  [ -z "$STRAY" ] || err "$PKG: carries a shim outside its own runtimes/ entries (BC-17, d10 L15):$STRAY"
  if [ "$id" = "$SERVER_ID" ]; then
    # No assembly, no managed file: the package is the shim and NuGet's own metadata.
    EXTRA=$(echo "$ENTRIES" | grep -vE '^(\[Content_Types\]\.xml|_rels/.*|package/.*|[^/]+\.nuspec|LICENSE|COPYING|THIRD-PARTY-LICENSES\.txt|README\.md|lib/[^/]+/_\._|runtimes/.*)$' | tr '\n' ' ' || true)
    [ -z "$EXTRA" ] || err "$PKG: carries entries outside its allow-list (BC-17, d10 L15): $EXTRA"
  fi
  if echo "$ENTRIES" | grep -qi 'probe'; then
    err "$PKG carries the test-only probe shim"
  fi
  # d9 L8: the server shim links Arrow C++, so its notice lists arrow; the edge notice does not.
  if echo "$ENTRIES" | grep -qx THIRD-PARTY-LICENSES.txt; then
    if member "$PKG" THIRD-PARTY-LICENSES.txt | awk '/^=+$/ {n++; next} n == 1 && /^  \* / {print}' | grep -q '^  \* arrow '; then
      [ "$id" = "$SERVER_ID" ] || err "$PKG: THIRD-PARTY-LICENSES.txt lists arrow, which only the server package's shim links (the edge notice, d9 L8)"
    elif [ "$id" = "$SERVER_ID" ]; then
      err "$PKG: THIRD-PARTY-LICENSES.txt does not list arrow, which the server shim links (d9 L8, D-BIND-152)"
    fi
  fi
  echo "$PKG: checked"
done
for rid in linux-x64 win-x64; do
  for lock in "fletcher-shim-$rid.conan.lock" "fletcher-shim-server-$rid.conan.lock"; do
    if [ -s "$DIR/$lock" ]; then
      echo "$DIR/$lock: present"
    else
      err "no $lock beside the packages, the lockfile a release attaches for the relinking route (D-BIND-173)"
    fi
  done
done
exit $fail
