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
set(GOWA_LIFTED_DIR "" CACHE PATH "Directory with the lifter output (ppu_recomp.h, ...). Empty = runtime library only")

# --- sources (mirrors ps3kit/CMakeLists.txt) ------------------------------
# the Windows port globs libs/ recursively (libs/network/psnr/psnr.c lives in a subdirectory)
file(GLOB_RECURSE HLE_SOURCES "${PS3KIT}/libs/*.c")
list(APPEND HLE_SOURCES "${PS3KIT}/libs/codec/edge_inflate.cpp" "${PS3KIT}/libs/video/rsx_shader_spirv.cpp"
                        "${PS3KIT}/libs/video/rsx_shader_msl.cpp")
file(GLOB _diag "${PS3KIT}/runtime/diag/*.c")
list(APPEND HLE_SOURCES ${_diag})
file(GLOB AT3_SOURCES "${PS3KIT}/third_party/at3_standalone/*.cpp")
set_source_files_properties(${AT3_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
list(APPEND HLE_SOURCES ${AT3_SOURCES})
# Windows/Apple-only RSX backends
list(FILTER HLE_SOURCES EXCLUDE REGEX "/tests/|rsx_d3d12_backend\\.c$|rsx_metal_backend")

# --- renderer migration (branch android/renderer-upstream): the video layer from render/libs/video (pinned upstream v0.20.0 + the Ascension
# changes reapplied so far, see android/render/MERGE_LOG.md) instead of ps3kit/libs/video. ps3kit/libs/video is not touched either way.
option(GOWA_RENDER_UPSTREAM "build the video layer from render/libs/video" OFF)
set(GLSLANG_PREFIX "" CACHE PATH "arm64 glslang install prefix (HLSL front end), needed by render/libs/video/rsx_shader_spirv.cpp")
set(SDL2_PREFIX "" CACHE PATH "arm64 SDL2 (static) prefix, for the pure-upstream video layer which calls SDL2 directly (scene tests)")
if(GOWA_RENDER_UPSTREAM)
    list(FILTER HLE_SOURCES EXCLUDE REGEX "/ps3kit/libs/video/|/libs/video/rsx_shader_spirv")
    file(GLOB RENDER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../render/libs/video/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/../render/libs/video/*.cpp")
    # not built yet: Apple's MSL translator; fork-only Ascension files whose APIs live in decompiler/dispatch changes not reapplied yet
    list(FILTER RENDER_SOURCES EXCLUDE REGEX "rsx_shader_msl|rsx_fp_uber|rsx_texpack|tex_stream_diag")
    list(APPEND HLE_SOURCES ${RENDER_SOURCES})
    # what upstream's CMake sets when PS3RECOMP_RSX_VULKAN is on and glslang is found
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../render/libs/video/rsx_shader_spirv.cpp" PROPERTIES COMPILE_DEFINITIONS PS3RECOMP_HAVE_SPIRV_TRANSLATION=1)
endif()

file(GLOB_RECURSE RUNTIME_SOURCES "${PS3KIT}/runtime/*.c" "${PS3KIT}/runtime/*.cpp")
# spu_dispatch_mt.c needs musttail and is compiled inside the game target (like on Windows)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/tests/|/runtime/ppu/|/runtime/host/|/runtime/diag/|spu_dispatch_mt\\.c$")

add_library(ps3kit_android STATIC ${HLE_SOURCES} ${RUNTIME_SOURCES})
# Bring-up stand-ins for the symbols the Windows port provides itself (see the file for the list). Not part of the library, and **off by default**:
# a build that needs them must say so (-DGOWA_BRINGUP_STUBS=ON), and every stub function announces itself when it first runs.
option(GOWA_BRINGUP_STUBS "build libgowa_bringup_stubs.a (bring-up tests only; NOT implementations)" OFF)
if(GOWA_BRINGUP_STUBS)
    add_library(gowa_bringup_stubs STATIC "${CMAKE_CURRENT_SOURCE_DIR}/host/port_stubs.c")
endif()
# The game-agnostic host pieces upstream keeps out of the library (inert reservation/loader hooks) for hosts without a game
add_library(gowa_host_stubs STATIC "${PS3KIT}/runtime/host/host_stubs.c")
target_include_directories(gowa_host_stubs PRIVATE ${PS3KIT}/include ${PS3KIT}/runtime ${PS3KIT}/runtime/platform ${PS3KIT}/runtime/ppu)
if(GOWA_RENDER_UPSTREAM)
    target_include_directories(ps3kit_android PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../render/include")   # before ps3kit/include on purpose
    target_include_directories(ps3kit_android PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../render/libs/video" ${GLSLANG_PREFIX}/include)
endif()
target_include_directories(ps3kit_android PUBLIC ${PS3KIT}/include)
file(GLOB _inc LIST_DIRECTORIES true "${PS3KIT}/libs/*" "${PS3KIT}/runtime/*")
foreach(d ${_inc})
    if(IS_DIRECTORY ${d}) 
        target_include_directories(ps3kit_android PRIVATE ${d})
    endif()
endforeach()
target_include_directories(ps3kit_android PRIVATE ${PS3KIT}/runtime ${PS3KIT}/libs)

if(GOWA_RENDER_UPSTREAM)
    target_compile_definitions(ps3kit_android PUBLIC PS3RECOMP_RSX_VULKAN)    # the hosts select the Vulkan backend on this
endif()
if(GOWA_RENDER_UPSTREAM AND SDL2_PREFIX)
    # the pure-upstream video layer calls SDL2 directly; one SDL version per binary (SDL2 and SDL3 export the same names)
    target_compile_definitions(ps3kit_android PUBLIC _GNU_SOURCE)
    target_include_directories(ps3kit_android PUBLIC ${SDL2_PREFIX}/include)
else()
    target_compile_definitions(ps3kit_android PUBLIC PS3_SDL3=1 _GNU_SOURCE)
endif()
target_compile_options(ps3kit_android PRIVATE
    -O2 -fno-strict-aliasing -fdeclspec
    # PPC fmadd must stay a fused op only where the lifter says so; arm64 clang
    # would otherwise contract a*b+c on its own (see ps3kit/CMakeLists.txt)
    -ffp-contract=off
    ${GOWA_ARCH_FLAGS}
    -w)

# --- SDL ---------------------------------------------------------------------
if(GOWA_RENDER_UPSTREAM AND SDL2_PREFIX)
    target_link_libraries(ps3kit_android PUBLIC ${SDL2_PREFIX}/lib/libSDL2.a)
elseif(SDL3_ROOT)
    add_library(SDL3::SDL3 SHARED IMPORTED)
    set_target_properties(SDL3::SDL3 PROPERTIES
        IMPORTED_LOCATION "${SDL3_ROOT}/lib/libSDL3.so"
        INTERFACE_INCLUDE_DIRECTORIES "${SDL3_ROOT}/include")
    target_link_libraries(ps3kit_android PUBLIC SDL3::SDL3)
else()
    message(FATAL_ERROR "Pass -DSDL3_ROOT=<prefix> (include/SDL3, lib/libSDL3.so)")
endif()
if(GOWA_RENDER_UPSTREAM AND GLSLANG_PREFIX)
    target_link_libraries(ps3kit_android PUBLIC ${GLSLANG_PREFIX}/lib/libglslang.a ${GLSLANG_PREFIX}/lib/libMachineIndependent.a ${GLSLANG_PREFIX}/lib/libGenericCodeGen.a
        ${GLSLANG_PREFIX}/lib/libSPIRV.a ${GLSLANG_PREFIX}/lib/libglslang-default-resource-limits.a ${GLSLANG_PREFIX}/lib/libOSDependent.a)
endif()
# Vulkan headers come from the NDK sysroot; the backend dlopens libvulkan.so at run time.
target_link_libraries(ps3kit_android PUBLIC log android dl m)

# --- the upstream scene runner (render/host: upstream's host_posix.c + host_stubs.c, unmodified) -------------------------------------------
if(GOWA_RENDER_UPSTREAM)
    add_executable(gowa_render_host "${CMAKE_CURRENT_SOURCE_DIR}/../render/host/host_posix.c" "${CMAKE_CURRENT_SOURCE_DIR}/../render/host/host_stubs.c")
    target_include_directories(gowa_render_host PRIVATE ${PS3KIT}/runtime/host   # upstream host files include ../memory/vm.h etc.: resolved against the fork runtime
        "${CMAKE_CURRENT_SOURCE_DIR}/../render/libs/video" "${CMAKE_CURRENT_SOURCE_DIR}/../render/include" ${PS3KIT}/include ${PS3KIT}/runtime)
    add_library(gowa_render_shims STATIC "${CMAKE_CURRENT_SOURCE_DIR}/host/render_runtime_shims.c")   # see the file: runtime functions newer than the fork's runtime
    set(GOWA_RENDER_BRINGUP_STUBS "")
    if(TARGET gowa_bringup_stubs)
        set(GOWA_RENDER_BRINGUP_STUBS gowa_bringup_stubs)    # g_spu_iojrn_rt (data symbol) and friends; needs -DGOWA_BRINGUP_STUBS=ON
    endif()
    target_link_libraries(gowa_render_host PRIVATE gowa_render_shims ps3kit_android gowa_render_shims ${GOWA_RENDER_BRINGUP_STUBS} OpenSLES)
    set_target_properties(gowa_render_host PROPERTIES LINKER_LANGUAGE CXX)
    # the same runner as a shared library with a JNI entry, to run inside a debuggable APK (debug layers only load in app processes)
    add_library(gowa_scenes SHARED "${CMAKE_CURRENT_SOURCE_DIR}/../render/host/host_posix.c" "${CMAKE_CURRENT_SOURCE_DIR}/../render/host/host_stubs.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/render/apk/native/scene_jni.c")
    target_compile_definitions(gowa_scenes PRIVATE main=gowa_host_main)
    target_include_directories(gowa_scenes PRIVATE ${PS3KIT}/runtime/host "${CMAKE_CURRENT_SOURCE_DIR}/../render/libs/video" "${CMAKE_CURRENT_SOURCE_DIR}/../render/include" ${PS3KIT}/include ${PS3KIT}/runtime)
    target_link_libraries(gowa_scenes PRIVATE gowa_render_shims ps3kit_android gowa_render_shims ${GOWA_RENDER_BRINGUP_STUBS} OpenSLES GLESv1_CM GLESv2 EGL)
    set_target_properties(gowa_scenes PROPERTIES LINKER_LANGUAGE CXX)
    target_link_options(gowa_scenes PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/render/apk/native/exports.map")   # export only the JNI entry so --gc-sections drops what the exe also drops
endif()

# --- benchmarks (no game needed) --------------------------------------------
add_subdirectory(bench)
