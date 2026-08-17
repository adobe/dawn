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

option(WEBGPU_AAR_ENABLE_NATIVE_SDK
       "Build full Kotlin+C+C++ SDK (separate libwebgpu_dawn.so, full headers)" OFF)

if (WEBGPU_AAR_ENABLE_NATIVE_SDK)
    set(DAWN_BUILD_MONOLITHIC_LIBRARY SHARED CACHE STRING "" FORCE)
else()
    set(DAWN_BUILD_MONOLITHIC_LIBRARY STATIC CACHE STRING "" FORCE)
endif()

set(DAWN_DIR "${webgpu_SOURCE_DIR}/../../../../../..")
add_subdirectory(${DAWN_DIR} dawn)

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

# Small JNI-only SHARED library. The dynamic linker will auto-load
# libwebgpu_dawn.so via DT_NEEDED when System.loadLibrary("webgpu_c_bundled")
# runs.
add_library(webgpu_c_bundled SHARED)
target_sources(webgpu_c_bundled PRIVATE
    ${WEBGPU_JNI_SOURCES}
    ${WEBGPU_JNI_HEADERS}
    GPUImportedAHB.cpp
    JNIContext.cpp
    JNIContext.h
    Util.cpp
)

target_include_directories(webgpu_c_bundled PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/dawn/gen/cpp)
target_link_libraries(webgpu_c_bundled PRIVATE android sync log webgpu_dawn dawn_common dawn_internal_config)
target_link_options(webgpu_c_bundled PRIVATE "-Wl,-z,max-page-size=16384")

if (WEBGPU_AAR_ENABLE_NATIVE_SDK)
    # Full SDK flavor: mirror the minimum header set required by
    # <webgpu/webgpu.h> and <webgpu/webgpu_cpp.h> consumers into
    # ${CMAKE_SOURCE_DIR}/gen/include so the Gradle prefab block can ship them
    # with the AAR. Sources are split between the committed include/webgpu/
    # tree and the generated dawn/gen/include tree.
    set(_webgpu_prefab_include_dir "${CMAKE_SOURCE_DIR}/gen/include")
    set(_webgpu_committed_include_dir "${DAWN_DIR}/include/webgpu")
    set(_webgpu_generated_include_dir "${CMAKE_CURRENT_BINARY_DIR}/dawn/gen/include")

    set(_webgpu_committed_headers
        webgpu.h
        webgpu_cpp.h
        webgpu_enum_class_bitmasks.h
        webgpu_cpp_print.h
    )
    set(_webgpu_generated_dawn_headers
        dawn/webgpu.h
        dawn/webgpu_cpp.h
        dawn/webgpu_cpp_print.h
    )
    set(_webgpu_generated_webgpu_headers
        webgpu/webgpu_cpp_chained_struct.h
    )

    set(_webgpu_prefab_outputs)
    foreach(_h IN LISTS _webgpu_committed_headers)
        set(_src "${_webgpu_committed_include_dir}/${_h}")
        set(_dst "${_webgpu_prefab_include_dir}/webgpu/${_h}")
        add_custom_command(
            OUTPUT "${_dst}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "${_dst}"
            DEPENDS "${_src}"
            COMMENT "Copying webgpu/${_h} into prefab include tree"
            VERBATIM
        )
        list(APPEND _webgpu_prefab_outputs "${_dst}")
    endforeach()
    foreach(_h IN LISTS _webgpu_generated_dawn_headers _webgpu_generated_webgpu_headers)
        set(_src "${_webgpu_generated_include_dir}/${_h}")
        set(_dst "${_webgpu_prefab_include_dir}/${_h}")
        add_custom_command(
            OUTPUT "${_dst}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "${_dst}"
            DEPENDS dawncpp_headers "${_src}"
            COMMENT "Copying ${_h} into prefab include tree"
            VERBATIM
        )
        list(APPEND _webgpu_prefab_outputs "${_dst}")
    endforeach()

    add_custom_target(webgpu_prefab_headers ALL DEPENDS ${_webgpu_prefab_outputs})
    add_dependencies(webgpu_prefab_headers dawncpp_headers)
    add_dependencies(webgpu_dawn webgpu_prefab_headers)
else()
    # Bare Kotlin flavor: only ship dawn/webgpu.h, matching the pre-SDK
    # behavior.
    add_custom_command(DEPENDS dawncpp_headers POST_BUILD
                       OUTPUT ${CMAKE_SOURCE_DIR}/gen/include/dawn/webgpu.h
                       COMMAND rm -r "${CMAKE_SOURCE_DIR}/gen" || echo
                       COMMAND ${CMAKE_COMMAND} -E copy
                               ${CMAKE_CURRENT_BINARY_DIR}/dawn/gen/include/dawn/webgpu.h
                               ${CMAKE_SOURCE_DIR}/gen/include/dawn/webgpu.h)
    add_custom_target(webgpu_prefab_headers ALL
                      DEPENDS ${CMAKE_SOURCE_DIR}/gen/include/dawn/webgpu.h)
    add_dependencies(webgpu_c_bundled webgpu_prefab_headers)
endif()

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
target_link_options(webgpu_wrapper_tests PRIVATE "-Wl,-z,max-page-size=16384")
