cmake_minimum_required(VERSION 3.15)
project(PackageTest CXX)

find_package(eiva-fletcher-core CONFIG REQUIRED)

add_executable(example src/example.cpp)
target_link_libraries(example eiva-fletcher-core::eiva-fletcher-core)