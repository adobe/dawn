cmake_minimum_required(VERSION 3.22.1)
project(test_cpp_consumer)

find_package(webgpu REQUIRED CONFIG)

add_library(test_native SHARED test_cpp_wrappers.cpp)
target_link_libraries(test_native PRIVATE webgpu::webgpu_c_bundled log)
