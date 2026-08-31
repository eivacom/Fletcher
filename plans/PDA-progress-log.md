# PDA — Progress Log

One section is appended per item by the runbook after it is green, reviewed,
logged, and pushed. See
[PDA-protocol-driver-abi.md](PDA-protocol-driver-abi.md) for the tracker and
[../docs/protocol-driver-abi-spec.md](../docs/protocol-driver-abi-spec.md) for the
oracle.

## Round kickoff (2026-08-31)

Round designed on 2026-08-31. Decisions L1–L7 were answered by the maintainer in
the design conversation; L8–L10 in the follow-up scope questions. The design
session was lost before any artifact was written; it was recovered from the
session transcript and the design is preserved verbatim at
`c:\tmp\PDA-abi-design-recovered.txt`.

Two citation errors in the recovered design were corrected while writing the spec
and must not be reintroduced:

- The MCU `<75 KB` Flash figure comes from **TD-004** (rationale) and **TD-007**
  (context) — *not* TD-005, which is schema transport via companion topics.
- The recorded dynamic-linking skepticism is **TD-007's** "alternatives
  considered", and it is narrower than it first reads: it rejected dynamic linking
  as a way to make **Arrow C++** optional in the edge/server tier split, not
  plugin ABIs in general. The counter-argument PDA-11 owes is correspondingly
  narrower — see spec §11.1.

One estimate was also corrected: the Fast DDS config blast radius was recorded in
the design conversation as "4 code sites and 2 docs". Measured, it is **4 external
consumer files / 19 occurrences**, plus **39 provider-internal occurrences across
7 files** (24 of them in the provider's own QoS test TU, which is substantially
rewritten) and **10** in XRCE. The file count for *external* consumers was right;
the total churn was understated. See spec §10.

<!-- Entries appended below by the round runbook -->
