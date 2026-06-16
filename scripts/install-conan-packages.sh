#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# Download all Fletcher Conan packages for the current platform from GitHub
# Releases and restore them into the local Conan cache.
#
# Usage:
#   ./scripts/install-conan-packages.sh [--platform linux|windows] [component=version ...]
#
#   --platform        linux | windows (default: auto-detected)
#   component=version pin a component, e.g. fastdds-pubsub-provider=0.3.2-alpha.
#
# By default each component is fetched at its LATEST release (resolved via the
# GitHub CLI). Components you don't pin still use their latest release. PATCH
# floats independently per component (see the versioning section in README.md).
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

PLATFORM=""
declare -A PIN=()
while [ $# -gt 0 ]; do
  case "$1" in
    --platform)    shift; PLATFORM="${1:?--platform needs a value}" ;;
    linux|windows) PLATFORM="$1" ;;
    *=*)           PIN["${1%%=*}"]="${1#*=}" ;;
    *) echo "Error: unrecognized argument '$1' (expected --platform, a platform, or component=version)." >&2; exit 1 ;;
  esac
  shift
done

# Fail fast on a pin for an unknown component (otherwise a typo would be
# silently ignored and the latest release installed instead).
for key in "${!PIN[@]}"; do
  printf '%s\n' "${COMPONENTS[@]}" | grep -qxF "${key}" || {
    echo "Error: unknown component '${key}' in pin (valid: ${COMPONENTS[*]})." >&2
    exit 1
  }
done

if [ -z "$PLATFORM" ]; then
  case "$(uname -s)" in
    Linux*)               PLATFORM="linux" ;;
    MINGW*|CYGWIN*|MSYS*) PLATFORM="windows" ;;
    *) echo "Error: platform not auto-detected; pass --platform linux|windows." >&2; exit 1 ;;
  esac
fi

# All release tags, newest first. tr -d '\r' guards against CRLF in the CLI
# output (Git Bash) leaving a stray carriage return in a tag. The latest release
# of a component is its most recent tag matching '<component>-v'.
ALL_TAGS="$(gh release list --repo "${REPO}" --limit 300 --json tagName --jq '.[].tagName' | tr -d '\r')"

echo "Installing Fletcher packages (${PLATFORM}) into the local Conan cache..."
echo

INSTALLED=()
for component in "${COMPONENTS[@]}"; do
  version="${PIN[${component}]:-}"
  if [ -z "${version}" ]; then
    tag="$(printf '%s\n' "${ALL_TAGS}" | grep -E "^${component}-v" | head -n1 || true)"
    [ -n "${tag}" ] || { echo "Error: no release found for '${component}'." >&2; exit 1; }
    version="${tag#"${component}-v"}"
  else
    tag="${component}-v${version}"
  fi
  file="fletcher-${component}-${PLATFORM}-conan-package.tgz"

  echo "▶  ${tag}"
  gh release download "${tag}" \
    --repo "${REPO}" \
    --pattern "${file}" \
    --clobber
  conan cache restore "${file}"
  rm "${file}"
  INSTALLED+=("fletcher-${component}/${version}")
  echo
done

echo "✓  Done. Add to your conanfile.py:"
for ref in "${INSTALLED[@]}"; do
  echo "     self.requires(\"${ref}\")"
done
