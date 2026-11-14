# Ascension: from ps3recomp's templates/project, with its defaults pointed at
# ../ps3recomp, src/recomp and src/spu.
cmake_minimum_required(VERSION 3.20)
project(AscensionRecomp
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "God of War: Ascension (PS3), recompiled on ps3recomp"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: the toolkit checked out beside this repository, as ps3kit (the
    # fork's current name) or ps3recomp.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../ps3kit/CMakeLists.txt")
        get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ps3kit" ABSOLUTE)
    else()
        get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ps3recomp" ABSOLUTE)
    endif()
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
# ASCENSION_PPU_REGCACHE: build the lift made with `ppu_lifter.py --cache-regs`
# (src/recomp-regcache): each lifted function keeps the GPRs/FPRs/CR/XER/CTR it
# uses in C locals instead of reading and writing ctx on every instruction
# (tools/ppu_lifter.py, _cache_regs). OFF: src/recomp (or RECOMP_DIR), the
# classic lift, built exactly as before. Switching rebuilds every lifted file.
option(ASCENSION_PPU_REGCACHE "Lifted PPU code from src/recomp-regcache (ppu_lifter.py --cache-regs)" OFF)
if(ASCENSION_PPU_REGCACHE)
    set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp-regcache")
endif()
# ASCENSION_PPU_LEAN_STORES: build the lift made with
# `ppu_lifter.py --cache-regs --lean-stores` (src/recomp-lean): cheaper guest
# stores and loads, the guest-stack stores above all (ppu_lifter.py,
# _lean_stores; PPU_LEAN_STORES in the generated ppu_recomp.h). Same results.
# Needs the runtime's g_ppu_stk_map (runtime/ppu/ppu_loader.cpp). Takes
# precedence over ASCENSION_PPU_REGCACHE.
option(ASCENSION_PPU_LEAN_STORES "Lifted PPU code from src/recomp-lean (ppu_lifter.py --cache-regs --lean-stores)" OFF)
if(ASCENSION_PPU_LEAN_STORES)
    set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp-lean")
endif()
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
# Profile-guided optimisation (clang). ASCENSION_PGO=GEN: an instrumented
# build that writes LLVM_PROFILE_FILE when the game window is closed (the
# runtime ends the process there; PS3_PGO_GEN makes it flush the counters
# first). ASCENSION_PGO=USE: optimise with ASCENSION_PGO_PROFILE, the
# llvm-profdata merge of those runs. Set before the runtime is added so the
# runtime, the lifted PPU and SPU code and the host all get it.
set(ASCENSION_PGO "" CACHE STRING "Profile-guided optimisation: empty, GEN or USE")
set(ASCENSION_PGO_PROFILE "" CACHE FILEPATH "Merged .profdata for ASCENSION_PGO=USE")
if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND ASCENSION_PGO STREQUAL "GEN")
    add_compile_options(-fprofile-instr-generate)
    add_compile_definitions(PS3_PGO_GEN=1)
    get_filename_component(_llvm_bin "${CMAKE_C_COMPILER}" DIRECTORY)
    file(GLOB _pgo_rt "${_llvm_bin}/../lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib")
    if(NOT _pgo_rt)
        message(FATAL_ERROR "ASCENSION_PGO=GEN: clang_rt.profile-x86_64.lib not found under ${_llvm_bin}/../lib/clang")
    endif()
    list(GET _pgo_rt 0 ASCENSION_PGO_RUNTIME)
    message(STATUS "PGO: instrumented build (${ASCENSION_PGO_RUNTIME})")
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang" AND ASCENSION_PGO STREQUAL "USE")
    if(NOT EXISTS "${ASCENSION_PGO_PROFILE}")
        message(FATAL_ERROR "ASCENSION_PGO=USE needs ASCENSION_PGO_PROFILE (a merged .profdata)")
    endif()
    add_compile_options("-fprofile-instr-use=${ASCENSION_PGO_PROFILE}"
                        -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-profile-instr-missing)
    message(STATUS "PGO: optimising with ${ASCENSION_PGO_PROFILE}")
endif()

add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    # The table is made from the handlers the library sources define, so a
    # handler added there must regenerate it, not only a change to the tools.
    file(GLOB_RECURSE HLE_LIB_SOURCES CONFIGURE_DEPENDS "${PS3RECOMP_DIR}/libs/*.c")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
                ${HLE_LIB_SOURCES}
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)

set(SPU_SOURCES "")
if(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        # Native replacements of lifted SPU functions and their recorder
        # (host/spu/spu_fnrec.h, tools/spu_wrap.py): built like lifted code.
        file(GLOB SPU_HOST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/host/spu/*.c")
        list(APPEND SPU_SOURCES ${SPU_HOST_SOURCES})
        # Extra native SPU sources for a private build (;-separated full paths).
        set(ASCENSION_EXTRA_SPU_SOURCES "" CACHE STRING "Extra native SPU sources (;-separated)")
        list(APPEND SPU_SOURCES ${ASCENSION_EXTRA_SPU_SOURCES})
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c jobmods_register.c pic_register.c taskset_policy_bytes.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

add_executable(${PROJECT_NAME}
    # ps3recomp's full boot harness, as the Twisted Metal port builds it, not
    # the template's minimal main.cpp: it adds the vblank ticker the GCM
    # waits need, the hang watchdog, the PS3_DEBUG console and the
    # PARAM.SFO read at boot.
    "${PS3RECOMP_DIR}/runtime/ppu/tests/boot_main.cpp"
    # Runtime switches this title needs on by default (live draw engine,
    # host Edge Zlib).
    "${CMAKE_CURRENT_SOURCE_DIR}/host/ascension_defaults.cpp"
    # PS3_PATCHES: the game patches, switched when the game starts
    # (tools/rpcs3_patch.py runtime writes host/ascension_patch_table.inc).
    "${CMAKE_CURRENT_SOURCE_DIR}/host/ascension_patches.cpp"
    # PPU_VRAM_GUARD: the VRAM manager's copy/move argument check
    # (tools/vram_guard_patch.py hooks the two lifted functions into it).
    "${CMAKE_CURRENT_SOURCE_DIR}/host/ascension_vram_guard.cpp"
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
)
# The boot harness asks the title before booting (ascension_pre_main in
# host/ascension_defaults.cpp): SPU_FN_REPLAY runs a replay instead of the game.
set_property(SOURCE "${PS3RECOMP_DIR}/runtime/ppu/tests/boot_main.cpp" APPEND PROPERTY COMPILE_DEFINITIONS ASCENSION_PRE_MAIN)

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- the target-wide -O1 exists for the huge PPU TUs; the
#                          SPU sources are small and decode-hot, so they get
#                          full optimization. The last -O on the line wins.
if(SPU_SOURCES)
    # SPU_XFLOAT_APPROX: float add/sub/mul/fma are one SSE instruction with no
    # SPU extended-range check, as RPCS3's default "Approximate" xfloat (see
    # spu_helpers.h). 1.64x on module 218's vertex stage, outputs unchanged on
    # its 48 captures.
    #   Off by default here: with every SPU program built that way the lighting
    #   in the first level went black (Inf/NaN reaching the light jobs).
    # SPU_XFLOAT_RESULT_CHECK (the default): the same single instruction, then a
    # three-instruction look at the RESULT for Inf/NaN/FLT_MAX, which redoes the
    # op on the exact path -- exact results, most of the speed.
    option(ASCENSION_SPU_XFLOAT_APPROX "Lifted SPU float ops without the extended-range check" OFF)
    # SPU_LS_NO_HOOKS: no LS diagnostics in lifted code (SPU_LS_WATCH,
    # SPU_SMC_WATCH, SPU_LS_LOWREAD stop working there; spu_context.h).
    option(ASCENSION_SPU_LS_HOOKS "Keep the LS diagnostic hooks in lifted SPU code" OFF)
    set(SPU_DEFS "SPU_USE_DISPATCH_MT;SPU_ASSUME_SPU_FP")
    if(ASCENSION_SPU_XFLOAT_APPROX)
        string(APPEND SPU_DEFS ";SPU_XFLOAT_APPROX")
    else()
        string(APPEND SPU_DEFS ";SPU_XFLOAT_RESULT_CHECK")
    endif()
    if(NOT ASCENSION_SPU_LS_HOOKS)
        string(APPEND SPU_DEFS ";SPU_LS_NO_HOOKS")
    endif()
    # ASCENSION_SPU_CODEGEN2: faster lifted SPU code with bit-identical results
    # (my-changes/spu-codegen/README.md). SPU_HELPERS2 = exact SSE4.1 forms of
    # spu_helpers.h helpers that were scalar loops, an ldexp call or a stack
    # round trip (cwd/cbd/chd/cdd); /GS- = no stack cookie in lifted functions.
    # The register-local form of the lifted code comes from the relift
    # (spu_lifter.py --xregs, SPU_LIFT_XREGS=1), which works either way.
    option(ASCENSION_SPU_CODEGEN2 "Lifted SPU code: SSE helper forms, no stack cookies" OFF)
    set(SPU_CG2_FLAGS "")
    if(ASCENSION_SPU_CODEGEN2)
        string(APPEND SPU_DEFS ";SPU_HELPERS2")
        if(MSVC)
            set(SPU_CG2_FLAGS ";/GS-")
        endif()
    endif()
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu;${CMAKE_CURRENT_SOURCE_DIR}/host/spu"
        # SPU_ASSUME_SPU_FP: lifted code always runs in the SPU float mode
        # (spu_run_with_halt), so the float ops skip reading MXCSR each time.
        COMPILE_DEFINITIONS "${SPU_DEFS}")
    # The images whose float ops may skip the extended-range handling
    # (SPU_XFLOAT_APPROX) while the rest stay exact: module 218, the per-draw
    # geometry job and the biggest SPU cost in a level, checked unchanged on 48
    # captured jobs. Every image approximate lit the level black.
    set(ASCENSION_SPU_APPROX_IMAGES "jobmod_7868e9b847d1_e6D98" CACHE STRING
        "Lifted SPU images built with SPU_XFLOAT_APPROX (;-separated folder names under src/spu)")
    if(NOT ASCENSION_SPU_XFLOAT_APPROX)
        string(REPLACE "SPU_XFLOAT_RESULT_CHECK" "SPU_XFLOAT_APPROX" SPU_DEFS_APPROX "${SPU_DEFS}")
        foreach(img IN LISTS ASCENSION_SPU_APPROX_IMAGES)
            if(EXISTS "${SPU_DIR}/${img}/spu_recomp.c")
                set_source_files_properties("${SPU_DIR}/${img}/spu_recomp.c" PROPERTIES
                    COMPILE_DEFINITIONS "${SPU_DEFS_APPROX}")
                message(STATUS "  SPU image ${img}: approximate float ops")
            endif()
        endforeach()
    endif()
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # SSE4.1 as well (any x86-64 since ~2008): spu_shufb is pshufb/blendv
        # only under __SSE4_1__, and without it every shufb in lifted code is
        # a 16-byte loop (a third of the job worker in Ascension's profile).
        # The runtime gets the same flag; probed, as Apple arm64 clang rejects it.
        include(CheckCCompilerFlag)
        check_c_compiler_flag(-msse4.1 PORT_HAS_MSSE41)
        # The SPU float fast path (spu_helpers.h) runs with MXCSR rounding toward
        # zero; -frounding-math keeps the compiler from folding float ops on
        # constants under round-to-nearest. clang-cl takes it as /clang:.
        if(MSVC)
            set(SPU_ROUNDING_MATH "/clang:-frounding-math")
        else()
            set(SPU_ROUNDING_MATH "-frounding-math")
        endif()
        # FMA3 (Haswell / Zen and later): spu_fma/fms/fnms are one vfmadd in the
        # SPU float mode instead of the exact double path, and the menu's job
        # list (16 jobs on one SPU per frame) is mostly matrix fma.
        option(ASCENSION_SPU_FMA "Lifted SPU code uses FMA3 (needs a CPU with FMA)" ON)
        check_c_compiler_flag(-mfma PORT_HAS_MFMA)
        set(SPU_FMA_FLAG "")
        if(ASCENSION_SPU_FMA AND PORT_HAS_MFMA)
            # -ffp-contract=off: with FMA available clang would otherwise fuse
            # every a*b+c in the helpers' scalar paths, which the SPU rounds twice.
            set(SPU_FMA_FLAG ";-mfma;-ffp-contract=off")
            # AVX2 too (every FMA3 CPU has it, and the PPU code already needs
            # it: ASCENSION_PPU_AVX2). Measured in the first level's combat:
            # no gain (24.7 / 25.0 fps against 26.0 without), so off.
            option(ASCENSION_SPU_AVX2 "Lifted SPU code uses AVX2" OFF)
            if(ASCENSION_SPU_AVX2)
                string(APPEND SPU_FMA_FLAG ";-mavx2")
            endif()
        endif()
        # No warnings for generated SPU code: with registers promoted to locals
        # (SPU_LIFT_PROMOTE) a line flushes dozens of them, every warning
        # reprints its line, and the gigabytes of text stalled ninja for an
        # hour with the compilers idle.
        if(MSVC)
            set(SPU_NOWARN "/w")
        else()
            set(SPU_NOWARN "-w")
        endif()
        if(PORT_HAS_MSSE41)
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-msse4.1${SPU_FMA_FLAG};${SPU_ROUNDING_MATH};${SPU_NOWARN}${SPU_CG2_FLAGS}")
        else()
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;${SPU_ROUNDING_MATH};${SPU_NOWARN}")
        endif()
    endif()
    # ASCENSION_SPU_NO_TLSGUARD (my-changes/spu-cg2/README.md, T1): clang-cl
    # puts an on-demand TLS initialisation guard (__dyn_tls_on_demand_init,
    # MSVC 19.25+ ABI) in front of every access to an extern thread-local, C
    # included: every lifted exit's g_spu_trampoline_fn store, the drain
    # loop's thread-locals. The guard only serves C++ thread_local objects
    # with dynamic initialisers, which no C file has (C++ files keep theirs).
    # /Zc:tlsGuards- is ignored by clang 19; -fms-compatibility-version=19.20
    # drops the guards. That also lowers _MSC_VER / _MSC_FULL_VER and removes
    # _MSVC_CONSTEXPR_ATTRIBUTE, so those three are defined back to the
    # compiler's own values (read from it here): the preprocessed sources stay
    # exactly as they were. C files only: the lifted SPU code, the natives,
    # host/spu and the runtime's runtime/spu/*.c.
    option(ASCENSION_SPU_NO_TLSGUARD "C SPU sources: no TLS init guard on thread-local accesses" OFF)
    if(ASCENSION_SPU_NO_TLSGUARD AND MSVC AND CMAKE_C_COMPILER_ID MATCHES "Clang")
        set(_tg_probe "${CMAKE_BINARY_DIR}/tlsguard_probe.c")
        file(WRITE "${_tg_probe}" "\n")
        execute_process(COMMAND "${CMAKE_C_COMPILER}" /nologo /E /clang:-dM "${_tg_probe}"
                        OUTPUT_VARIABLE _tg_macros ERROR_QUIET RESULT_VARIABLE _tg_rc)
        string(REGEX MATCH "#define _MSC_VER ([0-9]+)" _m1 "${_tg_macros}")
        set(_tg_msc_ver "${CMAKE_MATCH_1}")
        string(REGEX MATCH "#define _MSC_FULL_VER ([0-9]+)" _m2 "${_tg_macros}")
        set(_tg_msc_full "${CMAKE_MATCH_1}")
        string(REGEX MATCH "#define _MSVC_CONSTEXPR_ATTRIBUTE ([0-9]+)" _m3 "${_tg_macros}")
        set(_tg_cexpr "${CMAKE_MATCH_1}")
        if(_tg_rc EQUAL 0 AND _tg_msc_ver AND _tg_msc_full)
            set(_tg_opts "-fms-compatibility-version=19.20;-D_MSC_VER=${_tg_msc_ver};-D_MSC_FULL_VER=${_tg_msc_full};-Wno-macro-redefined")
            if(_tg_cexpr)
                list(APPEND _tg_opts "-D_MSVC_CONSTEXPR_ATTRIBUTE=${_tg_cexpr}")
            endif()
            set(_tg_c "${SPU_SOURCES}")
            list(FILTER _tg_c INCLUDE REGEX "\\.c$")
            set_property(SOURCE ${_tg_c} APPEND PROPERTY COMPILE_OPTIONS ${_tg_opts})
            file(GLOB _tg_rt "${PS3RECOMP_DIR}/runtime/spu/*.c")
            list(FILTER _tg_rt EXCLUDE REGEX "/spu_dispatch_mt\\.c$")
            set_property(SOURCE ${_tg_rt} DIRECTORY "${PS3RECOMP_DIR}" APPEND PROPERTY COMPILE_OPTIONS ${_tg_opts})
            list(LENGTH _tg_c _tg_n1)
            list(LENGTH _tg_rt _tg_n2)
            message(STATUS "SPU TLS guards off: ${_tg_n1} SPU C sources, ${_tg_n2} runtime/spu sources (_MSC_VER ${_tg_msc_ver}, _MSC_FULL_VER ${_tg_msc_full})")
        else()
            message(WARNING "ASCENSION_SPU_NO_TLSGUARD: could not read the compiler's _MSC_VER; left off")
        endif()
    endif()
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
)
if(ASCENSION_PGO_RUNTIME)
    target_link_libraries(${PROJECT_NAME} PRIVATE "${ASCENSION_PGO_RUNTIME}")
endif()

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        user32
        gdi32
        winmm
        # The DXGI interface IDs (IID_IDXGIFactory6) the D3D12 backend asks
        # for; clang-cl's lld-link found no other library that defines them.
        dxguid
    )
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
    )
    # clang-cl links with lld-link: the map file as in the MSVC branch below,
    # for the sampling profiler (PS3_DEBUG's `prof`, tools\maprva.py).
    if(MSVC)
        target_link_options(${PROJECT_NAME} PRIVATE /MAP)
    endif()
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
    # AscensionRecomp.map beside the executable: the runtime's sampling
    # profiler (PS3_DEBUG's `prof`) prints module offsets, and a release build
    # has no other way to name them.
    target_link_options(${PROJECT_NAME} PRIVATE /MAP)
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
#   - AVX2 + FMA with clang-cl (ASCENSION_PPU_AVX2). The default target is
#     SSE2, where the VMX helpers' per-lane fmaf() is a library call; with FMA
#     it is one instruction. Contraction stays off so only the lifter's own
#     fused ops fuse, as on the PPU.
#   - ASCENSION_PPU_FAST_MEM (default ON) adds nothing; OFF builds the lifted
#     code the way it was before the lean guest-access checks in ppu_recomp.h
#     (two compares per load/store, always-inlined loads, no TLS-init guard on
#     g_trampoline_fn). Same results either way; it is there to measure.
option(ASCENSION_PPU_AVX2 "Lifted PPU code for AVX2 + FMA CPUs (clang-cl)" ON)
option(ASCENSION_PPU_FAST_MEM "Lifted PPU code with ppu_recomp.h's lean access checks" ON)
set(PPU_OLD_MEM "")
if(NOT ASCENSION_PPU_FAST_MEM)
    set(PPU_OLD_MEM ";-DPPU_VM_INL_CHECKS;-DPPU_MEM_INL_HINT;-DPPU_TLS_GUARDS")
endif()
# The register-cached lift needs clang's loop peeling off: with the PGO
# profile, clang 19's LoopUnrollPass never finishes peeling some of its loops
# (func_00461AF0: >15 min, 2.6 GB, 1 s without -fprofile-instr-use or with this).
if((ASCENSION_PPU_REGCACHE OR ASCENSION_PPU_LEAN_STORES) AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    string(APPEND PPU_OLD_MEM ";-mllvm;-unroll-allow-peeling=false")
endif()
if(MSVC)
    set(PPU_OPTS "/w;/bigobj")
    if(ASCENSION_PPU_AVX2 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        string(APPEND PPU_OPTS ";/arch:AVX2;/clang:-mfma;/clang:-ffp-contract=off")
    endif()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${PPU_OPTS}${PPU_OLD_MEM}")
else()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-w;-ffp-contract=off${PPU_OLD_MEM}")
endif()

# ---------------------------------------------------------------------------
# rrc_replay: draws an RPCS3 frame capture (.rrc) with the live-draw engine,
# so a frame can be compared with RPCS3 on identical inputs. A dev tool, not
# part of the game; it builds only when asked (ninja RrcReplay).
# ---------------------------------------------------------------------------
if(WIN32)
    add_executable(RrcReplay EXCLUDE_FROM_ALL
        "${PS3RECOMP_DIR}/libs/video/tests/rrc_replay_main.c"
        "${PS3RECOMP_DIR}/runtime/host/host_stubs.c")
    target_include_directories(RrcReplay PRIVATE
        "${PS3RECOMP_DIR}/include"
        "${PS3RECOMP_DIR}/runtime"
        "${PS3RECOMP_DIR}/libs/video")
    target_link_libraries(RrcReplay PRIVATE ps3recomp_runtime
        dbghelp user32 gdi32 winmm dxguid d3d12 dxgi d3dcompiler)
    target_compile_definitions(RrcReplay PRIVATE _CRT_SECURE_NO_WARNINGS)
    set_target_properties(RrcReplay PROPERTIES LINKER_LANGUAGE CXX)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
