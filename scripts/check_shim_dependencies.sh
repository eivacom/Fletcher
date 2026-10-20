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
# Prints the dependency list (the README's floors are read off it) and exits non-zero
# naming any dependency outside the allowed set.
#   linux-x64: exactly libstdc++, libgcc_s, libm, libc and the loader.
#   win-x64:   the VC++ runtime (msvcp140*, vcruntime140*), the UCRT (api-ms-win-*,
#              ucrtbase) and Windows system DLLs, meaning a file present in
#              System32 that is not one of Fletcher's dependencies by name.
set -euo pipefail

SHIM=${1:?usage: check_shim_dependencies.sh <shim> [dumpbin]}
[ -f "$SHIM" ] || { echo "::error::no shim at $SHIM"; exit 1; }

# Names of libraries that are linked INTO the shim; any of them as a load-time
# dependency means a static link silently became a shared one.
OURS='fletcher|fast|foonathan|tinyxml|nanoarrow|microxrce|xrce|arrow|protobuf|ssl|crypto|zlib|bz2'

bad=()
case "$SHIM" in
  *.so)
    deps=$(objdump -p "$SHIM" | awk '/NEEDED/ {print $2}')
    echo "linux-x64 shim loads:"; echo "$deps" | sed 's/^/  /'
    while read -r dep; do
      [ -z "$dep" ] && continue
      case "$dep" in
        libstdc++.so.6|libgcc_s.so.1|libm.so.6|libc.so.6|ld-linux-x86-64.so.2) ;;
        *) bad+=("$dep") ;;
      esac
    done <<< "$deps"
    echo "glibc floor: $(objdump -T "$SHIM" | grep -oE 'GLIBC_[0-9]+[.][0-9]+' | sort -u -V | tail -1)"
    echo "libstdc++ floor: $(objdump -T "$SHIM" | grep -oE 'GLIBCXX_[0-9]+[.][0-9]+[.0-9]*' | sort -u -V | tail -1)"
    ;;
  *.dll)
    DUMPBIN=${2:?a .dll needs the path to dumpbin.exe}
    # dumpbin lists the dependents indented under "Image has the following dependencies:".
    deps=$("$DUMPBIN" //nologo //dependents "$SHIM" | tr -d '\r' \
      | awk '/following dependencies/ {on=1; next} on && /^ *$/ && seen {exit} on && NF {seen=1; print $1}')
    echo "win-x64 shim loads:"; echo "$deps" | sed 's/^/  /'
    SYS32=${SYSTEMROOT:-C:/Windows}/System32
    while read -r dep; do
      [ -z "$dep" ] && continue
      lower=$(echo "$dep" | tr 'A-Z' 'a-z')
      if echo "$lower" | grep -qE "^($OURS)"; then bad+=("$dep"); continue; fi
      case "$lower" in
        msvcp140*.dll|vcruntime140*.dll|api-ms-win-*.dll|ucrtbase.dll) continue ;;
      esac
      [ -f "$SYS32/$dep" ] || [ -f "$SYS32/$lower" ] || bad+=("$dep (not a Windows system DLL)")
    done <<< "$deps"
    ;;
  *) echo "::error::unknown shim type: $SHIM"; exit 1 ;;
esac

if [ "${#bad[@]}" -gt 0 ]; then
  for dep in "${bad[@]}"; do
    echo "::error::the shim loads $dep, which is neither the operating system nor the C/C++ runtime (D-BIND-134)"
  done
  exit 1
fi
echo "$SHIM: self-contained (only the operating system and the C/C++ runtime)"
