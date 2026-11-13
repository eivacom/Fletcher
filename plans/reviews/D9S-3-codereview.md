# D9S-3: code review (step 4b)

**Diff:** `git diff 3dcfd69..HEAD`: `scripts/check_edge_arrow_free.sh` (new), `ci.c-abi.yml` (both legs), `ci.pr.yml` filter, d9 and register edits.
**Verdict:** 0 blocking, 1 should-fix, 4 nits. I found no reachable path that ends green without having read anything.

## What I checked and found sound (one line each, no action needed)
- **pipefail and masking.** Every `|| true` sits after a step whose empty result is refused later. An empty `list` makes `HAS_OURS=0` and exits 1 for the edge shim. An empty `hits` file fails the control. No `grep -q` reads a pipe, so SIGPIPE cannot occur.
- **`hosts=$(graph_hosts) || exit 1` turns errexit off inside the function.** A python crash is still caught. Before it prints `root`, the root guard catches it. After it prints `root`, the hosts list comes out empty, and the step-3 control fails because it uses the same parser.
- **Graph parsing.** The script reads `nodes["0"].name` and the `context=="host"` names. Revisions never appear in `name`. With `grep -x`, the alternation in `ARROW_PKG` is anchored as a whole. A tool-requires Arrow in the build context is not linked, so leaving it out is correct. All three package names match the recipes in the tree.
- **Windows scan.** Under leftmost-longest matching, every `@arrow@@` sits inside a run of the character class, so `-o` cannot split it out. Nested namespaces end in `@arrow@@`. `@nanoarrow@@`, `@narrow@@`, `@arrow_vendored@@` and `fletcher::arrow` (`@arrow@fletcher@@`) do not match.
- **Linux regex.** `nanoarrow::`, `pubsub_arrow::` and `Arrow::` do not match. `<arrow::`, ` arrow::` and `U` (undefined) lines do. A stripped `.so` has no `fletcher::` in its listing, so it is refused.
- **Docker `bash -c '…'`.** The added lines contain no apostrophe. `$PROFILE`/`$SHIM` are expanded by the inner shell. The script's non-zero exit trips the inner `set -e`.
- **Python detection.** It falls back to `python3` where `python` is missing or is the Store stub. `tr -d '\r'` handles the CRLF that Windows Python writes.
- **Paths with spaces.** Every path is quoted. `$(echo $bad)` / `$(echo $ctl)` are unquoted on purpose, and package names contain no glob characters.
- **Runtime.** Two `graph info` runs, one `nm -C` or `grep -a` over the shim and one over the test binary: seconds, not minutes.

## Findings

### S1 · should-fix · confidence medium: the symbol control is "the first match by file name", the selection D9S-2 forbade
`bin=$(find …/p/b \( -name c_abi_tests -o -name c_abi_tests.exe \) -type f -print -quit)` picks whichever `c_abi_tests` the directory walk reaches first.
- In CI it can be the server build or the edge build. On a developer cache it can be a stale binary from another revision or toolchain, possibly one that is not even this platform's.
- The result is never wrong today: both builds link Arrow, and the wrong-platform case dies loudly (no `nm` on Git Bash).
- But the control is meant to show that *this scan, on a binary built like the shim it judged*, sees Arrow. "Some binary somewhere in the cache" proves less, and it breaks the project rule `locate_shim.sh` exists for.
- **Fix, in the forbidding direction:** take the control from the edge package's own build folder, the sibling of the package folder that `locate_shim.sh` already resolved: `B="$(dirname "$(dirname "$(dirname "$EDGE")")")/b"`. Then require exactly one `c_abi_tests[.exe]` under it, and refuse on zero or on more than one, naming them.
- This also removes the dependence on `${CONAN_HOME:-$HOME/.conan2}`. As a side effect, a missing `p/b` stops dying through errexit on `bin=$(find …)` with find's raw message and no `::error::` line.

## Nits
- N1 (medium): `conan graph info` runs without `--no-remote`. Binary analysis queries the remotes for every node not built in the cache, including the edge `with_probe_shim=False` root. Every recipe is already in the cache, so `-nr` makes the check hermetic and removes a remote outage as a cause of red.
- N2 (high): `scan`'s `*)` branch sends anything that is not `.dll`/`.exe` to `nm`. The case could name `*.so|*/c_abi_tests` and refuse everything else. Today a wrong file already dies through the `HAS_OURS` guard, so this is only for tidiness.
- N3 (high): "among the edge build's N host nodes" counts the root, and counts 1 when `hosts` is empty. The message is cosmetic.
- N4 (medium): on Windows, the symbol half is only as sensitive as the RTTI and import names it can see. An Arrow link that adds no polymorphic type would pass it. This is disclosed (d9 L8, 0b note 2) and the graph half and the Linux leg cover it. I list it only so it is not lost.

## RECORD
- RECORD: d9 L8 "Holds at: `conanfile.py:58-83`" (and register BC-11 `conanfile.py:73-82`). The Arrow requires are now at `c-abi/conanfile.py:103-104`, inside `requirements()` at ~87-107.
- RECORD: register BC-11's "outside test requires" is wider than the check, which does not tell a host `test_requires` apart (0b note 1). Either the text or the check should say so.
