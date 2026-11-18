# PDA-DEC-9 — independent code review

Diff: `git diff 7740a9d..5d06812` (11 files, +587/-68). The code portion reviewed in full —
`core/tests/test_status_taxonomy.cpp`, `core/tests/CMakeLists.txt`, `core/conanfile.py`,
`core/README.md` — plus `core/include/fletcher/core/status.hpp` for context. Docs read only for
consistency with the code.

Verdict: **no blocking findings.** One should-fix (a silent hole in the parser, demonstrated by
running it), one should-fix on the durability of the compile-time promotion, four nits.

## Verified by running, not by reading

Baseline, existing MSVC tree, `cmake --build core/build --config Release --target core_tests`:

- **29 ctest entries** (`ctest -C Release -N` → `Total Tests: 29`, #29 `Taxonomy.PublishedNumbersMatchTheEnum`)
- **29 gtest cases** (`core_tests.exe --gtest_list_tests`)
- `29/29` passing. Entries and cases stated separately, as asked; they agree here.

Mutations (each reverted; `git status` clean afterwards apart from the review files):

| Mutation | Result |
|---|---|
| append `kZzReviewMutationDoNotShip = 10` to `PubSubStatus` | **build fails**: `error C4062: enumerator '...kZzReviewMutationDoNotShip' in switch of enum 'fletcher::PubSubStatus' is not handled` — part 1 is live on this toolchain |
| delete the whole published table | **red**, "no taxonomy rows parsed …" (does not fall through to the later `### Build profiles` table, whose second cells are not numeric) |
| rename the `## Error taxonomy (published)` heading | **red**, same assertion |
| replace the `kSubscriptionEnded` row with a duplicate `kPending` row | **red**, "the published numbers are not a contiguous prefix from 0" — the contiguity check is load-bearing, not decoration; do not remove it |
| add an extra row whose Number cell is `TBD`, or `4294967296` | **GREEN** — see F1 |

Also checked mechanically: the generated `core/build/tests/core_tests.vcxproj` carries
`<TreatSpecificWarningsAsErrors>4062</TreatSpecificWarningsAsErrors>` on
`test_status_taxonomy.cpp` **only** (all four configurations), and
`FLETCHER_CORE_README_PATH="…/core/tests/../README.md"` on the target — the property is attached
to the right source and the define to the right target.

## F1 — should-fix (high confidence): a published row whose `Number` cell does not parse is silently dropped, and the suite stays green

`ParsePublishedRows` treats *any* line it cannot make a number of as a header/separator row and
`continue`s. That is how the two structural rows are skipped — and it is also how a real row
escapes the comparison. Demonstrated: adding

    | `kFutureThing` | TBD | reserved for a status the enum does not have |

or a number outside `int` range (`4294967296` → `std::stoi` throws `out_of_range` → caught →
`continue`) leaves the test **passing** while `core/README.md` — declared in its own text to be
**"the only enumeration"** of the taxonomy, read in prose by two independent bindings — publishes
a status the enum does not have.

Edits to *existing* rows are covered (a mangled number drops the row, and either the contiguity
check or part 3 then reddens), so the exposure is confined to **added** rows. But a
reserved/planned row, or a `1 0` / `l0` typo, is exactly the edit a documentation-shaped table
attracts, and the guard's advertised property is "editing a name or a number on either side alone
turns it red".

Fix, in the forbidding direction — refuse the malformed row at the door instead of tolerating it:
skip the first two table lines **structurally** (a markdown table always has a header line and a
separator line; the separator matches `^[|: -]+$`), then require **every** remaining row to yield
a non-empty name and a fully consumed integer, and `ADD_FAILURE()` naming the offending line
otherwise. Both `catch (const std::exception&) { continue; }` and `cells.size() < 2 → continue`
become failures. That is fewer lines than today and one fewer tolerated state.

## F2 — should-fix (medium confidence): losing the compile-time promotion is silent, and it is the only part that catches an append

The three parts do not overlap on the append case. If the promotion stops taking — a CMake
refactor that moves `test_status_taxonomy.cpp` under another directory's target (source-file
`COMPILE_OPTIONS` are directory-scoped, as the comment itself notes), a dropped
`set_source_files_properties`, or a toolchain that does not honour the flag (`if(MSVC)` also
matches **clang-cl**, whose `/we<n>` mapping table is not guaranteed to contain 4062) — then an
appended enumerator with no `StatusName` case and no README row leaves `StatusName(rows.size())`
returning `""` and **the whole suite green**, with an unpublished value in a frozen public
numbering. Nothing reddens; the loss is invisible.

Concrete fix: make the promotion prove itself at configure time, beside where it is set.

```cmake
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "${FLETCHER_CORE_SWITCH_TOTALITY_FLAG}")
check_cxx_source_compiles(
    "enum class E { a, b }; int f(E e) { switch (e) { case E::a: return 0; } return 1; }
     int main() { return f(E::a); }"
    FLETCHER_SWITCH_TOTALITY_IS_TOLERATED)
if(FLETCHER_SWITCH_TOTALITY_IS_TOLERATED)
    message(FATAL_ERROR
        "${FLETCHER_CORE_SWITCH_TOTALITY_FLAG} does not reject a non-exhaustive default-less "
        "switch with this compiler: the PubSubStatus append guard would be silently absent")
endif()
```

That closes the toolchain half mechanically. It does not prove the flag reached *this* source
file; if that matters, put `StatusName` in its own small object library with
`target_compile_options`, which is the tree's existing precedent
(`pubsub-arrow/tests/CMakeLists.txt`) and is not directory-scoped.

**For the owner, not a finding against the implementer.** The design rejected a `kLastStatus`
sentinel as "+1 public surface, therefore an owner question". Having watched all three parts run:
the sentinel is the only construction in which the append case is caught by the *test* instead of
by a build flag whose absence is silent — with it, `StatusName(kLastStatus)` plus
`rows.size() == static_cast<size_t>(kLastStatus)` make the flag a convenience rather than the
whole guard. It costs one enumerator that is not a status and that `PubSubError::Sanitize` must
refuse. Worth asking; not something to change without the ruling.

## Checked and clean

- **`core/conanfile.py`.** `README.md` added to `exports_sources` only. `package()` still copies
  `*.hpp` from `include/` and `*.cmake` from `cmake/` — the README cannot reach a consumer's
  package folder. `package_id()` is `self.info.clear()`, so core's package ID is unchanged; no
  profile sets a revision-sensitive `package_id_mode`, and all four consumers
  (`arrow-bridge`, `pubsub`, `fastdds-pubsub-provider`, `xrcedds-pubsub-provider`) require
  `fletcher-core/0.5.0-alpha` by version with no revision pin, so only the recipe revision moves.
- **The README path in the cache build — run, not reasoned.** `conan create . -o "&:run_tests=True"`
  genuinely compiled (`test_status_taxonomy.cpp` in the log, `Copied 3 '.cpp' files`) and reported
  `29/29 tests passed`, `Test #29 Taxonomy.PublishedNumbersMatchTheEnum ... Passed`. Package
  `da39a3ee5e6b4b0d3255bfef95601890afd80709` built and created — **unchanged**, as claimed.
  `README.md` is present in the cache *build* folder (`…/p/b/fletc7f9216e02dc3f/b/README.md`) and
  absent from the *package* folder (which holds only `cmake/`, `include/`, `conaninfo.txt`,
  `conanmanifest.txt`), so nothing about the README reaches a consumer. `#error` if the define is
  missing.
- **`StatusName`'s empty return.** Cannot be confused with a valid name: `Trim` plus the
  leading-empty-cell erasure means a parsed `name` is never the empty string, so parts 2 and 3
  cannot both be satisfied by the same row.
- **The `rows.size()` probe.** Correct given contiguity, and contiguity is asserted rather than
  assumed. `PubSubStatus` has a fixed underlying type (`: int32_t`), so
  `static_cast<PubSubStatus>(10)` is a representable value — no UB in the out-of-range probe.
- **No false positives from the flag.** The only other `switch` reachable from this TU is
  `PubSubError::Sanitize`, which has a `default:`; neither C4062 nor `-Wswitch` fires on it.
- **CRLF, trailing whitespace, backticks.** File read binary, `getline` on `'\n'`, `Trim` strips
  `\r`, space, tab and backtick — handled.
- **Missing or empty README.** `ReadWholeFile` returns `{}`, caller `ASSERT_FALSE`s: loud.
- **Length.** 188 lines for ten enumerators looks long, but 33 are the design-mandated rationale
  header and the mutation table above shows each of the four assertions catching a class the
  others miss. The parser is where the fat is, and F1's fix shortens it. No simplification to
  demand beyond F1.

## Nits

- Reordering rows in the published table (without renumbering) passes, though §5.1's prose says
  "never reordered"; row order is presentational, so this is arguably correct — just not what the
  README implies.
- If the table is deleted the parser scans on to the *next* markdown table in the file; today that
  is `### Build profiles` and it reddens, but bounding the scan to the next `## ` heading removes
  the coupling to unrelated tables.
- A row with an empty `Name` cell is silently dropped rather than reported (the
  erase-leading-empties step removes a genuinely empty first cell along with the split artefact);
  the contiguity check still reddens, so the failure is loud but mislabelled.
- `-Werror=switch` on the GCC/Clang branch has not been compiled anywhere yet (no automated build
  on this branch); if a gtest header in this TU carries a default-less switch, Linux CI breaks —
  loudly, and cheap to fix if it happens.

## RECORD (paperwork for the PM, never blocking)

- `core/README.md`'s "### 3. Include the headers" and minimal-usage snippets still list four
  headers and omit `status.hpp`, which the header list at the top of the same file now includes.
