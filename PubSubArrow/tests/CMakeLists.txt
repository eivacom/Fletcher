include(CTest)

add_executable(pubsub_arrow_tests
    test_pubsub_arrow.cpp)

target_link_libraries(pubsub_arrow_tests PRIVATE
    Catch2::Catch2WithMain
    pubsub_arrow)

include(Catch)
catch_discover_tests(pubsub_arrow_tests)
