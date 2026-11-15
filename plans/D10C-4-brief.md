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
| Finding the library via the system search path (PATH, LD_LIBRARY_PATH) | **DELETED** | only the app's own folders are searched (disclosure below) |
| Finding a packaged app's library on a distro's own .NET (Ubuntu, RHEL) | kept | the package's folder is named from OS and processor, not from the runtime's distro name |
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

**Disclosed (settled, not asked):** an app with no copy of the library beside it, which relied on PATH or LD_LIBRARY_PATH
to find one, now stops at start-up; the message lists the folders searched. Packaged and published apps, including those
on a distro's own .NET, are unaffected. Settled by your ruling 2026-11-15 "Presence; fail loudly" (one resolver site,
no fallback branch), domain 10's L5/D28 ("in every route… its default probing included") and register row BC-10.

## Decisions for you
None.

## Risks accepted / debt carried
The C# CI lane also builds Arrow C++ on both OSes, so it is slower; the next stage (packing the server package) needs
that build anyway. A refused load reaches the app inside the runtime's "type initializer" exception, as every load
failure does today; the named reason is the inner exception.

## Numbers
Declared net lines: +125 / −48 (net ≈ +77) counted production; tests ≈ +295 / −10 · new public surface: 0 · design cycles used: 2/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above>.
