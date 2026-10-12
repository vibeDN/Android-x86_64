# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.16)
if(NOT PROJECT_NAME)
  project(armtr_proxy CXX)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# jni.h: not vendored (Apache-2.0). Provide -DARMTR_JNI_INCLUDE_DIR=... or it is searched in AOSP / JDK.
find_path(ARMTR_JNI_INCLUDE_DIR jni.h
  HINTS /mnt/storage-adata/aosp15/libnativehelper/include_jni $ENV{JAVA_HOME}/include)

add_library(armtr_proxy STATIC proxy_core.cpp proxy_va.cpp proxy_jni.cpp)
target_include_directories(armtr_proxy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_include_directories(armtr_proxy PRIVATE ${ARMTR_JNI_INCLUDE_DIR})
target_compile_options(armtr_proxy PRIVATE -Wall -Wextra)

# Tests run against a fake engine (tests/fake_engine.cpp), not the real one.
if(NOT DEFINED ARMTR_PROXY_TESTS OR ARMTR_PROXY_TESTS)
  enable_testing()
  find_package(Threads REQUIRED)
  add_library(armtr_fake_engine STATIC tests/fake_engine.cpp)
  target_include_directories(armtr_fake_engine PUBLIC tests ../include)
  foreach(t adapter va jni guest)
    add_executable(test_proxy_${t} tests/test_${t}.cpp)
    target_include_directories(test_proxy_${t} PRIVATE ${ARMTR_JNI_INCLUDE_DIR})
    target_link_libraries(test_proxy_${t} armtr_proxy armtr_fake_engine Threads::Threads)
    add_test(NAME proxy_${t} COMMAND test_proxy_${t})
  endforeach()
endif()
