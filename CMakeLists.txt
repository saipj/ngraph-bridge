# Copyright 2018-2020 Intel Corporation
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at

#     http://www.apache.org/licenses/LICENSE-2.0

# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.4)

project (ngraph_tensorflow_bridge CXX)

# set directory where the custom finders live
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")

# set(CMAKE_CXX_COMPILER "clang++")
include(ExternalProject)
include(CMakeDependentOption)
include(cmake/sdl.cmake)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "^(Apple)?Clang$")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Wno-comment -Wno-sign-compare -Wno-backslash-newline-escape")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Wno-comment -Wno-sign-compare -Wignored-attributes")
endif()

# In order to compile ngraph-tf with memory leak detection, run `cmake` with `-DCMAKE_BUILD_TYPE=Sanitize`.
# N.B.: This *will* crash python unit tests because ngraph-tf will be loaded "too late" via `dlopen`,
# so only use this with C++ tests.
# (In theory using `LD_PRELOAD` should address the python issue, but it doesn't appear to work on OS X, at least.)
# If there are any memory leaks, then upon running the binary a report will be automatically generated.
SET(CMAKE_CXX_FLAGS_SANITIZE
    "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -D_LIBCPP_HAS_NO_ASAN -fsanitize-address-use-after-scope"
    CACHE STRING "Flags used by the C++ compiler during sanitized builds."
    FORCE )
SET(CMAKE_C_FLAGS_SANITIZE
    "${CMAKE_C_FLAGS_DEBUG} -fsanitize=address -D_LIBCPP_HAS_NO_ASAN -fsanitize-address-use-after-scope"
    CACHE STRING "Flags used by the C compiler during sanitized builds."
    FORCE )
SET(CMAKE_EXE_LINKER_FLAGS_SANITIZE
    "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=address -D_LIBCPP_HAS_NO_ASAN -fsanitize-address-use-after-scope"
    CACHE STRING "Flags used for linking binaries during sanitized builds."
    FORCE )
SET(CMAKE_SHARED_LINKER_FLAGS_SANITIZE
    "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=address -D_LIBCPP_HAS_NO_ASAN -fsanitize-address-use-after-scope"
    CACHE STRING "Flags used by the shared libraries linker during coverage builds."
    FORCE )
MARK_AS_ADVANCED(
    CMAKE_CXX_FLAGS_SANITIZE
    CMAKE_C_FLAGS_SANITIZE
    CMAKE_EXE_LINKER_FLAGS_SANITIZE
    CMAKE_SHARED_LINKER_FLAGS_SANITIZE)

# These variables are undocumented but useful.
set(CMAKE_DISABLE_SOURCE_CHANGES ON)
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)

# Invoke a command to determine how many CPU cores we have, and set
# NUM_MAKE_PROCESSES accordingly so we know what number to pass to make -j.
if(APPLE)
   set (PROCESSOR_COUNT_COMMAND sysctl -n hw.physicalcpu)
else()
   set (PROCESSOR_COUNT_COMMAND nproc)
endif()

execute_process(
    COMMAND ${PROCESSOR_COUNT_COMMAND}
    RESULT_VARIABLE NPROC_RESULT
    OUTPUT_VARIABLE NUM_MAKE_PROCESSES
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT APPLE)
    # FIXME: Doesn't work for Ubuntu
    execute_process(COMMAND cat /etc/os-release
        OUTPUT_VARIABLE LSB_RELEASE_ID_SHORT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    string(REGEX MATCH "ID=\"([a-z])+\"" OS_VERSION "${LSB_RELEASE_ID_SHORT}")
    string(REGEX MATCH "\"([a-z])+\"" OS_VERSION "${OS_VERSION}")
    message("OS version is ${OS_VERSION}")
else()
    # Handle the case for MacOS
    # TBD
endif()

# Default to four jobs if the command fails.
if(NPROC_RESULT)
    message (WARNING "Unable to detect number of processors. Building nGraph with make -j4.")
    set(NUM_MAKE_PROCESSES 4)
endif()

# Need to setup the RPATH here - else it won't work.
# During installation, a Python pip package is created which when
# installed is located in the same level as the tensorflow directory
# site-packages/
#     /ngraph
#       libngraph_bridge.so
#       ...
#     /tensorflow
#       libtensorflow_framework.so.1
#       python/
#           _pywrap....so
# Therefore we are setting two entries in the RPATH:
# 1. $ORIGIN/.
# 2. $ORIGIN/../tensorflow/
#
set(CMAKE_BUILD_RPATH_USE_ORIGIN true)
if(APPLE)
    set(CMAKE_MACOSX_RPATH 1)
    set(CMAKE_INSTALL_RPATH "@loader_path/;@loader_path/../tensorflow;")
elseif(DEFINED NGRAPH_TF_RPATH)
    set(CMAKE_INSTALL_RPATH "\$ORIGIN:\$ORIGIN/../tensorflow:${NGRAPH_TF_RPATH}")
else()
    set(CMAKE_INSTALL_RPATH "\$ORIGIN:\$ORIGIN/../tensorflow")
endif()

# Find TensorFlow
find_package(TensorFlow REQUIRED)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(NGRAPH_TF_CXX11_ABI "${TensorFlow_CXX_ABI}")
    message( STATUS "nGraph-TensorFlow using CXX11 ABI:  ${NGRAPH_TF_CXX11_ABI}" )
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_GLIBCXX_USE_CXX11_ABI=${NGRAPH_TF_CXX11_ABI}")
endif()

if(APPLE)
    set(LIBNGRAPH "libngraph.dylib")
else()
    set(LIBNGRAPH "libngraph.so")
endif(APPLE)

# Build options
option(UNIT_TEST_ENABLE "Control the building of unit tests" FALSE)
option(UNIT_TEST_TF_CC_DIR "Location where TensorFlow CC library is located" FALSE)
option(USE_OPENVINO_FROM_LOCATION "Use OpenVINO located in OPENVINO_ARTIFACTS_DIR" FALSE)
option(OPENVINO_ARTIFACTS_DIR "Where would OpenVINO be installed after build" FALSE)

set(InferenceEngine_DIR ${OPENVINO_ARTIFACTS_DIR}/deployment_tools/inference_engine/share)
find_package(InferenceEngine REQUIRED)
include_directories(${InferenceEngine_INCLUDE_DIRS})
if (THREADING STREQUAL "TBB" OR THREADING STREQUAL "TBB_AUTO")
    set(TBB_DIR ${OPENVINO_ARTIFACTS_DIR}/deployment_tools/inference_engine/external/tbb/cmake)
    find_package(TBB COMPONENTS tbb tbbmalloc)
endif()

message(STATUS "UNIT_TEST_TF_CC_DIR:        ${TF_PRE_BUILT_LOCATION}")
if(UNIT_TEST_TF_CC_DIR)
    # Check if the path specified is ABSOLUTE or RELATVE
    if (NOT IS_ABSOLUTE ${UNIT_TEST_TF_CC_DIR})
        set(UNIT_TEST_TF_CC_DIR ${CMAKE_CURRENT_BINARY_DIR}/${UNIT_TEST_TF_CC_DIR})
    endif()

    # Create absolute path for the directory
    get_filename_component(
        TF_PRE_BUILT_LOCATION
        "${UNIT_TEST_TF_CC_DIR}"
        ABSOLUTE
    )
    if (NOT EXISTS ${TF_PRE_BUILT_LOCATION})
        message(FATAL_ERROR
            "TensorFlow pre-built directory doesn't exist: " ${TF_PRE_BUILT_LOCATION} )
    endif()
endif()

message(STATUS "UNIT_TEST_ENABLE:           ${UNIT_TEST_ENABLE}")
message(STATUS "OPENVINO_ARTIFACTS_DIR:     ${OPENVINO_ARTIFACTS_DIR}")
message(STATUS "USE_PRE_BUILT_OPENVINO:       ${USE_PRE_BUILT_OPENVINO}")

if(OS_VERSION STREQUAL "\"centos\"")
    set(LIB "lib64")
else()
    set(LIB "lib")
endif()

set(NGRAPH_INSTALL_DIR ${OPENVINO_ARTIFACTS_DIR})
set(NGRAPH_IMPORTED_LOCATION ${NGRAPH_INSTALL_DIR}/${LIB}/${LIBNGRAPH})
add_library(ngraph_lib SHARED IMPORTED)
set_target_properties(
    ngraph_lib
    PROPERTIES IMPORTED_LOCATION
    ${NGRAPH_IMPORTED_LOCATION}
)

SET(BASEPATH "${CMAKE_SOURCE_DIR}")
INCLUDE_DIRECTORIES("${BASEPATH}")

# Add the directories to be built
add_subdirectory(third-party)
add_subdirectory(ngraph_bridge)
add_subdirectory(tools)

# The following targets depend on the Tensorflow source code directory
# Get the absolute file name for the source
get_filename_component(
    TensorFlow_SRC_DIR
    "${TF_SRC_DIR}"
    ABSOLUTE
)

add_subdirectory(examples)
if (DEFINED TF_SRC_DIR)
    message(STATUS "TensorFlow_SRC_DIR: ${TensorFlow_SRC_DIR}")
    add_subdirectory(examples/cpp)
else()
    message(
        STATUS 
        "TensorFlow source directory not provided. "
        "C++ Examples won't be built"
    )
endif()

if (UNIT_TEST_ENABLE)
    if (NOT DEFINED TF_SRC_DIR)
        message(
            STATUS
            "TensorFlow source directory not provided. "
            "C++ unit tests won't be built"
        )
    else()
        if (NOT EXISTS ${TensorFlow_SRC_DIR})
            message(
                STATUS 
                "TensorFlow source directory doesn't exist"
            )
        endif()

        # Check if the path specified is ABSOLUTE or RELATVE
        if (NOT IS_ABSOLUTE ${TF_SRC_DIR})
            set(TF_SRC_DIR ${CMAKE_CURRENT_BINARY_DIR}/${TF_SRC_DIR})
        endif()
    endif()

    add_subdirectory(test)
    message(STATUS "unit tests enabled")
endif()

