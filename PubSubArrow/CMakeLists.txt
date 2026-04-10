add_library(pubsub_arrow
    src/pubsub_arrow.cpp)
target_include_directories(pubsub_arrow PUBLIC include)
target_link_libraries(pubsub_arrow PUBLIC pubsub row_codec arrow::arrow)

add_subdirectory(tests)
