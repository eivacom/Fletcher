# Vendored nanoarrow 0.8.0 (Apache Arrow)
# Amalgamation build — core + IPC + flatcc

add_library(nanoarrow STATIC
    nanoarrow.c
    nanoarrow_ipc.c
    flatcc.c)

target_include_directories(nanoarrow PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Suppress warnings in vendored code.
if(MSVC)
    target_compile_options(nanoarrow PRIVATE /W0)
else()
    target_compile_options(nanoarrow PRIVATE -w)
endif()
