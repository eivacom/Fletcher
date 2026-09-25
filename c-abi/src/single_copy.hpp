// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The single-copy check (BIND-2d, D-BIND-17).
//
// ── The hazard ──────────────────────────────────────────────────────────────
// Fletcher's re-entrancy machinery is an `inline thread_local` in a header-only
// target, so every separately linked binary carrying Fletcher gets its OWN
// delivery-frame stack. Two copies in one process means the doors stop seeing
// each other's frames: a re-entrant call that must be refused with
// `kReentrantCall` is served instead, silently, with no symptom until it
// deadlocks or corrupts something far from here. The shim is therefore one
// shared library that statically links every Fletcher component it uses, and a
// process must contain exactly one of it.
//
// ── What is detectable, and what is not ─────────────────────────────────────
// TWO SHIMS is the detectable case and the likely one: both export
// `fl_single_copy_marker`, so enumerating the process's modules finds them both
// and names them. That is what this file does.
//
// A C++ host that statically links Fletcher and also hosts a language runtime is
// NOT detectable — it exports no marker. D-BIND-17 rules that case out by fact
// rather than by check: the consumers are managed processes hosting this binding
// only. A future host of that shape is a stop-and-ask before design, never a
// registration handshake papering over a forked thread-local.
//
// ── Why the scan is here and not only in a load-time hook ───────────────────
// It is an ordinary function so that it can be TESTED. A load-time constructor
// that can only be exercised by arranging two shims in a process is a check
// nobody can prove works; a function the suite calls with a decoy module loaded
// is one the suite can. The shim's own initializer calls exactly this function.
#ifndef FLETCHER_C_ABI_SRC_SINGLE_COPY_HPP_
#define FLETCHER_C_ABI_SRC_SINGLE_COPY_HPP_

#include <string>
#include <vector>

namespace fletcher::abi {

/// The NUL-terminated identification string `fl_single_copy_marker` returns.
///
/// Carries the component and its ABI version, because the diagnostic a duplicate
/// produces is read by someone who does not yet know which two things collided.
const char* MarkerText();

/// Every loaded module that exports `fl_single_copy_marker`, by path.
///
/// One entry is the healthy answer: this shim. Two or more is the fault
/// D-BIND-17 exists to catch, and the paths are what make the report actionable
/// — "two copies of Fletcher" without saying which two costs its reader an
/// afternoon.
///
/// Returns an EMPTY vector when the platform's module enumeration fails, which
/// is deliberately indistinguishable from nothing to report: a check that
/// refused to initialise the shim because it could not enumerate would turn a
/// diagnostic into an outage.
std::vector<std::string> ModulesExportingTheMarker();

/// The latched conflict, or empty when there is none.
///
/// Set once by `CheckSingleCopy()` at load. Read by the containment site, so a
/// poisoned shim refuses every fallible entry point with one message rather than
/// each entry point inventing its own.
const std::string& SingleCopyRefusal();

/// Run the scan and latch the verdict. Idempotent; called by the shim's own
/// load-time initializer and by the tests.
void CheckSingleCopy();

/// TEST ONLY: latch an arbitrary refusal, or clear it with an empty string.
///
/// It exists because the refusal is the half of this feature that MATTERS and
/// the half that is otherwise unreachable. Detection can be tested with a real
/// decoy module; the refusal cannot, because reaching it for real needs two
/// shims in one process, and building a second shim means building the entire
/// eProsima chain again for one assertion.
///
/// Not exported - `binding.h` does not declare it, so it is not in the shim's
/// export table and no binding can call it. It is reachable only by a test that
/// links the object library, which is exactly the audience.
void SetSingleCopyRefusalForTest(std::string refusal);

}  // namespace fletcher::abi

#endif  // FLETCHER_C_ABI_SRC_SINGLE_COPY_HPP_
