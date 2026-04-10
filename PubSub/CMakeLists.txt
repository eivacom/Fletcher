add_library(pubsub
    src/driver.cpp
    src/pubsub_provider.cpp)
target_include_directories(pubsub PUBLIC include)
target_link_libraries(pubsub PUBLIC arrow::arrow row_codec)

add_subdirectory(tests)
