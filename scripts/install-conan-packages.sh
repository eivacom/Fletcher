#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# Download all Fletcher Conan packages for the current platform from GitHub
# Releases and restore them into the local Conan cache.
#
# Usage:
#   ./scripts/install-conan-packages.sh [VERSION] [PLATFORM]
#
#   VERSION   e.g. 0.3.0-alpha        (default: 0.3.0-alpha)
#   PLATFORM  linux | windows         (default: auto-detected)
#
# Requires: gh (GitHub CLI, https://cli.github.com), conan 2

set -euo pipefail

for tool in gh conan; do
  command -v "${tool}" >/dev/null 2>&1 || {
    echo "Error: '${tool}' is required but not installed." >&2
    exit 1
  }
done

gh auth status >/dev/null 2>&1 || {
  echo "Error: GitHub CLI is not authenticated. Run 'gh auth login' (or set GH_TOKEN)." >&2
  exit 1
}

case "$(conan --version)" in
  "Conan version 2."*) ;;
  *) echo "Error: Conan 2 is required (found: '$(conan --version)')." >&2; exit 1 ;;
esac

VERSION="${1:-0.3.0-alpha}"

case "$(uname -s)" in
  Linux*)               PLATFORM="${2:-linux}"   ;;
  MINGW*|CYGWIN*|MSYS*) PLATFORM="${2:-windows}" ;;
  *) PLATFORM="${2:?Platform not auto-detected. Pass 'linux' or 'windows' as \$2.}" ;;
esac

REPO="eivacom/Fletcher"

COMPONENTS=(
  core
  pubsub
  arrow-bridge
  pubsub-arrow
  fastdds-pubsub-provider
  xrcedds-pubsub-provider
  protoc
)

echo "Installing Fletcher ${VERSION} (${PLATFORM}) into the local Conan cache..."
echo

for component in "${COMPONENTS[@]}"; do
  tag="${component}-v${VERSION}"
  file="fletcher-${component}-${PLATFORM}-conan-package.tgz"

  echo "▶  ${tag}"
  gh release download "${tag}" \
    --repo "${REPO}" \
    --pattern "${file}" \
    --clobber
  conan cache restore "${file}"
  rm "${file}"
  echo
done

echo "✓  Done. Add to your conanfile.py:"
for component in "${COMPONENTS[@]}"; do
  echo "     self.requires(\"fletcher-${component}/${VERSION}\")"
done
