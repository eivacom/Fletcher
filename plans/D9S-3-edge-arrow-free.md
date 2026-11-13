# D9S-3 — The edge build carries no Arrow C++, checked by name (design)

Round D9S, domain 9 (`docs/domains/d9-c-abi-shim.md`), ladder rung 3 (`plans/D9S-test-ladder.md`).
Brief: `plans/D9S-3-brief.md`. **Lane:** full loop. The stage was ejected from the light lane at
phase A: the script as authored has 59 non-comment lines and the workflow adds about 16, so about 75
touched lines against the 60-line admission. Branch `feature/shim-server-variant`, after D9S-2.

## Summary

This stage adds one CI check, `scripts/check_edge_arrow_free.sh`, to both legs of `ci.c-abi.yml`.
It fails, and names what it found, if Arrow C++ can reach the **edge** build. It looks along two
independent paths:

- **The graph:** the edge configuration's Conan graph.
- **The names:** the names inside the edge shim the package actually holds.

Two controls run on every invocation, and each must go red by name. This turns d9 L8 from
`[built as a property; no code owner]` into a checked law (BC-11, Q4). It lands before D9S-4 first
links Arrow into the server build, and it must stay green from then on (ladder guard c).

## Design

**Authority.**
- d9 **L8**: "The edge variant links no Arrow C++ … Arrow C++ and `arrow-bridge` may appear only in
  test binaries."
- d9 **L12**: "Nothing else in the edge build changes … no link (L8), no size check, no behaviour."
- Owner, 2026-11-12: "Arrow C++ should never be in the native library, because it needs to be able
  to run on embedded devices."
- Register **BC-11**: the structural test is "the edge configuration's Conan host graph has no
  `arrow`, `fletcher-arrow-bridge` or `fletcher-pubsub-arrow` … `nm -C` on the **unstripped** edge
  shim finds no `arrow::` symbol."

### Structure of the check (script as authored, 90 lines)

`check_edge_arrow_free.sh <recipe dir> <profile> <edge shim>` runs four steps in order. It collects
failures and exits non-zero if any step failed. A refusal ("nothing was read") exits at once.

1. **Graph half.**
   - What it runs: `conan graph info <recipe> -pr:a=<profile> -o "&:variant=edge" --format=json`,
     with every other option at its shipped default (no `run_tests`, no `with_probe_shim`).
   - How it reads the result: Python reads the JSON and emits the root node's name and every node
     whose `context` is `host`.
   - Fails if any host node is `arrow`, `fletcher-arrow-bridge` or `fletcher-pubsub-arrow`, listing
     each one by name.
   - Refuses if the root is not `fletcher-c-abi`.
2. **Symbol half**, on the edge shim. The CI step finds the shim with
   `locate_shim.sh fletcher-c-abi variant=edge …` (by package identity, D9S-2). The script picks the
   method by file extension:
   - **Linux** (any file that is not `.dll` or `.exe`):
     - Runs `nm -C` with all symbols, both defined and undefined.
     - A hit is `(^|[^A-Za-z0-9_])arrow::`.
     - The listing must contain `fletcher::`.
   - **Windows** (`.dll` or `.exe`):
     - Runs `grep -aoE '[A-Za-z0-9_?$@]*@(arrow|fletcher)@@'` over the image. This finds the
       decorated class names in the type information and the decorated names of imported functions.
     - A hit is `@arrow@@`.
     - The listing must contain `@fletcher@@`.
3. **Graph control.** Step 1 again with `-o "&:run_tests=True"` added. The result must name
   `fletcher-arrow-bridge`; otherwise the step fails ("graph half cannot be trusted").
4. **Symbol control.** Step 2 run on the first `c_abi_tests` or `c_abi_tests.exe` found under
   `${CONAN_HOME:-~/.conan2}/p/b`. That binary links Arrow (T10), so it must yield at least one
   Arrow name. If no binary is found, or no Arrow name is found in it, the step fails.

### Why this is a structural check, not a site list (BC-11)

A site list enumerates the places that could bring Arrow in and inspects each one. Examples: the
`requires` lines in `conanfile.py`, the shim's `target_link_libraries`, the sources of
`fletcher-c-abi-impl`. Such a list is only as complete as its author's foresight. Today's
incumbents (`conanfile.py:95-107`, the CMake link lines, D-BIND-148's DLL list) are exactly that,
and BC-11 calls them "all proxies".

This check judges the **outcome** instead, at the two places every path must pass through:

- **The resolved dependency graph of the shipped configuration.** Every Conan path ends there: a
  direct `requires`, a transitive one through any Fletcher package, an option default flipped, or a
  `requires` placed under the wrong condition.
- **The names inside the built artefact.** Every link path ends there, whether it goes through
  Conan, CMake, a vendored source or a prebuilt library added by path, and whether the link is
  static or shared.

The check names no file and no line of the recipe or the CMake. A new route for Arrow needs no edit
to the check to be caught. The names it does carry (three package names, one namespace) are the
**subject**, named by L8 and BC-11. They are not sites.

### Why the graph is the *shipped* configuration and the symbols are the *built* artefact

- `run_tests` is deleted from the package ID (`conanfile.py:109-110`), so CI's edge package, which
  is built with `run_tests=True`, differs from the shipped identity only by `with_probe_shim`, which
  adds a separate target and leaves the shim's own link unchanged (`c-abi/CMakeLists.txt:183-187`,
  `:203-238`). Its build graph lawfully contains Arrow, for the test binaries (L8).
- So the graph half judges the configuration that **defines** the shipped package, with
  `run_tests` off. That is the recipe's own contract.
- The symbol half judges the **artefact** that was actually built under the test configuration. It
  catches a CMake link from the shim target to a test-only dependency, which the graph half cannot
  see.
- Each half covers the other's blind spot. Neither on its own is the proof.

### CI wiring (phase B, `.github/workflows/ci.c-abi.yml`)

Both edge and server `conan create` runs must have completed before the check runs. They create the
edge package, put the Arrow recipe in the cache for the graph control, and leave a `c_abi_tests`
binary for the symbol control.

- **Windows leg.** A new step goes after "Compare the export tables with binding.h", with
  `shell: bash` and the repo root as working directory:
  ```
  EDGE=$(bash scripts/locate_shim.sh fletcher-c-abi variant=edge with_probe_shim=True)
  bash scripts/check_edge_arrow_free.sh c-abi .conan-profiles/Windows-msvc194-x86_64-Release "$EDGE"
  ```
  Python is the one `conan-install` set up.
- **Linux leg.** Add one call inside the existing `docker run … bash -c '…'` block, after the export
  control, reusing the block's `$PROFILE` and `$SHIM`:
  `bash /workspace/scripts/check_edge_arrow_free.sh /workspace/c-abi "$PROFILE" "$SHIM"`. The call
  must sit inside that block: the Conan cache does not outlive the container (the comment at
  `ci.c-abi.yml:180-183`). The image ships `python3`; the script falls back to it.
- **Both legs:**
  - The step's comment cites d9 L8 and BC-11.
  - The comment at the head of the workflow (`:29-32`) gains one line saying the edge build is also
    checked for Arrow by name.
  - No `if:`, no `continue-on-error`, and no change to the size steps (L12).

### Staying green at D9S-4

D9S-4 first links Arrow, via `pubsub-arrow`, into the server build. The check stays green, and stays
meaningful, only if D9S-4 meets three conditions. They go to D9S-4's designer as its premises:

- **(a)** `pubsub-arrow` is required in `requirements()` only when `variant == "server"`. Otherwise
  the graph half goes red, which is correct.
- **(b)** The Arrow-using sources and the `pubsub-arrow` link belong to the **server shim target
  only**, never to `fletcher-c-abi-impl`, the object library both builds share. Otherwise the
  symbol half goes red on the edge shim, which is correct.
- **(c)** The script is not edited to accommodate D9S-4. A D9S-4 change that turns this check red is
  a D9S-4 defect, or a stop-and-ask. It is never a reason to relax the patterns.

D9S-4 may *add* the server shim as a second symbol control, since it will link Arrow. That is
optional and does not replace the test-binary control.

## Corner cases forbidden

Rung 2, refused at the door, one typed message each:

- **A listing that read nothing passes as clean.**
  - The symbol half refuses unless the listing holds a Fletcher name (`fletcher::` or
    `@fletcher@@`). This covers a stripped `.so`, a non-ELF file given to `nm`, or a DLL that is not
    ours.
  - The graph half refuses unless the root is `fletcher-c-abi`. This covers wrong JSON, a wrong
    recipe path, or a schema change.
  - Phase A: a stripped or unrelated DLL was refused, as was the wrong root.
- **A false hit on nanoarrow, the edge codec.**
  - Linux: the pattern needs a non-identifier character before `arrow::`, so `nanoarrow::` cannot
    match.
  - Windows: the pattern needs `@` directly before `arrow`, so `@nanoarrow@@` cannot match.
  - Phase A: a listing with only nanoarrow names was not a hit.
- **Measuring the wrong file.** The edge shim is located by package identity (`locate_shim.sh`,
  D9S-2, audit F8), never by name or first match. The server shim is in the same cache and is never
  judged.
- **An untrusted check.**
  - Both controls run on every invocation and must go red by name. A control that stays green fails
    the step.
  - Phase A showed each control can itself fail: equal graphs, an Arrow-free test binary, no test
    binary.
- **Arrow linked as a shared library.**
  - Linux lists undefined `arrow::` references (phase A: `U arrow::Make()` was red).
  - Windows sees decorated names of imported Arrow functions.
  - D-BIND-148's DLL list (dotnet lane) remains a third net.
- **No usable Python.** The script probes `python -I -c 'import json'` and then `python3`, and
  refuses if neither works. The probe rejects the Windows Store `python3` stub.
- **`nm` missing.** Refused by name.

**Handled residue** (each with its *why not forbidden?*):

- **The symbol control takes the first `c_abi_tests` that `find` returns**, with no ordering.
  *Why not forbidden:*
  - The subject of the control is "a binary that links Arrow". Every `c_abi_tests` in the cache is
    one, whichever build made it (edge or server, T10).
  - A build folder carries no `conaninfo.txt`, so it cannot be chosen by package identity the way
    `locate_shim.sh` chooses a package.
  - Nothing is *measured* from this file, so the "never first match" rule (audit F8), which protects
    measurements, does not apply.
- **Dispatch by file extension** (`.dll`/`.exe` means Windows, anything else means `nm`).
  *Why not forbidden:* the two platforms have no common listing tool. A wrong guess still fails safe:
  `nm` on a PE file, or `grep` on an ELF file, holds no Fletcher name, so the run is refused, never
  passed.

## Premises and stop conditions

1. **The packaged Linux edge `.so` is unstripped, and its `.symtab` holds `fletcher::` names.**
   - Basis: `--exclude-libs ALL` and the version script make symbols *local*; they do not remove
     them. `ci.dotnet.yml:155` strips the packaged file afterwards, which implies it is unstripped.
   - **Unexercised:** no real unstripped edge `.so` has been listed. Phase A used synthetic `.so`s
     in WSL.
   - STOP-AND-ASK if the first Linux CI run refuses the shim with "holds no Fletcher name". Do not
     drop or weaken the guard, and do not switch to `nm -D`, which sees only `fl_*` and so would
     make the half vacuous.
2. **The devcontainer's GNU `nm` demangles as WSL's did (`-C`).** Unexercised in the image. If it
   prints mangled names, the `arrow::` pattern never matches, and the **symbol control** goes red
   for that reason. That is the control doing its job. STOP: fix the tool invocation, not the
   pattern.
3. **Conan keeps build folders (`p/b`) after `conan create`.** True on Windows (phase A found a
   cached `c_abi_tests.exe`). If a CI leg cleans them, the symbol control fails "no c_abi_tests
   binary". STOP-AND-ASK; do not skip the control.
4. **The test binaries keep linking Arrow (T10, lawful under L8).** Both controls depend on it. If a
   later round removes Arrow from the tests, the controls are **re-anchored**, not deleted: for
   example, to the server shim after D9S-4, or to the `arrow` package's own graph. Their subject,
   "the check can see Arrow", is lawful.
5. **The Windows edge DLL carries type information for Fletcher classes.** Phase A: 51
   `@fletcher@@` names. If a future `/GR-` removed them, the guard refuses, which is correct. STOP
   and ask before changing the guard.

## The Windows symbol half: its limit, and why it is not closed here

- **What it sees.** A Release DLL keeps no symbol table, and the edge link produces no PDB or map.
  So on Windows the symbol half can read only the names the image **itself** carries:
  - decorated names in the type information of polymorphic classes (`.?AV…@arrow@@`);
  - decorated names of imported functions.
- **What passes it.** An Arrow link that brings in **no polymorphic Arrow class and no Arrow
  import**. Phase A found 1248 Arrow names in a test binary, so in practice any real Arrow use
  leaves names, but that is evidence, not proof.
- **What still catches that case:**
  - the graph half, for every Conan path;
  - the size ceiling, which is coarse;
  - the Linux leg's full `nm` listing, because the same CMake builds both platforms.
- **Why it is not closed here.** Closing it needs the edge shim's link to emit a PDB (`/DEBUG`) or a
  map (`/MAP`).
  - L12: "Nothing else in the edge build changes … no link, no size check, no behaviour."
  - `/DEBUG` changes the image itself. It adds a debug directory, and unless `/OPT` is set
    explicitly, MSVC's default turns `/OPT:REF` and `/OPT:ICF` off. That is a change to the edge
    build, forbidden by L12.
  - `/MAP` leaves the DLL unchanged, so L12 does **not** forbid it. It is not taken for two
    reasons. First, the map is written to the build folder, not the package, so the check could not
    choose the edge build's map by package identity, as it chooses the shim (audit F8). Second, the
    Linux leg already reads the full symbol table, so rung 3 does not need it.
- **Recorded as debt.** If the owner wants Windows parity, a `/MAP` emitted and packaged for the
  edge build is a question for a later round. It leaves the DLL unchanged, so L12 does not stand
  in the way.

## Forcing-test mapping

| Test | What turns it green | Red for the right reason |
|---|---|---|
| CI step "the edge build carries no Arrow C++ (d9 L8)", Windows leg | Steps 1 and 2 find no hit on the shipped edge graph or on the edge DLL located by identity | Green on the pre-stage tree by construction: nothing links Arrow. Its red is steps 3 and 4 **on every run**. Phase A on Windows: graph control red naming `fletcher-arrow-bridge arrow`; symbol control red with 1248 Arrow names |
| Same step, Linux leg | Same steps, `nm -C` on the unstripped edge `.so` | Same controls. Phase A, WSL with real GNU `nm`: red on `T arrow::Make()` and on `U arrow::Make()`; stripped refused. **Not yet run against the real image or the real `.so`** (premises 1 and 2) |
| Guard c (ladder): the same step at D9S-4 and later | Unchanged script; D9S-4 premises (a) and (b) | An Arrow link reaching the edge build is red by name |

**Test categories (step 0b).**
- The **rung** is the CI step. It is external: it reads the built artefacts and the recipe's graph,
  and calls no internal code.
- The two **controls** prove it can fail. Each asserts only that the check *sees* Arrow where L8
  permits it (test configuration, test binary). Neither asserts nor requires Arrow in the edge
  build.
- The refusals are **guards**.
- No supplementary legs are committed. Phase A's stub `conan`, synthetic `.so`s and Python stub are
  implementer evidence, not shipped tests. No test asserts a forbidden behaviour.

## Risks / Unknowns

- **Linux unexercised for real** (premises 1 and 2). The first green run on the owner's draft PR
  (ruling 2026-11-13) is the evidence. Until then, d9 L8 and BC-11 are marked "Windows, by hand; CI
  and Linux unrun", as at D9S-2.
- **The Windows limit** (above): debt, disclosed in the brief.
- **`conan graph info` needs ConanCenter for `arrow/23.0.1` in the control.** The cache already
  holds it from the earlier `create`. An offline local run without that recipe fails the control
  loudly, never silently.
- **Light-lane ejection.** The script grew from about 40 estimated non-comment lines to 59, mostly
  from refusals and readable messages that review would otherwise ask for. It is still well under
  the 250 ceiling. No split is proposed.
- **No coexistence window.** The size ceilings are not a bridge: L12 keeps them permanently as the
  coarse check.

## Files-to-touch

- `scripts/check_edge_arrow_free.sh`: **new**, as authored at phase A (90 lines).
- `.github/workflows/ci.c-abi.yml`: one step on the Windows leg, one call inside the Linux docker
  block, one header-comment line.
- Docs, not production:
  - `docs/domains/d9-c-abi-shim.md`: L8 mark, Q4 resolved, and the §8 "Findings" line on "only the
    size ceiling".
  - `docs/domains/bounded-context-register.md`: BC-11 status.
  - The marks read "Windows, by hand" until a CI run is green.

## Files-to-delete

None. The incumbents this check stands beside are retained by law, not left over: the size ceilings
(L12: "no size check" changes), the recipe's `run_tests` gating, and D-BIND-148's DLL list
(domain 10's). Nothing is superseded. The new check is the code owner that L8 lacked.

## Declared numbers (raw diff)

| File | + | − | Non-comment touched |
|---|---|---|---|
| `scripts/check_edge_arrow_free.sh` | 90 | 0 | 59 |
| `.github/workflows/ci.c-abi.yml` | ~16 | 0 | ~10–16 |
| **Total** | **~+106** | **−0** | **~70–75, 2 files** |

New public surface: 0 (a CI script, not ABI). Ceiling: 250 net production lines per stage; this
stage's net is about 106.
