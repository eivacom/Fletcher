# Vendored nanoarrow 0.8.0 (Apache Arrow)
# Amalgamation build — core + IPC + flatcc

add_library(nanoarrow STATIC
    nanoarrow.c
    nanoarrow_ipc.c
    flatcc.c)

target_include_directories(nanoarrow PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

set_target_properties(nanoarrow PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(nanoarrow PRIVATE /W0)
else()
    target_compile_options(nanoarrow PRIVATE -w)
endif()
