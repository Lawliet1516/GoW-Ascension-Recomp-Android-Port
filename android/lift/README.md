# Lifting the game for Android without redoing the infrastructure

Goal: when the user's `EBOOT.elf` is available, getting the lifted PPU/SPU code into an Android build is a short, repeatable procedure that
touches **no** other part of the port.

```
your EBOOT.ELF  ->  prepare.sh  ->  ascension/src/{recomp,spu}, ascension/meta/spu     (Python, any Linux, ~5-15 min)
                 ->  build_lifted.sh  ->  libgow_ppu.a, libgow_spu.a                    (clang cross-compile, needs RAM)
                 ->  link with libps3kit_android.a + the port glue + SDL3 + the Vulkan renderer  (app build, next milestone)
```

## What is verified today (without the game)

| check | result |
|---|---|
| `selftest.sh`: the lifter's host tests (chunking, branches, jump tables, fall-through, VMX lane order, `vcmp` CR6) | pass on this machine |
| PPU instruction conformance suite, generated from the lifter's actual emissions: **1,202 cases, 1,533 checks**, cross-compiled for arm64 and **run on the Cortex-A78** | **0 failures** |
| `prepare.sh --dry-run` without the EBOOT / without the Python packages | stops with the exact instruction, exit 1 / 0 as documented |
| `build_lifted.sh` without a lift | stops at configure: "No lifted PPU code ... run prepare.sh" |
| `build_lifted.sh` with a **synthetic** tree (2 files) | the CMake mechanics, job pool and flags work; this says nothing about the real code |

**Not verified** (needs the EBOOT): that the lift of the real game succeeds, how long the real compile takes, the size of the result, anything about speed.

## Input

`work/pristine/EBOOT.ELF`: the decrypted, unpatched EBOOT of your own disc, **Europe BCES01741 version 1.00** (the script's carve step checks the SPU job
modules against that exact disc; a US EBOOT needs `ascension/tools/make_us_eboot_delta.py`, not wired here). `work/` is git-ignored; never commit
or upload anything made from it.

## Machine

The lift itself is light (Python, minutes). The **compile is heavy**: the Windows build documents 2-4 GB of RAM *per lifted PPU file* in clang (12 files) and a
lifted-source tree on the order of gigabytes. The test phone's host (5 GB RAM, 2 cores) cannot do it. Use any Linux x86-64 machine with **>= 16 GB RAM
(32 GB comfortable), >= 8 cores, 40 GB disk**: your own PC, a friend's, or a rented cloud VM that you delete afterwards.
**Do not use a shared CI service for this step**: it would require uploading the EBOOT or the translated code to a third party, which the project
rules (and the original author's) forbid. The runtime-only build (`android/build.sh`) has no game data and is fine in CI.

Packages (Ubuntu 24.04): `python3 python3-venv python3-pip cmake ninja-build build-essential adb`; Android NDK r29 (`sdkmanager "ndk;29.0.14206865"`);
`pip install -r ps3kit/requirements.txt`.

## Steps

```sh
cp /path/to/EBOOT.ELF work/pristine/EBOOT.ELF
android/lift/selftest.sh                 # toolchain sanity, no game
android/lift/prepare.sh --dry-run        # checks tools and inputs, prints the plan
android/lift/prepare.sh -j 16            # lift (logs in ascension/build/logs/)
android/lift/build_lifted.sh             # compile for arm64; LIFT_JOBS defaults to RAM/4
```

Options worth knowing: `LIFT_SPU_FASTFP=ON` (must match `GOWA_SPU_FASTFP` of the runtime library, experimental), `LIFT_PPU_NO_PEELING=ON` (needed by register-cached
lifts in the Windows build, which lists a function that took >15 min and 2.6 GB otherwise), `LIFT_ARCH_FLAGS` (default `-mcpu=cortex-a78`).

## Stubs

`android/host/port_stubs.c` exists **only** so the runtime library links for bring-up tests. It is not linked by this pipeline, and no lifted-code
build may depend on it: the real symbols come from the port (`ascension/host`) and the renderer, which are separate work items.
