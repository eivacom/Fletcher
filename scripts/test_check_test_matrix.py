#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# D-BIND-57. Proves check_test_matrix.py refuses every way a table can be stale,
# each FOR ITS OWN REASON, and accepts a table that agrees with its tree - on a
# scratch tree, so the real repository is never edited.
#
# A checker that has never failed is worth nothing: the CallerTier checker's
# first version accepted eight of ten ways to fake a mirror (BIND-4 review, B4).
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
CHECK = HERE / 'check_test_matrix.py'

GOOD_DOC = """\
| Bucket | Files (cases) | Total | Treatment |
|---|---|---|---|
| 1 codec | `test_alpha` 2, `test_beta` 1 | **3** | port |
| 2 gateway client | `test_gamma` 1, TS 5 | **6** | port |
| Conformance | `integration-tests/pubsub-conformance` 3 cases; `CallerTier` 2 of them | — | oracle |
"""

FILES = {
    'core/tests/test_alpha.cpp': 'TEST(A, One) {}\nTEST_F(A, Two) {}\n',
    'pubsub/tests/test_beta.cpp': 'TEST_P(B, One) {}\n  TEST(B, IndentedIsNotACase) {}\n',
    'gateway/tests/test_gamma.cpp': 'TEST(G, One) {}\n',
    'pubsub-arrow/tests/helper_probe.cpp': '// no cases: a helper, and not a bucket member\n',
    'integration-tests/pubsub-conformance/src/caller_tier.cpp': 'TEST(CallerTier, A) {}\nTEST(CallerTier, B) {}\n',
    'integration-tests/pubsub-conformance/subjects/subject.cpp': 'TEST(Other, C) {}\n',
}


def run(files: dict, doc: str):
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        for rel, text in files.items():
            path = root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8')
        for d in ['arrow-bridge/tests', 'fastdds-pubsub-provider/tests', 'xrcedds-pubsub-provider/tests', 'protoc/tests']:
            (root / d).mkdir(parents=True, exist_ok=True)
        (root / 'plans').mkdir(exist_ok=True)
        (root / 'plans/matrix.md').write_text(doc, encoding='utf-8')
        r = subprocess.run([sys.executable, str(CHECK), '--root', str(root), 'plans/matrix.md'],
                           capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr


CASES = [
    ('a table that agrees', FILES, GOOD_DOC, 0, 'agrees with the tree'),
    ('a wrong count', FILES, GOOD_DOC.replace('`test_alpha` 2', '`test_alpha` 1').replace('**3**', '**2**'),
     1, '`test_alpha` has 2 cases in the tree, the table says 1'),
    ('a total that does not add up', FILES, GOOD_DOC.replace('**3**', '**4**'), 1,
     'its files add up to 3, its total says 4'),
    ('a TS term that is not counted in the total', FILES, GOOD_DOC.replace('**6**', '**1**'), 1,
     'its files add up to 6, its total says 1'),
    ('a new file in no bucket', {**FILES, 'arrow-bridge/tests/test_delta.cpp': 'TEST(D, One) {}\n'}, GOOD_DOC, 1,
     '`test_delta`'),
    ('a named file that does not exist', {k: v for k, v in FILES.items() if 'test_gamma' not in k}, GOOD_DOC, 1,
     'names `test_gamma`, which is not a test file'),
    ('a stale conformance count', FILES, GOOD_DOC.replace('` 3 cases;', '` 4 cases;'), 1,
     'the conformance suite has 3 cases, the table says 4'),
    ('a stale CallerTier count', FILES, GOOD_DOC.replace('2 of them', '1 of them'), 1,
     'CallerTier has 2 cases, the table says 1'),
    ('a document with no table', FILES, '# nothing here\n', 1, 'no bucket table found'),
]


def main() -> int:
    failures = 0
    for name, files, doc, want_code, want_text in CASES:
        code, out = run(files, doc)
        ok = code == want_code and want_text in out
        failures += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {name}")
        if not ok:
            print(f'     wanted exit {want_code} and "{want_text}", got exit {code}:\n' + out)

    # And the real repository's tables, which must agree with the real tree.
    root = HERE.parent
    r = subprocess.run([sys.executable, str(CHECK), '--root', str(root)], capture_output=True, text=True)
    real_ok = r.returncode == 0
    failures += not real_ok
    print(f"{'ok  ' if real_ok else 'FAIL'} the repository's own tables")
    if not real_ok:
        print(r.stdout + r.stderr)

    print('all fixtures behave' if failures == 0 else f'{failures} fixture(s) misbehaved')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
