#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# BIND-4d-iv. Fails if the C# arm of CallerTier does not mirror EVERY C++ case.
#
# ── Why a script rather than a test ─────────────────────────────────────────
# The property is about two FILES agreeing, and one of them is C++. A C# test
# asserting it would have to read the C++ source, which means the dotnet lane
# would need that path in its sparse checkout for a reason that has nothing to do
# with running the binding. A script can be pointed at both files from anywhere,
# including a developer's working tree before they push.
#
# ── What "total" means here, and what it does NOT ───────────────────────────
# Total means every C++ case name has a mirror marker in the C# arm. It does NOT
# mean the two assert the same strength: two mirrors are deliberately weaker than
# their originals, because the windows they need cannot be constructed from
# managed code (inprocess delivers one at a time instance-wide, and D-BIND-24
# withholds provider registration so a managed ProbeProvider is impossible).
# Those two say so in their own XML docs. THIS SCRIPT CANNOT SEE STRENGTH - it
# checks coverage, and saying that plainly is the point, because a green run here
# is not a claim that the arm is as strong as the suite it mirrors.
#
# Exit 0 when the mapping is total; 1 otherwise, naming what is missing.
import argparse
import pathlib
import re
import sys

CPP_CASE = re.compile(r"^TEST(?:_F)?\(CallerTier,\s*([A-Za-z0-9_]+)\s*\)", re.M)

# The `<summary>` form specifically. The arm's file header explains the marker in
# prose, and a looser pattern would count that explanation as a mirror - which is
# exactly the kind of self-satisfying check this repo keeps finding.
CS_MIRROR = re.compile(r"<summary>\s*Mirrors CallerTier\.([A-Za-z0-9_]+)\.\s*</summary>")


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

    cpp_cases = CPP_CASE.findall(args.cpp.read_text(encoding="utf-8"))
    cs_mirrors = CS_MIRROR.findall(args.cs.read_text(encoding="utf-8"))

    # A suite that found no cases would otherwise pass while asserting nothing -
    # the vacuity this round has been bitten by twice (the sparse-checkout
    # goldens, and a theory whose data was silently empty).
    if not cpp_cases:
        print(f"error: no CallerTier cases found in {args.cpp} - has the suite moved?", file=sys.stderr)
        return 1

    missing = [case for case in cpp_cases if case not in cs_mirrors]
    unknown = [m for m in cs_mirrors if m not in cpp_cases]
    duplicated = sorted({m for m in cs_mirrors if cs_mirrors.count(m) > 1})

    ok = True

    if missing:
        ok = False
        print(f"error: {len(missing)} C++ CallerTier case(s) have no C# mirror:", file=sys.stderr)
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
        print(f"CallerTier mapping is total: {len(cpp_cases)} C++ cases, {len(cs_mirrors)} C# mirrors.")
        return 0

    return 1


if __name__ == "__main__":
    sys.exit(main())
