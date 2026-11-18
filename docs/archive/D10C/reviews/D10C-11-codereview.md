# D10C-11 code review (step 4b)

Diff base `924adcb`, working tree. Read: `run.sh`, `minimal.sh`, `Program.cs`, the consumer's csproj,
`NativeLoader.cs:150-166,226-233`, `binding.cpp:236`, `Subscriber.cs:487`, `ci.package-consumer.yml:158-193`.
Ran `check_readme_snippets.py` for both READMEs: 7 and 2 snippets match.

**Verdict: 0 blocking, 0 should-fix, 3 nits.**

## Checked, and holds
- **Shell.** `set -euo pipefail` is in both scripts. The new code has no pipes. `run_in`'s `exit 1` inside `if ! run_in` exits
  the script. `$3` (base/server) in the `sh -c` string is a fixed word. `rm -rf "$WORK/dotnet"` runs before both `cp -r`s, so
  the copy never nests into a stale `PreviewConsumerServer`, and it has no bin/obj. `\.` survives inside double quotes, so
  `grep -E` gets `GLIBCXX_3\.4\.3[12]' not found`.
- **`dotnet add package` is deterministic.** `--no-restore` touches no feed and writes the literal `[$VERSION]`. Restore then
  maps `Eiva.Fletcher*` to the bundle's feed alone, more specific than `*`, into an empty `NUGET_PACKAGES`.
  `assemble_preview_bundle.sh:34` requires `Eiva.Fletcher.Server.$VERSION.nupkg`, and the grep at `run.sh:97` refuses a no-op.
  `DOTNET_NOLOGO` is exported before the first `dotnet` call.
- **Loaded-module check.** It collects modules named as either shim and requires exactly one, at this configuration's
  `runtimes/<rid>/native/` path. `/proc/self/maps` (Linux) and `EnumProcessModules` (Windows) both give full paths. Two
  loaded shims, or the wrong one, fail.
- **Wrong argument.** Base told `server`: the uncaught `NotSupported` kills the process, which is loud. Server told `base`: the
  batch succeeds, the shim is wrong and a server file is present, so three failures. A missing or unknown argument exits 2
  (`args.Length` is checked before `args[1]`).
- **No vacuous arm.** The logs hold only container output; no command line is echoed. The floor sentence has
  `GLIBCXX_3.4.32)`, never `' not found`, so the regex can only match dlerror's text (the evidence row's regex control
  agrees). `fletcher-c-abi-server` occurs in the shim's text (`binding.cpp:236`) and not in the binding's sentence
  (`Subscriber.cs:487`), so DEBT-1's check can fail. "That file is the edge build" is specific to the loader's `server ?`
  branch, so the negative grep cannot be tripped by the "chosen over the edge build" clause.
- **CI portability.** No YAML change is needed. Windows uses `cygpath -m` paths for `dotnet add`, and `grep -q` tolerates
  CRLF. `minimal.sh` runs only on `runner.os == 'Linux'`. Without a `global.json`, the 10.0.x SDK is picked, the SDK this was
  rehearsed with.

## Nits
- n1 (high): `minimal.sh` `set -- $config` is an unquoted split that clobbers the positional parameters (SC2086). It is
  harmless here, since `WORK` was already captured; `read -r proj cfg <<<"$config"` says it plainly.
- n2 (medium): nothing asserts that the edge file is present in the server output, which the comment at `minimal.sh:78-79` and
  the README rely on ("sits beside it"). It is not a defect: with the file absent, no fallback is possible. A
  `File.Exists(edge packaged path)` in the server branch would make the claim tested.
- n3 (low): base told `server` ends as exit 134 (or e0434352) with no `PreviewConsumer:` failure line. It is loud, so this is
  cosmetic.

## RECORD
- `Program.cs:13-16` says the shim that answered is the one *Eiva.Fletcher.Interop's* package put under `runtimes/`. In
  `server` it is `Eiva.Fletcher.Server`'s. Design cites `Program.cs:163-179` for the refusal arm; the tree has `:160-181`.
