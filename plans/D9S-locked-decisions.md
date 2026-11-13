# D9S — Locked decisions (digest)

The digest every architect, reviewer and implementer in round D9S must honour. The laws are in
[docs/domains/d9-c-abi-shim.md](../docs/domains/d9-c-abi-shim.md) §3; the owner's words are in
[RIX-rulings.md](RIX-rulings.md) (2026-11-12 and 2026-11-13 entries). Both win over this digest. A
proposed deviation from any line here is stop-and-ask.

- **D-D9S-1 — One source, two builds** (L12, L13). The edge build gains only the variant query and
  named refusals from the batched entry points; it never links Arrow C++ (L8). The server build only
  adds; no edge entry point changes its answer except the query and the batched ones.
- **D-D9S-2 — The shim batches nothing** (L15, BC-9). Windows, counts, timers, order and the batch
  thread are domain 7's. Options pass through as given: `max_rows`, the timeout in ms, `INT64_MAX` =
  `kNoTimeout` untranslated; domain 7's refusal arrives as `FL_INVALID_ARGUMENT` with its message
  (BC-8). No range check in the shim.
- **D-D9S-3 — A batch is borrowed for its callback; kept only by Arrow's move** (L14, L11, ruling
  2026-11-13). What is not moved, the shim releases when the callback returns. The callback never
  receives an `fl_schema`.
- **D-D9S-4 — A failed handoff is reported** (L18, BC-12): a no-batch delivery for that window, rows
  counted dropped, reason kept; the thunk never throws into domain 7. Any fault hook used to prove it
  is test-only and absent from the shipped server build.
- **D-D9S-5 — `FL_NOT_SUPPORTED`, widened**, answers a batched call on the edge build; no new status
  number (ruling 2026-11-13).
- **D-D9S-6 — One ABI version for both builds, bumped once, in stage 1**, together with the only
  permitted domain-10 edit: the expected version line in `dotnet/src/Fletcher.Interop/NativeLoader.cs`
  (D27; PM decision 2026-11-13: one bump at the first header addition satisfies "never silent" and the
  one-line exception; per-stage bumps would exceed it).
- **D-D9S-7 — One shim per process across builds** (L5, L17): the later one refuses and names both.
- **D-D9S-8 — Out of scope:** NuGet packaging, the server .NET package, its DLL lists/floors/ceiling
  (Q5a, R-9, T6–T9's server halves), C#'s use of the batched entry points, D-BIND-68's future — all
  domain 10. Domain 7 and domain 2 code and laws — **one named exception** (owner ruling 2026-11-13, "Allow the build option"): D9S-4a adds an `fPIC` option to `pubsub-arrow/conanfile.py` and `arrow-bridge/conanfile.py`, exactly as `pubsub` has it. `core/README.md`'s status wording (domain 3).
