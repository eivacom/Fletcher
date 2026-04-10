add_library(pubsub
    src/driver.cpp
    src/schema_ipc.cpp)
target_include_directories(pubsub PUBLIC include)
target_link_libraries(pubsub PUBLIC arrow::arrow)

add_subdirectory(tests)
