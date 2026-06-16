# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# Download all Fletcher Conan packages for Windows from GitHub Releases and
# restore them into the local Conan cache.
#
# Usage:
#   .\scripts\install-conan-packages.ps1 [-Pin @{ component = "version"; ... }]
#
#   -Pin   pin specific components, e.g. -Pin @{ "fastdds-pubsub-provider" = "0.3.2-alpha" }
#
# By default each component is fetched at its LATEST release (resolved via the
# GitHub CLI). Components you don't pin still use their latest release. PATCH
# floats independently per component (see the versioning section in README.md).
#
# Requires: gh (GitHub CLI, https://cli.github.com), conan 2

param(
    [hashtable]$Pin = @{}
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

$Repo     = "eivacom/Fletcher"
$Platform = "windows"

$Components = @(
    "core",
    "pubsub",
    "arrow-bridge",
    "pubsub-arrow",
    "fastdds-pubsub-provider",
    "xrcedds-pubsub-provider",
    "protoc"
)

# Fail fast on a pin for an unknown component (otherwise a typo would be
# silently ignored and the latest release installed instead).
foreach ($key in $Pin.Keys) {
    if ($Components -notcontains $key) {
        throw "Unknown component '$key' in -Pin (valid: $($Components -join ', '))."
    }
}

# All release tags, newest first. Split on \r?\n so CRLF output doesn't leave a
# stray carriage return in a tag. The latest release of a component is its most
# recent tag matching '<component>-v'.
$tagsRaw = gh release list --repo $Repo --limit 300 --json tagName --jq '.[].tagName'
if ($LASTEXITCODE -ne 0) {
    throw "Failed to list releases from $Repo."
}
$tagList = $tagsRaw -split "\r?\n" | Where-Object { $_ -ne "" }

Write-Host "Installing Fletcher packages ($Platform) into the local Conan cache..."
Write-Host ""

$installed = @()
foreach ($component in $Components) {
    if ($Pin.ContainsKey($component)) {
        $version = $Pin[$component]
        $tag = "$component-v$version"
    }
    else {
        $tag = $tagList | Where-Object { $_ -like "$component-v*" } | Select-Object -First 1
        if (-not $tag) { throw "No release found for '$component'." }
        $version = $tag.Substring("$component-v".Length)
    }
    $file = "fletcher-$component-$Platform-conan-package.tgz"

    Write-Host ">>>  $tag"
    gh release download $tag --repo $Repo --pattern $file --clobber
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to download $file from release $tag."
    }
    conan cache restore $file
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to restore $file into the local Conan cache."
    }
    Remove-Item $file
    $installed += "fletcher-$component/$version"
    Write-Host ""
}

$q = [char]34
Write-Host "Done. Add to your conanfile.py:"
foreach ($ref in $installed) {
    Write-Host "     self.requires(${q}${ref}${q})"
}
