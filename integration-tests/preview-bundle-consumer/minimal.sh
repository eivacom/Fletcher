#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# BIND-9b-7 (review T14, D-BIND-172): the linux-x64 platform floor, tested on machines that
# have only what a user's has. run.sh builds and runs the consumers on a CI runner, which
# carries compilers and development libraries; this runs the Fletcher consumer run.sh built,
# with the bundle's gateway, in Microsoft's official .NET runtime images, which hold the
# operating system and .NET and nothing else.
#
#   minimal.sh <the work folder run.sh used>
#
#   * Ubuntu 24.04 (mcr.microsoft.com/dotnet/runtime:<8.0|10.0>-noble), the oldest
#     distribution the package README names and the home of the Linux-gcc13 build profile's
#     compiler: every route must pass, on .NET 8 and .NET 10, the gateway included.
#   * Debian 12 (runtime:8.0-bookworm-slim), below the floor (glibc 2.36, GCC 12's C++
#     runtime): the consumer must FAIL, with the loader's message for a shim that is there
#     and refused (the operating system's reason, naming GLIBCXX_3.4.32, and the floor), not
#     with a message blaming the RID.
# D10C-11: both configurations run.sh built (PreviewConsumer: Eiva.Fletcher only; and
# PreviewConsumerServer: plus Eiva.Fletcher.Server) are run in each image. At the floor the base
# one must get its named refusal for a batch and the server one its batches, each on its own shim.
# Below it the server build must be refused for its own file, and the app must not fall back to the
# edge shim (d10 L5).
# One container at a time: the fastdds route uses a fixed domain, and two consumers on one
# Docker network would cross. Linux only (Docker with Linux containers).
set -euo pipefail

WORK=$(cd "${1:?usage: minimal.sh <the work folder run.sh used>}" && pwd)
[ -f "$WORK/gateway/gateway" ] || { echo "::error::no gateway in $WORK/gateway: run run.sh first"; exit 1; }

# run_in <image> <project folder> <base|server> <tfm> <output file>: the consumer, as built, in the
# image, told its configuration; returns its exit code.
run_in() {
  local app=$WORK/dotnet/$2/bin/Release
  [ -s "$app/$4/PreviewConsumer.dll" ] || { echo "::error::no $4 build in $app: run run.sh first"; exit 1; }
  docker run --rm -v "$WORK:/c" "$1" sh -c \
    "ldd --version 2>&1 | head -1; cd /c/dotnet/$2/bin/Release/$4 && exec dotnet PreviewConsumer.dll /c/gateway/gateway $3" \
    > "$5" 2>&1
}

for tfm in net8.0 net10.0; do
  image=mcr.microsoft.com/dotnet/runtime:${tfm#net}-noble
  echo "── $tfm on $image"
  docker pull -q "$image" > /dev/null
  for config in "PreviewConsumer base" "PreviewConsumerServer server"; do
    read -r project mode <<< "$config"
    log=$WORK/minimal-$tfm-$mode.log
    if ! run_in "$image" "$project" "$mode" "$tfm" "$log"; then
      cat "$log"
      echo "::error::the $mode consumer failed on $image, a machine at the stated floor"; exit 1
    fi
    head -1 "$log"
    grep -q '^PreviewConsumer: OK$' "$log" || { cat "$log"; echo "::error::no OK from the $mode consumer on $image"; exit 1; }
    echo "$mode: $(grep '^shim loaded from: ' "$log")"
  done
done

image=mcr.microsoft.com/dotnet/runtime:8.0-bookworm-slim
echo "── net8.0 on $image, below the floor"
docker pull -q "$image" > /dev/null
if run_in "$image" PreviewConsumer base net8.0 "$WORK/minimal-below.log"; then
  cat "$WORK/minimal-below.log"
  echo "::error::the consumer ran on $image, below the stated floor: the floor is lower than stated"; exit 1
fi
head -1 "$WORK/minimal-below.log"
# The operating system's own reason ("version `GLIBCXX_3.4.32' not found"), not only the floor
# sentence, which names GLIBCXX_3.4.32 too.
for expected in "found its native shim" "GLIBCXX_3.4.32' not found" "glibc 2.34 and the GCC 13 C++ runtime"; do
  grep -qF "$expected" "$WORK/minimal-below.log" || {
    cat "$WORK/minimal-below.log"
    echo "::error::below the floor, the loader's message does not say '$expected'"; exit 1
  }
done
echo "below the floor, the consumer fails with the loader's message naming the cause"

# The server build has the same floor (d10 L15): refused, naming ITS file, and never the edge shim
# loaded in its place (L5). The edge shim shares the floor, so Debian 12 would refuse it too: a
# fallback could not run here and would show only as a refusal naming the edge file, which this
# arm forbids. That no fallback happens where it could run is the at-floor exactly-one check.
echo "── net8.0 on $image, below the floor, with Eiva.Fletcher.Server"
if run_in "$image" PreviewConsumerServer server net8.0 "$WORK/minimal-below-server.log"; then
  cat "$WORK/minimal-below-server.log"
  echo "::error::the server consumer ran on $image, below the stated floor"; exit 1
fi
head -1 "$WORK/minimal-below-server.log"
for expected in "found its native shim" "glibc 2.34 and the GCC 13 C++ runtime" "That file is the server build ('fletcher-c-abi-server')"; do
  grep -qF "$expected" "$WORK/minimal-below-server.log" || {
    cat "$WORK/minimal-below-server.log"
    echo "::error::below the floor, the server build's message does not say '$expected'"; exit 1
  }
done
# The operating system's own reason, as a REGEX (grep -E, not -F: the brackets are a class). The
# loader names the FIRST unmet GLIBCXX_ version it reaches, not the highest: Debian 12 reports
# 3.4.31 for the server shim and 3.4.32 for the edge shim, though both have the same floor,
# GLIBCXX_3.4.32. That value is proven at pack time, where check_shim_dependencies.sh requires each
# shim's highest GLIBCXX_ to EQUAL scripts/shim_floors.env (ci.dotnet.yml), and the floor sentence
# above is a constant of the loader, so it proves nothing about the shim: the "' not found" suffix
# is what keeps this the OS's reason (D10C-11 step 0b, PM decision).
if ! grep -qE "GLIBCXX_3\.4\.3[12]' not found" "$WORK/minimal-below-server.log"; then
  cat "$WORK/minimal-below-server.log"
  echo "::error::below the floor, the server build's message does not carry the operating system's reason (GLIBCXX_3.4.31 or 3.4.32 not found)"; exit 1
fi
if grep -qF "That file is the edge build" "$WORK/minimal-below-server.log"; then
  cat "$WORK/minimal-below-server.log"
  echo "::error::below the floor, the server build's refusal names the edge file: the loader fell back to it"; exit 1
fi
echo "below the floor, the server build is refused for its own file, with no fallback to the edge shim"
echo "the platform floor holds: green on Ubuntu 24.04 (.NET 8 and 10, both configurations), refused with a clear message on Debian 12 (both builds)"
