include(CTest)

add_executable(pubsub_tests
    test_driver.cpp)

target_link_libraries(pubsub_tests PRIVATE
    Catch2::Catch2WithMain
    pubsub)

include(Catch)
catch_discover_tests(pubsub_tests)
