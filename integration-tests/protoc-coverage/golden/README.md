# Parity-oracle golden bytes (GIR-2)

Each `coverage.<Message>[.<variant>].v1.bin` here is the **source-controlled
wire-contract baseline** for one fixture: the exact positional-format bytes the
current generator's `Encode()` produces for that fixture. The parity oracle
(`tests/test_parity_oracle.cpp`) asserts, per fixture:

```
row.Encode()                      == <this golden>
Codec.EncodeRow(ToArrowRow(row))  == <this golden>
row.Encode()                      == Codec.EncodeRow(ToArrowRow(row))
```

and that both the fresh encode and this golden **decode** back to the fixture
values field-by-field through the generated Arrow View.

## `.v1` and churn policy

`.v1` is the baseline generation, not a schema-evolution knob. A byte change to
a golden for an **already-supported** fixture is a **stop-and-ask** (locked
decision #2), not a routine rebaseline. New goldens are expected only for a
newly added fixture or a formerly parked input that GIR-10 makes faithful.

## Regenerating (reviewed, never automatic)

Normal `ctest` never rewrites these files. To (re)baseline against the current
generator output, run the gated mode explicitly and review the resulting diff:

```
FLETCHER_REGEN_PARITY_GOLDENS=1 \
  ./coverage_parity_oracle_tests --gtest_filter=ParityOracle.RegenerateGoldens
```
