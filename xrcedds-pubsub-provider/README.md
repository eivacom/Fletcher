# xrcedds-pubsub-provider

Implements `fletcher::PubSubProvider` using [eProsima Micro XRCE-DDS Client](https://micro-xrce-dds.docs.eprosima.com/) (v3.0.x). Transports `EncodedRow` byte buffers between a constrained client and an XRCE-DDS Agent over UDP or TCP. `transport=serial` is nameable in the document and refused as *unsupported* - this build cannot do serial.

## How it works

`XrceDDSPubSubProvider` connects to a running **XRCE-DDS Agent** at construction time. A background thread calls `uxr_run_session_time` continuously to pump the session. All XRCE-DDS entities (participant, topic, publisher/subscriber, data writer/reader) are created on the Agent when `CreateTopic` or `Subscribe` is called.

Incoming messages arrive through a single global `on_topic` callback demultiplexed by reader object ID to the correct per-topic subscriber callback.

### Wire format

The binary payload is a serialized `Envelope`:

```
[ROW_LEN:4 LE][ROW_DATA:ROW_LEN][ATTACH_COUNT:4 LE][attachments...]
```

This format is shared with the FastDDS provider, so payloads are wire-compatible between provider implementations.

Wire compatibility is necessary but not sufficient: DDS matches endpoints by **type name**, and the Fletcher row type's name carries the payload bound (`fletcher::FletcherTypeName`, e.g. `fletcher_65536`). `ProviderConfig::max_payload_bytes` therefore has to equal the `max_payload_bytes` of any FastDDS peer this client is meant to reach — otherwise the two never discover each other and no diagnostic says so. Both default to 64 KiB, and **0 means unset**, which resolves to exactly that (65536) - so a caller who leaves it alone gets the same type name this provider has always registered. The bound is a naming token on this side only: this provider writes variable-length envelopes and does not enforce it, so a row larger than the peer's bound reaches that peer and is refused by *its* preallocated payload pool — the peer reports `on_sample_rejected` / `on_sample_lost`, and the row never reaches Fletcher's own length check.

### Topic name

Topic segments are joined with `/`. Segments `{"integration", "TelemetryFeed", "TelemetryStream"}` produce the DDS topic name `"integration/TelemetryFeed/TelemetryStream"`.

### QoS

| Entity | Reliability | Durability | History |
|---|---|---|---|
| Data writer / reader | `RELIABLE` | `TRANSIENT_LOCAL` | `KEEP_ALL` depth 16 |
| Schema writer / reader | `RELIABLE` | `TRANSIENT_LOCAL` | `KEEP_LAST` depth 1 |

### Schema discovery

`CreateTopic` publishes serialized schema bytes to a companion `<topic>/__schema` DDS topic. When `Subscribe` is called before `CreateTopic` (subscriber-side), it polls the `__schema` topic for up to 5 seconds to retrieve the schema.

## Usage

```cpp
#include <fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp>
using namespace fletcher;

// Selected by NAME through the registry - the same call that resolves `inprocess` and
// `fastdds`, and the same call a runtime-loaded driver will arrive through later.
ProviderRegistry registry;
RegisterXrceProvider(registry);

ProviderConfig config;
config.domain_id = 145;                       // must match any DDS peer's domain
config.document  = "agent=192.168.1.10:2018\n"
                   "session_key=305419896\n"
                   "connect_timeout_ms=5000";
auto provider = registry.Create(ProviderSelector::Parse("xrce"), config);

// Or constructed directly, with the same configuration and no registry.
XrceDDSPubSubProvider direct(config);
```

There is **no typed XRCE options struct**. `XrceConfig` and `XrceTransport` were *retired* in
PDA-DEC-7 - not deprecated, no coexistence window - so there is one configuration path and one
path to test. Fletcher itself understands only `{max_payload_bytes, domain_id}`; everything
protocol-specific is a line in the document, which only this provider reads (spec §4.1/§4.2,
locked decision 8: Fletcher gains no parser and no configuration dependency).

### Configuration

`ProviderConfig`'s typed core:

| Field | Default | Meaning |
|---|---|---|
| `domain_id` | `0` | The DDS domain the Agent creates this client's participant on. `uint32_t` at the seam, `uint16_t` on the XRCE wire, so **above 65535 is refused, never narrowed** - a truncated domain id is a wrong answer with no error. |
| `max_payload_bytes` | `0` = unset -> `65536` | The row payload bound this client's DDS topics advertise; part of the registered type name (see above). Must satisfy `IsPayloadBound`. Write it as `kPayloadBytes<N>` to be told at compile time instead. |
| `document` | empty = all defaults | This provider's `key=value` document. |

#### The document: `key=value`, one setting per line

| Key | Values | Default |
|---|---|---|
| `transport` | `udp`, `tcp` (`serial` is nameable and refused as *unsupported*) | `udp` |
| `agent` | `HOST:PORT` - exactly one colon, port 1-65535 | `127.0.0.1:2018` |
| `session_key` | decimal `uint32`; must be unique per client on one Agent | `2864434397` |
| `connect_timeout_ms` | decimal 0-60000 | `3000` |

The address is **one** key, not two. Two would let a document name only the host and silently
keep port 2018 - a half-specified address, which is the kind of silence this shape exists to
remove. A key nobody mentions keeps its published default below.

`connect_timeout_ms` is coarse, and the rounding is stated rather than implied: the client does
not take a duration at all, it takes a **count of handshake attempts**, each of which costs up
to ~1000 ms. The budget is converted by rounding **up** to whole attempts - `1`-`1000` ms is one
attempt, `1001`-`2000` two, the default `3000` three, `60000` sixty - so no accepted value ever
buys zero attempts. `0` is the one budget that does: it sends one datagram and does not wait for
an answer, which means it can report a *reachable* Agent as a failure. That is what `0` is for
(a probe that must not block), and it is the only value in the range that cannot connect.

> Until PDA-DEC-7 fix cycle 1 this rounded **down**, so every budget from 1 to 1000 ms bought
> zero attempts and could never connect - to any Agent, however healthy - while the error
> message asked whether the Agent was running. Larger budgets were each one attempt short. Only
> `0` was ever tested, which is why the interior of the range is now a table
> (`XrceConfig.ConnectTimeoutBudgetBuysWholeAttempts`).

Tolerance is strict, and shares the in-process loopback’s rules (spec §4.1 is the one oracle
for both readers): `\n`-separated entries, a trailing `\r` stripped so a CRLF document means the
same thing everywhere, blank lines and a trailing newline skipped - and **nothing else trimmed**,
no case folding, no comments.

One rule is stronger than "nothing is trimmed", because that rule is weaker than it sounds:
**any byte below `0x21`, or a `0x7F` (DEL), inside an entry is refused** - a space, a tab, a
mid-entry CR, a DEL. So
` agent=x`, `agent =x`, `agent= 127.0.0.1:2018` and `agent=127.0.0.1:2018 ` are all refused by
that one rule, rather than by whichever later check happened to catch them (and `AGENT=x` and
`# a comment` are refused by the no-folding and no-comments rules above). A host with whitespace in it is not representable, so it cannot be handed to a resolver to
reject a layer down. The refusal is about bytes *inside* an entry and never about the separators
between entries: CRLF documents and blank lines are unaffected.

#### The published default document

This is what an empty document means, spelled out - the copy-paste starting point for a real
one. It cannot drift from the code: `XrceConfig.PublishedDefaultsAreExact` reads **this block,
out of this file, at run time** and compares what it parses to the provider's defaults
whole-struct, so editing either side alone turns that test red.

```
transport=udp
agent=127.0.0.1:2018
session_key=2864434397
connect_timeout_ms=3000
```

#### Refused, and all of it before any I/O

Every document refusal is a construction-time refusal, and structurally so: the document is
read to completion before a buffer is sized, before a socket exists and before a session is
created. No key here is topic-scoped, so unlike the Fast DDS provider this one defers nothing -
a constructed provider is one whose whole document has been read.

Each refusal is a `PubSubError` carrying a stable status (spec §5.1) and quoting the offending
entry:

- **`kInvalidArgument`** - an embedded NUL; an entry with no `=`; an unknown key; a duplicate
  key; an unknown value; a space or control byte anywhere inside an entry - below `0x21` or
  `0x7F` (` agent=x`, `agent =x`, `agent= x:2018`), though not `0x80`-`0xFF`, so a non-ASCII
  hostname stays representable and is the resolver's business; an `agent` without exactly one colon, with an empty host, or
  with a port outside 1-65535; a `connect_timeout_ms` above
  60000; a `session_key` above 4294967295; a `domain_id` above 65535; an unusable
  `max_payload_bytes`. Numbers are parsed wide and then range-checked per key, so no value is
  ever silently narrowed.
- **`kNotSupported`** - `transport=serial`. Nameable, and refused *distinctly* from a typo:
  "this build cannot do serial" is a different problem from a mistyped key.
- **`kTransportFailure`** - a transport that will not initialise, or an Agent that does not
  answer within `connect_timeout_ms`.

#### Settings that no longer exist

| Was | Now |
|---|---|
| `agent_ip` + `agent_port` | one `agent=HOST:PORT` line |
| `transport` (enum) | `transport=udp` / `tcp` |
| `session_key`, `connect_timeout_ms` | the keys of the same name |
| `payload_bound`, `domain_id` | `ProviderConfig`'s typed core |
| `max_payload` | **gone.** A documented 512-byte cap that capped nothing: no code in this repository ever read it. Nothing observable changes; what disappears is this README's old claim that it bounded anything. |
| `serial_device`, `serial_baudrate` | **gone.** Reachable only through a transport that refuses. |
| `stream_history`, `run_loop_ms` | **gone, and this one is a real narrowing.** They are now fixed at 4 and 10 ms - the values every caller in the tree already used. Nothing in the repository set either and no test could observe either, so a document key for one would have been a range-check with nothing behind it. If either is ever wanted it comes back **with** a test proving it took effect, which is the thing neither ever had. |

#### Handled, not forbidden

- **An unresolvable or unreachable host is a transport failure, not a document refusal.** The
  host is handed to the XRCE client unchanged, and Fletcher does not know what that client's
  resolver accepts - requiring IPv4 literals would refuse hostnames that work today. Still a
  construction-time failure, typed `kTransportFailure`.
- **IPv4 only.** The one-colon rule means `[::1]:2018` and `::1` are refused rather than
  half-accepted. Nothing is lost that ever worked - the client is initialised `UXR_IPv4` - but
  it is a deliberate foreclosure: IPv6 is a separate change that also has to move off
  `UXR_IPv4`.
- **An empty document means every published default**, including an Agent on
  `127.0.0.1:2018`. "The operator meant to configure this and forgot" is not a question this
  provider can answer; the gateway is where an empty configuration file is refused.
- **A `session_key` colliding with another client on one Agent.** Uniqueness is a property of
  that Agent's client population, which is not observable from here. Pre-existing.

### PubSubProvider interface

```cpp
provider.CreateTopic({"my", "topic"}, schema);
provider.Publish({"my", "topic"}, encoder);
auto result = provider.Subscribe({"my", "topic"}, [](const uint8_t* data, size_t len,
                                                      SharedSchema, Attachments) { ... });
provider.Unsubscribe({"my", "topic"});
```

### Constraints

- `CreateTopic` must be called before `Publish`. Calling it twice on the same topic throws.
- `Subscribe` can be called without a prior `CreateTopic` — it polls the `__schema` companion topic for up to 5 seconds.
- Only one subscription per topic per provider instance. Call `Unsubscribe` before re-subscribing.
- The subscription callback is invoked from the background run-loop thread. Shared state accessed from the callback must be protected externally.
- `XrceDDSPubSubProvider` is non-copyable and non-movable.
- `transport=serial` is refused with `PubSubError(kNotSupported)` - not implemented, and said distinctly from a typo.

## Runtime requirement

A running [MicroXRCEAgent](https://micro-xrce-dds.docs.eprosima.com/en/latest/agent.html) is required at the configured IP and port. Start one with:

```bash
MicroXRCEAgent udp4 -p 2018
```

The unit tests do **not** require an Agent - not one of them. The document is guarded by a pure
function, and the one guard that watches the *transport* brings its own socket.

> **Test duration note:** two cases here cost real wall clock; every other one is
> sub-millisecond.
>
> `XrceConfig.DocumentConfiguresTransport` - budget 3-6 s. It opens a listening socket on an
> **ephemeral** port, hands the provider `transport=tcp` plus that port, and asserts the
> connection arrives: the port is chosen at run time, so no build can hard-code its way past it.
> Its first row sets `connect_timeout_ms=0` (one datagram, no wait) and asserts the constructor
> fails *inside* 1000 ms, which is what would catch a constructor that ignored the operator’s
> budget and used its own. Its second row is a harness control on the defaults, so it pays the
> full default 3000 ms budget - three ~1000 ms attempts.
>
> `XrceConfig.AgentUnreachableIsATransportFailure` - ~1 s. It sets `connect_timeout_ms=1`, which
> is one whole attempt, so the handshake against the unused port 19999 is genuinely awaited and
> genuinely unanswered. At `0` it would not be awaited at all and the row could not witness
> unreachability.

## Building the package locally

### Windows (MSVC)

**Prerequisites:** Visual Studio 2022 with C++ workload, CMake, Python, Conan 2.

Conan profiles live in [`../.conan-profiles/`](../.conan-profiles/) in the repo and are referenced by relative path — no separate profile-install step is needed.

Build and package (Release, no tests):

```bat
conan create . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release
```

Build, run tests, and package:

```bat
conan create . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release -o "&:run_tests=True"
```

The built package lands in the local Conan cache (`%USERPROFILE%\.conan2`).

To iterate without the full `conan create` cycle use `conan build` against the source tree:

```bat
conan build . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release -o "&:run_tests=True"
```

Run tests with CTest after a `conan build` (Visual Studio is a multi-config generator so `-C` is required):

```bat
ctest --test-dir build -C Debug --output-on-failure
```

Add `-V` for full GTest output:

```bat
ctest --test-dir build -C Debug --output-on-failure -V
```

### Linux (devcontainer)

See the repo root's [Development environment](../README.md#development-environment) section for how to open the devcontainer (VS Code or manual Docker). Once inside, from this directory.

If the `build/` folder contains stale artifacts from a previous Windows build, remove it first — `DartConfiguration.tcl` bakes in absolute paths at configure time and will cause CTest to fail:

```bash
rm -rf build/
```

Build and run tests:

```bash
conan build . --build=missing -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release -o "&:run_tests=True"
```

Build, package, and run tests (equivalent to CI):

```bash
conan create . --build=missing -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release -o "&:run_tests=True"
```

Run tests with CTest after a `conan build` (the Linux build lives under `build/<BuildType>`):

```bash
ctest --test-dir build/Debug --output-on-failure
```

Add `-V` for full GTest output:

```bash
ctest --test-dir build/Debug --output-on-failure -V
```

## Consuming the package

### 1. Add to your conanfile.py

```python
def requirements(self):
    self.requires("fletcher-xrcedds-pubsub-provider/0.5.1-alpha")
```

Install dependencies:

```bash
conan install . --build=missing -pr:a=<your-profile>
```

### 2. Wire up CMake

```cmake
find_package(fletcher-xrcedds-pubsub-provider REQUIRED)

# Fully qualified target name:
target_link_libraries(my_app PRIVATE
    fletcher-xrcedds-pubsub-provider::fletcher-xrcedds-pubsub-provider)

# Or the convenience alias injected by the package's build module:
target_link_libraries(my_app PRIVATE fletcher::xrcedds-pubsub-provider)
```

Micro XRCE-DDS Client (v3.0.1) and Micro-CDR (v2.0.2) are built from source as part of this package via CMake `FetchContent`; consumers do not need to declare them separately.

## CI pipeline

The build workflow is defined in `.github/workflows/ci.xrcedds-pubsub-provider.yml`.
It is `workflow_call`-only — invoked from `ci.pr.yml` for pull requests
touching `xrcedds-pubsub-provider/**` and from `cd.xrcedds-pubsub-provider.yml`
on `xrcedds-pubsub-provider-v*` tag pushes. The matching upload job
lives in `cd.xrcedds-pubsub-provider.yml`, not here.

```
ci.pr.yml (PRs) / cd.xrcedds-pubsub-provider.yml (tag push)
        │
        ├──────────────────────────────────────┐
        ▼                                      ▼
build-windows                            build-linux
windows-2022                             ubuntu-latest
Native runner                            Docker container (.devcontainer)
Profile: Windows-msvc194-                Profile: Linux-gcc13-
         x86_64-Release                            x86_64-Release
        │                                      │
        └──────────────────┬───────────────────┘
                           │ both must pass
                           ▼ (only on tag push)
                        upload
              (cd.xrcedds-pubsub-provider.yml job)
              Creates GitHub Release with
              fletcher-xrcedds-pubsub-provider-{windows,linux}-conan-package.tgz
```

### Build profiles

| Job | Runner | Profile | Build type |
|---|---|---|---|
| `build-windows` | `windows-2022` | `.conan-profiles/Windows-msvc194-x86_64-Release` | Release |
| `build-linux` | `ubuntu-latest` (Docker) | `.conan-profiles/Linux-gcc13-x86_64-Release` | Release |

Both jobs build with `-o "&:run_tests=True"` so the full GTest suite runs as part of every CI build.

### Package handoff

Both platforms produce a separate binary package. Each build job saves
its package to a GitHub Actions workflow artifact; on a tag push the
`upload` job in `cd.xrcedds-pubsub-provider.yml` downloads both and
attaches them as GitHub Release assets:

```
conan cache save  →  actions/upload-artifact  →  actions/download-artifact  →  gh release create
```

The `upload` job only runs from `cd.xrcedds-pubsub-provider.yml`
(tag push), and verifies that the tag version matches the version in
`conanfile.py` before creating the release.
