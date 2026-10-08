#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# Proves `check_caller_tier_mapping.py` still REFUSES every way the CallerTier arm
# could claim a mirror it does not have - and still ACCEPTS the real files.
#
# ── Why this exists ─────────────────────────────────────────────────────────
# A checker that has never failed is worth nothing, and the first version of this
# one proved it: its CI lane tested one failure mode (a marker removed), and a
# skipped test, a test commented out, and a test with its only assertion deleted
# all passed as "total" (BIND-4 review, B4). Each mutation below is one of those,
# applied to a COPY of the real arm or suite, and each must fail with its own
# message - a checker that failed for the wrong reason would pass a fixture that
# only checked the exit code.
#
# Run from the repository root: `python3 scripts/test_check_caller_tier_mapping.py`.
# Exit 0 when every fixture behaves; 1 otherwise, naming the one that did not.
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
CHECKER = ROOT / "scripts" / "check_caller_tier_mapping.py"
CPP = ROOT / "integration-tests" / "pubsub-conformance" / "src" / "caller_tier.cpp"
CS = ROOT / "dotnet" / "tests" / "Fletcher.Tests" / "CallerTierArmTests.cs"

MARKER = re.compile(r"<summary>Mirrors CallerTier\.([A-Za-z0-9_]+)\.</summary>")


def first_mirror(lines: list[str]) -> tuple[int, int, int]:
    """(marker line, declaration line, last body line) of the first mirror in the arm."""
    marker = next(i for i, line in enumerate(lines) if MARKER.search(line) and line.lstrip().startswith("///"))
    declaration = next(i for i in range(marker + 1, len(lines)) if re.match(r"^\s*public\s", lines[i]))
    depth, opened = 0, False
    for end in range(declaration, len(lines)):
        depth += lines[end].count("{") - lines[end].count("}")
        opened = opened or "{" in lines[end]
        if opened and depth == 0:
            return marker, declaration, end
    raise AssertionError("unbalanced braces in the first mirror")


def doc_start(lines: list[str], marker: int) -> int:
    start = marker
    while start > 0 and lines[start - 1].lstrip().startswith("///"):
        start -= 1
    return start


def skipped(cs: str, cpp: str) -> tuple[str, str]:
    return cs.replace("[Fact]", '[Fact(Skip = "fixture")]', 1), cpp


def attribute_removed(cs: str, cpp: str) -> tuple[str, str]:
    lines = cs.splitlines()
    marker, declaration, _ = first_mirror(lines)
    for i in range(marker, declaration):
        if re.search(r"\[\s*(Fact|Theory)", lines[i]):
            lines[i] = ""
    return "\n".join(lines), cpp


def commented_out_whole(cs: str, cpp: str) -> tuple[str, str]:
    lines = cs.splitlines()
    marker, _, end = first_mirror(lines)
    for i in range(doc_start(lines, marker), end + 1):
        lines[i] = "// " + lines[i]
    return "\n".join(lines), cpp


def commented_out_method_doc_kept(cs: str, cpp: str) -> tuple[str, str]:
    # The careless edit: the doc comment survives, the method below it does not.
    lines = cs.splitlines()
    marker, declaration, end = first_mirror(lines)
    for i in range(marker + 1, end + 1):
        if not lines[i].lstrip().startswith("///"):
            lines[i] = "// " + lines[i]
    return "\n".join(lines), cpp


def block_commented(cs: str, cpp: str) -> tuple[str, str]:
    lines = cs.splitlines()
    marker, _, end = first_mirror(lines)
    start = doc_start(lines, marker)
    lines[start] = "/* " + lines[start]
    lines[end] = lines[end] + " */"
    return "\n".join(lines), cpp


def assertions_removed(cs: str, cpp: str) -> tuple[str, str]:
    lines = cs.splitlines()
    _, declaration, end = first_mirror(lines)
    for i in range(declaration, end + 1):
        if "Assert." in lines[i]:
            lines[i] = ""
    return "\n".join(lines), cpp


def method_renamed(cs: str, cpp: str) -> tuple[str, str]:
    lines = cs.splitlines()
    marker, declaration, _ = first_mirror(lines)
    case = MARKER.search(lines[marker]).group(1)
    lines[declaration] = lines[declaration].replace(case + "(", case + "Renamed(", 1)
    return "\n".join(lines), cpp


def marker_removed(cs: str, cpp: str) -> tuple[str, str]:
    return cs.replace("<summary>Mirrors CallerTier.", "<summary>marker removed by the fixture: ", 1), cpp


def stale_marker(cs: str, cpp: str) -> tuple[str, str]:
    # The C++ case renamed, the arm not updated.
    first_case = re.search(r"^TEST(?:_F)?\(CallerTier,\s*([A-Za-z0-9_]+)", cpp, re.M).group(1)
    return cs, cpp.replace(f"CallerTier, {first_case})", f"CallerTier, {first_case}Renamed)", 1)


def unparsed_cpp_macro(cs: str, cpp: str) -> tuple[str, str]:
    return cs, cpp + "\nTEST_P(CallerTier, AParameterisedCaseTheParserDoesNotKnow) {}\n"


FIXTURES = [
    (skipped, "the test is skipped"),
    (attribute_removed, "has no [Fact] or [Theory]"),
    (commented_out_whole, "have no live C# mirror"),
    (commented_out_method_doc_kept, "the method is named"),
    (block_commented, "have no live C# mirror"),
    (assertions_removed, "the test asserts nothing"),
    (method_renamed, "the method is named"),
    (marker_removed, "have no live C# mirror"),
    (stale_marker, "name a C++ case that does not exist"),
    (unparsed_cpp_macro, "only"),
]


def run(cs_text: str, cpp_text: str, scratch: pathlib.Path) -> subprocess.CompletedProcess:
    cs_path = scratch / "arm.cs"
    cpp_path = scratch / "suite.cpp"
    cs_path.write_text(cs_text, encoding="utf-8")
    cpp_path.write_text(cpp_text, encoding="utf-8")
    return subprocess.run(
        [sys.executable, str(CHECKER), "--cs", str(cs_path), "--cpp", str(cpp_path)],
        capture_output=True,
        text=True,
    )


def main() -> int:
    cs_text = CS.read_text(encoding="utf-8")
    cpp_text = CPP.read_text(encoding="utf-8")
    failures = 0

    with tempfile.TemporaryDirectory() as tmp:
        scratch = pathlib.Path(tmp)

        baseline = run(cs_text, cpp_text, scratch)
        if baseline.returncode != 0:
            print(f"FAIL baseline: the real files were rejected\n{baseline.stderr}")
            failures += 1
        else:
            print("ok   baseline: the real files are accepted")

        for mutate, expected in FIXTURES:
            mutated_cs, mutated_cpp = mutate(cs_text, cpp_text)
            if (mutated_cs, mutated_cpp) == (cs_text, cpp_text):
                print(f"FAIL {mutate.__name__}: the mutation changed nothing, so it proves nothing")
                failures += 1
                continue
            result = run(mutated_cs, mutated_cpp, scratch)
            if result.returncode == 0:
                print(f"FAIL {mutate.__name__}: the checker ACCEPTED it\n{result.stdout}")
                failures += 1
            elif expected not in result.stderr:
                print(f"FAIL {mutate.__name__}: rejected, but not for the expected reason "
                      f"({expected!r})\n{result.stderr}")
                failures += 1
            else:
                print(f"ok   {mutate.__name__}: rejected ({expected})")

    print(f"{len(FIXTURES) + 1 - failures} of {len(FIXTURES) + 1} fixtures behaved")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
