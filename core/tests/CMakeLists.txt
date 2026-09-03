# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

add_executable(core_tests
    test_envelope.cpp
    test_positional_io.cpp
    test_status_taxonomy.cpp)

# PDA-DEC-9: Taxonomy.PublishedNumbersMatchTheEnum reads the published error-taxonomy table out
# of core/README.md itself rather than holding a copy of the numbers, so editing either side
# alone turns the test red. The README is exported by conanfile.py so the cache build reads the
# same file the repository holds. It is read at RUN time - a file(READ)/configure_file bake-in
# would re-create the held-copy defect the disk read exists to kill.
target_compile_definitions(core_tests PRIVATE
    FLETCHER_CORE_README_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../README.md")

# The third part of that guard, and the only one that catches an APPENDED status: the
# unhandled-enumerator diagnostic is promoted to an error on this one source file, so
# test_status_taxonomy.cpp's default-less switch over PubSubStatus fails to COMPILE when an
# enumerator is added. Narrowest scope that works - no new target, and core/tests has no other
# switch. (MSVC C4062 fires only when the switch has no `default:` label, which is exactly the
# shape the test mandates; -Werror=switch implies -Wswitch, so it needs no -Wall.) The tree has
# no global /WX or -Werror, so the flag must be explicit; the narrow-scope-promotion precedent
# is pubsub-arrow/tests/CMakeLists.txt:45-60, which uses target_compile_options on a dedicated
# object target rather than this per-source form. Source-file COMPILE_OPTIONS are
# directory-scoped, so this must stay in the same CMakeLists.txt as the add_executable above.
if(MSVC)
    set(FLETCHER_CORE_SWITCH_TOTALITY_FLAG /we4062)
else()
    set(FLETCHER_CORE_SWITCH_TOTALITY_FLAG -Werror=switch)
endif()

# That flag is the ONLY part of the guard that catches an APPENDED enumerator, and losing it
# would otherwise be SILENT: `if(MSVC)` also matches clang-cl, whose /we<n> mapping need not
# contain 4062, and a compiler that ignores the flag leaves the append case green forever. So
# the flag proves itself HERE, at configure time: a default-less non-exhaustive switch must NOT
# compile with it. Configuring fails loudly instead (design P3b's stop-and-ask, made mechanical;
# PDA-DEC-9 code review F2). Do not delete this without the owner's ruling on the alternative.
include(CheckCXXSourceCompiles)
set(FLETCHER_CORE_SAVED_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
set(CMAKE_REQUIRED_FLAGS "${FLETCHER_CORE_SWITCH_TOTALITY_FLAG}")
check_cxx_source_compiles(
    "enum class E { a, b };
     int f(E e) { switch (e) { case E::a: return 0; } return 1; }
     int main() { return f(E::a); }"
    FLETCHER_CORE_SWITCH_TOTALITY_IS_TOLERATED)
set(CMAKE_REQUIRED_FLAGS "${FLETCHER_CORE_SAVED_REQUIRED_FLAGS}")
if(FLETCHER_CORE_SWITCH_TOTALITY_IS_TOLERATED)
    message(FATAL_ERROR
        "${FLETCHER_CORE_SWITCH_TOTALITY_FLAG} does not reject a non-exhaustive default-less "
        "switch with this compiler, so appending a PubSubStatus enumerator would compile "
        "silently and spec §12 condition 3's guard would be absent. This is a stop-and-ask "
        "(design premise P3b): the owner chooses between a sentinel and relabelling the "
        "condition. Do not work around it by dropping the check.")
endif()

set_source_files_properties(test_status_taxonomy.cpp PROPERTIES
    COMPILE_OPTIONS "${FLETCHER_CORE_SWITCH_TOTALITY_FLAG}")

target_link_libraries(core_tests PRIVATE
    GTest::gtest_main
    fletcher-core)

include(GoogleTest)

# ---------------------------------------------------------------------------
# TIMEOUT, stated because this suite has no default one to fall back on.
#
# `core/CMakeLists.txt` calls `enable_testing()` rather than `include(CTest)`, so no
# `DartConfiguration.tcl` is generated and ctest's usual 1500 s per-test cap DOES NOT APPLY: an
# unbounded test here runs until the CI job itself is cancelled. That is not hypothetical - PR
# #126's first Linux run of the xrcedds provider suite, which has the same gap, spent 2 h 04 m
# inside one hung test and was cancelled with no result at all. The precedent and the reasoning
# in full are in `xrcedds-pubsub-provider/tests/CMakeLists.txt`.
#
# 30 s, chosen from this suite's own measured wall clock: all 29 rows run in 0.02-0.03 s and the
# whole suite in 0.93 s (msvc-194 x86_64 Release). Every row is pure in-process computation -
# encode/decode of a row buffer and two file reads of the READMEs - so there is no wait here that
# a slower runner could stretch, and no subprocess whose cost is a compiler. 30 s is ~1000x the
# slowest row, which absorbs any amount of runner contention, cold page-in or AV interference
# while still turning a future hang into a 30-second red instead of a cancelled job.
gtest_discover_tests(core_tests DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 30)
