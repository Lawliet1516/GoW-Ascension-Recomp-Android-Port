# God of War: Ascension PC port: source code

<p align="left">
  <a href="https://www.youtube.com/@BilalReloaded"><img alt="YouTube: BilalReloaded" src="https://img.shields.io/badge/YouTube-BilalReloaded-FF0000?style=for-the-badge&logo=youtube&logoColor=white"></a>
  <a href="https://www.patreon.com/cw/bilalreloaded"><img alt="Support on Patreon" src="https://img.shields.io/badge/Patreon-Support-F96854?style=for-the-badge&logo=patreon&logoColor=white"></a>
</p>

> **A message from the author**
>
> Don't expect many updates from me from now on. I won't be able to continue the project because of my finances. The project is open source, so you're welcome to build on it. Thank you for all the support.
>
> BilalReloaded

This is the source code of the PC port of the PS3 game **God of War: Ascension** (BCES01741, Europe, version 1.00).

It is a **static recompilation**, not an emulator. Tools in this repository translate the game's PowerPC (PPU) and SPU machine code into C and C++ ahead of time. That code is then compiled into a normal Windows program. A runtime supplies the PS3 system libraries, and a Direct3D 12 renderer draws the RSX graphics work.

> [!IMPORTANT]
> **No game code or game data is in this repository.** Nothing here was taken from the game: no translated code, no executables, no assets, no textures, no audio, no shader caches. You need **your own legally dumped copy** of the game (the European disc BCES01741, version 1.00) and you translate it yourself on your own PC with the steps below. The translated code you make is made from the game, so do not share it.
>
> This is a fan project. It is not affiliated with Sony Interactive Entertainment or Santa Monica Studio. God of War is a trademark of Sony Interactive Entertainment.

**Single player only.** Online and multiplayer were removed from this source release. Nothing in it connects to a game server.

## What is in here

```
ps3kit/      the runtime and the translators (a fork of ps3recomp, MIT)
ascension/   the port: CMake project, game-specific host code, scripts, tools, launcher, patches
licenses/    licence texts of third-party code and libraries
work/        (you make it) your decrypted EBOOT.ELF and what is made from it; never committed
```

`MANIFEST.md` lists what this release contains and what was left out, and why.

## Building it (Windows 10/11, x64)

A build takes about 10-40 minutes on a fast PC. You need about 30 GB of free disk space and at least 16 GB of RAM.

### 1. Install the tools

- **Visual Studio 2022** (the free Build Tools are enough) with the workload *Desktop development with C++* and the component *C++ Clang Compiler for Windows* (clang-cl). CMake and Ninja come with it.
- **Python 3.10 or newer** (python.org), on PATH. Then, from this folder:
  ```powershell
  python -m pip install -r ps3kit\requirements.txt
  python -m pip install pillow      # for the launcher
  ```
- **FFmpeg**, a shared LGPL build for Windows x64, for example `ffmpeg-n7.1-latest-win64-lgpl-shared-7.1.zip` from [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases). Extract it anywhere.
- **SDL2** (optional, for PlayStation controllers): `SDL2-devel-2.32.x-VC.zip` from [libsdl-org/SDL](https://github.com/libsdl-org/SDL/releases). Extract it anywhere.

### 2. Decrypt your own EBOOT

From your own disc, take `PS3_GAME\USRDIR\EBOOT.BIN` and decrypt it to `EBOOT.ELF`, with either:

- RPCS3: *Utilities > Decrypt PS3 Binaries*, or
- `python ascension\tools\decrypt_self.py` with your own scetool-format key file (no keys are included).

Put the result here (make the folders):

```
work\pristine\EBOOT.ELF
```

It must be the disc EBOOT of BCES01741 version 1.00. The scripts check this and stop if it is not.

### 3. Local settings

```powershell
cd ascension
copy .env.example .env
notepad .env
```

Set `VCVARS`, `FFMPEG_INCLUDE_DIR`, `FFMPEG_BIN_DIR` and `SDL2_DIR` to the folders on your PC. Leave `PS3KIT_DIR=../ps3kit` and `WORK_DIR=../work` as they are. Set `JOBS` to about your RAM in GB divided by 4.

### 4. Translate your EBOOT

Still in `ascension\`:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\windows\prepare.ps1
```

This takes a few minutes. It reads `work\pristine\EBOOT.ELF` and writes `src\recomp\` (the PPU code as C++), `src\spu\` (the SPU programs as C), `meta\spu\`, `meta\patch_sites.json`, `host\ascension_patch_table.inc` and `work\jobmods\`. All of these are made from the game: they are in `.gitignore` and must not be shared.

### 5. Build

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\windows\build.ps1
```

The first run configures CMake. The game is `ascension\build\win\AscensionRecomp.exe`, with `SDL2.dll` and the FFmpeg DLLs copied beside it. The build log is `ascension\build\logs\build.log`.

### 6. Play

The game also needs your disc's files. Start the launcher and use it to pick your game folder (the folder that holds `PS3_GAME`) or your `.iso`, then press PLAY:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\windows\launcher.ps1
```

Or run the game directly with `.\scripts\windows\run.ps1` (it expects the extracted disc in `work\disc`, with the decrypted `EBOOT.ELF` at `work\disc\PS3_GAME\USRDIR\EBOOT.ELF`).

If you want the launcher to decrypt `EBOOT.BIN` by itself, put a scetool-format `keys.txt` next to `ascension\launcher\` (or set `PS3_KEYS` to its path). No console keys are included in this source.

## Game patches

`ascension\patches-rpcs3\ascension-1.00.yml` has RPCS3-format patches for version 1.00: Unlock FPS (60 fps), Disable Depth of Field, Disable Motion Blur, Disable MLAA (ported from the RPCS3 community patches for 1.04 by ZEROx and illusion) and a few cheats. They are switched on and off when the game starts (no rebuild), from the launcher's Patches page.

## Credits

- [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet and contributors: the recompiler, the runtime and the system libraries this port is built on (MIT). See `ps3kit/CONTRIBUTORS.md`.
- `ascension/tools/decrypt_self.py` comes from the [Twisted Metal](https://github.com/sp00nznet/twistedmetal) port (MIT).
- `ascension/launcher/app/unself.py` is a port of part of [RPCS3](https://github.com/RPCS3/rpcs3)'s `Crypto/unself.cpp` (GPL-2.0).
- RPCS3 community patches for God of War: Ascension 1.04 by ZEROx and illusion, ported to 1.00.
- AMD FidelityFX (MIT), SMAA (MIT), the ATRAC3+ decoder from PPSSPP / FFmpeg (LGPL-2.1), stb_image (public domain), SDL2 (zlib), FFmpeg (LGPL), the Cinzel font (SIL Open Font License).
- The launcher follows the look of FelipePrado0's [gow3_pc](https://github.com/FelipePrado0/gow3_pc) launcher.

## Licence

The port's own code (everything in `ascension/` unless a file says otherwise) is MIT, see `LICENSE`. `ps3kit/` is MIT (ps3recomp), see `ps3kit/LICENSE`. Third-party code keeps its own licence; the texts are in `licenses/`. `ascension/launcher/app/unself.py` is GPL-2.0 because it is derived from RPCS3.
