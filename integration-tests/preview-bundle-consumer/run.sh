#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# BIND-9a-2 (D-BIND-139): consumes an UNPACKED preview bundle as a tester would, using only
# what is in it plus public tools (the .NET SDK, nuget.org for Apache.Arrow and protoc).
#
#   run.sh <unpacked bundle folder> [<work folder>]
#
# Copies dotnet/PreviewConsumer and proto/ into the work folder (outside any checkout, so
# nothing of the repository's build applies), writes a nuget.config whose package source mapping
# takes every Eiva.Fletcher* package from the bundle's nuget/ and nowhere else, restores into
# an empty package cache, unpacks the bundle's gateway, then builds and runs the consumer
# for each target framework the packages ship. Linux, or Git Bash on Windows.
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
echo "preview bundle $VERSION"

rm -rf "$WORK/dotnet" "$WORK/proto" "$WORK/gateway" "$WORK/plugin" "$WORK/packages"
cp -r "$HERE/dotnet" "$HERE/proto" "$WORK/"
rm -rf "$WORK/dotnet/PreviewConsumer/bin" "$WORK/dotnet/PreviewConsumer/obj"
mkdir -p "$WORK/gateway" "$WORK/plugin"

# A downloaded zip keeps no executable bit, so the plugin and the gateway are copied out
# of the bundle and made executable, as README-preview.md tells a tester to.
if [ "$WINDOWS" = 1 ]; then
  unzip -q "$BUNDLE/gateway/gateway-windows.zip" -d "$WORK/gateway"
  GATEWAY=$WORK/gateway/gateway.exe
  cp "$BUNDLE/protoc/protoc-gen-fletcher-windows-x64.exe" "$WORK/plugin/"
  PLUGIN=$WORK/plugin/protoc-gen-fletcher-windows-x64.exe
else
  tar -xzf "$BUNDLE/gateway/gateway-linux.tar.gz" -C "$WORK/gateway"
  GATEWAY=$WORK/gateway/gateway
  cp "$BUNDLE/protoc/protoc-gen-fletcher-linux-x64" "$WORK/plugin/"
  PLUGIN=$WORK/plugin/protoc-gen-fletcher-linux-x64
  chmod +x "$GATEWAY" "$PLUGIN"
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
dotnet restore "$PROJECT" --configfile "$(native "$WORK/nuget.config")" -p:FletcherPreviewVersion="$VERSION"

for tfm in net8.0 net10.0; do
  echo "── $tfm"
  dotnet run --project "$PROJECT" -c Release -f "$tfm" --no-restore \
    -p:FletcherPreviewVersion="$VERSION" \
    -p:FletcherPlugin="$(native "$PLUGIN")" \
    -p:FletcherBundleProtoc="$(native "$BUNDLE/protoc")" \
    -- "$(native "$GATEWAY")"
done
echo "preview bundle $VERSION: the consumer passed on every target framework"
