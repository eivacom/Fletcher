#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# BIND-9b-6 (D-BIND-28): the tag's version must be the packages' version, read out of
# Directory.Build.props with sed alone (no SDK on the runner): <VersionPrefix>, and
# -<VersionSuffix> when the suffix is not empty.
#
#   verify.sh <Directory.Build.props> <tag-prefix> <ref>     e.g. ... dotnet-v refs/tags/dotnet-v0.6.0-alpha
set -euo pipefail

PROPS=${1:?usage: verify.sh <props> <tag-prefix> <ref>}
PREFIX=${2:?tag prefix}
REF=${3:?ref}

value() { sed -n "s:.*<$1>\([^<]*\)</$1>.*:\1:p" "$PROPS" | head -1; }
prefix=$(value VersionPrefix)
suffix=$(value VersionSuffix)
if [ -z "$prefix" ]; then
  echo "::error::$PROPS has no <VersionPrefix>"
  exit 1
fi
version=$prefix${suffix:+-$suffix}

case "$REF" in
  refs/tags/"$PREFIX"*) tag_version=${REF#refs/tags/"$PREFIX"} ;;
  *) echo "::error::'$REF' is not a tag starting with $PREFIX"; exit 1 ;;
esac
if [ "$tag_version" != "$version" ]; then
  echo "::error::Tag version '$tag_version' does not match $PROPS version '$version' (tag: $REF)"
  exit 1
fi
echo "Tag and $PROPS versions match: $version"
