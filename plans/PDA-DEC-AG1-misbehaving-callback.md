# PDA-DEC-AG1 — The misbehaving callback (A2 + A3 + A8)

## Summary

The seam gains one answer, identical on all three providers, to *"what happens when
a subscriber's delivery callback misbehaves?"* — an exception it lets escape is
absorbed at the dispatch site before any transport frame, and any seam call it makes
back into the **same provider instance on the same thread** is refused with the newly
allocated `kReentrantCall = 10`. Both are implemented once, in shared code, not three
times in three providers. Spec §5.3's frozen obligation is discharged and §6 gains a
sixth clause.

## Design

Two mechanisms, one thread-local, deliberately in different files so neither can
substitute for the other.

**1. The delivery frame** (`core/include/fletcher/core/internal/delivery_frame.hpp`,
new; `internal/` is not public surface — `segments.hpp` precedent). An inline
`thread_local std::vector<const void*>` of provider-identity tokens, an RAII scope
that pushes/pops one, and `bool InsideDeliveryOn(const void*)`. Tokens are compared by
address and **never dereferenced** — the same discipline `subscriber.cpp:36` already
documents and exercises.

**2. `DeliveryChannel`** (`pubsub/include/fletcher/pubsub/delivery_channel.hpp`, new;
public surface item 2). Move-only, constructed from a `SubscribeCallback` plus the
owning provider's identity token. Its **only** operation is

```cpp
void Deliver(const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) noexcept;
```

which pushes the frame, invokes the callback inside `try` / `catch (...)`, absorbs
whatever escapes, logs it, and pops. It exposes no accessor to the wrapped callback.
A provider stores a `DeliveryChannel` where it stores a `SubscribeCallback` today.
`noexcept` is load-bearing, not decoration: it is what makes an unwind across XRCE's C
frames (`xrce_dds_pubsub_provider.cpp:259-263` calls that path UB) a type property
rather than a comment.

**3. The entry guard.** `TranslateSeamFailure` — already the funnel every provider
entry point uses, 12 sites, and already published as provider-authoring API — takes a
new first parameter, the instance token:

```cpp
template <typename Fn> decltype(auto) TranslateSeamFailure(const void* instance, Fn&& fn);
```

Before running `fn` it asks `InsideDeliveryOn(instance)` and, if true, throws
`PubSubError(kReentrantCall, …)` **before any lock is taken**. The one-argument form
is **deleted**, so every existing site is a compile error until converted: the
conversion is proved by the compiler, not by a ledger in this document. Taking `const
void*` rather than `const PubSubProvider*` is what keeps `core` free of a dependency
on `pubsub`.

**Scope of the refusal, exactly.** Same thread **and** same instance. A call from
another thread while a delivery is in flight is not re-entrancy and still works —
that is A4's drain, and it must keep working. A callback that calls a **second**
provider instance is not re-entrancy either, and still works — that is the
multi-instance property PDA-DEC-8 proved. The scope is the object the guarantee is
about (owner ruling 2026-09-04, "fix it at the scope the guarantee is actually about").

**4. Fletcher's own teardown never triggers the refusal.**
`subscriber.cpp:588` and `~Subscriber` call `provider->Unsubscribe(...)`, and a
handler cancelling its own last subscription reaches that line from inside the
provider's delivery frame. Rather than catch the refusal, `Subscriber` **checks the
same predicate at the door** and skips the transport-level teardown, leaving
`provider_subscribed` true and the fan-out empty. No callback runs (A4's gates are
already retired), so A4's memory-safety promise is untouched; the cost is that the
transport-level subscription lives until the `Subscriber` is destroyed or that topic
is subscribed again. Published in `subscriber.hpp` beside A4's carve-out. Note the
predicate asked is the **provider-instance** one, not A4's per-`Subscriber` one: a
handler on subscriber X cancelling on subscriber Y over the same provider is the case
a per-`Subscriber` predicate would miss.

**Behaviour that changes, per provider.** Loopback: a re-entering callback is refused
instead of deadlocking on the non-recursive `mu` held across dispatch
(`in_process_provider.cpp:258-280`), and a throwing callback no longer reaches
`TranslateSeamFailure` at `:251` — so the `std::overflow_error → kPayloadTooLarge`
mapping can no longer charge a subscriber's bug to an unrelated publisher. XRCE: the
same, and a callback can no longer reach a publisher's `CreateTopic` through the
session pump at `:758`. Fast DDS: re-entrant `Unsubscribe` is refused instead of
self-waiting on reader deletion (`fast_dds_pubsub_provider.cpp:570-574`).

**What is not touched.** `std::recursive_mutex` in XRCE (`:168`) stays; the guard
removes its external justification but a mutex-type change re-enters the code A4 needed
four fix cycles to settle. `OrderedDelivery::draining_` stays: it also claims the drain
slot against a *second thread*, which §6 clause 2 permits and this guard must not
touch. What is deleted there is only the `catch (...) { …; throw; }` pairs, which
become unreachable once `Deliver` is `noexcept`.

**Spec amendments** (both inside the eight authorised by owner ruling 2026-09-03):
§5.3's task sentence becomes the rule; §6 gains clause 6 stating the refusal and its
exact scope; §7 clause 6's *"that question is open and is owned elsewhere"* pointer
(`spec:795-799`) is redirected to §6 clause 6. The `kReentrantCall` row lands in
`core/README.md` in the same change — `Taxonomy.PublishedNumbersMatchTheEnum`'s
exhaustive `switch` makes an unpublished append a **compile failure**, so that is
enforced, not requested (owner ruling 2026-09-03).

## Corner cases forbidden

Rung 1 — unrepresentable:
- **A subscriber's exception reaching a publisher's status.** `Deliver` is `noexcept`
  and catches strictly inside the dispatch site, so no callback frame ever reaches
  `TranslateSeamFailure`. No "whose frame threw" discrimination is added to
  `status.hpp` because no frame arrives to discriminate.
- **An unwind across a transport's C frames** (XRCE's documented UB / process
  termination). Same mechanism; enforced by a type instead of a comment.
- **An unguarded dispatch inside a provider.** A provider stores a `DeliveryChannel`,
  which has no accessor for the callback it wrapped.
- **Three providers, three behaviours.** One guard and one absorption, in shared code.
  A provider cannot express a local policy because there is nowhere to write one.
- **A refusal escaping Fletcher's own teardown path.** `Subscriber` checks the
  predicate before entering the provider, so the refusal is never raised by Fletcher's
  own code — only by an application's own re-entry.

Rung 2 — refused typed at the door, no recovery path, no partial mode:
- **Re-entry itself.** One refusal, `kReentrantCall`, before any lock. No queue-it,
  no serve-it-if-safe, no recursive-lock licence, no per-provider subset. A partial
  answer is precisely the three-way divergence A3 exists to end.
- **The unconverted entry point.** Deleting the one-argument `TranslateSeamFailure`
  makes an unguarded entry point a compile error at every site that exists today.

Handled residue — each with *why it could not be forbidden*:
- **An application may swallow the `kReentrantCall` it was thrown.** The refusal is a
  C++ exception handed to application code; the seam has no way to compel an
  application to observe it, and terminating instead would be worse than the loud
  failure the owner has preferred eight times.
- **A callback that never returns still blocks its delivering thread.** §7 clause 6
  already publishes that the seam cannot bound foreign callback duration
  (`spec:773`); bounding it here would be a new contract, not this item's.
- **A provider could copy the callback before wrapping it in a `DeliveryChannel`.**
  Closing this needs a non-virtual-interface split of `PubSubProvider`, which the
  runbook's stop-and-ask list makes a method-set change. Not taken here — see Risks;
  the conformance clauses bind any provider, and PDA-ABI's host-side adapter closes
  it by construction for loaded drivers.

## Premises and stop conditions

- **P1 — one frame stack per process.** Every Fletcher component is a STATIC library
  (`pubsub/CMakeLists.txt:11`; `fletcher-core` is INTERFACE) linked into one
  executable, so the inline `thread_local` has exactly one instance.
  **STOP-AND-ASK** if any component becomes a shared library, or if PDA-ABI places the
  dispatch adapter inside the driver DLL rather than host-side — do not paper over a
  forked thread-local with a registration handshake.
- **P2 — every entry point funnels through `TranslateSeamFailure`, and there is no
  thirteenth.** Four sites per provider, spot-checked on all three (Fast DDS
  `:288,:378,:428,:532`; XRCE `:633,:773,:828,:966`; loopback `:179,:251,:286,…`).
  Deleting the one-argument form is what turns "all of them" into a compiler fact
  rather than this list. **STOP-AND-ASK** if a path into provider state exists that
  does not funnel through it — the guard would be bypassable and the design needs the
  method-set (NVI) split instead.
- **P3 — the harness's `Unsubscribe` is a real re-entry.** `ProviderSubject::Unsubscribe`
  reaches the provider instance directly, on the calling thread, on local *and* peer
  subjects (`subject.hpp:120,140`). **STOP-AND-ASK** if any subject routes it through a
  queue or another thread: the clause would then test nothing and needs a different
  re-entrant verb.
- **P4 — A4's lock graph is settled and stays settled.** This item adds one predicate
  read in `Subscriber::Unsubscribe`, no new lock and no new edge. **STOP-AND-ASK** if
  the change turns out to need a new lock edge in `subscriber.cpp` — that is a scope
  question for the owner, not a local fix (owner ruling 2026-09-04, fix cycle 3).

## Forcing-test mapping

All four clauses live in `ProviderConformance` (`clauses.cpp`), so they run against
every subject — including both cross-process ones — with no per-provider assertions.
Per-clause ctest entries carry a **`TIMEOUT`**: a hang that is not capped is not a red
(PDA-DEC-A4 precedent). The runbook scope becomes the whole suite plus
`|conformance_xrce`, never the forcing test alone.

1. **`ProviderConformance.HostileCallbackNeitherEscapesNorIsChargedElsewhere`**
   *(forcing test)*. The callback re-enters `Unsubscribe` on this instance, records the
   status it catches, then throws `std::overflow_error`. Asserts: recorded status is
   `kReentrantCall`; the publishing call's `Reply` is `ok()`; a following unrelated
   `DeclareTopic`/`PublishRow` is `ok()`.
   *Red for the right reason today:* the loopback and Fast DDS **hang** (TIMEOUT), XRCE
   returns no status at all because it serves the call, and the loopback's publisher
   returns `kPayloadTooLarge` for a subscriber's throw. It cannot compile before
   `kReentrantCall` exists.
2. **`ProviderConformance.ReentrantCallIsRefusedWithoutAnyThrow`** *(live negative
   control for A3)*. The callback re-enters and does **not** throw. **This is how the
   charter constraint is met structurally:** there is no exception anywhere on this
   clause's path, so A2's absorption is not on it and cannot green it.
   *Red today:* hang on two providers, wrong/absent status on the third.
3. **`ProviderConformance.ThrowingCallbackIsAbsorbedWithoutReentering`** *(live
   negative control for A2)*. The callback throws and never re-enters, so the guard is
   not on this clause's path and cannot green it.
   *Red today:* the loopback charges the publisher; XRCE unwinds across C frames.
4. **`ProviderConformance.AnotherThreadIsNotRefusedDuringADelivery`** *(the
   not-too-wide control)*. A second thread calls the seam on the same instance while a
   delivery runs; it must succeed and must not see `kReentrantCall`.
   *Red if* the guard is ever widened from per-thread to per-instance.
5. **`CallerTier.CancelFromInsideDeliveryDoesNotEnterTheProvider`**. The probe provider
   records its `Unsubscribe` calls; a handler cancelling its own last subscription must
   produce none while its frame is live. *Red today:* `subscriber.cpp:588` calls
   straight through.

Clauses 2 and 3 are the pair the charter constraint asks for: each defect has a control
on which the *other* mechanism is absent, so neither can mask the other regardless of
landing order or implementer care.

## Risks / Unknowns

- **Charter constraint — satisfied, and by shape rather than by sequencing.** See
  clauses 2 and 3 above. No absorption ledger and no counter type is invented; the
  observation is made **inside the callback's own frame**, before any catch exists to
  hide it.
- **Size.** Prior amendments landed at 1.7–4× their estimate. Declared below. If the
  two DDS conversions blow the budget, the split is (i) enum + spec + guard +
  `DeliveryChannel` + loopback, (ii) Fast DDS and XRCE. **A2 and A3 must not be split
  from each other** — that is exactly the masking the charter forbids.
- **Rung 1 not taken on the entry side.** A non-virtual-interface split of
  `PubSubProvider` would make the guard unforgettable for any future provider, but the
  runbook lists a `PubSubProvider` method-set change as a stop-and-ask and §11 restates
  it as frozen. Not proposed here: PDA-ABI writes **one** host-side driver adapter, so
  every loaded driver inherits the guard without any method-set change. Recorded as a
  forward obligation on PDA-ABI, not as an open question here.
- **No coexistence window anywhere.** The one-argument `TranslateSeamFailure` is
  deleted in the same change, not overloaded; the per-provider dispatch catches are
  deleted, not left beside `DeliveryChannel`.
- **Behaviour narrowed, deliberately:** an XRCE application that cancels from inside a
  handler stops working (loudly); a transport-level subscription outlives a
  self-cancelling handler. Both are Stage-Brief decisions.
- **Assumed, not verified:** that `EPROSIMA_LOG_*` is not the only logging path
  available to `pubsub`/`core`. If there is no shared logging facility, `Deliver`
  absorbs silently and the controls still hold — a diagnostic, not a contract.

## Files-to-touch

- `core/include/fletcher/core/internal/delivery_frame.hpp` **(new)**
- `core/include/fletcher/core/status.hpp` · `core/README.md` ·
  `core/tests/test_status_taxonomy.cpp`
- `pubsub/include/fletcher/pubsub/delivery_channel.hpp` **(new)** ·
  `pubsub/src/delivery_channel.cpp` **(new)** · `pubsub/CMakeLists.txt`
- `pubsub/include/fletcher/pubsub/provider.hpp` ·
  `pubsub/include/fletcher/pubsub/subscriber.hpp` · `pubsub/src/in_process_provider.cpp` ·
  `pubsub/src/subscriber.cpp`
- `fastdds-pubsub-provider/src/fast_dds_pubsub_provider.cpp` ·
  `src/internal/data_reader_listener.hpp` · `src/internal/ordered_delivery.hpp` ·
  `tests/test_fast_dds_pubsub_provider.cpp` · `README.md`
- `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp` ·
  `tests/test_xrce_provider.cpp` · `README.md`
- `docs/pubsub-interface-spec.md` (§5.3, §6, §7 clause 6 pointer)
- `integration-tests/pubsub-conformance/src/clauses.cpp` · `src/caller_tier.cpp` ·
  `CMakeLists.txt` (per-clause entries + `TIMEOUT`) · `README.md`
- `.claude/runbook.PDA-DEC.config.md` (scope + rebaselined counts; local-only)

## Files-to-delete

- **`TranslateSeamFailure`'s one-argument form** (`status.hpp:132-156`) — replaced by
  the two-argument form. No overload, no window; the deletion is what makes the
  conversion mechanical.
- **The dispatch-site catches**: `data_reader_listener.hpp:46-56`'s
  `catch (const std::exception&)` / `catch (...)` and `ordered_delivery.hpp:97-103` and
  `:156-161`'s `catch (...) { …; throw; }` — replaced by `DeliveryChannel::Deliver`.
- **`XrceProviderTest.ReentrantUnsubscribeNoUseAfterFree`**
  (`test_xrce_provider.cpp:36`) — retired as written; behaviour deliberately changed.
  Replaced in place by a case asserting `kReentrantCall`, plus clauses 1–2 above.
- **The loopback's deadlock-by-design comments** (`in_process_provider.cpp:248`,
  `:270-275`) — no replacement: the statement stops being true.
- **XRCE's UB warning** (`xrce_dds_pubsub_provider.cpp:259-263`) narrows to a citation
  of the `noexcept` that now enforces it, rather than a rule addressed to a reader.

## Numbers

Declared net lines: **+520 / −100**. New public surface: **2 of 3** —
`PubSubStatus::kReentrantCall` (owner-allocated, mandatory) and `DeliveryChannel`;
`TranslateSeamFailure`'s new parameter is a change to an existing published item, and
`internal/delivery_frame.hpp` is `internal/`, which the runbook's budget note excludes.
