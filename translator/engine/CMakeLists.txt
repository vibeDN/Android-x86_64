# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.16)
project(armtr_engine C CXX)

set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(UNICORN_DIR "/mnt/storage-adata/translator-research/unicorn" CACHE PATH "Unicorn 2.1.4 tree (src/ + build/)")
set(ARMTR_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

add_library(armtr_engine_obj OBJECT engine.cc uc_repair.c)
set_property(TARGET armtr_engine_obj PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(armtr_engine_obj PUBLIC ${ARMTR_INCLUDE}
  PRIVATE ${UNICORN_DIR}/src/include ${UNICORN_DIR}/src/qemu/include ${UNICORN_DIR}/src/qemu
          ${UNICORN_DIR}/src/glib_compat ${UNICORN_DIR}/build)
target_compile_options(armtr_engine_obj PRIVATE -Wall)

find_library(UNICORN_LIB unicorn PATHS ${UNICORN_DIR}/build NO_DEFAULT_PATH REQUIRED)
find_package(Threads REQUIRED)

add_library(armtr_engine SHARED $<TARGET_OBJECTS:armtr_engine_obj>)
add_library(armtr_engine_static STATIC $<TARGET_OBJECTS:armtr_engine_obj>)
set_target_properties(armtr_engine_static PROPERTIES OUTPUT_NAME armtr_engine)
foreach(t armtr_engine armtr_engine_static)
  target_include_directories(${t} PUBLIC ${ARMTR_INCLUDE})
  target_link_libraries(${t} PUBLIC ${UNICORN_LIB} Threads::Threads)
endforeach()
set_target_properties(armtr_engine PROPERTIES BUILD_RPATH ${UNICORN_DIR}/build)

# --- tests ---
enable_testing()
set(AOSP_CLANG_GLOB "/mnt/storage-adata/aosp15/prebuilts/clang/host/linux-x86/clang-r*/bin")
file(GLOB AOSP_CLANG_BINS ${AOSP_CLANG_GLOB})
find_program(ARM_CLANG clang PATHS ${AOSP_CLANG_BINS} NO_DEFAULT_PATH)
find_program(ARM_OBJCOPY llvm-objcopy PATHS ${AOSP_CLANG_BINS} NO_DEFAULT_PATH)
if(NOT ARM_CLANG OR NOT ARM_OBJCOPY)
  message(WARNING "AOSP clang not found: engine tests disabled")
  return()
endif()
add_custom_command(OUTPUT guest.bin
  COMMAND ${ARM_CLANG} --target=aarch64-linux-gnu -nostdlib -c -x assembler
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest.S -o guest.o
  COMMAND ${ARM_OBJCOPY} -O binary -j .text guest.o guest.bin
  DEPENDS tests/guest.S)
add_custom_target(guest_bin DEPENDS guest.bin)
foreach(t test_engine bench_engine)
  add_executable(${t} tests/${t}.cc)
  target_link_libraries(${t} armtr_engine)
  add_dependencies(${t} guest_bin)
  target_compile_definitions(${t} PRIVATE GUEST_BIN="${CMAKE_CURRENT_BINARY_DIR}/guest.bin")
endforeach()
add_test(NAME engine COMMAND test_engine)
# Stock Unicorn has curr_cflags()==0 (no CF_PARALLEL): cross-engine atomics are broken. scripts/fetch-unicorn.sh patches it.
file(STRINGS ${UNICORN_DIR}/src/qemu/include/exec/exec-all.h _par REGEX "return CF_PARALLEL")
if(NOT _par)
  message(WARNING "Unicorn at ${UNICORN_DIR} is not CF_PARALLEL-patched: atomics across guest threads are not atomic")
  set_tests_properties(engine PROPERTIES ENVIRONMENT ARMTR_UNPATCHED_UNICORN=1)
endif()
