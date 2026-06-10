# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# Download all Fletcher Conan packages for Windows from GitHub Releases and
# restore them into the local Conan cache.
#
# Usage:
#   .\scripts\install-conan-packages.ps1 [-Version <version>]
#
#   -Version  e.g. 0.3.0-alpha  (default: 0.3.0-alpha)
#
# Requires: gh (GitHub CLI, https://cli.github.com), conan 2

param(
    [string]$Version = "0.3.0-alpha"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

foreach ($tool in "gh", "conan") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "'$tool' is required but not installed."
    }
}

gh auth status *> $null
if ($LASTEXITCODE -ne 0) {
    throw "GitHub CLI is not authenticated. Run 'gh auth login' (or set GH_TOKEN)."
}

$conanVersion = conan --version
if ($conanVersion -notmatch '^Conan version 2\.') {
    throw "Conan 2 is required (found: '$conanVersion')."
}

$Repo      = "eivacom/Fletcher"
$Platform  = "windows"

$Components = @(
    "core",
    "pubsub",
    "arrow-bridge",
    "pubsub-arrow",
    "fastdds-pubsub-provider",
    "xrcedds-pubsub-provider",
    "protoc"
)

Write-Host "Installing Fletcher $Version ($Platform) into the local Conan cache..."
Write-Host ""

foreach ($component in $Components) {
    $tag  = "$component-v$Version"
    $file = "fletcher-$component-$Platform-conan-package.tgz"

    Write-Host ">>>  $tag"
    gh release download $tag --repo $Repo --pattern $file --clobber
    conan cache restore $file
    Remove-Item $file
    Write-Host ""
}

$q = [char]34
Write-Host "Done. Add to your conanfile.py:"
foreach ($component in $Components) {
    Write-Host "     self.requires(${q}fletcher-$component/$Version${q})"
}
