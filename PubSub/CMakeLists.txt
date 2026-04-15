add_library(pubsub
    src/driver.cpp
    src/schema_ipc.cpp)
target_include_directories(pubsub PUBLIC include)
target_link_libraries(pubsub PUBLIC nanoarrow)

add_library(pubsub_capi STATIC
    src/pubsub_capi.cpp)
target_include_directories(pubsub_capi PUBLIC include)
target_link_libraries(pubsub_capi PRIVATE pubsub)

add_subdirectory(tests)
