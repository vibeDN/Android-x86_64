# SPDX-License-Identifier: GPL-2.0-or-later
# cmake -S translator/runner -B /mnt/storage-adata/tmp/run-build && cmake --build /mnt/storage-adata/tmp/run-build -j2
# Builds the four modules from source into one static-ish host program, armtr-run.
cmake_minimum_required(VERSION 3.16)
project(armtr_runner C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
get_filename_component(T ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
# Must be the CF_PARALLEL-patched build from scripts/fetch-unicorn.sh.
set(UNICORN_DIR ${T}/third_party-build/unicorn CACHE PATH "Unicorn tree (src/ + build/)")
find_library(UNICORN_LIB unicorn PATHS ${UNICORN_DIR}/build NO_DEFAULT_PATH REQUIRED)
find_path(ARMTR_JNI_INCLUDE_DIR jni.h HINTS /mnt/storage-adata/aosp15/libnativehelper/include_jni $ENV{JAVA_HOME}/include)
find_package(Threads REQUIRED)

add_executable(armtr-run armtr_run.cpp ${T}/runtime/guest_rt.cpp
  ${T}/engine/engine.cc ${T}/engine/uc_repair.c
  ${T}/loader/loader.cpp ${T}/syscalls/syscalls.cpp
  ${T}/proxy/proxy_core.cpp ${T}/proxy/proxy_va.cpp ${T}/proxy/proxy_jni.cpp)
target_include_directories(armtr-run PRIVATE ${T}/include ${T}/runtime ${T}/loader/include ${T}/syscalls ${T}/proxy ${ARMTR_JNI_INCLUDE_DIR})
set_source_files_properties(${T}/engine/engine.cc ${T}/engine/uc_repair.c PROPERTIES INCLUDE_DIRECTORIES
  "${UNICORN_DIR}/src/include;${UNICORN_DIR}/src/qemu/include;${UNICORN_DIR}/src/qemu;${UNICORN_DIR}/src/glib_compat;${UNICORN_DIR}/build")
target_compile_options(armtr-run PRIVATE -Wall)
target_link_libraries(armtr-run ${UNICORN_LIB} Threads::Threads dl)
set_target_properties(armtr-run PROPERTIES BUILD_RPATH ${UNICORN_DIR}/build)
