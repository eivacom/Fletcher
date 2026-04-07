add_library(row_codec
    src/row_codec.cpp
    src/schema_evolution.cpp)
target_include_directories(row_codec PUBLIC include PRIVATE src)
target_link_libraries(row_codec PUBLIC arrow::arrow pubsub)

add_library(arrow_row_codec_capi STATIC
    src/arrow_row_codec_capi.cpp)
target_include_directories(arrow_row_codec_capi PUBLIC include)
target_link_libraries(arrow_row_codec_capi PRIVATE row_codec arrow::arrow)

add_subdirectory(tests)
