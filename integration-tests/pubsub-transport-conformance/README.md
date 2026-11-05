# pubsub-transport-conformance

BIND-4d-iii. **One body of C#, every transport, selected by name.**

## What this lane asserts

BIND-4's acceptance names the property precisely:

> Fast DDS **and** XRCE reachable by selector with **no per-transport C# code**.

A claim about the *absence* of per-transport code cannot be proved by reading the
source — it is proved by running one implementation against every transport and
seeing it work unchanged. So every row here is an xUnit `[Theory]` with a selector
parameter and a single body. A binding that had grown a special case for Fast DDS
would need a second body, and there is nowhere in this file to put one.

## Why it is not 80 ported cases

The tracker's bucket 4 lists 80 provider cases, and BIND-4's acceptance states the
bucket as **file sets rather than counts** — saying why, too: bucket 4's own total
moved 102 → 104 between the matrix being derived and BIND-2 closing.

Most of those 80 assert things the managed surface cannot observe and does not
implement: QoS read back out of DDS discovery with a bare observer participant,
sample loans, data-sharing, memory policies, the ordered-delivery buffer, SIGPIPE
handling. By D-BIND-47's test — *does the managed surface implement this, or only
wrap it?* — they belong to the providers that own them, and they are covered where
they live, in C++. What the **binding** owes is the property above.

## Why a lane of its own rather than the unit suite

These rows create **real Fast DDS participants**: discovery traffic, shared memory
segments and multicast on the machine running them. That does not belong in
`dotnet`'s unit lane, which is otherwise deterministic and transport-free.

The concrete reason is recorded in this repo's own history: a false `0xC0000005`
from ~127 leaked segments in `C:\ProgramData\eprosima\fastdds_interprocess` was
once read as a code defect and cost a review cycle. **Clear that directory before
believing any provider access violation.** In a lane whose name says *transport*,
such a failure is diagnosable; in the unit lane it would look like a regression in
the binding.

## XRCE: an Agent this suite starts and proves it owns (D-BIND-56)

`xrce` is a row of every theory. Until 2026-09-25 it was not: this lane started no
Agent, the XRCE row asserted only a typed refusal, and the round trip was deferred to
`integration-test-fastdds-xrce-interop` — which is C++, so no C# had published or
subscribed over XRCE-DDS at all.

* **The Agent** is eProsima's `MicroXRCEAgent`, built by the lane through
  [`agent/`](agent) from the recipe the interop lane uses,
  [`integration-tests/cmake/MicroXrceAgent.cmake`](../cmake/MicroXrceAgent.cmake), and
  handed to the suite as `MICRO_XRCE_AGENT_PATH` (and `MICRO_XRCE_AGENT_LIB_DIR`).
* **The fixture** (`XrceAgent.cs`) starts it on UDP **2020** — used by nothing else in
  the tree — waits until an XRCE session opens, and then **proves it owns the port**:
  a leftover Agent answers the probe just as well, so an answer is not enough. The rule
  and its two forcing tests are the C++ harnesses' (PDA-DEC-1H), in C#
  (`UdpPortOwnership.cs`, `XrceAgentTests.cs`).
* **A row varies only its deployment configuration** — an XRCE client needs its
  Agent's address and a session key of its own. No body reads it.
* **One case crosses the bridge**: a C# XRCE client's row reaches a C# Fast DDS
  subscriber through the Agent, byte for byte.
* **Without an Agent the XRCE rows fail, not skip**; the other rows still run.

`XrceIsSelectableAndAnUnreachableAgentIsATypedTransportFailure` still asserts the
no-Agent answer, against the default address 127.0.0.1:2018, where this lane runs
nothing.

## Two properties of the test bodies worth knowing before editing them

**They are written for the weakest transport.** `inprocess` delivers synchronously
on the publishing thread; Fast DDS delivers asynchronously, after discovery, and
may drop a sample published before a reader is matched — that is discovery, not
loss. Every body therefore waits on a signal with a deadline and republishes until
it arrives. This costs `inprocess` nothing: its first publish satisfies the wait
before the loop can run again. A body written for `inprocess` alone would fail on
Fast DDS for a reason that is not a defect in anything.

**Topics carry a GUID.** Fast DDS is a real bus. Two jobs on one runner, or this
suite open twice locally, would otherwise publish into each other's topics and
produce row counts that make no sense.

## Running it locally

The shim is passed in, because a source build has no NuGet layout to find it in:

```bash
conan create c-abi --build=missing -pr:a=.conan-profiles/<profile>
(cd integration-tests/pubsub-transport-conformance/agent && conan build . --build=missing -pr:a=../../../.conan-profiles/<profile>)
export MICRO_XRCE_AGENT_PATH=<the path the Agent build prints>
cd integration-tests/pubsub-transport-conformance/dotnet/PubSubTransportConformance
dotnet test -p:FletcherNativeShim=<path to the built shim>
```

The Agent build prints `MICRO_XRCE_AGENT_PATH=` and `MICRO_XRCE_AGENT_LIB_DIR=`; on
Windows the install is `C:/fl-uxa-install`, shared with the interop suite, and a
complete install there is reused rather than rebuilt.

Absent the shim the rows **fail rather than skip** — a transport suite that
silently ran on no transport is the vacuity this round keeps catching, and
`EveryTransportInThisSuiteWasActuallyExercised` guards the other half of it: a
selector lost to a rename would otherwise leave the suite green while exercising
one transport.
