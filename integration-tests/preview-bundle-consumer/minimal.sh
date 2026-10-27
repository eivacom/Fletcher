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
# One container at a time: the fastdds route uses a fixed domain, and two consumers on one
# Docker network would cross. Linux only (Docker with Linux containers).
set -euo pipefail

WORK=$(cd "${1:?usage: minimal.sh <the work folder run.sh used>}" && pwd)
APP=$WORK/dotnet/PreviewConsumer/bin/Release
[ -f "$WORK/gateway/gateway" ] || { echo "::error::no gateway in $WORK/gateway: run run.sh first"; exit 1; }

# run_in <image> <tfm> <output file>: the consumer, as built, in the image; returns its exit code.
run_in() {
  [ -s "$APP/$2/PreviewConsumer.dll" ] || { echo "::error::no $2 build in $APP: run run.sh first"; exit 1; }
  docker run --rm -v "$WORK:/c" "$1" sh -c \
    "ldd --version 2>&1 | head -1; cd /c/dotnet/PreviewConsumer/bin/Release/$2 && exec dotnet PreviewConsumer.dll /c/gateway/gateway" \
    > "$3" 2>&1
}

for tfm in net8.0 net10.0; do
  image=mcr.microsoft.com/dotnet/runtime:${tfm#net}-noble
  echo "── $tfm on $image"
  docker pull -q "$image" > /dev/null
  if ! run_in "$image" "$tfm" "$WORK/minimal-$tfm.log"; then
    cat "$WORK/minimal-$tfm.log"
    echo "::error::the Fletcher consumer failed on $image, a machine at the stated floor"; exit 1
  fi
  head -1 "$WORK/minimal-$tfm.log"
  grep -q '^PreviewConsumer: OK$' "$WORK/minimal-$tfm.log" || { cat "$WORK/minimal-$tfm.log"; echo "::error::no OK from the consumer on $image"; exit 1; }
  grep '^shim loaded from: ' "$WORK/minimal-$tfm.log"
done

image=mcr.microsoft.com/dotnet/runtime:8.0-bookworm-slim
echo "── net8.0 on $image, below the floor"
docker pull -q "$image" > /dev/null
if run_in "$image" net8.0 "$WORK/minimal-below.log"; then
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
echo "the platform floor holds: green on Ubuntu 24.04 (.NET 8 and 10), refused with a clear message on Debian 12"
