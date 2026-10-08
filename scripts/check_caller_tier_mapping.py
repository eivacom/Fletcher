#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# BIND-4d-iv. Fails if the C# arm of CallerTier does not mirror EVERY C++ case
# with a LIVE test.
#
# ── Why a script rather than a test ─────────────────────────────────────────
# The property is about two FILES agreeing, and one of them is C++. A C# test
# asserting it would have to read the C++ source, which means the dotnet lane
# would need that path in its sparse checkout for a reason that has nothing to do
# with running the binding. A script can be pointed at both files from anywhere,
# including a developer's working tree before they push.
#
# ── What counts as a mirror (hardened after the BIND-4 review, B4) ──────────
# The first version counted `<summary>Mirrors CallerTier.X.</summary>` strings
# wherever they appeared. A skipped test, a test commented out line by line, and
# a test with its only assertion deleted all still counted - reproduced, on a
# scratch copy, as "total: 22 C++ cases, 22 C# mirrors". A mirror now has to be:
#
#   * a marker in a `///` doc comment that is NOT itself commented out (`//` and
#     `/* */` comments are stripped before anything is matched);
#   * attached to a method: only doc lines, blank lines and attributes between
#     the marker and the declaration;
#   * a live test: a `[Fact]` or `[Theory]` attribute, and no `Skip` in any
#     attribute on it;
#   * NAMED after its case, so a renamed C++ case cannot be "mirrored" by a
#     method about something else;
#   * a method whose body contains at least one `Assert.`.
#
# On the C++ side, a line that opens a CallerTier test with a macro this script
# does not parse fails the run, rather than silently dropping out of the count.
#
# ── What "total" means here, and what it does NOT ───────────────────────────
# Total means every C++ case has a live, named, asserting C# test carrying its
# marker. It does NOT mean the two assert the same strength: some mirrors are
# deliberately weaker than their originals, because the windows they need cannot
# be constructed from managed code (inprocess delivers one at a time
# instance-wide, and D-BIND-24 withholds provider registration so a managed
# ProbeProvider is impossible). Those say so in their own XML docs. THIS SCRIPT
# CANNOT SEE STRENGTH - it checks coverage and liveness, and saying that plainly is
# the point, because a green run here is not a claim that the arm is as strong as
# the suite it mirrors.
#
# `scripts/test_check_caller_tier_mapping.py` proves every refusal above fires;
# the CI lane runs it on every change to this script.
#
# Exit 0 when the mapping is total; 1 otherwise, naming what is wrong.
import argparse
import pathlib
import re
import sys

CPP_CASE = re.compile(r"^TEST(?:_F)?\(CallerTier,\s*([A-Za-z0-9_]+)\s*\)", re.M)

# Any line that OPENS a CallerTier test, whatever the macro. Compared with the
# cases CPP_CASE parsed, so a TEST_P or a new macro fails loudly instead of
# shrinking the denominator.
CPP_ANY_CASE = re.compile(r"^\s*[A-Z_]*TEST[A-Z_]*\(\s*CallerTier\b", re.M)

# The `<summary>` form specifically, on a `///` line. The arm's file header
# explains the marker in prose, and a looser pattern would count that explanation
# as a mirror - which is exactly the kind of self-satisfying check this repo keeps
# finding.
CS_MARKER = re.compile(r"^\s*///.*<summary>\s*Mirrors CallerTier\.([A-Za-z0-9_]+)\.\s*</summary>")
CS_ATTRIBUTE = re.compile(r"^\s*\[.*\]\s*$")
CS_TEST_ATTRIBUTE = re.compile(r"\[\s*(?:Xunit\.)?(?:Fact|Theory)\b")
CS_METHOD = re.compile(r"^\s*public\s+(?:static\s+)?(?:async\s+)?[\w<>\[\],\s]+?\s+([A-Za-z0-9_]+)\s*\(")


def strip_comments(text: str) -> list[str]:
    """The file's lines with `/* */` and `//` comments blanked; `///` doc comments kept.

    Line numbers are preserved (a stripped line becomes empty), so a report can
    point at the source. A test commented out line by line turns `/// <summary>`
    into `// /// <summary>`, whose first token is `//`, so its marker goes too.
    """
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    lines = []
    for line in text.splitlines():
        stripped = line.lstrip()
        if stripped.startswith("//") and not stripped.startswith("///"):
            lines.append("")
        else:
            lines.append(line)
    return lines


def method_body(lines: list[str], start: int) -> str:
    """The text of the method whose declaration is at `start`, braces balanced."""
    depth = 0
    seen_open = False
    body = []
    for line in lines[start:]:
        body.append(line)
        for ch in line:
            if ch == "{":
                depth += 1
                seen_open = True
            elif ch == "}":
                depth -= 1
        if seen_open and depth == 0:
            break
    return "\n".join(body)


def cs_mirrors(text: str) -> tuple[list[str], list[str]]:
    """(cases mirrored by a live test, problems found with markers that are not)."""
    lines = strip_comments(text)
    mirrored: list[str] = []
    problems: list[str] = []

    for index, line in enumerate(lines):
        marker = CS_MARKER.match(line)
        if not marker:
            continue
        case = marker.group(1)
        where = f"line {index + 1} (CallerTier.{case})"

        attributes: list[str] = []
        declaration = None
        for cursor in range(index + 1, len(lines)):
            candidate = lines[cursor]
            if not candidate.strip() or candidate.lstrip().startswith("///"):
                continue
            if CS_ATTRIBUTE.match(candidate):
                attributes.append(candidate)
                continue
            declaration = cursor
            break

        method = CS_METHOD.match(lines[declaration]) if declaration is not None else None
        if method is None:
            problems.append(f"{where}: the marker is not attached to a method")
            continue
        if not any(CS_TEST_ATTRIBUTE.search(a) for a in attributes):
            problems.append(f"{where}: the method has no [Fact] or [Theory], so it never runs")
            continue
        if any("Skip" in a for a in attributes):
            problems.append(f"{where}: the test is skipped")
            continue
        if method.group(1) != case:
            problems.append(f"{where}: the method is named {method.group(1)}, not {case}")
            continue
        if "Assert." not in method_body(lines, declaration):
            problems.append(f"{where}: the test asserts nothing")
            continue

        mirrored.append(case)

    return mirrored, problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--cpp",
        default="integration-tests/pubsub-conformance/src/caller_tier.cpp",
        type=pathlib.Path,
    )
    parser.add_argument(
        "--cs",
        default="dotnet/tests/Fletcher.Tests/CallerTierArmTests.cs",
        type=pathlib.Path,
    )
    args = parser.parse_args()

    for path in (args.cpp, args.cs):
        if not path.is_file():
            print(f"error: {path} does not exist", file=sys.stderr)
            return 1

    cpp_text = args.cpp.read_text(encoding="utf-8")
    cpp_cases = CPP_CASE.findall(cpp_text)
    cs_cases, problems = cs_mirrors(args.cs.read_text(encoding="utf-8"))

    # A suite that found no cases would otherwise pass while asserting nothing -
    # the vacuity this round has been bitten by twice (the sparse-checkout
    # goldens, and a theory whose data was silently empty).
    if not cpp_cases:
        print(f"error: no CallerTier cases found in {args.cpp} - has the suite moved?", file=sys.stderr)
        return 1

    ok = True

    opened = len(CPP_ANY_CASE.findall(cpp_text))
    if opened != len(cpp_cases):
        ok = False
        print(
            f"error: {opened} lines open a CallerTier test but only {len(cpp_cases)} parse as "
            "TEST/TEST_F - a case written with another macro would silently drop out of the count",
            file=sys.stderr,
        )

    if problems:
        ok = False
        print(f"error: {len(problems)} mirror marker(s) not on a live, named, asserting test:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)

    missing = [case for case in cpp_cases if case not in cs_cases]
    unknown = [m for m in cs_cases if m not in cpp_cases]
    duplicated = sorted({m for m in cs_cases if cs_cases.count(m) > 1})

    if missing:
        ok = False
        print(f"error: {len(missing)} C++ CallerTier case(s) have no live C# mirror:", file=sys.stderr)
        for case in missing:
            print(f"  - CallerTier.{case}", file=sys.stderr)

    # A marker naming a case that no longer exists is just as much a drift as a
    # missing one: it means a C++ case was renamed and the arm still claims it.
    if unknown:
        ok = False
        print(f"error: {len(unknown)} C# mirror(s) name a C++ case that does not exist:", file=sys.stderr)
        for case in unknown:
            print(f"  - CallerTier.{case}", file=sys.stderr)

    if duplicated:
        ok = False
        print(f"error: {len(duplicated)} case(s) mirrored more than once:", file=sys.stderr)
        for case in duplicated:
            print(f"  - CallerTier.{case}", file=sys.stderr)

    if ok:
        print(f"CallerTier mapping is total: {len(cpp_cases)} C++ cases, {len(cs_cases)} live C# mirrors.")
        return 0

    return 1


if __name__ == "__main__":
    sys.exit(main())
