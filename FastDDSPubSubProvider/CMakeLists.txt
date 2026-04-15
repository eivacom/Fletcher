add_library(fast_dds_pubsub_provider
    src/fast_dds_pubsub_provider.cpp)
target_include_directories(fast_dds_pubsub_provider PUBLIC include)
target_link_libraries(fast_dds_pubsub_provider
    PUBLIC  pubsub
    PRIVATE fast-dds::fast-dds)

add_library(fast_dds_capi STATIC
    src/fast_dds_capi.cpp)
target_include_directories(fast_dds_capi PUBLIC include)
target_link_libraries(fast_dds_capi
    PRIVATE fast_dds_pubsub_provider pubsub_capi)
