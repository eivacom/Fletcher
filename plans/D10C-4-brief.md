# D10C-4 — Stage Brief (2026-11-15)

**In one sentence:** a C# app runs on the server build of Fletcher's native library when that file is beside it, and
on the small edge build otherwise. A server file that is present but will not load stops the app, naming the file and
the reason; the app never quietly drops back to the small build.
**Forcing test:** `TheServerShimLoadsWhenPresentBesideTheApp`: an app run from a folder holding both files (loose and
NuGet `runtimes/` layouts) has the server file loaded, and only that one. Its control,
`AnUnloadableServerShimStopsTheAppAndLoadsNoShim`: a broken server file stops the app with no library loaded.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Which native library a C# app loads | CHANGED | server if its file is beside the app, else edge (ruling 2026-11-15, "Presence; fail loudly") |
| A present but broken server file | CHANGED | the app stops, naming the file and the OS's reason; today it is ignored and edge loads |
| Finding the library via the system search path (PATH, LD_LIBRARY_PATH) | **DELETED** | only the app's own folders are searched (Decision 1) |
| CI: C# test lane / conformance lane | CHANGED | builds the server library and runs the whole C# suite again with both files present / drops the renamed copy |

New public API: none; which build is loaded stays internal, and the tests watch which file the process loaded.

## Deleted
- The runtime's search by the edge file's name, and moving on to another copy after one fails to load. Replaced by
  one decision from which files are present: the first present, loaded once.
- The conformance lane's renamed copy and its byte comparisons, replaced by the file under its own name plus a check
  of which file loaded. One loader-test assertion pinning "the server file is never looked for" is rewritten, not
  kept. No test is deleted.

## Corner cases forbidden vs handled
**Forbidden:** the edge build loaded while a server file is present · any drop-back after a refused file, including
from a broken edge copy to another edge copy (this used to work and now stops the app; disclosed) · two builds in one
process, or a choice that changes later · loading the library outside the one decision site (a source scan checks it).
**Handled:** an app with no assembly folder (NativeAOT, single-file) is searched in its app folder only (unchanged; NativeAOT is supported).

## Decisions for you
1. **Should a C# app still find Fletcher's native library through the system search path, or only in its own folders?**
   - **(a) Own folders only.** User/operator: packaged and published apps are unaffected; an app relying on PATH or
     LD_LIBRARY_PATH, with no copy beside it, stops at start-up and lists the folders searched. Architecture: serves
     d10 L5 and your ruling that presence decides, in every route, from one site (BC-10).
   - **(b) Keep the system search as the edge build's last resort.** User/operator: such apps keep running on edge; a
     server file on the system path is silently ignored. Architecture: strains L5 and BC-10, because a second route
     loads by name and cannot see a broken file.
   **Recommendation:** (a). It follows L5, "Presence; fail loudly" and this rung's purpose: one visible rule decides
   which build an app runs on. **Default if you don't answer:** (a).
   *Background (skippable): the route deleted is `NativeLibrary.TryLoad` by name, `NativeLoader.cs:73`.*

## Risks accepted / debt carried
The C# CI lane also builds Arrow C++ on both OSes, so it is slower; the next stage (packing the server package) needs
that build anyway. A refused load reaches the app inside the runtime's "type initializer" exception, as every load
failure does today; the named reason is the inner exception.

## Numbers
Declared net lines: +113 / −39 (net ≈ +74) counted production; tests ≈ +285 / −10 · new public surface: 0 · design cycles used: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above>.
