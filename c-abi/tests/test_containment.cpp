// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The containment site's own branches, reached directly.
//
// ── Why these are not in test_binding_entry_points.cpp ──────────────────────
// That file drives the ABI through the shim's export table, which is the right
// way to test what a binding sees. But two branches of the containment site have
// NO path through that table, and both matter:
//
//   THE POISONED BRANCH. A shim that found a second copy of itself refuses every
//   fallible call. Reaching that for real needs two shims in one process, and
//   building a second shim means building the whole eProsima chain again for one
//   assertion. Untested, it was the half of D-BIND-17 that MATTERS — detection
//   is only the trigger — and a condition inverted there would either refuse
//   everything always or never refuse at all.
//
//   THE UNKNOWN-EXCEPTION ARM. `catch (...)` decides whether a non-std::exception
//   crossing the boundary becomes FL_INTERNAL or undefined behaviour. Nothing in
//   the ABI can produce one: every callback it takes is a C function that returns
//   a status or a count, so the arm is defensive and unreachable from outside.
//   Defensive code that has never run is a guess.
//
// So these link the object library and call `Contain` in this process. What they
// do NOT prove is that the SHIM wires the same logic — that is
// `SingleCopy.AHealthyProcessIsNotPoisoned`, which reaches a real entry point
// through the real export table and shows it is not refusing.
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "../src/containment.hpp"
#include "../src/single_copy.hpp"

namespace {

using fletcher::abi::Contain;
using fletcher::abi::SetSingleCopyRefusalForTest;
using fletcher::abi::SingleCopyRefusal;

std::string MessageOf(const fl_error& err) {
    if (err.message == nullptr) return {};
    return {reinterpret_cast<const char*>(err.message), err.message_len};
}

/// Release an error THIS FILE produced - and the one place in the suite that
/// must not release it through the ABI.
///
/// Every other test releases with `fl_error_dispose`, which is correct there: the
/// shim allocated the message inside itself and frees it inside itself. Here the
/// call went straight to `Contain`, so the message was allocated by the OBJECT
/// LIBRARY's copy of `SetMessage`, which is linked into this test binary
/// (`tests/CMakeLists.txt` links both the shim and the objects it is built from).
/// Disposing through the ABI would hand that allocation to the SHIM's `delete[]`.
///
/// Those are one heap only while both modules share a C runtime. They do today,
/// which is why either spelling passes now - and BIND-3 links the shim's MSVC CRT
/// STATICALLY, which is exactly when they stop being one. That is D-BIND-32's own
/// argument pointed the other way: the rule there says the shim must not free what
/// a caller allocated, and this is a caller not asking it to. Getting it wrong
/// would surface as heap corruption in whichever row ran first after the CRT flag
/// flipped, in a file about the containment taxonomy.
void DisposeLocal(fl_error& err) {
    delete[] err.message;
    err = fl_error{};
}

/// Restores the refusal slot however the test ends, so one red row cannot
/// poison every row after it in the same binary.
class PoisonedForThisTest {
   public:
    explicit PoisonedForThisTest(const std::string& refusal) {
        SetSingleCopyRefusalForTest(refusal);
    }
    ~PoisonedForThisTest() { SetSingleCopyRefusalForTest(std::string()); }

    PoisonedForThisTest(const PoisonedForThisTest&) = delete;
    PoisonedForThisTest& operator=(const PoisonedForThisTest&) = delete;
};

// ---------------------------------------------------------------------------
// The poisoned branch (D-BIND-17)
// ---------------------------------------------------------------------------

/// A poisoned shim refuses, carries the reason, and DOES NOT RUN THE BODY.
///
/// The last clause is the one worth asserting. A refusal that still ran the call
/// would be worse than no check at all: the process would do the thing that is
/// unsafe with two copies of Fletcher in it and then report that it had not.
TEST(Containment, APoisonedShimRefusesWithoutRunningTheCall) {
    const PoisonedForThisTest poisoned("two copies of the shim: /a/one.so and /b/two.so");

    bool body_ran = false;
    fl_error err = {};
    const fl_status status = Contain(&err, FL_ORIGIN_SEAM, [&] { body_ran = true; });

    EXPECT_EQ(status, FL_INTERNAL);
    EXPECT_FALSE(body_ran) << "the call ran anyway, so the refusal is a report rather than a "
                              "refusal and the process did the unsafe thing";
    EXPECT_EQ(err.status, FL_INTERNAL);
    EXPECT_NE(MessageOf(err).find("/a/one.so"), std::string::npos)
        << "the refusal did not carry the offending module paths, so its reader cannot act on "
           "it: "
        << MessageOf(err);
    EXPECT_NE(MessageOf(err).find("/b/two.so"), std::string::npos) << MessageOf(err);
    DisposeLocal(err);
}

/// Clearing the refusal restores ordinary service — so the branch is a condition
/// on state, not a latch this suite can never get out of.
TEST(Containment, ClearingTheRefusalRestoresService) {
    {
        const PoisonedForThisTest poisoned("poisoned");
        fl_error err = {};
        ASSERT_EQ(Contain(&err, FL_ORIGIN_SEAM, [] {}), FL_INTERNAL);
        DisposeLocal(err);
    }
    ASSERT_TRUE(SingleCopyRefusal().empty());

    bool body_ran = false;
    fl_error err = {};
    EXPECT_EQ(Contain(&err, FL_ORIGIN_SEAM, [&] { body_ran = true; }), FL_OK);
    EXPECT_TRUE(body_ran);
    EXPECT_EQ(err.message, nullptr);
}

// ---------------------------------------------------------------------------
// The unknown-exception arm
// ---------------------------------------------------------------------------

/// A throw that is not a `std::exception` becomes FL_INTERNAL and says so.
///
/// "A non-`std::exception` type loses its identity entirely - the price of a
/// boundary that cannot let an untyped exception through" (seam §5.1). What must
/// NOT happen is that it escapes: a C++ exception unwinding through a C function
/// is undefined behaviour, and on MSVC it is process termination.
TEST(Containment, AnUnknownExceptionTypeIsContainedAsInternal) {
    fl_error err = {};
    const fl_status status = Contain(&err, FL_ORIGIN_SEAM, [] { throw 42; });

    EXPECT_EQ(status, FL_INTERNAL);
    EXPECT_EQ(err.origin, FL_ORIGIN_SEAM);
    EXPECT_FALSE(MessageOf(err).empty())
        << "an unknown exception produced no message at all, so the caller learns nothing beyond "
           "a number";
    DisposeLocal(err);
}

/// The normative mapping, at the site that performs it: `std::overflow_error`
/// becomes FL_PAYLOAD_TOO_LARGE by TYPE, wherever it came from (seam §5.1).
TEST(Containment, OverflowIsPayloadTooLargeAtEitherOrigin) {
    for (const fl_origin origin : {FL_ORIGIN_SEAM, FL_ORIGIN_CODEC}) {
        fl_error err = {};
        EXPECT_EQ(Contain(&err, origin, [] { throw std::overflow_error("too big"); }),
                  FL_PAYLOAD_TOO_LARGE);
        EXPECT_EQ(err.origin, origin);
        DisposeLocal(err);
    }
}

/// The two tables, at their boundary. The reader's refusal types are
/// FL_INVALID_ARGUMENT at a CODEC site and FL_INTERNAL at a SEAM one, because
/// the seam's mapping is normative and total while `binding.h`'s `fl_origin`
/// documentation requires the codec's number to be the finer one.
TEST(Containment, TheReadersRefusalTypesDependOnTheOrigin) {
    fl_error codec = {};
    EXPECT_EQ(Contain(&codec, FL_ORIGIN_CODEC, [] { throw std::invalid_argument("bad bytes"); }),
              FL_INVALID_ARGUMENT);
    DisposeLocal(codec);

    fl_error seam = {};
    EXPECT_EQ(Contain(&seam, FL_ORIGIN_SEAM, [] { throw std::invalid_argument("bad bytes"); }),
              FL_INTERNAL)
        << "a seam site reported a finer number than the seam's normative mapping allows; that "
           "mapping is total and everything unlisted is kInternal";
    DisposeLocal(seam);
}

/// A null `fl_error*` is tolerated: the caller still gets the number.
TEST(Containment, ANullErrorStillReturnsTheStatus) {
    EXPECT_EQ(Contain(nullptr, FL_ORIGIN_SEAM, [] { throw std::overflow_error("x"); }),
              FL_PAYLOAD_TOO_LARGE);
    EXPECT_EQ(Contain(nullptr, FL_ORIGIN_SEAM, [] {}), FL_OK);
}

}  // namespace
