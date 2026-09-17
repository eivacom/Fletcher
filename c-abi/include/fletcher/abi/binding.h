// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The BINDING ABI — the pure C surface that language bindings call Fletcher
// through (round BIND, item BIND-1).
//
// Position: ABOVE the seam. Fletcher is the CALLEE here; an application, via a
// generated or hand-written binding, is the caller. That is the mirror image of
// the DRIVER ABI (`fletcher/abi/driver.h`, PDA-ABI), which sits BELOW the seam
// with Fletcher as the caller. The two headers are derived from the same seam
// specification and are deliberately NOT shared, NOT included by one another and
// NOT defined in terms of one another (seam §1, PDA-ABI decision 2, D-BIND-2').
// Any resemblance between the two is a consequence of one seam, not a coupling:
// each boundary CONSTRUCTS its own C form from the same C++ value, and no layout
// compatibility exists between them or with the C++ types (seam §3.2).
//
// Pure C99, self-contained, no dependency on any Fletcher C++ header: P/Invoke
// (and, next round, Rust's `extern "C"`) can see nothing else.
//
// ── How to read this header ─────────────────────────────────────────────────
// Every declaration below is derived from `docs/pubsub-interface-spec.md`, and
// the derivation is named in the comment above it. Where the spec states a rule
// normatively, this header restates the rule rather than pointing at it: a
// binding author must not have to read the C++ tree to use the C surface
// correctly. Where this header adds something the seam does not have (the
// `origin` field of `fl_error`, the codec surface's three steps) the comment
// says so and says why.
//
// ── Ownership, stated once ──────────────────────────────────────────────────
// Three words are used precisely throughout, and nothing else is implied:
//
//   BORROWED   Valid for the duration of the call only. The callee may read it
//              and must not retain it. A callee that wants to keep it copies the
//              bytes or takes its own reference. (Seam §3.2 clause 1, §3.5.)
//   OWNED      The caller receives a reference it must release through the named
//              function. Releasing twice, or releasing something never received,
//              is undefined.
//   OUT        Written only on success. On any failure the callee leaves the
//              object untouched, so a caller may reuse a variable across calls
//              without clearing it.
//
// A pointer this header does not qualify is BORROWED.
//
// ── Versioning and compatibility ────────────────────────────────────────────
// `fl_binding_abi_version()` returns the ABI version of the shim actually
// loaded, packed as (major << 16) | minor, so a binding compiled against
// FL_BINDING_ABI_VERSION can refuse a shim it does not understand rather than
// crash inside a struct whose layout moved.
//
// Within a major version this header is APPEND-ONLY: existing structs grow only
// at the end, existing functions never change signature, enumerators keep their
// numbers, and nothing is removed. A binding that reads a struct field by offset
// therefore keeps working against a later minor.
//
// **Before 1.0 there is NO compatibility guarantee at all.** The repository runs
// under a pre-1.0 exemption and this header inherits it: any declaration here
// may change shape or disappear between 0.x releases, and a binding is expected
// to be rebuilt against the shim it ships with. What the exemption does NOT
// license is a silent change — the version above is bumped, and the change is
// recorded in the round's decision log.
//
// **From 1.0 the deprecation policy is:** a declaration to be removed is marked
// deprecated in one MINOR release, keeps working unchanged for the whole of the
// major it was deprecated in, and is removed only in the next MAJOR. A status
// number is never renumbered, reordered, reused or removed at any version — the
// seam's own rule (`core/README.md`), inherited here because a number that has
// reached an application cannot be taken back.
//
// ── Status of the surface ───────────────────────────────────────────────────
// This header is a SPECIFICATION that the shim grows into. A declaration below
// may or may not be implemented by the shim you are holding, and the rule that
// makes that safe is:
//
//   A DECLARATION IS EXPORTED ONLY ONCE ITS ITEM LANDS. Nothing here is ever
//   stubbed out to return FL_NOT_SUPPORTED because it has not been written yet -
//   that status means "this build cannot do the thing", which is a different and
//   much more confusing statement than "not linked yet".
//
// So an unimplemented entry point is a LINK ERROR, not a run-time surprise, and
// `fl_binding_abi_version()` is the run-time handshake for everything else: it
// answers for the shim actually loaded, so a binding compiled against a different
// FL_BINDING_ABI_VERSION can refuse it rather than crash inside a struct whose
// layout moved.
//
// Which items have landed is deliberately NOT listed here. That inventory has one
// maintained home - the round's tracker, `plans/BIND-csharp-bindings.md` - and a
// second copy in a header nobody edits at an item boundary is a copy that goes
// stale silently. (It did: this paragraph claimed for a while that only
// `fl_binding_abi_version()` existed, some fifteen entry points after that
// stopped being true.)
#ifndef FLETCHER_C_ABI_INCLUDE_FLETCHER_ABI_BINDING_H_
#define FLETCHER_C_ABI_INCLUDE_FLETCHER_ABI_BINDING_H_

#include <stddef.h>
#include <stdint.h>

/* The Arrow C Data Interface structures, by forward declaration only.
 *
 * `ArrowSchema` and `ArrowArray` are defined by the Arrow project's `abi.h`, and
 * this header deliberately does not include it: a binding that never touches the
 * Arrow tier (a relay, a WAL writer) would otherwise inherit a dependency it has
 * no use for, and a binding that DOES touch it already has its own copy of those
 * definitions - `Apache.Arrow`'s `CArrowSchema`/`CArrowArray` in C#, `arrow2`'s
 * or `nanoarrow`'s in Rust. Only pointers to them cross this boundary, so an
 * incomplete type is sufficient and correct. A consumer that needs the
 * definitions includes Arrow's header itself, in either order. */
struct ArrowSchema;
struct ArrowArray;

/* Symbol visibility. FLETCHER_C_ABI_BUILDING is defined by the shim's own build
 * only (CMake PRIVATE define), so a consumer gets the import form on Windows and
 * a plain declaration elsewhere.
 *
 * What that does NOT do by itself is make the export table equal to the
 * declarations below, and the difference is worth stating because it is measured
 * rather than assumed:
 *
 *   LINUX - the property holds, and it holds because `c-abi/cmake/exports.map`
 *   says so at the link. Hidden visibility governs what our own translation units
 *   emit BY DEFAULT and `--exclude-libs ALL` localises what arrives from a static
 *   archive; neither reaches a symbol libstdc++ marks with EXPLICIT default
 *   visibility, and it marks several `shared_ptr` internals that way so RTTI
 *   works across a shared object. One `std::make_shared` inside the shim was
 *   enough to export six of them. The version script is what settles it; CI
 *   asserts the result on every run.
 *
 *   WINDOWS - the property does NOT hold, and cannot be made to from here.
 *   Nothing leaves a DLL without __declspec(dllexport), but ConanCenter's
 *   `fast-dds` static build is compiled with EPROSIMA_USER_DLL_EXPORT and its
 *   objects carry /EXPORT directives the linker honours when it pulls them into
 *   a DLL; a module-definition file does not suppress them (measured: 3531 names
 *   either way). Nothing resolves incorrectly because of it - Windows binds
 *   imports per module, so there is no ELF-style interposition and P/Invoke finds
 *   a function by name regardless - but the table is larger than this header, and
 *   a consumer inspecting it will see names that are not ours. Revisited at
 *   BIND-9 with the packed-size budget in hand.
 *
 * Either way, every function a binding may call is marked below and nothing that
 * is not marked is callable. */
#if defined(_WIN32)
#if defined(FLETCHER_C_ABI_BUILDING)
#define FL_ABI_EXPORT __declspec(dllexport)
#else
#define FL_ABI_EXPORT __declspec(dllimport)
#endif
#else
#if defined(FLETCHER_C_ABI_BUILDING)
#define FL_ABI_EXPORT __attribute__((visibility("default")))
#else
#define FL_ABI_EXPORT
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ══ Version ═══════════════════════════════════════════════════════════════ */

/* The version this header describes. Compare against the loaded shim's
 * fl_binding_abi_version() at load time; `Eiva.Fletcher.Interop` does exactly
 * that in its static constructor. */
#define FL_BINDING_ABI_VERSION_MAJOR 0
#define FL_BINDING_ABI_VERSION_MINOR 1
#define FL_BINDING_ABI_VERSION                                   \
    ((uint32_t)(((uint32_t)FL_BINDING_ABI_VERSION_MAJOR << 16) | \
                (uint32_t)FL_BINDING_ABI_VERSION_MINOR))

/* The ABI version of the loaded shim, packed as (major << 16) | minor.
 *
 * Callable before anything else and from any thread; it touches no state. */
FL_ABI_EXPORT uint32_t fl_binding_abi_version(void);

/* ══ Status ════════════════════════════════════════════════════════════════ */

/* The seam's status taxonomy, number for number.
 *
 * These are NOT this header's numbers to choose. They are published in
 * `core/README.md`, machine-compared against the C++ enum by
 * `Taxonomy.PublishedNumbersMatchTheEnum`, and are fixed integers appended only -
 * never renumbered, reordered, reused or removed. The C# enum is compared to the
 * same published table by the port of `test_status_taxonomy`, so three
 * enumerations agree by test rather than by care.
 *
 * Adding one is a stop-and-ask against the seam spec (§12); the owner allocates
 * the number and the table row lands in the same change. */
typedef enum fl_status {
    /* Success. Present because both C boundaries need a success value in the
     * same enum; the C++ `PubSubError` refuses to carry it, so a failed call
     * cannot be reported as a success. */
    FL_OK = 0,

    /* The caller passed something the seam refuses to interpret: an empty
     * topic-segment list, a blob with bytes and no owner, a negative timeout. */
    FL_INVALID_ARGUMENT = 1,

    /* A topic was re-declared with a provably different schema (seam §7
     * clause 3). */
    FL_SCHEMA_CONFLICT = 2,

    /* The topic has not been declared on this instance. */
    FL_TOPIC_NOT_DECLARED = 3,

    /* The encoded sample does not fit the transport's payload bound. Also the
     * normative mapping for a C++ `std::overflow_error` escaping a seam entry
     * point (seam §5.1). */
    FL_PAYLOAD_TOO_LARGE = 4,

    /* The transport refused or failed: an endpoint that would not be created, a
     * write that did not go out, a session that is gone. */
    FL_TRANSPORT_FAILURE = 5,

    /* This provider does not implement the requested behaviour. Distinct from
     * FL_REENTRANT_CALL, which says "not from there". */
    FL_NOT_SUPPORTED = 6,

    /* The total catch-all. Anything with no better home arrives here carrying
     * the original message - a taxonomy that lets an untyped failure through is
     * not one. */
    FL_INTERNAL = 7,

    /* A wait OUTCOME, never a failure: the answer is not available yet, within
     * the timeout that was asked for. See fl_schema_arrival_wait. */
    FL_PENDING = 8,

    /* A wait OUTCOME, never a failure: the answer will never arrive, because the
     * subscription that would have produced it is gone. */
    FL_SUBSCRIPTION_ENDED = 9,

    /* The caller re-entered the seam from inside a delivery callback, on the
     * same provider instance and the same thread, through a door that cannot
     * serve it there (seam §6 clause 6). */
    FL_REENTRANT_CALL = 10
} fl_status;

/* Which containment site produced an error (D-BIND-19 rule 1).
 *
 * This field is BIND's own and is not part of the seam's taxonomy - it adds no
 * status number and changes none. It exists because the number alone cannot
 * separate two things a caller must handle differently: an FL_INVALID_ARGUMENT
 * from the seam ("no such provider name", which the operator fixes in a
 * configuration file) and an FL_INVALID_ARGUMENT from the positional reader on a
 * truncated buffer (which is malformed data arriving from a peer). A binding
 * maps FL_ORIGIN_CODEC to its own typed format exception (D-BIND-15). */
typedef enum fl_origin {
    FL_ORIGIN_NONE = 0,    /* status == FL_OK */
    FL_ORIGIN_SEAM = 1,    /* a PubSubError, or a std::exception, from the seam */
    FL_ORIGIN_CODEC = 2,   /* malformed bytes: the positional reader, envelope parsing */
    FL_ORIGIN_CALLBACK = 3 /* a caller-supplied writer or handler reported failure */
} fl_origin;

/* A failure, carried by value to the caller and owned by it.
 *
 * NEVER a global or thread-local slot: the seam forbids one (§5.1), several
 * provider instances share a process, and a per-thread slot cannot serve a
 * delivery arriving on a transport's own thread. Every fallible entry point in
 * this header therefore takes an `fl_error*` OUT parameter.
 *
 * Both the number and the message always cross. The number alone is not enough -
 * FL_INVALID_ARGUMENT is shared by many refusals, and a registry refusal that
 * lists every registered provider is the difference between a five-second fix
 * and an afternoon.
 *
 * `message` is UTF-8 bytes plus a length and is NOT NUL-terminated; it may
 * contain no zero byte, because the C++ side rewrites each zero byte as the four
 * characters \x00 before the message reaches any boundary. Read
 * [message, message + message_len). It is heap-allocated by the shim: a fixed
 * inline buffer was rejected because the messages worth carrying are the long
 * ones.
 *
 * Zero-initialise before the call (`fl_error err = {0};`). On FL_OK the callee
 * leaves it untouched. On failure the caller owns `message` and releases it with
 * fl_error_dispose, which is why every binding's throw site is a `finally`.
 *
 * ── The other direction: an fl_error a CALLBACK fills (D-BIND-32) ───────────
 * Everything above describes the SHIM filling an error for the caller. The
 * reverse also happens: fl_grow_fn is handed an fl_error* and fills it when it
 * refuses. Ownership there is the opposite way round, and rather than a second
 * rule to remember it is the same BORROW discipline fl_str uses everywhere else
 * in this header:
 *
 *   A CALLBACK THAT SETS `message` KEEPS THOSE BYTES ALIVE UNTIL IT RETURNS.
 *   The shim copies what it needs before the callback's frame is gone and frees
 *   NOTHING. fl_error_dispose is for errors the shim filled; a callback never
 *   calls it on one of its own.
 *
 * The alternative - the shim freeing what a binding allocated - is rejected
 * because the two are one heap only while both sides share a C runtime, and this
 * shim links the MSVC CRT statically. Static storage, or a buffer the binding
 * owns for the duration of the call, both satisfy the rule and neither
 * allocates. The cost is one copy, on a path that is already failing. */
typedef struct fl_error {
    int32_t status;     /* an fl_status value */
    int32_t origin;     /* an fl_origin value */
    uint8_t* message;   /* OWNED by the caller; NULL when status == FL_OK */
    size_t message_len; /* length in bytes; 0 when message is NULL */
} fl_error;

/* Release the message an fl_error carries and zero the struct.
 *
 * Safe on a zeroed struct and safe to call twice. Never throws, never re-enters
 * the seam, callable from any thread - including from inside a delivery, where a
 * binding's containment site runs. */
FL_ABI_EXPORT void fl_error_dispose(fl_error* err);

/* ══ Strings, topics, and the single-copy marker ═══════════════════════════ */

/* Bytes plus a length, BORROWED for the duration of the call.
 *
 * The seam's C idiom for every string-shaped thing (§3.5): the LENGTH is
 * authoritative, never a terminator. A NUL-terminated form is refused wherever
 * the seam refuses embedded zero bytes, because a binding that marshalled a
 * length-carrying value as a C string would truncate it silently and two
 * distinct values would become one abroad.
 *
 * `data` may be NULL only when `len` is 0. */
typedef struct fl_str {
    const uint8_t* data;
    size_t len;
} fl_str;

/* A topic, as an ordered list of segments, BORROWED for the duration of the call
 * (seam §3.5). A callee that keeps a segment copies its bytes.
 *
 * The seam - not the provider, and not this boundary - joins the segments with
 * '/' and that joined name IS the topic's identity. Six refusals apply to every
 * entry point that takes one, each FL_INVALID_ARGUMENT, with no default topic,
 * no recovery and no partial mode:
 *
 *   1. an empty segment list: it names no topic;
 *   2. a segment containing a zero byte: the name would not reach the wire whole;
 *   3. a segment containing '/': the joined name would not split back to the
 *      list it came from, so {"a/b"} and {"a","b"} would be one topic;
 *   4. an empty segment: {""} reproduces the name rule 1 forbids;
 *   5. a segment beginning "__": the reserved namespace companion names live in
 *      (both DDS providers derive name + "/__schema");
 *   6. a joined name longer than 246 BYTES - 255 less the 9 bytes of "/__schema",
 *      because Fast DDS announces a topic as a fixed 255-byte string and
 *      truncates silently above it.
 *
 * Every bound here is in BYTES, never characters or UTF-16 code units
 * (D-BIND-20). A binding is expected to validate these rules on its own side as
 * well, so the refusal carries its caller's stack trace; the shim re-validates,
 * and one table-driven test proves both sides refuse the same inputs. */
typedef struct fl_topic {
    const fl_str* segments;
    size_t count;
} fl_topic;

/* An owned, immutable list of byte strings returned by the shim.
 *
 * Used by fl_publisher_list_topics, where the count is not known to the caller
 * in advance. The strings it lends out are BORROWED from the list and die with
 * it. Release with fl_string_list_dispose. */
typedef struct fl_string_list fl_string_list;

FL_ABI_EXPORT size_t fl_string_list_size(const fl_string_list* list);
FL_ABI_EXPORT fl_str fl_string_list_at(const fl_string_list* list, size_t index);
FL_ABI_EXPORT void fl_string_list_dispose(fl_string_list* list);

/* The single-copy marker (D-BIND-17).
 *
 * Fletcher's re-entrancy machinery is an `inline thread_local` in a header-only
 * target, so every separately linked binary carrying Fletcher gets its OWN
 * delivery-frame stack and the doors stop seeing each other's frames - silently,
 * with no symptom until a re-entrant call is served that should have been
 * refused. The shim is therefore one shared library that statically links every
 * Fletcher component it uses, and a process must contain exactly one of it.
 *
 * At load the shim enumerates the process's modules (EnumProcessModules /
 * dl_iterate_phdr) looking for a second export of this symbol, and REFUSES TO
 * INITIALISE if it finds one, naming both modules. Two shims in one process is
 * the detectable case and the likely one.
 *
 * The undetectable case remains: a C++ host that statically links Fletcher and
 * also hosts a language runtime. That is ruled out by fact rather than by check
 * (D-BIND-17, Q11) - the consumers are managed processes hosting this binding
 * only - and a future host of that shape is a stop-and-ask, not a bug report.
 *
 * Returns a NUL-terminated identification string for diagnostics. A binding does
 * not need to call it; the shim's own loader does. */
FL_ABI_EXPORT const char* fl_single_copy_marker(void);

/* ══ Shared ownership: blobs and schemas ═══════════════════════════════════ */

/* Bytes plus what keeps them alive (seam §3.2).
 *
 * The C form of shared ownership is a handle plus retain/release, and naming the
 * bytes separately from their owner is what lets the seam carry memory Fletcher
 * did not allocate - a transport's loaned sample, say - without copying it.
 *
 * `owner` is opaque: it is NOT a pointer to the bytes, not a reference count a
 * binding may read, and not layout-compatible with the C++ `shared_ptr` it is
 * constructed from. Only fl_blob_retain / fl_blob_release may be applied to it.
 *
 * Rules, all from the seam and all normative here:
 *   1. A blob crossing as an argument is BORROWED for the call. A callee that
 *      keeps it calls fl_blob_retain and later fl_blob_release.
 *   2. The bytes are IMMUTABLE once they cross.
 *   3. Retain and release are safe from any thread, concurrently.
 *   4. Release never fails and never re-enters the seam.
 *   5. Empty is `{NULL, NULL, 0}` and Fletcher ENFORCES it: a zero-size blob
 *      normalises its data pointer to NULL however it was built, so a binding
 *      may test `size == 0` and `data == NULL` interchangeably. An empty blob
 *      needs no owner - there is no byte to keep alive.
 *
 * There is deliberately no view-only form: a non-NULL `data` with a NULL `owner`
 * is refused with FL_INVALID_ARGUMENT, which is what makes rule 1 exactly true
 * rather than mostly true. */
typedef struct fl_blob {
    void* owner;         /* opaque; NULL only when size == 0 */
    const uint8_t* data; /* NULL when size == 0 */
    size_t size;
} fl_blob;

/* Take a reference. Safe from any thread; a no-op on an empty blob. */
FL_ABI_EXPORT void fl_blob_retain(const fl_blob* blob);

/* Drop a reference. Never fails, never re-enters the seam, safe from any thread,
 * a no-op on an empty blob. Releasing a reference never taken is undefined. */
FL_ABI_EXPORT void fl_blob_release(const fl_blob* blob);

/* A schema and what keeps it alive (seam §3.4).
 *
 * `schema` points at an Arrow C Data Interface structure the shim owns and
 * SHARES. Releasing is done through `owner`, with fl_schema_release.
 *
 * A binding must NEVER call the Arrow C Data Interface's own `release` callback
 * on this structure. That callback destroys the schema for every other holder,
 * including the provider that is still delivering on it, and the failure appears
 * later and elsewhere. This is the single most expensive mistake available at
 * this boundary, which is why the type carries an owner rather than being a bare
 * `ArrowSchema*`.
 *
 * A binding that wants an Arrow schema of its own imports a COPY - in C#,
 * `CArrowSchemaImporter.ImportSchema` over a deep copy the shim provides - and
 * releases this handle. */
typedef struct fl_schema {
    void* owner;                      /* opaque; NULL when schema is NULL */
    const struct ArrowSchema* schema; /* BORROWED from the owner; never released directly */
} fl_schema;

/* Drop a reference to a shared schema. Never fails; a no-op when `owner` is
 * NULL. Safe from any thread, including from inside a delivery. */
FL_ABI_EXPORT void fl_schema_release(const fl_schema* schema);

/* ══ The schema arrival ════════════════════════════════════════════════════ */

/* A waitable handle for a topic's schema (seam §3.4).
 *
 * The seam has exactly one waiting mechanism and this is its C form. Nothing
 * resembling a future crosses this boundary - not a `std::shared_future`, not a
 * promise, not a callback that fires once (D-BIND-22). A binding maps this onto
 * its own idiom: C# returns a `SchemaWaitResult` value from a blocking `Wait`
 * and offers an async helper above it.
 *
 * Copyable in C++; here, a handle the caller owns and disposes. */
typedef struct fl_schema_arrival fl_schema_arrival;

/* Wait for the schema, up to `timeout_ms`.
 *
 * FIVE outcomes, and two of them are values rather than failures. Getting this
 * distinction wrong is the seam's named failure mode - silent wrong-slot
 * decoding - so it is spelled out:
 *
 *   FL_OK, *out non-NULL      the schema arrived. `*out` is a NEW REFERENCE the
 *                             caller releases with fl_schema_release.
 *   FL_OK, *out all-NULL      RESERVED: this transport carries no schemas at all
 *                             (seam §7 clause 1). NOT an error, NOT "not yet",
 *                             and there is nothing to release. A subscriber that
 *                             treats this as a failure refuses to work on a
 *                             transport that is behaving correctly.
 *   FL_PENDING                not yet, within the timeout asked for. `*out` is
 *                             untouched. Ask again.
 *   FL_SUBSCRIPTION_ENDED     no schema will ever arrive: the subscription that
 *                             would have produced it is gone. `*out` untouched.
 *                             Terminal - asking again cannot change it.
 *   anything else             the provider failed to produce one; `err` carries
 *                             the number and message, `*out` is untouched.
 *
 * `timeout_ms` is milliseconds. Negative is REFUSED with FL_INVALID_ARGUMENT, so
 * that "negative means forever" cannot be invented by one binding and not
 * another; INT64_MAX is the unbounded form and is implemented as a deadline-free
 * wait. A binding whose own "infinite" is -1 (C#'s Timeout.Infinite) maps it to
 * INT64_MAX BEFORE the call and refuses every other negative in its own code
 * (D-BIND-20). Spelling "forever" as a large finite number is not equivalent: a
 * deadline computed as now + timeout overflows well below INT64_MAX and would
 * return immediately, turning a wait into a poll.
 *
 * Never blocks the transport: waiting here does not hold a lock a delivery
 * needs. Safe to call from any thread, and from more than one concurrently. */
FL_ABI_EXPORT fl_status fl_schema_arrival_wait(fl_schema_arrival* arrival, int64_t timeout_ms,
                                               fl_schema* out, fl_error* err);

/* Release the arrival handle. Does not cancel the subscription behind it and
 * does not release a schema already handed out. Never fails. */
FL_ABI_EXPORT void fl_schema_arrival_dispose(fl_schema_arrival* arrival);

/* ══ Attachments ═══════════════════════════════════════════════════════════ */

/* A sealed, ordered set of key/blob entries (seam §3.2, clauses A1-A3).
 *
 * BORROWED wherever it appears as an argument, including in a delivery callback.
 *
 * A1 - ENUMERATION IS POSITIONAL. size/key_at/value_at are the enumeration, and
 * there is no iterator, no map API and no second mechanism.
 *
 * A2 - THE ORDER IS PART OF THE VALUE: ascending unsigned-byte order of the key
 * bytes, which is the order the entries are written to the wire in. A BINDING
 * NEVER SORTS. It reads the sequence Fletcher publishes. This matters more than
 * it looks: C#'s ordinal string comparison is UTF-16 code-unit order, which
 * disagrees with byte order (U+FF5E sorts before U+10000 in one and after it in
 * the other), so a binding that "helpfully" re-sorted would publish a different
 * wire order than the C++ side for the same value. Decoders match by key, never
 * by position.
 *
 * A3 - A KEY CONTAINING A ZERO BYTE IS REFUSED, both when attached and on
 * arrival. A binding marshalling a key as a NUL-terminated string would truncate
 * it silently and two distinct keys would become one. Keys are UTF-8 by
 * convention only: nothing validates it, and the order is over bytes precisely
 * so that no boundary has to reproduce a collation. */
typedef struct fl_attachments fl_attachments;

/* The number of entries. */
FL_ABI_EXPORT size_t fl_attachments_size(const fl_attachments* atts);

/* The key at `index`, BORROWED from the set. Out of range yields {NULL, 0}. */
FL_ABI_EXPORT fl_str fl_attachments_key_at(const fl_attachments* atts, size_t index);

/* The value at `index`, BORROWED from the set: the returned blob is valid for as
 * long as the set is, and a caller that keeps the bytes past that calls
 * fl_blob_retain first. Out of range yields an empty blob. */
FL_ABI_EXPORT fl_blob fl_attachments_value_at(const fl_attachments* atts, size_t index);

/* Find by key. Returns 1 and writes `out` when present, 0 when absent; `out` is
 * BORROWED exactly as value_at's result is. Absence is not a failure and carries
 * no error. */
FL_ABI_EXPORT int fl_attachments_find(const fl_attachments* atts, fl_str key, fl_blob* out);

/* The write end: build a set, then seal it.
 *
 * A builder exists because the set is sealed and ordered - a binding cannot
 * construct one field by field and cannot insert into a published one. The
 * builder sorts into the published order on `build`, which is the ONLY place
 * sorting happens at this boundary. */
typedef struct fl_attachments_builder fl_attachments_builder;

FL_ABI_EXPORT fl_status fl_attachments_builder_create(fl_attachments_builder** out, fl_error* err);

/* Add or replace an entry. `key` is copied; `value` is RETAINED by the builder,
 * so the caller may release its own reference immediately afterwards. A key
 * containing a zero byte is refused with FL_INVALID_ARGUMENT (A3). */
FL_ABI_EXPORT fl_status fl_attachments_builder_set(fl_attachments_builder* builder, fl_str key,
                                                   const fl_blob* value, fl_error* err);

/* Seal the builder into an attachments set the caller OWNS and disposes. The
 * builder is left empty and reusable. */
FL_ABI_EXPORT fl_status fl_attachments_builder_build(fl_attachments_builder* builder,
                                                     fl_attachments** out, fl_error* err);

FL_ABI_EXPORT void fl_attachments_builder_dispose(fl_attachments_builder* builder);

/* Release an attachments set the caller owns. Never fails; releases the
 * references it holds on its values. */
FL_ABI_EXPORT void fl_attachments_dispose(fl_attachments* atts);

/* ══ The write window ══════════════════════════════════════════════════════ */

/* The C form of the seam's `WriteBuffer` (§3.1): a window plus a refill hook.
 *
 * A WINDOW MEANS ONE CROSSING PER REFILL, NOT ONE PER APPEND. That is the whole
 * reason this type exists rather than an append-a-byte function.
 *
 * Normative properties a binding must honour when it writes into one:
 *   1. RANDOM ACCESS, NOT A STREAM. Fletcher back-patches length prefixes and
 *      null bitfields at offsets below `pos`, so bytes already written must not
 *      move except inside a refill, which preserves them verbatim.
 *   2. BOUNDS BY SUBTRACTION, NEVER ADDITION (`len > capacity - pos`), so a
 *      hostile length cannot wrap.
 *   3. `data` is readable over [data, data + pos) only, and any refilling write
 *      invalidates it.
 *   4. A fixed-capacity window reports overflow as FL_PAYLOAD_TOO_LARGE; a
 *      growable one refills through `grow`.
 *
 * `grow` asks for room for at least `min_bytes` more. It returns FL_OK having
 * updated `data`, `capacity` and preserved `pos` and the bytes below it, or a
 * failure status having changed nothing. A window with a NULL `grow` is
 * fixed-capacity.
 *
 * A `grow` that fills `err` BORROWS its message to the shim, which copies it and
 * frees nothing - see fl_error (D-BIND-32). */
typedef struct fl_write_window fl_write_window;

typedef fl_status (*fl_grow_fn)(fl_write_window* window, size_t min_bytes, fl_error* err);

struct fl_write_window {
    void* ctx;       /* the producer's own state; Fletcher never reads it */
    uint8_t* data;   /* the window base */
    size_t capacity; /* bytes available at `data` */
    size_t pos;      /* bytes already written; the write cursor is data + pos */
    fl_grow_fn grow; /* NULL for a fixed-capacity window */
};

/* A producer that writes into the window's own cursor for exactly one call
 * (seam §3.1 clause 6, PDA-DEC-A1).
 *
 * This is how a binding puts bytes into the transport's buffer without having
 * them anywhere else first - without it, every foreign-language row costs one
 * whole-row copy that the seam's zero-copy property says need not exist.
 *
 *   `dst`   is the window's own write cursor, BORROWED FOR THIS CALL ONLY.
 *           Storing it past the return is a use-after-free.
 *   `room`  is the WHOLE remaining window and is always at least the `min_bytes`
 *           that was asked for, so a variable-length producer never needs a
 *           second crossing to ask how much is left.
 *   returns the number of bytes actually written.
 *
 * Note what the writer does NOT receive: a handle to the buffer. Foreign code
 * cannot re-enter the window at all.
 *
 * COMMIT BY RETURN: the position advances by exactly the returned count, never
 * by `min_bytes`, in one assignment after validation - there is no partial
 * commit to observe or unwind.
 *
 * Two refusals, both FL_INVALID_ARGUMENT and both committing nothing: a
 * `min_bytes` of 0, and a writer reporting more than `room`. A nested fill is
 * the same refusal. Room that cannot be produced is FL_PAYLOAD_TOO_LARGE and the
 * writer is not invoked at all.
 *
 * DISCLOSED RESIDUE: the count is trusted against `room`, not against what was
 * actually touched, so bytes in [written, reported) are committed and published
 * having been written by nobody. That is inherent in handing over real memory,
 * and it is disclosed rather than checked away.
 *
 * A writer must not throw across this boundary. A binding's writer thunk catches
 * everything, records the exception on its own side and returns 0; the shim then
 * reports FL_ORIGIN_CALLBACK and the binding rethrows the original exception
 * with its own stack (D-BIND-19 rule 3). */
typedef size_t (*fl_writer_fn)(void* ctx, uint8_t* dst, size_t room);

/* ══ Provider, publisher, subscriber ═══════════════════════════════════════ */

typedef struct fl_provider fl_provider;
typedef struct fl_publisher fl_publisher;
typedef struct fl_subscriber fl_subscriber;

/* The typed core of a provider's configuration (seam §4.1).
 *
 * Exactly two typed fields, plus bytes Fletcher transports and does not read.
 * Widening the typed core is a stop-and-ask against the seam, not an edit.
 *
 * `max_payload_bytes` 0 means UNSET and the provider's own default applies.
 * `domain_id` is the transport's domain identity, in whatever that transport
 * calls a domain; a provider on a narrower wire REFUSES a value it cannot carry
 * rather than narrowing it (the XRCE provider refuses above 65535).
 * `document` is the protocol's own settings, in that protocol's own syntax.
 * Fletcher parses none of it - explicitly a non-goal (seam §4.2) - and hands it
 * to the provider verbatim. */
typedef struct fl_provider_config {
    uint32_t max_payload_bytes;
    uint32_t domain_id;
    fl_str document;
} fl_provider_config;

/* Create a provider from a selector and a configuration (seam §4).
 *
 * `selector` is one string exactly as an operator wrote it, and its
 * classification is TOTAL AND DISJOINT: a non-empty string of [A-Za-z0-9_-] is a
 * NAME; every other non-empty string is a PATH. The rule does not consult the
 * registry, so a given string means the same thing in every build. An empty
 * string, or one containing a zero byte, is refused with FL_INVALID_ARGUMENT.
 *
 * The shim registers its own built-ins - `inprocess`, `fastdds` and `xrce`,
 * statically linked - and a binding can neither register a provider nor install
 * a path resolver (D-BIND-24). There is deliberately no fl_registry_register and
 * no fl_registry_set_path_resolver in this header: a managed provider would have
 * to satisfy the seam's threading and re-entrancy contract from the other side
 * of two boundaries, and nothing asks for it.
 *
 * A PATH selector is therefore answered with FL_NOT_SUPPORTED until PDA-ABI
 * fills the resolver seat. That refusal is the honest one: the string is
 * well-formed and names a driver this build cannot load.
 *
 * The returned provider is OWNED by the caller. Destroying it while a publisher,
 * subscriber or delivery still refers to it is undefined - the seam's lifetime
 * rule (§6) is that a provider outlives everything built on it. */
FL_ABI_EXPORT fl_status fl_provider_create(fl_str selector, const fl_provider_config* config,
                                           fl_provider** out, fl_error* err);

/* Destroy a provider. Quiescence is the caller's obligation: no delivery may be
 * in flight and no publisher or subscriber may still hold it. Never fails. */
FL_ABI_EXPORT void fl_provider_destroy(fl_provider* provider);

/* ── Publisher ──────────────────────────────────────────────────────────── */

/* Create a publisher over a provider. The provider is BORROWED and must outlive
 * the publisher. */
FL_ABI_EXPORT fl_status fl_publisher_create(fl_provider* provider, fl_publisher** out,
                                            fl_error* err);

FL_ABI_EXPORT void fl_publisher_destroy(fl_publisher* publisher);

/* Declare a topic and its schema.
 *
 * `schema` is BORROWED and deep-copied by the shim, so a binding may release its
 * Arrow export as soon as this returns. Re-declaring a topic with an IDENTICAL
 * schema is idempotent, so several publishers may share a topic; re-declaring it
 * with a conflicting one is refused with FL_SCHEMA_CONFLICT (seam §7 clause 3 -
 * a provider that silently overwrote the declared schema is non-conforming).
 *
 * Subscribers do not call this. They learn the schema through the arrival their
 * subscription returns. */
FL_ABI_EXPORT fl_status fl_publisher_create_topic(fl_publisher* publisher, fl_topic topic,
                                                  const struct ArrowSchema* schema, fl_error* err);

/* The topics this publisher has declared, as an OWNED list the caller disposes. */
FL_ABI_EXPORT fl_status fl_publisher_list_topics(const fl_publisher* publisher,
                                                 fl_string_list** out, fl_error* err);

/* Publish bytes a producer writes directly into the transport's window.
 *
 * The raw route, for a binding that has its own encoder or is relaying bytes it
 * already holds: the shim calls `writer` once with the window's cursor, and the
 * row's bytes come to exist inside the transport buffer rather than being copied
 * into it. `min_bytes` is the producer's lower bound on what it needs.
 *
 * `atts` is BORROWED and may be NULL for none. */
FL_ABI_EXPORT fl_status fl_publisher_publish_raw(fl_publisher* publisher, fl_topic topic,
                                                 fl_writer_fn writer, void* ctx, size_t min_bytes,
                                                 const fl_attachments* atts, fl_error* err);

/* ── Subscriber ─────────────────────────────────────────────────────────── */

/* Create a subscriber over a provider. The provider is BORROWED and must outlive
 * the subscriber. */
FL_ABI_EXPORT fl_status fl_subscriber_create(fl_provider* provider, fl_subscriber** out,
                                             fl_error* err);

/* Destroy a subscriber.
 *
 * DESTRUCTION REQUIRES QUIESCENCE, and from inside a delivery on this same
 * subscriber it is not merely refused - the seam's answer is process
 * termination, by design, because the alternative is to leak the transport
 * subscription silently. A binding must refuse this call in its own code, with
 * its own exception, before it can reach the shim (D-BIND-18). */
FL_ABI_EXPORT void fl_subscriber_destroy(fl_subscriber* subscriber);

/* Delivery: raw encoded row bytes, the topic's schema, and any attachments.
 *
 * EVERY POINTER IS BORROWED FOR THE DURATION OF THE CALL - `data`, `schema` and
 * `atts` alike. A handler that keeps the bytes copies them; one that keeps the
 * schema retains it; one that keeps an attachment's blob retains that.
 *
 * Invoked on the transport's own thread. The seam's rules bind here:
 *   * A HANDLER MUST NOT THROW ACROSS THIS BOUNDARY. A binding's thunk catches
 *     everything, counts it and raises its own diagnostic; an exception escaping
 *     into a transport's C frames is undefined behaviour on MSVC and process
 *     termination in practice (seam §5.3).
 *   * Re-entering the seam on this same provider and thread is refused with
 *     FL_REENTRANT_CALL, before any lock (seam §6 clause 6). The sanctioned route
 *     for work that must touch the seam is to defer it past the handler's return.
 *   * `subscription_id` identifies which subscription this delivery belongs to,
 *     so a binding can correlate without racing the subscribe call's return.
 *
 * `ctx` is whatever was passed to fl_subscriber_subscribe. Its lifetime is the
 * binding's problem, and it is the reason the seam guarantees no invocation
 * BEGINS after unsubscribe returns: that is what makes an in-flight counter
 * sufficient to free it (D-BIND-18). */
typedef void (*fl_delivery_fn)(void* ctx, uint64_t subscription_id, const uint8_t* data, size_t len,
                               const fl_schema* schema, const fl_attachments* atts);

/* Subscribe to a topic. NEVER BLOCKS.
 *
 * A subscriber may subscribe before any publisher exists, which is why the
 * schema comes back as a waitable arrival rather than as a schema. `out_id`
 * receives the subscription id; `out_arrival` receives an arrival handle the
 * caller OWNS and disposes. Both are written only on success.
 *
 * Several subscriptions to one topic share one provider subscription, so they
 * observe ONE arrival. */
FL_ABI_EXPORT fl_status fl_subscriber_subscribe(fl_subscriber* subscriber, fl_topic topic,
                                                fl_delivery_fn on_delivery, void* ctx,
                                                uint64_t* out_id, fl_schema_arrival** out_arrival,
                                                fl_error* err);

/* Cancel a subscription.
 *
 * On return, NO FURTHER DELIVERY BEGINS for this id - that guarantee is what
 * lets a binding free the context once the in-flight count reaches zero. A
 * delivery already running may still be running; the counter, not this call,
 * says when the last one has left.
 *
 * Cancelling from inside a delivery is served rather than refused, as a
 * documented carve-out, and cancelling a sibling subscription running on another
 * thread does not wait for it. */
FL_ABI_EXPORT fl_status fl_subscriber_unsubscribe(fl_subscriber* subscriber,
                                                  uint64_t subscription_id, fl_error* err);

/* Watch a topic's schema without subscribing to its data.
 *
 * Declared here, and answering FL_NOT_SUPPORTED, until the seam grows the
 * SubscribeSchema/UnsubscribeSchema pair currently in flight (D-BIND-29). The
 * pair is declared now rather than later because this header is reviewed as a
 * specification and a surface reviewed with a known hole in it gets reviewed
 * twice; the pre-1.0 exemption covers the shape changing if the seam's does.
 *
 * When implemented: never blocks, resolves when a publisher announces the topic,
 * and answers with the same arrival a subscribe on that topic would. A watch
 * registers no callback and delivers nothing, so it has no id - it is counted
 * per subscriber and idempotent per topic, and the provider's watch is released
 * by the last unsubscribe_schema. A data unsubscribe does not release it, and
 * holding a watch does not keep a data subscription alive. */
FL_ABI_EXPORT fl_status fl_subscriber_subscribe_schema(fl_subscriber* subscriber, fl_topic topic,
                                                       fl_schema_arrival** out_arrival,
                                                       fl_error* err);

FL_ABI_EXPORT fl_status fl_subscriber_unsubscribe_schema(fl_subscriber* subscriber, fl_topic topic,
                                                         fl_error* err);

/* ══ The codec ═════════════════════════════════════════════════════════════ */

/* A schema-bound field plan, opened once per topic. */
typedef struct fl_codec fl_codec;

/* One Arrow array bound to a codec: validated once, BORROWED throughout. */
typedef struct fl_rows fl_rows;

/* Three steps, because each has a different cost and a different lifetime
 * (development plan §3.2, D-BIND-1a and D-BIND-23). The shape exists to make two
 * things structural rather than remembered: values cross as ARROW ARRAYS, never
 * as per-field C arguments, and NO ENCODE ENTRY POINT RETURNS BYTES - a function
 * that returned a buffer would have written the row somewhere other than the
 * transport's window, which is exactly the whole-row copy the copy-accounting
 * oracle exists to catch. */

/* 1. OPEN - once per schema.
 *
 * Validates the schema against the wire mapping, refuses a type the mapping does
 * not carry NAMING THE FIELD, and precomputes the field plan. `schema` is
 * BORROWED and deep-copied, so the caller may release its export as soon as this
 * returns.
 *
 * An fl_codec is immutable after construction, so any number of threads may use
 * one concurrently without a lock. */
FL_ABI_EXPORT fl_status fl_codec_open(const struct ArrowSchema* schema, fl_codec** out,
                                      fl_error* err);

FL_ABI_EXPORT void fl_codec_close(fl_codec* codec);

/* 2. BIND - once per batch.
 *
 * Checks the array is a struct matching this codec's schema and builds the array
 * view. Every buffer is validated HERE, not per row.
 *
 * `array` is BORROWED AND NEVER CONSUMED: the codec does not call its release
 * callback, and the caller keeps the export alive until fl_rows_unbind and
 * releases it afterwards. This is the rule that lets ONE export serve N
 * publishes, and a binding is expected to enforce it with a type - C#'s
 * `BoundRows.Dispose()` unbinds and then releases, in that order, so the caller
 * cannot get it wrong by forgetting.
 *
 * Immutable after construction, like the codec: N threads may publish different
 * rows of one bound batch concurrently. */
FL_ABI_EXPORT fl_status fl_rows_bind(const fl_codec* codec, const struct ArrowArray* array,
                                     fl_rows** out, fl_error* err);

FL_ABI_EXPORT void fl_rows_unbind(fl_rows* rows);

/* 3a. PUBLISH row `i` - the zero-copy path.
 *
 * The codec runs INSIDE the seam's publish, writing straight into the provider's
 * window: no intermediate bytes exist, and the copy oracle counts zero encode
 * copies. `atts` is BORROWED and may be NULL. */
FL_ABI_EXPORT fl_status fl_publisher_publish_row(fl_publisher* publisher, fl_topic topic,
                                                 const fl_rows* rows, int64_t i,
                                                 const fl_attachments* atts, fl_error* err);

/* 3b. PUBLISH rows [first, first + count) - N samples, one crossing.
 *
 * `atts_per_row` is NULL for none, or an array of `count` pointers, each of
 * which may itself be NULL. All BORROWED. */
FL_ABI_EXPORT fl_status fl_publisher_publish_rows(fl_publisher* publisher, fl_topic topic,
                                                  const fl_rows* rows, int64_t first, int64_t count,
                                                  const fl_attachments* const* atts_per_row,
                                                  fl_error* err);

/* 3c. ENCODE row `i` into a caller-supplied window - for bytes in hand.
 *
 * The route for a write-ahead log, a test, or a relay: the same encoder, writing
 * into the caller's window instead of the transport's. One crossing per REFILL,
 * never per append. */
FL_ABI_EXPORT fl_status fl_encode_row(const fl_rows* rows, int64_t i, fl_write_window* sink,
                                      fl_error* err);

/* DECODE - the mirror.
 *
 * Produces a fresh struct array in `out` which the caller IMPORTS AND OWNS; its
 * release callback frees the underlying buffers. Row bytes are borrowed and
 * never copied into the result's own allocation beyond what Arrow's layout
 * requires.
 *
 * Touches no provider and takes no lock the delivery path holds, so it is
 * callable from INSIDE a delivery callback - which is where a subscriber
 * actually wants it. */
FL_ABI_EXPORT fl_status fl_decode_rows(const fl_codec* codec, const uint8_t* bytes, size_t len,
                                       int64_t count, struct ArrowArray* out, fl_error* err);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FLETCHER_C_ABI_INCLUDE_FLETCHER_ABI_BINDING_H_ */
