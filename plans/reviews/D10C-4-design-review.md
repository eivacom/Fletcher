# D10C-4 design review — cycle 1 of 2 — **NEEDS-REWORK** (1 BLOCKER, 4 DEBT)

Reviewed `plans/D10C-4-loader.md` and `plans/D10C-4-brief.md` at `9bab11f`. Proof of read: `plans/RIX-rulings.md` read top to bottom,
**603 lines, 45 `## ` entries, last `## 2026-11-15 — D10C ladder: the domain-7 port tests` (:595)**. No tier-1 collision found.

## B1 — BLOCKER (unstated premise, check 5): the runtime's folder name is not always the package's
Design :39-42 deletes `NL:70-76` because the folders it searches "are already candidates". That holds only while
`RuntimeInformation.RuntimeIdentifier` (`NL:85`) equals the package's folder name. Since .NET 8 it returns the RID the runtime
was *built* for. That is `linux-x64`/`win-x64` on Microsoft's builds, but a distro RID (`ubuntu.24.04-x64`, `rhel.10-x64`) on
source-built runtimes, which are the .NET that Ubuntu and RHEL ship. Both distros are named in the linux-x64 floor (`NL:157-158`).
A NuGet app's output holds `runtimes/linux-x64/native/` (`PreviewConsumer/Program.cs:72`), which `NL:73` reaches today through
the host's deps.json folders. After this stage that app stops with "if yours is not among them". The failure is loud but misleading,
and a supported platform regresses. P1 does not state the premise, CI (portable runtimes) cannot see it, and the brief's "packaged and
published apps are unaffected" is false there. **Acceptable fix:** name the RID folder from OS + `ProcessArchitecture` (D-BIND-27's
two RIDs), or add the host's `NATIVE_DLL_SEARCH_DIRECTORIES` as candidate folders, walked server-first as now. Key `PlatformFloor`
the same way, add one pure row proving a distro RID still yields `runtimes/linux-x64/native/`, and state the premise as P6.
Handling it (~5 lines) is cheaper than forbidding, which would refuse a platform the floor names.

## DEBT (registered in `design-debt.md`; none loops the design)
- **DEBT-1, brief: Decision 1 is settled, not open.** :575 (1)'s chosen text (R-1 (a), pre-round audit :270-281) reads "one
  resolver site (L5), no fallback branch". d10 L5/D28 says "Presence decides in every route the runtime takes, its default probing
  included". BC-10 says "delete the runtime's default probing deciding by the edge name (`NativeLoader.cs:73`)". Option (b) *is*
  that route, so offering it invites an unplanned amendment of D28/BC-10. Make it a disclosure (the DELETED row already is one)
  and keep P1's STOP. If it stays, B1 must first make (a)'s user text true.
- **DEBT-2, CI:** conformance `:123` carries both the rename and `env -u LD_LIBRARY_PATH` (D9S-7 DEBT-1, `157d3c3`). Keep the
  `env -u` when the rename goes. `conanrun.sh` (`:112`) puts Conan lib folders on `LD_LIBRARY_PATH`, where the server shim's own
  dependencies could resolve, so a P2 breach would stay hidden in that lane.
- **DEBT-3, claim:** "one latched decision per process" (:19-21, table row 3) holds per `AssemblyLoadContext`. A plugin host
  that loads `Fletcher.Interop` twice gets two resolvers and two `Lazy`s, each deciding from its own assembly folder. Different
  choices then meet d9 L17's marker, which is loud. Reword the row and the doc comment; no code needed.
- **DEBT-4, docs:** the documented full-suite commands (`README.md:324`, `docs/architecture-overview.md:354`,
  `dotnet/README.md:83`) pass only `FletcherNativeShim`/`FletcherProbeShim`, and `LoaderLayoutTests` now fail without
  `-p:FletcherServerShim=`. List them, plus `ci.dotnet.yml:115-117, :253-255` ("arrow-bridge is NOT in the loop"), under docs.

## Checked, no finding
- **:575 (1) exactly.** Server names come first, the walk is `File.Exists` only, `Load` runs once, and nothing loops. `Resolve`
  answers `LibraryName` with a handle or a throw, never zero, so the runtime's default probing and DllImport defaults never run
  for the shim. The one by-name import left is test-only: `CopyOracleTests.cs:340,344`, where the probe is staged alone.
  It sits outside the `dotnet/src` scan by design. Apart from B1, `runtimes/<rid>/native` is probed only by the resolver.
- **Cached refusal: correct.** A failed `NativeMethods` type initializer is already permanent for the process, so a retry
  could never pick up a later fix. `ExecutionAndPublication` plus the cctor lock leave no race.
- **Edge→edge narrowing: licensed** by R-1 (a)'s "no fallback branch" and L5's one site, and disclosed in user terms.
- **Test host and module list: a legitimate outside observation.** The host is driven through the public API, and the OS
  witnesses L5's own subject, which file loaded. It improves on the ladder's "internal observation" class without making the
  variant public. It is robust: a refused dlopen or LoadLibrary maps nothing, and `PreviewConsumer/Program.cs:73-78` already
  asserts exactly-one on both OSes.
- **CI.** The server leg is the ladder's ground rule. P5 stops on a red row that appears only on the server leg, and the batched
  subjects still run on the managed batcher, over exports both builds carry. The conformance server pass stays equivalent and
  gets stronger: it observes the load where `cmp` only showed the bytes were present. This holds given DEBT-2.
- **Tree claims verified:** `NL:70-76` and `:111-127`; T42 `:166/:173/:176`; T43 `:193/:224` wording kept; conformance
  `:119-127` and `:211-235`; `Link` keeps the file's own name (`BindingAbiConformance.csproj:37`); Interop packs shims but copies
  none, so the host stays shim-free.

## Budget
- **7a.** Design 150/150 and brief 55/55; new surface 0; net declared. The documents are dense, not padded: every section carries
  content found nowhere else.
- **7b, priced independently.** `NativeLoader.cs` about +30/−18 including B1's fix. YAML about +60/−30: two dotnet legs at about
  20 each, conformance −24/+6, header rewrite. csproj and slnx about +35-50, the new host csproj at about 20 in repo style. That
  gives a net of about 80-115 against a ceiling of 250. **It fits, so no split and no rung is needed.**
