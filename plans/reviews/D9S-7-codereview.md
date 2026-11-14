# D9S-7 code review (step 4b, correctness) — diff c3d9670..f1a099e

**Verdict: APPROVE.** 0 blocking, 2 should-fix, 6 nits. Read only, nothing built.

Checked and sound. The Linux `bash -c` quoting is right: `\$`, `\"`, the single quotes inside the outer double quotes, and `-o '&:variant=server'`. `!` is safe because the shell is not interactive, so history expansion is off. Both `locate_shim.sh` lookups are unique in a fresh cache: the cache is not restored, `--rm` gives a clean container, and the image pre-builds no fletcher package. The edge lookup finds only the edge package, because the emitter resolves the same package ID. The `-o` folder comes from `mktemp -d`, so a missing staged file makes `! cmp` go red. The tests load the other build once (a function-local static), by a path built at runtime, with `RTLD_LOCAL`, and never unload it. A failed load goes red through `ASSERT_NO_FATAL_FAILURE(RequireLoaded)`. The twin's definitions, visibility, `/EHs`, export map and `--exclude-libs` match the edge shim. Neither TARGET_FILE_NAME is a substring of the other. The two-builds tests run in `ci.c-abi.yml`'s server legs. Each doc `[built]` mark carries "Windows by hand; CI and Linux unrun", and the L17 and BC-15 marks also carry "test rebuild, not the shipped edge file".

## should-fix

**S1. The `cmp` control checks something that is always true, not the guard (conf: high).** On both legs the control copies `$SHIM` into a new folder and compares it with `$SERVER`. That is the same as `cmp $SHIM $SERVER`, and it goes red only if the edge and server builds were byte-identical, which cannot happen. It never runs the guard where the guard runs (the dotnet output folder), so it does not show that the guard catches the stale-edge case the header describes.
*Fix:* compare against the edge pass's real output instead. `cmp -s bin/Release/net10.0/<shim> "$SERVER"` must fail, because that folder holds the edge file the first pass staged. Drop the `CONTROL` copy.

**S2. The twin is a fourth hand-copied shim recipe (conf: medium).** The shipped shim, the probe shim, the hand-over fault shim and now `fletcher-c-abi-edge-twin` each repeat the same sources, definitions, visibility and link options. Change the edge recipe (a new source, a new flag) and the twin goes stale. It only fails if the variant define changes; any other gap still builds and still passes. Forbidding direction: if one function builds every shim, the twin cannot drift.
*Fix:* add `fletcher_c_abi_add_shim(<target> VARIANT <edge|server> [SOURCES ...])` in `c-abi/CMakeLists.txt` and build all four through it. If touching the shipped target is out of scope here, carry this as debt.

## nits
- `test_two_builds.cpp` Test 1: if the call returns FL_OK, `ASSERT_NE(refused.message, nullptr)` returns before `provider_destroy(never)`. The row is red anyway, but it leaks; check the message after cleanup.
- `RequireLoaded`: no `dlerror()` or `GetLastError()` in the failure message.
- `OtherPath` copies `DecoyPath` from `test_single_copy.cpp`; the brief said to reuse the decoy's helper.
- Linux server pass: `LD_LIBRARY_PATH` from `conanrun.sh` still lists the edge package's folder. `cmp` covers the missing-file case, but unsetting it for the server pass leaves no fallback to the edge file.
- Windows server step: `bash "$GITHUB_WORKSPACE/scripts/..."` passes a path with backslashes. The relative path `../../../../scripts/locate_shim.sh`, or `cygpath`, is safer.
- `ci.pr.yml` `integration-binding-abi`: `.github/actions/conan-install/**` is missing (the Windows leg uses it). The `for` loop's `&&` body also hides a failed iteration other than the last. Both predate this diff.

## RECORD:
- `c-abi/tests/CMakeLists.txt` says "`TwoBuilds`'s build assertion catches recipe drift", but it catches only a change to the variant define. Ladder rung 9's "about 10-20 lines of test CMake" is about 45 as landed. The brief's Linux finding ("in edge-first") also applies to server-first: there, the twin's calls to its own exported functions can bind to the server shim.
