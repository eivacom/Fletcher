add_library(fast_dds_pubsub_provider
    src/fast_dds_pubsub_provider.cpp)
target_include_directories(fast_dds_pubsub_provider PUBLIC include)
target_link_libraries(fast_dds_pubsub_provider
    PUBLIC  row_codec
    PRIVATE fast-dds::fast-dds)
