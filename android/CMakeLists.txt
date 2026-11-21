# God of War: Ascension -- Android ARM64 build of the ps3kit runtime.
#
# Standalone: it does not use ps3kit/CMakeLists.txt (Windows/clang-cl oriented)
# and does not modify it. It compiles the same sources (the file globs below
# mirror ps3kit/CMakeLists.txt) for aarch64-linux-android with SDL3 and the
# Vulkan backend. Configure with the NDK toolchain file, see android/README.md.
cmake_minimum_required(VERSION 3.22)
project(gowa_android C CXX ASM)

if(NOT ANDROID)
    message(FATAL_ERROR "Use the Android NDK toolchain file (android/build.sh does).")
endif()

set(PS3KIT "${CMAKE_CURRENT_SOURCE_DIR}/../ps3kit" CACHE PATH "ps3kit source tree")
set(SDL3_ROOT "" CACHE PATH "SDL3 prefix with include/ and lib/libSDL3.so")
# Snapdragon 7s Gen 2 (Redmi Note 13 Pro 5G), read from /proc/cpuinfo on the device: 4x Cortex-A78 (cpu4-7,
# 2.4 GHz) + 4x Cortex-A55 (cpu0-3, 1.96 GHz); ARMv8.2-A with fp16 and dotprod, NO sve/sve2/bf16/i8mm.
# (-mcpu=cortex-a710 compiles but dies with SIGILL there.)
set(GOWA_ARCH_FLAGS "-mcpu=cortex-a78" CACHE STRING "CPU target for the big cores")
option(GOWA_SPU_FASTFP "SPU float ops through the FPCR RZ|FZ fast path (spu_helpers.h; bit-exact vs the exact path, experimental)" OFF)
set(GOWA_LIFTED_DIR "" CACHE PATH "Directory with the lifter output (ppu_recomp.h, ...). Empty = runtime library only")

# --- sources (mirrors ps3kit/CMakeLists.txt) ------------------------------
set(_dirs system filesystem input audio video network spurs sync codec font misc hardware)
set(HLE_SOURCES)
foreach(d ${_dirs})
    file(GLOB _s "${PS3KIT}/libs/${d}/*.c")
    list(APPEND HLE_SOURCES ${_s})
endforeach()
list(APPEND HLE_SOURCES "${PS3KIT}/libs/codec/edge_inflate.cpp" "${PS3KIT}/libs/video/rsx_shader_spirv.cpp"
                        "${PS3KIT}/libs/video/rsx_shader_msl.cpp")
file(GLOB _diag "${PS3KIT}/runtime/diag/*.c")
list(APPEND HLE_SOURCES ${_diag})
file(GLOB AT3_SOURCES "${PS3KIT}/third_party/at3_standalone/*.cpp")
set_source_files_properties(${AT3_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
list(APPEND HLE_SOURCES ${AT3_SOURCES})
# Windows/Apple-only RSX backends
list(FILTER HLE_SOURCES EXCLUDE REGEX "/tests/|rsx_d3d12_backend\\.c$|rsx_metal_backend")

file(GLOB_RECURSE RUNTIME_SOURCES "${PS3KIT}/runtime/*.c" "${PS3KIT}/runtime/*.cpp")
# spu_dispatch_mt.c needs musttail and is compiled inside the game target (like on Windows)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/tests/|/runtime/ppu/|/runtime/host/|/runtime/diag/|spu_dispatch_mt\\.c$")

add_library(ps3kit_android STATIC ${HLE_SOURCES} ${RUNTIME_SOURCES})
target_include_directories(ps3kit_android PUBLIC ${PS3KIT}/include)
file(GLOB _inc LIST_DIRECTORIES true "${PS3KIT}/libs/*" "${PS3KIT}/runtime/*")
foreach(d ${_inc})
    if(IS_DIRECTORY ${d}) 
        target_include_directories(ps3kit_android PRIVATE ${d})
    endif()
endforeach()
target_include_directories(ps3kit_android PRIVATE ${PS3KIT}/runtime ${PS3KIT}/libs)

target_compile_definitions(ps3kit_android PUBLIC PS3_SDL3=1 _GNU_SOURCE)
if(GOWA_SPU_FASTFP)
    # PUBLIC: every translation unit that includes spu_helpers.h (the lifted SPU code too) must agree
    target_compile_definitions(ps3kit_android PUBLIC SPU_A64_FASTFP=1)
endif()
target_compile_options(ps3kit_android PRIVATE
    -O2 -fno-strict-aliasing -fdeclspec
    # PPC fmadd must stay a fused op only where the lifter says so; arm64 clang
    # would otherwise contract a*b+c on its own (see ps3kit/CMakeLists.txt)
    -ffp-contract=off
    ${GOWA_ARCH_FLAGS}
    -w)

# --- SDL3 -----------------------------------------------------------------
if(SDL3_ROOT)
    add_library(SDL3::SDL3 SHARED IMPORTED)
    set_target_properties(SDL3::SDL3 PROPERTIES
        IMPORTED_LOCATION "${SDL3_ROOT}/lib/libSDL3.so"
        INTERFACE_INCLUDE_DIRECTORIES "${SDL3_ROOT}/include")
    target_link_libraries(ps3kit_android PUBLIC SDL3::SDL3)
else()
    message(FATAL_ERROR "Pass -DSDL3_ROOT=<prefix> (include/SDL3, lib/libSDL3.so)")
endif()
# Vulkan headers come from the NDK sysroot; the backend dlopens libvulkan.so at run time.
target_link_libraries(ps3kit_android PUBLIC log android dl m)

# --- benchmarks (no game needed) --------------------------------------------
add_subdirectory(bench)
