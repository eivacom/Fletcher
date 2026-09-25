#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# D-BIND-57. Fails if a BIND test-matrix table disagrees with the tree.
#
# ── Why this exists ─────────────────────────────────────────────────────────
# Part 4 of plans/BIND-csharp-bindings.md (and §5 of the development plan) says
# which C++ test files each bucket ports, with a case count per file, and BIND's
# acceptance bullets name those FILE SETS. The counts rot whenever `main` adds a
# case: #128 grew bucket 3's `test_publisher_subscriber` from 18 cases to 44 and
# `test_pubsub_arrow` from 15 to 33, reached this branch by a merge on 2026-09-24,
# and nothing noticed for a day and two BIND-4 closes - 26 cases went unported
# under a bullet that said the file set was done. The tables said "these counts
# rot; re-run the command"; nobody did. This is the command, run on every PR.
#
# ── What it checks, per table ───────────────────────────────────────────────
#   1. Every `test_name` N the table names is a test file in the tree with EXACTLY
#      N cases, counted as the tables have always counted them:
#      `grep -c "^TEST\(_F\|_P\)\?("` - cases, not parameterised instances.
#   2. Every bucket row's bold total equals the sum of its file counts (plus a
#      `TS N` term, whose N this script cannot count and says so).
#   3. Every test file with at least one case in the component test directories
#      the tables cover is NAMED in the table - so a new file (#128's
#      `test_batch_decoder`) cannot sit in no bucket.
#   4. The conformance row's `integration-tests/pubsub-conformance` N cases and
#      `CallerTier` M of them match the suite.
#
# ── What it does NOT check ──────────────────────────────────────────────────
# Whether a case is PORTED. That is each bucket's mapping - the C# files that name
# their C++ originals - and, for CallerTier, check_caller_tier_mapping.py. This
# script makes a stale table impossible to miss; what a changed count OBLIGES is
# still a ruling.
#
# Stdlib only, for the reason the CallerTier checker gives.
import argparse
import pathlib
import re
import sys

COVERED_DIRS = [
    'core/tests', 'pubsub/tests', 'arrow-bridge/tests', 'pubsub-arrow/tests',
    'fastdds-pubsub-provider/tests', 'xrcedds-pubsub-provider/tests', 'gateway/tests',
    'protoc/tests',
]
CONFORMANCE_DIRS = ['integration-tests/pubsub-conformance/src', 'integration-tests/pubsub-conformance/subjects']
CALLER_TIER = 'integration-tests/pubsub-conformance/src/caller_tier.cpp'
DEFAULT_DOCS = ['plans/BIND-csharp-bindings.md', 'plans/BIND-csharp-development-plan.md']

CASE = re.compile(r'^TEST(_F|_P)?\(', re.M)
BUCKET_ROW = re.compile(r'^\| ([1-6]) [^|]*\|([^|]*)\| \*\*(\d+)\*\* \|', re.M)
FILE_COUNT = re.compile(r'`(test_[A-Za-z0-9_]+)` (\d+)')
TS_TERM = re.compile(r'\bTS (\d+)\b')
CONFORMANCE_ROW = re.compile(r'`integration-tests/pubsub-conformance` (\d+) cases; `CallerTier` (\d+) of them')


def cases(path: pathlib.Path) -> int:
    return len(CASE.findall(path.read_text(encoding='utf-8')))


def test_files(root: pathlib.Path, dirs) -> dict:
    found = {}
    for d in dirs:
        for f in sorted((root / d).glob('*.cpp')):
            found.setdefault(f.stem, []).append(f)
    return found


def check_doc(root: pathlib.Path, doc: pathlib.Path) -> list:
    text = doc.read_text(encoding='utf-8')
    problems = []
    files = test_files(root, COVERED_DIRS)
    named = set()

    rows = list(BUCKET_ROW.finditer(text))
    if not rows:
        return [f'{doc}: no bucket table found (rows "| N ... | files | **total** |")']

    for row in rows:
        bucket, cell, total = row.group(1), row.group(2), int(row.group(3))
        running = 0
        for name, stated in FILE_COUNT.findall(cell):
            named.add(name)
            stated = int(stated)
            paths = files.get(name, [])
            if not paths:
                problems.append(f'{doc}: bucket {bucket} names `{name}`, which is not a test file in {", ".join(COVERED_DIRS)}')
                continue
            if len(paths) > 1:
                problems.append(f'{doc}: `{name}` is ambiguous - {", ".join(str(p) for p in paths)}')
                continue
            actual = cases(paths[0])
            if actual != stated:
                problems.append(f'{doc}: bucket {bucket}: `{name}` has {actual} cases in the tree, the table says {stated}')
            running += stated
        running += sum(int(n) for n in TS_TERM.findall(cell))
        if running != total:
            problems.append(f'{doc}: bucket {bucket}: its files add up to {running}, its total says {total}')

    for name, paths in sorted(files.items()):
        if name not in named and any(cases(p) > 0 for p in paths):
            problems.append(f'{doc}: `{name}` ({paths[0].relative_to(root)}, {cases(paths[0])} cases) is in no bucket')

    conformance = CONFORMANCE_ROW.search(text)
    if conformance is None:
        problems.append(f'{doc}: no conformance row ("`integration-tests/pubsub-conformance` N cases; `CallerTier` M of them")')
    else:
        suite = sum(cases(f) for d in CONFORMANCE_DIRS for f in (root / d).glob('*.cpp'))
        tier = len(re.findall(r'^TEST(_F|_P)?\(\s*CallerTier\s*,', (root / CALLER_TIER).read_text(encoding='utf-8'), re.M))
        if suite != int(conformance.group(1)):
            problems.append(f'{doc}: the conformance suite has {suite} cases, the table says {conformance.group(1)}')
        if tier != int(conformance.group(2)):
            problems.append(f'{doc}: CallerTier has {tier} cases, the table says {conformance.group(2)}')

    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', default='.', help='repository root')
    parser.add_argument('docs', nargs='*', default=DEFAULT_DOCS)
    args = parser.parse_args()
    root = pathlib.Path(args.root)

    problems = []
    for doc in args.docs:
        problems += check_doc(root, root / doc)

    if problems:
        print('The BIND test matrix disagrees with the tree:')
        for p in problems:
            print('  ' + p)
        print('Re-derive it (see Part 4 of plans/BIND-csharp-bindings.md) and rule on what the change obliges.')
        return 1
    print(f'The BIND test matrix agrees with the tree ({len(args.docs)} table(s)). TS counts are not checked.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
