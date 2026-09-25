# BIND — Native transport protocols: superseded by PDA

Addendum to [BIND-csharp-bindings.md](BIND-csharp-bindings.md).

**Status (2026-08-31): largely SUPERSEDED.** An earlier revision of this file
designed a per-transport C ABI shim for Fast DDS and XRCE-DDS, with a per-RID
native build matrix and a Fast DDS QoS marshalling problem at its centre. Round
**PDA** (`feature/protocol-driver-abi`, spec `docs/protocol-driver-abi-spec.md`)
solves that problem generically and better. This file now records what changed,
what survives, and what BIND still owes.

---

## 1. What PDA changes

PDA turns every transport into a **runtime-loadable driver** behind a
`DriverProvider : PubSubProvider` adapter (PDA-L8). Therefore:

```
  C# app ──► binding ABI ──► Fletcher C++ ──────────► driver ABI ──► Fast DDS
   (BIND)                    (Publisher/Subscriber,   (PDA)          XRCE-DDS
                              codec, PubSubArrow)                    InProcess
                                                                     Zenoh / MQTT / …
```

**BIND binds `PubSubProvider` once and inherits every driver.** It does not bind
Fast DDS. It does not bind XRCE-DDS. There is no per-transport C# code at all.

| Earlier problem | PDA resolution |
|---|---|
| `FastDDSProviderOptions` embeds `eprosima::…::DataWriterQos` / `DataReaderQos`, which cannot cross a C ABI without losing fidelity | **PDA-L2/L9**: config becomes a typed core `{max_payload_bytes, domain_id}` plus an **opaque driver-defined document**; Fast DDS uses its native **XML QoS profile**, and `FastDDSProviderOptions` is **retired, not deprecated**. Fletcher gains no config parser. ⚠️ **Not yet landed** as of 2026-08-31 — the branch is doing the FastDDS zero-copy/data-sharing modernization first, and the QoS struct is still present. Which is exactly why Stage D waits. |
| Per-transport RID matrix (Fast DDS + fastcdr + foonathan_memory per RID; microxrcedds + microcdr per RID) | Only **one** native artifact ships from BIND (`Eiva.Fletcher.Interop`). Driver binaries are PDA's concern and are loaded at runtime from an explicit configured path (PDA-L7). |
| "Fast DDS yes, XRCE gated separately" — the cost/value asymmetry | **Moot.** XRCE costs BIND nothing extra; it is inherited. The XRCE-specific question disappears. |
| Adding a future protocol (Zenoh, MQTT) means new binding work | **Zero** BIND work — a new driver is a new driver. |

**Sequencing consequence:** BIND's pub/sub stage (Stage D — BIND-5/5b) is
**gated on PDA merging**. Binding the current `FastDDSProviderOptions` surface
would bind an API PDA retires as a breaking change. Stages T, A, B, C and F are
all PDA-independent, so this gate costs one stage, not the round.

---

## 2. What survives from the earlier analysis

Three findings still hold and are now folded into the main plan:

- **The zero-copy publish path survives the boundary — and got better.**
  `WriteBuffer` was reshaped on the PDA branch (2026-08-31) from five per-operation
  virtuals into a **window plus refill hooks**: appends are non-virtual writes into
  `{data_, capacity_, pos_}`, and only running out of room reaches a virtual. The
  ABI struct mirrors it (`{ctx, data, capacity, pos, grow, grow_zeros}`), so a
  publish crosses the boundary **once per window refill, not once per append** —
  strictly better than the "one indirect call per row" this file previously
  estimated, and still **no copy per row**. PDA-L4 makes zero-copy a *requirement*,
  and PDA §4.2 requires the struct be **shared** with the binding ABI precisely so
  a publish composes with zero copies across both hops.
- **`ArrowSchema` handoff is free.** Both sides use the Arrow C Data Interface
  verbatim (PDA §3.3), and arrow-dotnet supports schema/array/stream import and
  export in both directions. `Fletcher.Core` still wants a managed `ArrowSchema`
  reader so the schema can be surfaced **without** an `Apache.Arrow` dependency
  in the non-Arrow packages.
- **The interop plumbing rules are unchanged.** `[UnmanagedCallersOnly]` static
  function pointers, `GCHandle` context tokens, no exception across the boundary in
  either direction, and a delivery callback that must not be `async`. **Updated
  2026-08-31:** `data`, `schema` **and** `attachments` are all now borrowed for the
  duration of the call, so **none** of the three may escape — not just the row
  bytes.

---

## 3. What BIND still owes

| Owed | Where |
|---|---|
| The **binding ABI** itself (role 3 + descriptor-driven codec surface) | BIND-1, BIND-2 |
| Reusing PDA's **shared vocabulary** verbatim — buffer struct, blob `retain`/`release`, `ArrowSchema`, error/version conventions (PDA-L5; divergence is a stop-and-ask) | BIND-1 |
| A **copy-accounting oracle** proving zero-copy for rows *and* attachments | BIND-7 |
| Passing the **opaque driver document** through untouched, parsing nothing | BIND-5 |
| **One** native artifact with a RID matrix (`Eiva.Fletcher.Interop`) | BIND-8 |
| The **LGPL relinking review** for shipping prebuilt native binaries in NuGet | Open decision 6 — needs an owner |

---

## 4. The one non-engineering blocker

**LGPL relinking.** Fletcher is LGPL-3.0-or-later and today ships as Conan
**source** packages that consumers build, so relinking is trivially satisfied.
Shipping **prebuilt** native binaries inside a NuGet package changes that: LGPL §4
requires recipients be able to relink against a modified library, and static
vs. dynamic linking materially changes what must be provided alongside. Fast DDS
itself is Apache-2.0, so it is not the problem. PDA §11.3 records its own
licensing note; BIND's case is narrower (one artifact, `Interop`) but not
automatically covered by it.

**This needs an owner and a decision before BIND-8 designs packaging.** It gates
packaging only — not development — so it must be started at Stage A to stay off
the critical path.
