# SPDX-License-Identifier: GPL-2.0-or-later
# Standalone: cmake -S translator/loader -B build/loader && cmake --build build/loader -j2 && ctest --test-dir build/loader
cmake_minimum_required(VERSION 3.16)
project(armtr_loader CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_library(armtr_loader STATIC loader.cpp)
target_include_directories(armtr_loader PUBLIC include ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_options(armtr_loader PRIVATE -Wall -Wextra)

enable_testing()
add_executable(loader_test tests/loader_test.cpp tests/fake_engine.cpp)
target_link_libraries(loader_test armtr_loader)
target_include_directories(loader_test PRIVATE tests)

set(GUEST_DIR ${CMAKE_BINARY_DIR}/guest)
add_test(NAME build_guest COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/build_guest.sh ${GUEST_DIR})
add_test(NAME loader_synth COMMAND loader_test synth ${GUEST_DIR})
set_tests_properties(build_guest PROPERTIES FIXTURES_SETUP guest SKIP_RETURN_CODE 77)
set_tests_properties(loader_synth PROPERTIES FIXTURES_REQUIRED guest)

# Real libraries (skipped when absent). READELF cross-checks the relocation count.
set(READELF_GLOB /mnt/storage-adata/aosp15/prebuilts/clang/host/linux-x86/clang-r574158/bin/llvm-readelf)
set(REAL_LIBS /mnt/storage-adata/translator-research/tests-out/libjni_test.so)
file(GLOB YOUGRAM_LIBS /mnt/storage-adata/translator-research/yougram-libs/*.so)
if(EXISTS ${REAL_LIBS})
  add_test(NAME loader_real_jni COMMAND loader_test real -r ${READELF_GLOB} ${REAL_LIBS})
endif()
if(YOUGRAM_LIBS)
  add_test(NAME loader_real_yougram COMMAND loader_test real -r ${READELF_GLOB} ${YOUGRAM_LIBS})
endif()
