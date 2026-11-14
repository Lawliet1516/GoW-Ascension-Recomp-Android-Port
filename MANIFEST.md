# Manifest: what this source release contains, and what was left out

Made on 2026-11-14 from the author's development tree (the state of 0.1.31,
the last release). Layout:

```
ps3kit/      the runtime and translators (from the dev tree's ascension-recomp/ps3kit)
ascension/   the port (from the dev tree's ascension-recomp/ascension)
licenses/    third-party licence texts
work/        not included: you make it (your EBOOT.ELF and what is made from it)
```

The two folders keep the dev tree's layout, so `ascension/CMakeLists.txt`
finds `../ps3kit` by itself and `.env` needs no path changes for them.

## Included

| Path | What | Why it is safe to publish |
|---|---|---|
| `ps3kit/` (CMakeLists, cmake, config, docs, include, libs, packaging, runtime, templates, tests, third_party, tools, wave, README/LICENSE/CONTRIBUTORS/CHANGELOG/CONTRIBUTING, requirements.txt) | ps3recomp fork: lifters, runtime, HLE libraries, D3D12 live-draw renderer | MIT (upstream ps3recomp) plus the port's own changes; no game data |
| `ascension/CMakeLists.txt`, `app.rc`, `.gitattributes`, `.gitignore`, `.env.example` | the game project | own code |
| `ascension/host/ascension_defaults.cpp`, `ascension_patches.cpp`, `ascension_vram_guard.cpp` | title defaults, run-time patch switches, VRAM guard | own code (addresses only) |
| `ascension/host/spu/spu_fnrec.c/.h`, `edge_nocull.c` | SPU function record/replay hooks | own code |
| `ascension/scripts/windows/` (prepare, build, env, apply-patches, relift, relift-spu, run, launcher, snap, pgo-train, test-*) | build and run scripts | own code; paths come from `.env` |
| `ascension/tools/*.py`, `jobmods_extra_funcs.json`, `job_fragments.json` | patch tools, lift hooks, carving, analysis | own code; the JSON files hold addresses only |
| `ascension/meta/functions.json`, `EBOOT.imports.json` | function start/end addresses; firmware import NIDs | addresses and public NIDs only, no game bytes (the lifter needs them) |
| `ascension/launcher/` (app/, lang/, ascension_launcher.py, Cinzel font + OFL, README.md, supporters.txt) | the launcher | own code (unself.py: GPL-2.0, from RPCS3); online parts removed; no console keys |
| `ascension/packaging/` | icon drawing script, icons, launcher stub | own artwork (drawn by make_icons.py) |
| `ascension/patches-rpcs3/ascension-1.00.yml` | RPCS3-format patches for 1.00 | small community patches with credit (ZEROx, illusion; 1.04 ports) and the port's own cheats; a few original instruction words appear in comments only |
| `ascension/mods-builtin/skip-intro/mod.json` | Skip intro mod recipe | a recipe (writes an empty text file into your own archive), no game data |
| `LICENSE`, `README.md`, `MANIFEST.md`, `licenses/` | licences, docs | |

Count: see the end of this file.

## Left out

| What (dev tree) | Why |
|---|---|
| `ascension/src/` (recomp, recomp-*, spu, spu.relift2) | translated game code (PPU and SPU). `scripts/windows/prepare.ps1` makes it from your own EBOOT. |
| `ascension/meta/spu/*.elf`, `meta/patch_sites.json`, `meta/EBOOT.functions.json`, `EBOOT.image.json`, `EBOOT.loader.json` | SPU programs extracted from the game / generated from it; prepare.ps1 makes what is needed |
| `ascension/host/ascension_patch_table.inc` | generated patch table (holds original game bytes of the patch sites); prepare.ps1 makes it |
| `ascension/host/spu/natives/`, `host/spu/wip/`, `host/comb/`, `host/gpu_edge/` | native SPU job code: mechanically translated or derived from the game's SPU programs (generated `.inc` files, recorded jobs). The public build runs the SPU work through the code prepare.ps1 lifts from your EBOOT instead (exact, but slower in heavy scenes). |
| every other `ascension/host/*` folder (65 GB: uber*, dlss, mpnet, mp, mpmenu, pc-multiplayer, speed, x3d, shimmer, ...) | lab folders: test builds, logs, captures, recorded jobs and pipelines, notes; DLSS (stays private by the author's rule); multiplayer |
| `ascension/host/*.md`, `*.diff` | lab notes |
| `ascension/captures/`, `gamedata/`, `build/`, `scratch/`, `my-changes/`, `ascension-recomp/work/`, `shots/` | frames of the game, saves, build outputs, test folders |
| `*.bin`, `*.ls`, `recomp_spu_ls.bin`, `relbuild131.out` (ascension root) | SPU local-store dumps and logs |
| `ascension/.env`, `mpnet-token.txt` | local settings, an online sign-in token |
| `ascension/discord/` | Discord helper (multiplayer) |
| `ascension/launcher-c/` | an older copy of the launcher |
| `ascension/patches/ps3recomp-00xx.patch` | history: already applied inside `ps3kit/` |
| `ascension/docs/` (old README.en, MILESTONES, screenshots) | out of date; screenshots show game art |
| `ascension/CLAUDE.md`, `HANDOFF.md`, `NOTES*.md` and other lab books | full of local paths and test detail |
| `ascension/scripts/windows/` agent-build, bench-heavy, bootloop, burst, heavy, soak, flagtest, mute-game, set-affinity, spu-replay, build-gpu; `scripts/linux`, `mac`, `unix` | lab scripts tied to the author's saves and PC; the RPCS3 renderer build (its gpu/ folder is not public); untested platforms |
| `ps3kit/.git`, `.github/`, `scratch/`, `__pycache__`, `*.orig`, `*.bak*`, `stderr_*.txt` | history, CI of upstream, scratch files |
| `ps3kit/docs/RSXFIFOCommands - PS3 Developer wiki.pdf`, `docs/rsxfifo.txt` | copies of a third-party wiki |
| `ps3kit/docs/ONLINE.md` | online play documentation (online was removed) |

## Multiplayer removed

Online and multiplayer were removed; nothing in this tree contacts a game server.

- **Runtime (`ps3kit/libs/network`)**: `mpnet.c`, `mpnet.h`, `mpnet_sha256.c`, `sceNpEula.c/.h` removed. `cellHttp.c`, `cellNetCtl.c`, `sceNp.c`, `sceNpLookup.c`, `sceNpSignaling.c/.h`, `sceNpTrophy.c`, `sceNpTus.c/.h`, `sysNet.h` are the versions from before the multiplayer work (2026-11-04). On top of that:
  - `sysNet.c` is a small offline stub (no host sockets; every socket call fails like a console with no network).
  - `np_psnr.c` is an offline stub (the psnr matchmaking client `psnr/psnr.c` is removed; only the local player name for the console identity is kept, so saves are unchanged).
  - `cellNetCtl.c` always reports "not connected" (`PS3_NET_ONLINE` is ignored).
  - `cellHttp.c` refuses to send any request (offline build).
  - `cellNet.c` makes no DNS lookup (the resolver always fails, offline build).
  - `runtime/ppu/ppu_sysprx.cpp` no longer registers host sockets; `libs/input/cellPad.c` lost the `PS3_PAD_NO_HOST` test switch; `runtime/syscalls/sys_ppu_thread.c` is the pre-multiplayer version.
  - Tests of the removed online code removed (`libs/network/tests/test_np_matching2.c`, `test_np_nat.c`, `test_np_identity.c`, `test_sys_net.c`, `test_socket_sigpipe.c`, `test_score_init.c`, `tools/test_sys_net.py`, `tools/test_socket_sigpipe.py`).
- **Translated game code**: the dev tree's `mksrc.py` online lines (SSL off, TSS plaintext, two NULL guards) are not applied; prepare.ps1 makes a plain lift.
- **Offline Multiplayer patches removed** as well ("Offline Multiplayer (experimental)", "... no instant win when alone", "... skip the tutorial"): they bypass the game's network gate (PSN sign-in, Online Pass, TSS), so they are not published. The launcher's offline multiplayer mode/map settings and `ascension_patches.cpp`'s mode/map writer are removed too.
- **Launcher**: the Online page, community server list (`KNOWN_SERVERS`, two server IPs), leaderboards, match codes, Discord helper start/join, online token file, Online Pass check, online mods composer, multiplayer profile backups, `online_*` / `mp_*` settings and their translations removed.
- **Not included**: the community server, the Discord bot and helper, server addresses, tokens, admin keys.

## Other changes made for this release

- **No console keys**: `launcher/app/unself.py` and `upd104.py` had the APP SELF key and the PKG key built in; they now read a key file you supply (`PS3_KEYS` or `launcher/keys.txt`). `tools/decrypt_self.py` already needed one.
- **No downloads during a build**: `apply-patches.ps1` downloads RPCS3's patch file only with `-Update`; `prepare.ps1` downloads nothing.
- New `scripts/windows/prepare.ps1` (EBOOT -> job modules, patch table, PPU lift + post-lift hooks, SPU extraction and lift) and `.env.example`.
- `CMakeLists.txt`: the native-SPU glob, the COMB set and the RPCS3 renderer option were removed (their sources are not in this tree). `build.ps1` / `env.ps1`: the RPCS3 renderer settings removed.
- The speed test switches M_SPIN/M_PRIO stay in the runtime but their hint lines in the translated code (`cand_patch.py`) are not added: those switches were off in the releases.
- Launcher GitHub link points to the author's profile.

## Count

MEASURED: 930 files, about 19 MB (`git ls-files | wc -l`). A fresh clone of this commit, with the author's own EBOOT.ELF as the only input, ran `prepare.ps1` and `build.ps1` to a linked `AscensionRecomp.exe` (2026-11-14). The game itself was not started in that test.
