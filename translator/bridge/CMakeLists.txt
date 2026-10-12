# SPDX-License-Identifier: GPL-2.0-or-later
# Host (glibc) build of the NativeBridge for tests:
#   cmake -S translator/bridge -B /mnt/storage-adata/tmp/bridge-build && cmake --build /mnt/storage-adata/tmp/bridge-build -j2
#   translator/bridge/tests/run_tests.sh /mnt/storage-adata/tmp/bridge-build
# The device build is Soong (device/ours/pc_x86_64/translator/Android.bp).
cmake_minimum_required(VERSION 3.16)
project(armtr_bridge C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
get_filename_component(T ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
set(UNICORN_DIR ${T}/third_party-build/unicorn CACHE PATH "Unicorn tree (src/ + build/)")
find_library(UNICORN_LIB unicorn PATHS ${UNICORN_DIR}/build NO_DEFAULT_PATH REQUIRED)
# Apache-2.0 AOSP headers, not vendored.
find_path(ARMTR_JNI_INCLUDE_DIR jni.h HINTS /mnt/storage-adata/aosp15/libnativehelper/include_jni $ENV{JAVA_HOME}/include)
find_path(ARMTR_NB_INCLUDE_DIR nativebridge/native_bridge.h HINTS /mnt/storage-adata/aosp15/art/libnativebridge/include)
find_package(Threads REQUIRED)

add_library(armtr_bridge SHARED native_bridge.cc jni_thunks_x86_64.S ${T}/runtime/guest_rt.cpp
  ${T}/engine/engine.cc ${T}/engine/uc_repair.c ${T}/loader/loader.cpp ${T}/syscalls/syscalls.cpp
  ${T}/proxy/proxy_core.cpp ${T}/proxy/proxy_va.cpp ${T}/proxy/proxy_jni.cpp ${T}/hostlibs/hostlibs.cpp)
target_include_directories(armtr_bridge PRIVATE ${T}/include ${T}/runtime ${T}/loader/include ${T}/syscalls ${T}/proxy ${T}/hostlibs
  ${ARMTR_JNI_INCLUDE_DIR} ${ARMTR_NB_INCLUDE_DIR})
set_source_files_properties(${T}/engine/engine.cc ${T}/engine/uc_repair.c PROPERTIES INCLUDE_DIRECTORIES
  "${UNICORN_DIR}/src/include;${UNICORN_DIR}/src/qemu/include;${UNICORN_DIR}/src/qemu;${UNICORN_DIR}/src/glib_compat;${UNICORN_DIR}/build")
target_compile_options(armtr_bridge PRIVATE -Wall -fvisibility=hidden)
target_link_libraries(armtr_bridge ${UNICORN_LIB} Threads::Threads dl)
target_link_options(armtr_bridge PRIVATE -Wl,--version-script=${T}/../device/ours/pc_x86_64/translator/libarmtr_bridge.map)
set_target_properties(armtr_bridge PROPERTIES OUTPUT_NAME armtr_bridge BUILD_RPATH ${UNICORN_DIR}/build)

add_executable(bridge_host_test tests/bridge_host_test.cpp)
target_include_directories(bridge_host_test PRIVATE ${ARMTR_JNI_INCLUDE_DIR} ${ARMTR_NB_INCLUDE_DIR})
target_link_libraries(bridge_host_test dl Threads::Threads)

# Fake host liblog for the guest -> liblog stub -> hostlibs -> host test (ARMTR_HOSTLIBS_DIR).
add_library(fake_log SHARED ${T}/hostlibs/tests/fake_log.cpp)
set_target_properties(fake_log PROPERTIES OUTPUT_NAME log LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fakelibs)

add_executable(probe_lib tests/probe_lib.cpp)
target_include_directories(probe_lib PRIVATE ${T}/proxy ${ARMTR_JNI_INCLUDE_DIR} ${ARMTR_NB_INCLUDE_DIR})
target_link_libraries(probe_lib dl)
