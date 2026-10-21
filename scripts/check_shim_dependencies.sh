#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# BIND-9a-0 (D-BIND-134): the shim is the NuGet packages' only unmanaged file, and it
# is self-contained: Fletcher's core and pubsub, both providers, the eProsima chain,
# Micro XRCE-DDS and nanoarrow are linked INTO it. So what it loads at run time must
# be the operating system and the C/C++ runtime and nothing else; a dependency of
# ours appearing here would install from the package and then fail to load on a
# tester's machine.
#
#   check_shim_dependencies.sh <libfletcher-c-abi.so | fletcher-c-abi.dll> [dumpbin.exe]
#
# Prints the dependency list and exits non-zero naming any dependency outside the
# allowed set, both of them EXACT lists (D-BIND-148):
#   linux-x64: libstdc++, libgcc_s, libm, libc and the loader; and the highest GLIBC_
#              and GLIBCXX_ symbol versions must EQUAL the floors in shim_floors.env,
#              which the package README states (D-BIND-141, D-BIND-149).
#   win-x64:   the Windows system DLLs measured at 9d214c9 (KERNEL32, ADVAPI32,
#              WS2_32, MSWSOCK, IPHLPAPI, SHLWAPI), the VC++ runtime (MSVCP140,
#              MSVCP140_ATOMIC_WAIT, VCRUNTIME140, VCRUNTIME140_1) and the UCRT
#              (api-ms-win-crt-*). The debug runtime (msvcp140d, ucrtbased) is not
#              in it, and a new DLL needs a ruling before it is added here.
# A list that parses empty fails, and libc.so.6 or KERNEL32.dll must be in it: every
# real shim loads one, so an empty or truncated parse cannot pass as self-contained
# (BIND-9a review, B1).
set -euo pipefail

SHIM=${1:?usage: check_shim_dependencies.sh <shim> [dumpbin]}
[ -f "$SHIM" ] || { echo "::error::no shim at $SHIM"; exit 1; }
# shellcheck source=shim_floors.env
. "$(dirname "$0")/shim_floors.env"

bad=()
fail=0
case "$SHIM" in
  *.so)
    deps=$(objdump -p "$SHIM" | awk '/NEEDED/ {print $2}')
    anchor=libc.so.6
    echo "linux-x64 shim loads:"; echo "$deps" | sed 's/^/  /'
    while read -r dep; do
      [ -z "$dep" ] && continue
      case "$dep" in
        libstdc++.so.6|libgcc_s.so.1|libm.so.6|libc.so.6|ld-linux-x86-64.so.2) ;;
        *) bad+=("$dep") ;;
      esac
    done <<< "$deps"
    symbols=$(objdump -T "$SHIM")
    glibc=$(echo "$symbols" | grep -oE 'GLIBC_[0-9]+[.][0-9]+' | sed 's/^GLIBC_//' | sort -u -V | tail -1 || true)
    glibcxx=$(echo "$symbols" | grep -oE 'GLIBCXX_[0-9]+[.][0-9]+[.0-9]*' | sed 's/^GLIBCXX_//' | sort -u -V | tail -1 || true)
    echo "glibc floor: ${glibc:-none found} (stated: $GLIBC_FLOOR)"
    echo "libstdc++ floor: GLIBCXX_${glibcxx:-none found} (stated: GLIBCXX_$GLIBCXX_FLOOR)"
    if [ "$glibc" != "$GLIBC_FLOOR" ]; then
      echo "::error::the shim's highest GLIBC_ version is '${glibc:-none}', and the README states $GLIBC_FLOOR (scripts/shim_floors.env, D-BIND-149)"
      fail=1
    fi
    if [ "$glibcxx" != "$GLIBCXX_FLOOR" ]; then
      echo "::error::the shim's highest GLIBCXX_ version is '${glibcxx:-none}', and the README states $GLIBCXX_FLOOR (scripts/shim_floors.env, D-BIND-149)"
      fail=1
    fi
    ;;
  *.dll)
    DUMPBIN=${2:?a .dll needs the path to dumpbin.exe}
    anchor=kernel32.dll
    # dumpbin lists the dependents indented under "Image has the following dependencies:".
    deps=$("$DUMPBIN" //nologo //dependents "$SHIM" | tr -d '\r' \
      | awk '/following dependencies/ {on=1; next} on && /^ *$/ && seen {exit} on && NF {seen=1; print $1}')
    echo "win-x64 shim loads:"; echo "$deps" | sed 's/^/  /'
    while read -r dep; do
      [ -z "$dep" ] && continue
      case "$(echo "$dep" | tr 'A-Z' 'a-z')" in
        kernel32.dll|advapi32.dll|ws2_32.dll|mswsock.dll|iphlpapi.dll|shlwapi.dll) ;;
        msvcp140.dll|msvcp140_atomic_wait.dll|vcruntime140.dll|vcruntime140_1.dll) ;;
        api-ms-win-crt-*.dll) ;;
        *) bad+=("$dep") ;;
      esac
    done <<< "$deps"
    ;;
  *) echo "::error::unknown shim type: $SHIM"; exit 1 ;;
esac

if ! echo "$deps" | tr 'A-Z' 'a-z' | grep -qx "$anchor"; then
  echo "::error::the dependency list of $SHIM does not name $anchor, which every shim loads: the list was not read (an empty or unparsed tool output), so nothing was checked"
  fail=1
fi
for dep in "${bad[@]}"; do
  echo "::error::the shim loads $dep, which is not on the allowed list of the operating system and the C/C++ runtime (D-BIND-134, D-BIND-148)"
  fail=1
done
[ "$fail" -eq 0 ] || exit 1
echo "$SHIM: self-contained (only the operating system and the C/C++ runtime)"
