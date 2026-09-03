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
set_source_files_properties(test_status_taxonomy.cpp PROPERTIES
    COMPILE_OPTIONS "${FLETCHER_CORE_SWITCH_TOTALITY_FLAG}")

target_link_libraries(core_tests PRIVATE
    GTest::gtest_main
    fletcher-core)

include(GoogleTest)
gtest_discover_tests(core_tests DISCOVERY_MODE PRE_TEST)
