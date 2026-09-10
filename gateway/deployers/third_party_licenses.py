# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
r"""Conan deployer that writes THIRD-PARTY-LICENSES.txt into the deploy folder.

The gateway is released as a self-contained archive (gateway-linux.tar.gz /
gateway-windows.zip). Every third-party library it is built from carries a
permissive licence that requires its copyright notice and licence text to be
reproduced in binary distributions -- boost (BSL-1.0), Fast DDS / Fast CDR
(Apache-2.0), foonathan_memory and tinyxml2 (Zlib), zlib (Zlib), bzip2
(BSD-style), nlohmann_json (MIT), and whatever else the graph resolves to.
Shipping only the executable leaves those texts behind, so this deployer puts
them in the archive next to gateway(.exe).

The list is DERIVED, never hand-maintained: it is read off the resolved Conan
dependency graph at release time, so adding, dropping or bumping a dependency
updates the notice with no separate edit to remember. Everything in the host
context is included, because a static build bakes in header-only and static
dependencies just as thoroughly as it links shared ones -- the notice tracks
what went into the binary, not what happens to sit beside it.

Run it into the same folder runtime_deploy fills, with the binary-skipping
optimisation turned off so the packages still have a package folder to read the
texts from:

    conan install --requires="fletcher-gateway/<version>" \
        --deployer=deployers/third_party_licenses.py --deployer-folder=dist \
        -c tools.graph:skip_binaries=False -pr:a=<profile>

That conf is what makes this work at all: a static application's dependencies
carry nothing needed at run time, so Conan marks their binaries "Skip" and
leaves them without a package folder -- exactly the packages whose licences
must travel. It is deliberately NOT set on the runtime_deploy invocation, so
the bundle's contents stay decided by the run traits alone.

If any third-party package contributes no licence text, this raises and the
release fails rather than shipping a notice that silently omits a library.
"""

import os

from conan.api.output import ConanOutput
from conan.errors import ConanException

OUTPUT_FILENAME = "THIRD-PARTY-LICENSES.txt"

# Fletcher's own packages. They are the work this repository licenses under
# LGPL-3.0-or-later (see the project LICENSE, which the preamble points at), so
# they are not third parties and get no section of their own here.
FIRST_PARTY_PREFIX = "fletcher-"

# conan-center recipes package the upstream licence text under this subfolder of
# the package; it is a review requirement there, so in practice every dependency
# has one. A package that does not is reported rather than passed over.
LICENSES_SUBFOLDER = "licenses"

RULE = "=" * 78
THIN_RULE = "-" * 78


def _read_text(path):
    """Read a licence file, preserving its bytes as faithfully as possible."""
    with open(path, "rb") as handle:
        raw = handle.read()
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        # A few upstream licence files spell a copyright holder's name in a
        # legacy encoding. latin-1 decodes any byte sequence, so the text still
        # travels rather than the release failing on a stray accent.
        return raw.decode("latin-1")


def _license_files(package_folder):
    """(relative name, absolute path) for every file under <package>/licenses."""
    if not package_folder:
        return []
    root = os.path.join(package_folder, LICENSES_SUBFOLDER)
    if not os.path.isdir(root):
        return []
    found = []
    for dirpath, _, filenames in os.walk(root):
        for filename in filenames:
            absolute = os.path.join(dirpath, filename)
            relative = os.path.relpath(absolute, root).replace(os.sep, "/")
            found.append((relative, absolute))
    return sorted(found)


def _third_party_dependencies(conanfile):
    """Host-context dependencies that are not Fletcher's own, deduplicated.

    Host context only: build-context packages (cmake, b2, and the rest of the
    tooling) never reach the shipped binary, so their licences do not have to
    travel with it. `test` requires are out for the same reason -- gtest is
    linked into the unit tests, not into the released executable.

    Deliberately NOT `dependencies.host`, which also filters out requires Conan
    marked "skip". A skipped require is one that hands this consumer neither
    headers nor libraries nor a runtime -- asio, for instance, reaches the
    gateway only through Fast DDS -- but a header-only library still has its
    code compiled into the binary, and its licence still has to be reproduced.
    Which packages are IN the binary is not the same question as which packages
    the consumer compiles against.
    """
    by_ref = {}
    for dep in conanfile.dependencies.filter({"build": False, "test": False}).values():
        if dep.ref.name.startswith(FIRST_PARTY_PREFIX):
            continue
        by_ref[(dep.ref.name, str(dep.ref.version))] = dep
    # Sorted so the same graph always renders the same file.
    return [by_ref[key] for key in sorted(by_ref)]


def _section_header(dep):
    lines = [RULE, f"{dep.ref.name} {dep.ref.version}"]
    if dep.license:
        lines.append(f"License: {dep.license}")
    if dep.homepage:
        lines.append(f"Homepage: {dep.homepage}")
    lines.append(THIN_RULE)
    return "\n".join(lines)


def _preamble(subject, entries):
    covered = "\n".join(
        f"  * {dep.ref.name} {dep.ref.version}"
        + (f" -- {dep.license}" if dep.license else "")
        for dep, _ in entries
    )
    title = f"THIRD-PARTY LICENSES -- {subject}" if subject else "THIRD-PARTY LICENSES"
    return f"""\
{title}
{RULE}

The libraries listed below are compiled or linked into the binary shipped
beside this file -- statically in the default build, shipped as runtime
libraries next to it in the shared variant. Their licenses require their
copyright notices and license texts to be reproduced in binary distributions,
so those texts are reproduced verbatim below, including any license text a
package carries for code it in turn vendors. Where a package ships several
texts, the license the package itself is under is the one on its `License:`
line.

This file is generated at release time from the resolved Conan dependency graph
by gateway/deployers/third_party_licenses.py. It is not maintained by hand: it
lists exactly the packages that went into the binary shipped beside it.

That binary itself, and the fletcher-* libraries it is built from, are licensed
under LGPL-3.0-or-later -- see the LICENSE file distributed with it, also at
https://github.com/eivacom/Fletcher/blob/main/LICENSE.

Libraries covered by this notice
{THIN_RULE}
{covered}
"""


def _render(subject, entries):
    parts = [_preamble(subject, entries)]
    for dep, files in entries:
        parts.append(_section_header(dep))
        for relative, text in files:
            parts.append(f"--- {relative} ---\n")
            parts.append(text if text.endswith("\n") else text + "\n")
    return "\n".join(parts)


def _subject(graph):
    """What the notice is about: the package being deployed, not the virtual root.

    `conan install --requires=fletcher-gateway/<v>` builds a virtual consumer
    whose only direct host requirement is the thing being shipped; name that
    rather than the nameless consumer. None when the graph does not identify a
    single subject, in which case the notice simply goes untitled.
    """
    direct = list(graph.root.conanfile.dependencies.direct_host.values())
    if len(direct) == 1:
        return str(direct[0].ref)
    return str(graph.root.ref) if graph.root.ref else None


def deploy(graph, output_folder, **kwargs):
    output = ConanOutput(scope="third_party_licenses")

    entries = []
    missing = []
    for dep in _third_party_dependencies(graph.root.conanfile):
        files = _license_files(dep.package_folder)
        if not files:
            missing.append(str(dep.ref))
            continue
        entries.append((dep, [(relative, _read_text(path)) for relative, path in files]))

    if missing:
        raise ConanException(
            f"Cannot generate {OUTPUT_FILENAME}: no license text found for "
            f"{', '.join(missing)}. Every third-party package in the host graph "
            f"must contribute a '{LICENSES_SUBFOLDER}' folder, or the released "
            f"archive would omit a license it is required to reproduce. A package "
            f"with no package folder was skipped by the graph -- re-run with "
            f"'-c tools.graph:skip_binaries=False'; if the recipe genuinely "
            f"packages no license text, fix the recipe or vendor the text before "
            f"releasing.")

    if not entries:
        raise ConanException(
            f"Cannot generate {OUTPUT_FILENAME}: the host dependency graph "
            f"contains no third-party packages. That is a wrong invocation, not a "
            f"dependency-free binary.")

    target = os.path.join(output_folder, OUTPUT_FILENAME)
    with open(target, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(_render(_subject(graph), entries))

    output.success(f"Wrote {OUTPUT_FILENAME} covering {len(entries)} third-party "
                   f"package(s) to {output_folder}")
