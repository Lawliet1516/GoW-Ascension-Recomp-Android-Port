# Cross-compiles the lifted game code (made by android/lift/prepare.sh from YOUR OWN EBOOT) for Android arm64.
#
# Separate from android/CMakeLists.txt on purpose: that project builds the runtime without any game data and must keep building on a
# machine that has none. This one needs the lift and a big machine: each lifted PPU file takes 2-4 GB of RAM in clang.
#
#   cmake -S android/lift -B build-lifted -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a \
#         -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release -DLIFT_JOBS=4
#
# Output: libgow_ppu.a (PPU, C++), libgow_spu.a (SPU, C). They are linked with libps3kit_android.a and the port by the app build (not done yet).
cmake_minimum_required(VERSION 3.22)
project(gow_lifted C CXX)

set(LIFT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../ascension/src" CACHE PATH "directory made by prepare.sh (recomp/ and spu/)")
set(PS3KIT "${CMAKE_CURRENT_SOURCE_DIR}/../../ps3kit" CACHE PATH "ps3kit")
set(LIFT_JOBS 2 CACHE STRING "parallel compiles of lifted files: about RAM(GB) / 4")
set(LIFT_ARCH_FLAGS "-mcpu=cortex-a78" CACHE STRING "big-core target (Cortex-A78, ARMv8.2, no SVE: see android/RESULTS.md)")
option(LIFT_SPU_FASTFP "lifted SPU code built for the FPCR float fast path (must match libps3kit_android.a: GOWA_SPU_FASTFP)" OFF)
option(LIFT_PPU_NO_PEELING "-mllvm -unroll-allow-peeling=false (the Windows build needs it with register-cached lifts: one function took >15 min and 2.6 GB)" OFF)

file(GLOB PPU_SOURCES CONFIGURE_DEPENDS "${LIFT_ROOT}/recomp/*.cpp" "${LIFT_ROOT}/recomp/*.c")
file(GLOB_RECURSE SPU_SOURCES CONFIGURE_DEPENDS "${LIFT_ROOT}/spu/*.c")
if(NOT PPU_SOURCES OR NOT EXISTS "${LIFT_ROOT}/recomp/ppu_recomp.h")
    message(FATAL_ERROR "No lifted PPU code in ${LIFT_ROOT}/recomp. Run android/lift/prepare.sh with your own decrypted EBOOT.ELF first "
                        "(see android/lift/README.md). The lifted code is never part of this repository.")
endif()

set_property(GLOBAL PROPERTY JOB_POOLS lift=${LIFT_JOBS})
set(CMAKE_JOB_POOL_COMPILE lift)

add_library(gow_ppu STATIC ${PPU_SOURCES})
target_include_directories(gow_ppu PRIVATE "${LIFT_ROOT}/recomp" ${PS3KIT}/include ${PS3KIT}/runtime ${PS3KIT}/runtime/ppu ${PS3KIT}/runtime/platform)
# as ascension/CMakeLists.txt does for the lifted PPU files: warnings off, no contraction (only the lifter's own fused ops fuse)
target_compile_options(gow_ppu PRIVATE -w -O2 -ffp-contract=off -fno-strict-aliasing ${LIFT_ARCH_FLAGS})
if(LIFT_PPU_NO_PEELING)
    target_compile_options(gow_ppu PRIVATE -mllvm -unroll-allow-peeling=false)
endif()

add_library(gow_spu STATIC ${SPU_SOURCES})
target_include_directories(gow_spu PRIVATE "${LIFT_ROOT}/spu" ${PS3KIT}/include ${PS3KIT}/runtime/spu ${PS3KIT}/runtime/platform)
target_compile_options(gow_spu PRIVATE -w -O2 -fwrapv -ffp-contract=off ${LIFT_ARCH_FLAGS})
if(LIFT_SPU_FASTFP)
    target_compile_definitions(gow_spu PRIVATE SPU_A64_FASTFP=1)
endif()
message(STATUS "lifted sources: ${PPU_SOURCES} PPU files, SPU files: see target; jobs=${LIFT_JOBS}")
