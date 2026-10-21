#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# BIND-9a-4: fails if a README's code snippets are not the code a test compiles and runs.
#
#   check_readme_snippets.py <README.md> <source file>...
#
# A source marks a region with a line containing `<snippet name="NAME">` and ends it with a
# line containing `</snippet>`, in whatever comment the language has (`//`, `<!-- -->`). The
# README shows it as the fenced code block that follows a line `<!-- snippet: NAME -->`. The
# block must equal the region with its common indentation removed; every region must be shown
# and every shown snippet must have a region, so neither side can drift or be left behind.
#
# Stdlib only, as the other checkers here.
import difflib
import pathlib
import re
import sys
import textwrap

START = re.compile(r'<snippet name="([A-Za-z0-9_-]+)">')
END = re.compile(r'</snippet>')
SHOWN = re.compile(r'^<!-- snippet: ([A-Za-z0-9_-]+) -->\s*$')
FENCE = re.compile(r'^```')


def tidy(lines):
    text = textwrap.dedent("\n".join(lines))
    return text.strip("\n")


def regions(path, errors):
    found = {}
    name, body, start = None, [], 0
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        opened = START.search(line)
        if opened:
            if name is not None:
                errors.append(f"{path}:{number}: snippet {opened.group(1)!r} opens inside {name!r}")
            name, body, start = opened.group(1), [], number
        elif END.search(line):
            if name is None:
                errors.append(f"{path}:{number}: a snippet end with no start")
            else:
                found[name] = (tidy(body), f"{path}:{start}")
                name = None
        elif name is not None:
            body.append(line)
    if name is not None:
        errors.append(f"{path}:{start}: snippet {name!r} is never closed")
    return found


def shown(path, errors):
    found = {}
    lines = path.read_text(encoding="utf-8").splitlines()
    i = 0
    while i < len(lines):
        marker = SHOWN.match(lines[i])
        if not marker:
            i += 1
            continue
        name, at = marker.group(1), i + 1
        i += 1
        while i < len(lines) and not lines[i].strip():
            i += 1
        if i >= len(lines) or not FENCE.match(lines[i]):
            errors.append(f"{path}:{at}: snippet {name!r} is not followed by a fenced code block")
            continue
        i += 1
        body = []
        while i < len(lines) and not FENCE.match(lines[i]):
            body.append(lines[i])
            i += 1
        if name in found:
            errors.append(f"{path}:{at}: snippet {name!r} is shown twice")
        found[name] = (tidy(body), f"{path}:{at}")
        i += 1
    return found


def main(argv):
    if len(argv) < 3:
        print("usage: check_readme_snippets.py <README.md> <source file>...", file=sys.stderr)
        return 2
    errors = []
    readme = pathlib.Path(argv[1])
    sources = {}
    for source in argv[2:]:
        for name, value in regions(pathlib.Path(source), errors).items():
            if name in sources:
                errors.append(f"{value[1]}: snippet {name!r} is also defined at {sources[name][1]}")
            sources[name] = value
    in_readme = shown(readme, errors)

    for name, (code, where) in in_readme.items():
        if name not in sources:
            errors.append(f"{where}: snippet {name!r} has no region in the sources")
            continue
        if code != sources[name][0]:
            diff = "\n".join(difflib.unified_diff(
                sources[name][0].splitlines(), code.splitlines(),
                fromfile=sources[name][1], tofile=where, lineterm=""))
            errors.append(f"{where}: snippet {name!r} differs from its region\n{diff}")
    for name, (_, where) in sources.items():
        if name not in in_readme:
            errors.append(f"{where}: snippet {name!r} is not shown in {readme}")

    for error in errors:
        print(f"::error::{error}")
    if errors:
        return 1
    print(f"{readme}: {len(in_readme)} snippets match their regions")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
