#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# BIND-9b-5 (D-BIND-71, D-BIND-158): checks on the PACKED Eiva.Fletcher.Protoc, because the
# package is what a C# project restores.
#
#   check_protoc_package.sh <Eiva.Fletcher.Protoc.<version>.nupkg> <dist folder> [<commit>]
#
# <dist folder> holds what ci.protoc's legs uploaded: protoc-gen-fletcher-linux-x64,
# protoc-gen-fletcher-windows-x64.exe, LICENSE, COPYING and THIRD-PARTY-LICENSES.txt.
#
#   * the two plugins are there, under tools/linux-x64/ and tools/win-x64/, byte for byte
#     the binaries the release and the npm shim carry;
#   * fletcher/options.proto is there, the tree's, with its csharp_namespace (D-BIND-158);
#   * build/ holds exactly the package's props and targets;
#   * LICENSE, COPYING and THIRD-PARTY-LICENSES.txt are at the root, the plugin's own, and the
#     README the nuspec names;
#   * nothing a project would reference or load: no lib/, ref/ or runtimes/, no dependencies;
#   * the nuspec marks it a development dependency and, given a <commit>, names that commit.
set -euo pipefail

PKG=${1:?usage: check_protoc_package.sh <nupkg> <dist folder> [<commit>]}
DIST=${2:?dist folder}
COMMIT=${3:-}
HERE=$(cd "$(dirname "$0")" && pwd)
OPTIONS="$HERE/../protoc/include/fletcher/options.proto"
[ -s "$PKG" ] || { echo "::error::no package at $PKG"; exit 1; }

python3 - "$PKG" "$DIST" "$OPTIONS" "$COMMIT" <<'PY'
import re, sys, zipfile

pkg, dist, options, commit = sys.argv[1:5]
z = zipfile.ZipFile(pkg)
names = z.namelist()
failed = False

def err(message):
    global failed
    failed = True
    print(f"::error::Eiva.Fletcher.Protoc: {message}")

def same(member, path):
    if member not in names:
        err(f"has no {member}")
        return
    with open(path, "rb") as f:
        if z.read(member) != f.read():
            err(f"{member} is not {path}")

same("tools/linux-x64/protoc-gen-fletcher-linux-x64", f"{dist}/protoc-gen-fletcher-linux-x64")
same("tools/win-x64/protoc-gen-fletcher-windows-x64.exe", f"{dist}/protoc-gen-fletcher-windows-x64.exe")
same("include/fletcher/options.proto", options)
for licence in ("LICENSE", "COPYING", "THIRD-PARTY-LICENSES.txt"):
    same(licence, f"{dist}/{licence}")
if "include/fletcher/options.proto" in names and \
        b'option csharp_namespace = "Eiva.Fletcher.Protobuf";' not in z.read("include/fletcher/options.proto"):
    err("options.proto has no csharp_namespace")

build = sorted(n for n in names if n.startswith("build/"))
if build != ["build/Eiva.Fletcher.Protoc.props", "build/Eiva.Fletcher.Protoc.targets"]:
    err(f"build/ holds {build}, not exactly the props and the targets")
loadable = [n for n in names if n.split("/")[0] in ("lib", "ref", "runtimes")]
if loadable:
    err(f"carries what a project would reference or load: {loadable}")

nuspec = z.read(next(n for n in names if n.endswith(".nuspec"))).decode("utf-8")
if "<developmentDependency>true</developmentDependency>" not in nuspec:
    err("the nuspec does not mark it a development dependency")
if "<dependency " in nuspec:
    err("the nuspec declares dependencies")
readme = re.search(r"<readme>([^<]+)</readme>", nuspec)
if not readme or readme.group(1) not in names:
    err("the nuspec names no README that the package carries")
if commit and f'commit="{commit}"' not in nuspec:
    err(f"the nuspec's repository does not name commit {commit}")

if failed:
    sys.exit(1)
print(f"Eiva.Fletcher.Protoc checked: {len(names)} entries, both plugins, options.proto, licences, build files")
PY
