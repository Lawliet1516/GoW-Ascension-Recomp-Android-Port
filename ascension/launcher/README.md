# Launcher

`ascension_launcher.py` (Python 3 with Tk, and Pillow) sets up the game and starts it, in the look of SpartanPort's launcher (FelipePrado0's God of War III port): a dark sidebar of pages, red section titles, the cover art on Home and a PLAY bar at the bottom.

Open it with **`God of War Ascension Launcher.bat`** (in the folder above `ascension-recomp`; it installs Pillow once if it is missing and opens the window without a console), `scripts\windows\launcher.ps1`, or `python launcher\ascension_launcher.py` from `ascension\`. `--play` starts the game with the saved settings and no window; `--make-assets` makes the artwork from the disc (see below). The code is in `app/`: `config.py` (paths, settings, the pages' choices), `game.py` (disc, mods, saves, the game's environment, which switches the executable reads), `patches.py` (RPCS3 game patches), `assets.py` (artwork from the disc), `music.py`, `native.py` (macOS Dock, fonts) and `ui.py` (the window).

PLAY starts `build\win\AscensionRecomp.exe <EBOOT.ELF>` from `ascension\` (the saves are `ascension\gamedata\dev_hdd0\home\00000001\savedata`), exactly as `scripts\windows\run.ps1` does; the EBOOT.ELF is `work\disc\PS3_GAME\USRDIR\EBOOT.ELF`, the one `apply-patches.ps1` writes. The game opens in its own window by default (Graphics → Game window can put it inside the launcher's window instead); the launcher shows RUNNING, pauses its music and follows the game's log in Advanced → Log. Only one copy of the game may run: PLAY (and `--play`) refuse when `AscensionRecomp.exe` is already running, e.g. started by `run.ps1`.

## Pages

| Page | What it has |
|---|---|
| Home | The cover art with the logo, the disc found (`BCES01741 · version 1.00 · Europe`) and what the game starts with; quick settings |
| Graphics | Display mode, window size, game window (own / inside the launcher), hide the launcher while playing, the frame rate limit (30, fixed), FPS counter; the picture options that are not available yet are listed, greyed, under *Coming later* |
| Game | Skip cutscenes, game language, subtitles, save backups (restore any of them) |
| Controls | Confirm button, Esc closes the game, and the runtime's fixed controller and keyboard layout |
| Patches | RPCS3's game patches for this executable: tick, Apply (rebuilds the game), Update patch list, the build's output |
| Mods | Load order (first wins) with a details panel, the PT-BR mod from a US disc, the mods folder |
| Advanced | Game files, launcher language, music, desktop shortcut, remaking the artwork, extra runtime variables, which switches this build reads, the game's log |
| About | The project, links, credits |

## Settings and switches

Settings are kept in `%APPDATA%\ascension-launcher\settings.json` (macOS: `~/Library/Application Support/ascension-launcher/settings.json`, Linux: `~/.config/ascension-launcher/settings.json`) and reach the game as environment switches. Which switches the game reads is found in its executable (a `getenv()` name is a string in the `.exe`; the result is cached in `runtime-switches.json` beside the settings while the `.exe` is the same): **a setting whose switch the build does not read is not shown and not passed**, so a newer runtime brings its options back by itself. Advanced → Runtime lists the switches the current build reads.

| Setting | Switch | This build |
|---|---|---|
| Display mode, window size | `PS3_FULLSCREEN`, `PS3_WINDOW_SIZE` (F11 or Alt+Enter toggles in game) | works |
| Game window: inside the launcher | `PS3_PARENT_HWND` (the game renders inside the launcher's window) | works |
| Esc closes the game | `PS3_ESC_QUITS` | works |
| Confirm button | `PS3_ENTER_BUTTON` | works |
| FPS counter | `LD_PERF_OVERLAY` | works |
| Frame rate limit | `PS3_FPS_LIMIT=30` (the game's clock steps 1/30 s a frame, so only 30 is offered) | works |
| Game language | `PS3_SYS_LANG` (the console language, read when the game makes its system save), and bytes `0x1C` (text) / `0x1D` (voices) of `BCES01741-SYSDATA/SAVEDATA` afterwards | works |
| Subtitles | byte `0x28` of the same file | works |
| Mods | `PS3_MODS` (enabled folders, first wins; each mirrors `USRDIR`) | works |
| Disc folder | `PS3_VFS_ROOT` (only when set; empty = the EBOOT.ELF's own disc) | works |
| Icon | `PS3_WINDOW_ICON` (`icon-2.png` as an `.ico`) | works |
| Skip cutscenes | `PS3_SKIP_MOVIES=1` (prerendered cutscenes and intro movies); always passed when on | being added to the runtime; the Game page marks it *needs a newer build* until the `.exe` reads it |
| Renderer | `PS3_RSX=rpcs3`, `PS3_RSX_RENDERER` (RPCS3's RSX, a build with `-DASCENSION_RPCS3_RSX=ON`) | not in this build: not offered |
| Internal resolution, anisotropic filter, aspect ratio, image scaling, VSync | `PS3_RENDER_SCALE`, `PS3_ANISO`, `PS3_ASPECT`, `PS3_SCALE_FILTER`, `PS3_VSYNC` (RPCS3's renderer) | not read yet: listed as *not available yet* |
| Keyboard and controller remapping, dead zone | `PS3_KEYMAP`, `PS3_PADMAP`, `PS3_PAD_DEADZONE` | not read yet: the Controls page shows the fixed layout |

Controllers are read through SDL2 (Xbox, DualShock 4, DualSense…) with the PS3 layout (A/✕ jump, X/□ light attack, Y/△ heavy attack, B/○ grab, LB/L1 block…). With no controller connected the keyboard plays as the first controller: arrows (d-pad and left stick), Z ✕, X ○, A □, S △, Q/W L1/R1, 1/2 L2/R2, Enter START, Tab SELECT.

On Windows `/dev_hdd0` is left where `run.ps1` has it (the game's own data under the extracted disc's `game\` folder); macOS keeps `PS3_HDD0_ROOT`/`PS3_HDD1_ROOT` in `gamedata\` because its disc image is mounted read-only.

## Patches

RPCS3 patches a game in memory while it runs. This port translates the game's code ahead of time, so a patch is written into the executable and the code it touches is translated again. The Patches page lists the patches `tools\rpcs3_patch.py list-json --elf work\pristine\EBOOT.ELF` finds for this executable (RPCS3's `work\patches\patch.yml` and the ports in `patches-rpcs3\ascension-1.00.yml`), with their author and notes. **Apply** saves the ticked ones as `rpcs3_patches` in `settings.json` and runs `scripts\windows\apply-patches.ps1 -FromLauncher`, which writes the patched `work\disc\PS3_GAME\USRDIR\EBOOT.ELF`, re-translates what changed and rebuilds the game: seconds for SPU and data patches, a few minutes for main (PPU) code patches. Its output shows on the page and goes to `build\logs\apply-patches.log` (the build goes on if the launcher is closed). **Update patch list** runs `apply-patches.ps1 -Update -List` (downloads RPCS3's patch file again) and reads the list anew. Each patch shows *BUILT IN* when the current game was built with it (`build\patches-applied.json`); PLAY warns when the ticked patches differ from the built ones, and waits while a build runs.

Version 1.00 of the European disc (BCES01741), which this port is built from, only has RPCS3's *Disable SPU MLAA* patch so far; RPCS3's other Ascension patches are for the 1.04 and 1.12 updates and are being ported to 1.00. With no `rpcs3_patches` in `settings.json` yet, the launcher starts from the patches the game is built with, so a first Apply keeps them.

## Mods

Mods come from two places, listed together in one priority order (first wins): `mods-builtin/` in this repository (shown as included) and the user's mods folder. A mod is a folder that mirrors `USRDIR` (its files replace the disc's), and/or a `mod.json`:

```json
{"name": "Skip intro", "description": "...",
 "translations": {"pt-BR": {"name": "Pular intro", "description": "..."}},
 "psarc_patches": {"GAME.PSARC": {"/INTRO/BOOT_BCES01741.TXT": {"text": ""}}}}
```

`psarc_patches` are recipes for files inside an archive: `{"text": ...}` replaces one, `{"insert_after": <line>, "line": <new line>}` adds a line. At PLAY the launcher builds each patched archive once from the disc's copy (or from the copy the highest-priority mod ships) into `%APPDATA%\ascension-launcher\generated\`, applying every enabled mod's recipes, lowest priority first, and puts that folder first in `PS3_MODS`. `translations` (optional) gives the name and description in the launcher's other languages. Included: **Skip intro** (an empty boot-screen list; on by default). It complements **Skip cutscenes** (Game page), which skips the prerendered movies.

Brazilian Portuguese needs the PT-BR mod, which the Mods page builds from your own US disc (an `.iso` or its extracted folder) with `tools/make_ptbr_mod.py`; with it on, the language list offers Português (Brasil).

## Saves and logs

**Updating without losing saves.** The saves live inside the install folder (`ascension-recomp\ascension\gamedata`). To update with a full zip, extract it over the old folder, or copy the old folder's `ascension-recomp\ascension\gamedata` into the new one before the first PLAY; do not delete the old folder until your saves are there. Since 2026-11-14 a release launcher also does this itself at start (`check_lost_saves` in `app\ui.py`, `lost_save_plan` / `recover_save` in `app\game.py`): it records each install in `%APPDATA%\ascension-launcher\installs.json` and looks for story saves in the installs recorded there, the folders the old shared settings point at, the release folders beside this one and one level up, and in Downloads and on the Desktop. A save found where this folder has none, or one newer than the one here, is copied in; checkpoints that 0.1.26 and older saved loosely into `savedata\` (the AutoSave2 bug) are put into `<disc>-PROFDATA-0` the same way. A found save older than the one here is only offered. Nothing is deleted: a replaced save folder is moved to `gamedata\replaced-saves\<time>\`, and a save backup is made first. Each find is acted on or offered once; **Find lost saves** on the Game page looks again.

Saves are copied to `gamedata\save-backups\` before each start, unless the newest copy already matches them (the last ten are kept). The Game page restores any of them (the current saves become a backup first). The run's output goes to `build\logs\last_run.log`, the run before it to `last_run.prev.log`; when the game ends with an error the launcher offers to open the log. A boot that hangs (rare: the main thread stuck waiting for SPU jobs right after the game's second graphics setup, recognised from the game's own 8 s and 15 s watchdog samples, see `boot_hung` in `app\game.py`) is closed and started again, at most twice; the log then has a `[launcher] the boot hung` line and both runs. PLAY prepares the disc, saves and mods in the background, with each step in the status bar, and refuses a disc whose `PARAM.SFO` is not `BCES01741`. Saving the settings also builds the mods' archives ahead of time when the disc is there.

## Artwork and music

Nothing from the game is in the repository. On the first start with the disc in place (or with `python launcher\ascension_launcher.py --make-assets`, or Advanced → *Make artwork from the disc*), the launcher makes, into `..\work\data\launcher\`, from the disc's `PS3_GAME` folder:

- `background.jpg` from `PIC1.PNG` (the cover art),
- `logo.png`, the logo of `ICON0.PNG` cut from its dark brush stroke,
- `icon-2.png`, Kratos' head from `PIC1.PNG`, for the window and taskbar (as an `.ico`),
- `music.mp3` from `SND0.AT3` (the menu theme, ATRAC3plus) through FFmpeg, found on `PATH`, at `FFMPEG_BIN_DIR` in `ascension\.env`, or in `tools\ffmpeg\*\bin` beside the project; without FFmpeg there is no music.

The launcher reads `background*.jpg/png`, `logo.png`, `icon-2.png` and `music.mp3` from `launcher\assets\` first (your own artwork wins), then `..\work\data\launcher\`; without them it still works, plainer. `Cinzel-Black.ttf` is the Cinzel typeface (SIL Open Font License, `OFL-Cinzel.txt`).

## Languages

The launcher's own text is in English, Brazilian Portuguese or Spanish: Advanced → Launcher language, **Automatic** by default (the system's language, else English); changing it restarts the launcher. The game's language is a separate setting, on the Game page.

The code is written in English and passes each text through `tr()` (`app/i18n.py`). A language is a file in `lang/` named by its code (`pt-BR.json`, `es.json`) mapping each English text to its translation, with `"_name"` as the language's own name; `{name}` fields are kept as they are. Text missing from a file stays in English. Adding a language is adding a file.

## macOS

Paused, like the rest of the macOS support: `brew install python@3.12 python-tk@3.12`, a venv with `pip install pillow`, then `scripts/mac/launcher.sh`. The game opens a window of its own (the launcher hides until it closes), the disc `.iso` set in Advanced is mounted read-only at `work/disc` when needed, the executable is `build/mac/AscensionRecomp` (`scripts/mac/build.sh`) and the `EBOOT.ELF` is read from `input/`. Music plays through `/usr/bin/afplay`. Patches are applied on Windows only.

## Development

`python launcher\ascension_launcher.py --shot <folder>` opens the window, saves a screenshot of every page into `<folder>` and quits (no music, no artwork step).
