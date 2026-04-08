cmake_minimum_required(VERSION 3.15)
project(ArrowRow VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Conan compatibility: Release-only packages in a multi-config VS build ──────
#
# Several Conan packages (protobuf, FastDDS, fastcdr, tinyxml2,
# foonathan_memory) are available only as Release builds, compiled with /MD.
# Other packages (Arrow, SQLite3, ZLIB, Catch2 …) have both Debug (/MDd) and
# Release (/MD) Conan builds.
#
# Mixing /MD and /MDd static libs in the same binary is a hard MSVC error
# (_ITERATOR_DEBUG_LEVEL / RuntimeLibrary mismatch).  The only consistent
# solution when some packages are Release-only: compile ALL our code with /MD
# and redirect every Conan imported target to its Release variant, regardless
# of the active VS configuration.
#
# Mechanism
# ─────────
# (1) CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" — all our targets use /MD.
# (2) CMAKE_MAP_IMPORTED_CONFIG_* — CMake resolves IMPORTED_LOCATION_<CONFIG>
#     using the Release property when the Debug/MinSizeRel/RelWithDebInfo
#     property is absent.
# (3) After find_package: for every CONAN_LIB::*_DEBUG target that has a
#     *_RELEASE sibling, copy IMPORTED_LOCATION_RELEASE from the sibling so
#     the remap in (2) can resolve it (multi-config packages like Arrow/ZLIB
#     create _DEBUG CONAN_LIB targets with only IMPORTED_LOCATION_DEBUG set;
#     the remap would otherwise produce -NOTFOUND for those).
# (4) After find_package: for every imported target whose INTERFACE_* properties
#     are gated only on $<$<CONFIG:Release>:…> (i.e. Release-only packages),
#     duplicate those entries for Debug/MinSizeRel/RelWithDebInfo.  Multi-config
#     targets that already have $<$<CONFIG:Debug>:…> entries are left untouched.
#
# All find_package calls are centralised here (not in subdirectories) so that
# every Conan imported target is created before the patch below runs.
# ──────────────────────────────────────────────────────────────────────────────

# (1)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# (2)
set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG          Release)
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL     Release)
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)

find_package(Arrow         REQUIRED)
find_package(Boost         REQUIRED)
find_package(SQLite3       REQUIRED)
find_package(Protobuf      REQUIRED)
find_package(fastdds       REQUIRED)
find_package(Catch2        REQUIRED)
find_package(nlohmann_json REQUIRED)

# (2b) protobuf::protoc is an IMPORTED executable with only IMPORTED_LOCATION
#      (no config suffix).  CMAKE_MAP_IMPORTED_CONFIG_DEBUG = Release causes
#      $<TARGET_FILE:protobuf::protoc> to look only for IMPORTED_LOCATION_RELEASE,
#      skipping the configless fallback.  Copy it so all configs resolve.
if(TARGET protobuf::protoc)
    get_target_property(_protoc_loc protobuf::protoc IMPORTED_LOCATION)
    if(_protoc_loc)
        set_target_properties(protobuf::protoc PROPERTIES
            IMPORTED_LOCATION_RELEASE         "${_protoc_loc}"
            IMPORTED_LOCATION_DEBUG           "${_protoc_loc}"
            IMPORTED_LOCATION_MINSIZEREL      "${_protoc_loc}"
            IMPORTED_LOCATION_RELWITHDEBINFO  "${_protoc_loc}")
    endif()
    unset(_protoc_loc)
endif()

# (3) Fix CONAN_LIB::*_DEBUG targets so the remap in (2) can resolve them.
#     Most multi-config packages have matching lib names (e.g. zlib / zlib) so
#     the Release sibling is just CONAN_LIB::<same_base>_RELEASE.
#     Some packages use a trailing 'd' to distinguish Debug from Release builds
#     (e.g. Thrift: thriftmdd → thriftmd).  For those, strip one trailing 'd'
#     from the base name and try again before giving up.
get_directory_property(_cr_all DIRECTORY ${CMAKE_SOURCE_DIR} IMPORTED_TARGETS)
foreach(_cr_t IN LISTS _cr_all)
    if(_cr_t MATCHES "^CONAN_LIB::(.+)_DEBUG$")
        set(_cr_base "${CMAKE_MATCH_1}")
        set(_cr_rel "CONAN_LIB::${_cr_base}_RELEASE")
        if(NOT TARGET "${_cr_rel}")
            # Fallback: strip one trailing 'd' from the lib-name segment.
            string(REGEX REPLACE "d$" "" _cr_base_nd "${_cr_base}")
            set(_cr_rel "CONAN_LIB::${_cr_base_nd}_RELEASE")
        endif()
        if(TARGET "${_cr_rel}")
            get_target_property(_cr_loc "${_cr_rel}" IMPORTED_LOCATION_RELEASE)
            if(_cr_loc)
                set_target_properties("${_cr_t}" PROPERTIES
                    IMPORTED_LOCATION_RELEASE    "${_cr_loc}"
                    IMPORTED_LOCATION_MINSIZEREL "${_cr_loc}"
                    IMPORTED_LOCATION_RELWITHDEBINFO "${_cr_loc}")
            endif()
        endif()
    endif()
endforeach()
unset(_cr_all)
unset(_cr_t)
unset(_cr_base)
unset(_cr_base_nd)
unset(_cr_rel)
unset(_cr_loc)

# (4) Duplicate $<$<CONFIG:Release>:X> interface entries as Debug/MinSizeRel/
#     RelWithDebInfo on any imported target that has no Debug entries yet.
#     Note: CMake's genex parser treats $<…> as atomic even when its content
#     contains semicolons, so string(REPLACE …) and set_property with quoted
#     args correctly round-trip multi-valued genexes like
#     $<$<CONFIG:Release>:fastcdr;tinyxml2::tinyxml2;foonathan_memory>.
function(_conan_compat_patch _ccp_tgt)
    foreach(_ccp_prop
            INTERFACE_LINK_LIBRARIES  INTERFACE_INCLUDE_DIRECTORIES
            INTERFACE_LINK_DIRECTORIES INTERFACE_COMPILE_DEFINITIONS
            INTERFACE_COMPILE_OPTIONS)
        get_target_property(_ccp_vals "${_ccp_tgt}" "${_ccp_prop}")
        if(NOT _ccp_vals)
            continue()
        endif()
        # Already has Debug entries → multi-config package, leave it alone.
        set(_ccp_skip FALSE)
        foreach(_ccp_v IN LISTS _ccp_vals)
            if(_ccp_v MATCHES "CONFIG:Debug")
                set(_ccp_skip TRUE)
                break()
            endif()
        endforeach()
        if(_ccp_skip)
            continue()
        endif()
        # Duplicate each $<$<CONFIG:Release>:X> for the other configs.
        foreach(_ccp_v IN LISTS _ccp_vals)
            string(FIND "${_ccp_v}" "$<$<CONFIG:Release>:" _ccp_pos)
            if(_ccp_pos EQUAL 0)
                string(REPLACE "$<$<CONFIG:Release>:" "$<$<CONFIG:Debug>:"          _d "${_ccp_v}")
                string(REPLACE "$<$<CONFIG:Release>:" "$<$<CONFIG:MinSizeRel>:"     _m "${_ccp_v}")
                string(REPLACE "$<$<CONFIG:Release>:" "$<$<CONFIG:RelWithDebInfo>:" _r "${_ccp_v}")
                # Quoted args preserve semicolons inside genex content.
                set_property(TARGET "${_ccp_tgt}" APPEND PROPERTY "${_ccp_prop}"
                    "${_d}" "${_m}" "${_r}")
            endif()
        endforeach()
    endforeach()
endfunction()

get_directory_property(_cr_all DIRECTORY ${CMAKE_SOURCE_DIR} IMPORTED_TARGETS)
foreach(_cr_t IN LISTS _cr_all)
    if(NOT _cr_t MATCHES "^CONAN_LIB::")
        _conan_compat_patch("${_cr_t}")
    endif()
endforeach()
unset(_cr_all)
unset(_cr_t)

# Strip malformed/empty INTERFACE_COMPILE_OPTIONS fragments from Conan-generated targets.
#
# Conan stores per-language compile options as
#   $<$<COMPILE_LANGUAGE:CXX>:${CXX_OPTS}>  and  $<$<COMPILE_LANGUAGE:C>:${C_OPTS}>
# When these option variables are empty the genexes reduce to no-ops.
# They are passed to set_property via an unquoted argument
#   $<$<CONFIG:Release>:${opts_list}>
# where cmake's argument splitter breaks on the semicolons inside the expanded list,
# producing two malformed genex fragments:
#   $<$<CONFIG:Release>:$<$<COMPILE_LANGUAGE:CXX>:>   <- missing outer >
#   $<$<COMPILE_LANGUAGE:C>:>>                         <- extra >
# _conan_compat_patch then duplicates the first fragment for Debug/MinSizeRel/etc.
# CMake's VS generator partially evaluates the broken genexes and writes literal
# "$<0:" / "$<1:" tokens into AdditionalOptions.  MSVC interprets those as
# source-file arguments and fails with C1083.
#
# All $<$<COMPILE_LANGUAGE:…> entries in this project's Conan packages are empty
# no-ops, so removing them is safe.
get_directory_property(_fix_all DIRECTORY ${CMAKE_SOURCE_DIR} IMPORTED_TARGETS)
foreach(_fix_t IN LISTS _fix_all)
    get_target_property(_fix_opts "${_fix_t}" INTERFACE_COMPILE_OPTIONS)
    if(NOT _fix_opts)
        continue()
    endif()
    set(_fix_clean "")
    set(_fix_dirty FALSE)
    foreach(_fix_o IN LISTS _fix_opts)
        if(_fix_o MATCHES "\\$<\\$<COMPILE_LANGUAGE:")
            set(_fix_dirty TRUE)
        else()
            list(APPEND _fix_clean "${_fix_o}")
        endif()
    endforeach()
    if(_fix_dirty)
        set_target_properties("${_fix_t}" PROPERTIES INTERFACE_COMPILE_OPTIONS "${_fix_clean}")
    endif()
endforeach()
unset(_fix_all)
unset(_fix_t)
unset(_fix_opts)
unset(_fix_clean)
unset(_fix_dirty)
unset(_fix_o)

add_subdirectory(PubSub)
add_subdirectory(Codec)
add_subdirectory(Batcher)
add_subdirectory(ProtoPlugin)
add_subdirectory(ProtoIntegration)
add_subdirectory(FastDDSPubSubProvider)
add_subdirectory(WebGateway)
