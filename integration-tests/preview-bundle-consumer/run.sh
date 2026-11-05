#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# BIND-9a-2 (D-BIND-139): consumes an UNPACKED preview bundle as a tester would, using only
# what is in it plus public tools (the .NET SDK, nuget.org for Apache.Arrow and protoc).
#
#   run.sh <unpacked bundle folder> [<work folder>]
#
# Copies dotnet/ (PreviewConsumer with its fix.proto, and GrpcConsumer) into the work folder
# (outside any checkout, so nothing of the repository's build applies), writes a nuget.config
# whose package source mapping takes every Eiva.Fletcher* package from the bundle's nuget/ and
# nowhere else, restores into an empty package cache, unpacks the bundle's gateway, then builds
# and runs both consumers for each target framework the packages ship. The C# is generated with
# the plugin of the bundle's Eiva.Fletcher.Protoc (BIND-9b-5), and GrpcConsumer proves that
# package's Grpc.Tools wiring (D-BIND-158). Linux, or Git Bash on Windows.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BUNDLE=$(cd "${1:?usage: run.sh <unpacked bundle folder> [<work folder>]}" && pwd)
WORK=${2:-$(mktemp -d)}
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)

case "$(uname -s)" in
  MINGW* | MSYS* | CYGWIN*) WINDOWS=1 ;;
  *) WINDOWS=0 ;;
esac
# MSBuild and the processes it starts take Windows paths on Windows.
native() { if [ "$WINDOWS" = 1 ]; then cygpath -m "$1"; else echo "$1"; fi; }

VERSION=$(sed -n 's/^version: //p' "$BUNDLE/VERSION")
[ -n "$VERSION" ] || { echo "::error::$BUNDLE/VERSION names no version"; exit 1; }
# The protoc component's own version (its MAJOR.MINOR is the runtime's; in a preview bundle it
# is the bundle's version), read off the one Eiva.Fletcher.Protoc package in nuget/.
PROTOC_PACKAGES=("$BUNDLE"/nuget/Eiva.Fletcher.Protoc.[0-9]*.nupkg)
[ "${#PROTOC_PACKAGES[@]}" -eq 1 ] && [ -f "${PROTOC_PACKAGES[0]}" ] \
  || { echo "::error::$BUNDLE/nuget must hold exactly one Eiva.Fletcher.Protoc package"; exit 1; }
PROTOC_VERSION=$(basename "${PROTOC_PACKAGES[0]}" .nupkg); PROTOC_VERSION=${PROTOC_VERSION#Eiva.Fletcher.Protoc.}
echo "preview bundle $VERSION, Eiva.Fletcher.Protoc $PROTOC_VERSION"

rm -rf "$WORK/dotnet" "$WORK/gateway" "$WORK/plugin" "$WORK/packages"
cp -r "$HERE/dotnet" "$WORK/"
rm -rf "$WORK"/dotnet/*/bin "$WORK"/dotnet/*/obj
mkdir -p "$WORK/gateway"

# A downloaded zip keeps no executable bit, so the gateway is unpacked and made executable,
# as README-preview.md tells a tester to. The plugin comes from Eiva.Fletcher.Protoc, which
# makes itself executable.
if [ "$WINDOWS" = 1 ]; then
  unzip -q "$BUNDLE/gateway/gateway-windows.zip" -d "$WORK/gateway"
  GATEWAY=$WORK/gateway/gateway.exe
else
  tar -xzf "$BUNDLE/gateway/gateway-linux.tar.gz" -C "$WORK/gateway"
  GATEWAY=$WORK/gateway/gateway
  chmod +x "$GATEWAY"
fi

cat > "$WORK/nuget.config" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources>
    <clear />
    <add key="fletcher-preview" value="$(native "$BUNDLE/nuget")" />
    <add key="nuget.org" value="https://api.nuget.org/v3/index.json" />
  </packageSources>
  <packageSourceMapping>
    <packageSource key="fletcher-preview">
      <package pattern="Eiva.Fletcher*" />
    </packageSource>
    <packageSource key="nuget.org">
      <package pattern="*" />
    </packageSource>
  </packageSourceMapping>
</configuration>
EOF

# An empty cache: a package left by an earlier build cannot stand in for the bundle's.
NUGET_PACKAGES=$(native "$WORK/packages")
export NUGET_PACKAGES
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1

PROJECT=$(native "$WORK/dotnet/PreviewConsumer/PreviewConsumer.csproj")
GRPC_PROJECT=$(native "$WORK/dotnet/GrpcConsumer/GrpcConsumer.csproj")
VERSIONS=(-p:FletcherVersion="$VERSION" -p:FletcherProtocVersion="$PROTOC_VERSION")
dotnet restore "$PROJECT" --configfile "$(native "$WORK/nuget.config")" "${VERSIONS[@]}"
dotnet restore "$GRPC_PROJECT" --configfile "$(native "$WORK/nuget.config")" "${VERSIONS[@]}"

for tfm in net8.0 net10.0; do
  echo "── $tfm"
  dotnet run --project "$PROJECT" -c Release -f "$tfm" --no-restore "${VERSIONS[@]}" \
    -- "$(native "$GATEWAY")"
  echo "── $tfm, the gRPC consumer"
  dotnet run --project "$GRPC_PROJECT" -c Release -f "$tfm" --no-restore "${VERSIONS[@]}"
done
echo "preview bundle $VERSION: both consumers passed on every target framework"
