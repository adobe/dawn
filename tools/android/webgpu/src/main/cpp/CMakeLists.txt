# Copyright 2024 The Dawn & Tint Authors
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice, this
#    list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its
#    contributors may be used to endorse or promote products derived from
#    this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

cmake_policy(SET CMP0048 NEW)
project(webgpu VERSION 1.1.0 LANGUAGES CXX)

cmake_minimum_required(VERSION 3.22.1)

# Hide non-public symbols; set before dawn subdirectory is imported.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

set(DAWN_BUILD_MONOLITHIC_LIBRARY STATIC)

set(DAWN_DIR "${webgpu_SOURCE_DIR}/../../../../../..")
add_subdirectory(${DAWN_DIR} dawn)

# Export the C API symbols from webgpu_c_bundled so that consumers of the
# prefab can link against them when using the C++ wrapper headers. Without this,
# -fvisibility=hidden (set above) causes all Dawn symbols to be hidden.
target_compile_definitions(webgpu_dawn_objects PUBLIC WGPU_SHARED_LIBRARY)

list(INSERT CMAKE_MODULE_PATH 0 "${DAWN_DIR}/src/cmake")
include(DawnCompilerChecks)

# Make the library exposing both Dawn's API and the JNI methods.
find_package(Python3 REQUIRED)
DawnJSONGenerator(
    TARGET "jni"
    PRINT_NAME "Dawn JNI binding"
    OUTPUT_SOURCES WEBGPU_JNI_SOURCES
    OUTPUT_HEADERS WEBGPU_JNI_HEADERS
)

add_library(webgpu_c_bundled SHARED)
target_sources(webgpu_c_bundled PRIVATE
    ${WEBGPU_JNI_SOURCES}
    ${WEBGPU_JNI_HEADERS}
    JNIContext.cpp
    JNIContext.h
    ManualJNI.cpp
    Util.cpp
)

target_include_directories(webgpu_c_bundled PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(webgpu_c_bundled PRIVATE android webgpu_dawn dawn_common dawn_internal_config)

# Copy C and C++ headers into gen/include/ for the prefab package.
# Generated headers live under ${CMAKE_CURRENT_BINARY_DIR}/dawn/gen/include/;
# source-tree headers live under ${DAWN_DIR}/include/.
set(PREFAB_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/gen/include")
set(DAWN_GEN_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/dawn/gen/include")

add_custom_command(
    DEPENDS dawncpp_headers
    OUTPUT
        ${PREFAB_INCLUDE_DIR}/dawn/webgpu.h
        ${PREFAB_INCLUDE_DIR}/dawn/webgpu_cpp.h
        ${PREFAB_INCLUDE_DIR}/dawn/webgpu_cpp_print.h
        ${PREFAB_INCLUDE_DIR}/webgpu/webgpu.h
        ${PREFAB_INCLUDE_DIR}/webgpu/webgpu_cpp.h
        ${PREFAB_INCLUDE_DIR}/webgpu/webgpu_cpp_print.h
        ${PREFAB_INCLUDE_DIR}/webgpu/webgpu_cpp_chained_struct.h
        ${PREFAB_INCLUDE_DIR}/webgpu/webgpu_enum_class_bitmasks.h
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${PREFAB_INCLUDE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PREFAB_INCLUDE_DIR}/dawn"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PREFAB_INCLUDE_DIR}/webgpu"
    # Generated C header
    COMMAND ${CMAKE_COMMAND} -E copy
            "${DAWN_GEN_INCLUDE}/dawn/webgpu.h"
            "${PREFAB_INCLUDE_DIR}/dawn/webgpu.h"
    # Generated C++ headers
    COMMAND ${CMAKE_COMMAND} -E copy
            "${DAWN_GEN_INCLUDE}/dawn/webgpu_cpp.h"
            "${PREFAB_INCLUDE_DIR}/dawn/webgpu_cpp.h"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${DAWN_GEN_INCLUDE}/dawn/webgpu_cpp_print.h"
            "${PREFAB_INCLUDE_DIR}/dawn/webgpu_cpp_print.h"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${DAWN_GEN_INCLUDE}/webgpu/webgpu_cpp_chained_struct.h"
            "${PREFAB_INCLUDE_DIR}/webgpu/webgpu_cpp_chained_struct.h"
    # Source-tree forwarding/utility headers
    COMMAND ${CMAKE_COMMAND} -E copy
            "${DAWN_DIR}/include/webgpu/webgpu.h"
            "${PREFAB_INCLUDE_DIR}/webgpu/webgpu.h"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${DAWN_DIR}/include/webgpu/webgpu_cpp.h"
            "${PREFAB_INCLUDE_DIR}/webgpu/webgpu_cpp.h"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${DAWN_DIR}/include/webgpu/webgpu_cpp_print.h"
            "${PREFAB_INCLUDE_DIR}/webgpu/webgpu_cpp_print.h"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${DAWN_DIR}/include/webgpu/webgpu_enum_class_bitmasks.h"
            "${PREFAB_INCLUDE_DIR}/webgpu/webgpu_enum_class_bitmasks.h"
    COMMENT "Copying Dawn C/C++ headers for prefab packaging"
)

add_custom_target(copy_prefab_headers DEPENDS
    ${PREFAB_INCLUDE_DIR}/dawn/webgpu.h
    ${PREFAB_INCLUDE_DIR}/dawn/webgpu_cpp.h
    ${PREFAB_INCLUDE_DIR}/dawn/webgpu_cpp_print.h
    ${PREFAB_INCLUDE_DIR}/webgpu/webgpu.h
    ${PREFAB_INCLUDE_DIR}/webgpu/webgpu_cpp.h
    ${PREFAB_INCLUDE_DIR}/webgpu/webgpu_cpp_print.h
    ${PREFAB_INCLUDE_DIR}/webgpu/webgpu_cpp_chained_struct.h
    ${PREFAB_INCLUDE_DIR}/webgpu/webgpu_enum_class_bitmasks.h
)
add_dependencies(webgpu_c_bundled copy_prefab_headers)

# Add unit test build configuration
# To run use the following command:
# ./gradlew -i connectedAndroidTest
find_package(googletest REQUIRED CONFIG)
find_package(junit-gtest REQUIRED CONFIG)
add_library(webgpu_wrapper_tests SHARED ./../../androidTest/cpp/webgpu_wrapper_tests.cpp)
target_include_directories(webgpu_wrapper_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(webgpu_wrapper_tests
  PRIVATE
    webgpu_c_bundled
    android
    atomic
    log
    googletest::gtest
    googletest::gmock
    junit-gtest::junit-gtest
)
