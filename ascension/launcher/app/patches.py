"""RPCS3 game patches, built into the recompiled game.

RPCS3 patches a game in memory while it runs. Here the game's code is
translated to C++ ahead of time, so a patch is written into the executable
first and the code it touches is translated again: tools/rpcs3_patch.py lists
and applies the patches for this EBOOT.ELF, scripts/windows/apply-patches.ps1
writes the patched work/disc/.../EBOOT.ELF, re-translates and rebuilds
(seconds for SPU and data patches, minutes for main-code ones). The list of
patches to apply is the launcher's settings.json key `rpcs3_patches`
(apply-patches.ps1 -FromLauncher reads it).

Since 2026-11-01 a build made with apply-patches.ps1 -Runtime switches every
patch when the game starts instead (its .exe reads PS3_PATCHES, the ticked
names joined by '|', which game.py passes): ticking one needs no rebuild.
Such a game also lists its patches itself (PS3_PATCH_LIST=1, exe_patches),
so the switches work in a release, where no patch files exist.
"""

import ast
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from .config import LOG_DIR, ROOT, WINDOWS, WORK

PRISTINE = WORK / 'pristine' / 'EBOOT.ELF'          # the decrypted, unpatched executable
TOOL = ROOT / 'tools' / 'rpcs3_patch.py'
SCRIPT = ROOT / 'scripts' / 'windows' / 'apply-patches.ps1'
SUMMARY = ROOT / 'build' / 'patches-applied.json'
PATCH_DB = WORK / 'patches' / 'patch.yml'             # RPCS3's patch file (apply-patches.ps1 -Update)
PORTED = ROOT / 'patches-rpcs3' / 'ascension-1.00.yml'  # patches ported to 1.00
LOG = LOG_DIR / 'apply-patches.log'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def python_exe():
    """python.exe beside pythonw.exe: the tool prints to a pipe."""
    exe = Path(sys.executable)
    if exe.name.lower() == 'pythonw.exe' and exe.with_name('python.exe').is_file():
        return str(exe.with_name('python.exe'))
    return str(exe)


def problem():
    """Why patches cannot be listed or applied here, or None."""
    if not WINDOWS:
        return 'Patches are applied by scripts\\windows\\apply-patches.ps1 (Windows).'
    if not TOOL.is_file() or not SCRIPT.is_file():
        return f'{TOOL.name} or {SCRIPT.name} is missing from this checkout.'
    if not PRISTINE.is_file():
        return f'The unpatched executable is missing: {PRISTINE}'
    return None


def clean_notes(text):
    """RPCS3's Notes field as plain text (the tool passes a list as its repr)."""
    text = str(text or '').strip()
    if text.startswith('[') and text.endswith(']'):
        try:
            value = ast.literal_eval(text)
            if isinstance(value, (list, tuple)):
                text = ' '.join(str(v) for v in value)
        except (ValueError, SyntaxError):
            pass
    text = re.sub(r'[ \t]+', ' ', text.replace('\r', ''))
    text = re.sub(r'\n\s*\n+', '\n', text)
    return '\n'.join(line.strip() for line in text.split('\n')).strip()


_exe_lists = {}


def exe_patches(exe):
    """[{name, kind, default, author, notes, file}]: the patches this game
    executable switches as it starts, printed by the exe itself
    (PS3_PATCH_LIST=1; it exits at once). Cached per file version. Raises
    OSError when the exe cannot list them."""
    path = Path(exe)
    try:
        st = path.stat()
    except OSError as e:
        raise OSError(f'The game is missing: {path}') from e
    key = (str(path), st.st_size, st.st_mtime_ns)
    if key in _exe_lists:
        return [dict(p) for p in _exe_lists[key]]
    env = dict(os.environ, PS3_PATCH_LIST='1')
    try:
        r = subprocess.run([str(path)], cwd=str(path.parent), env=env, capture_output=True, timeout=30,
                           stdin=subprocess.DEVNULL, creationflags=NO_WINDOW)
    except (OSError, subprocess.SubprocessError) as e:
        raise OSError(f'Could not ask the game for its patches: {e}') from e
    out = r.stdout.decode('utf-8', 'replace')
    begin, end = out.find('PATCH-LIST-BEGIN'), out.find('PATCH-LIST-END')
    if begin < 0 or end < begin:
        raise OSError(f'The game did not list its patches (exit code {r.returncode}).')
    try:
        items = json.loads(out[begin + len('PATCH-LIST-BEGIN'):end])
    except ValueError as e:
        raise OSError(f'The game listed its patches unreadably: {e}') from e
    items = [p for p in items if isinstance(p, dict) and p.get('name')]
    for p in items:
        p['notes'] = clean_notes(p.get('notes'))
        p['author'] = str(p.get('author') or '').strip()
        p['default'] = bool(p.get('default'))
    _exe_lists[key] = items
    return [dict(p) for p in items]


def patches_on(s, listed):
    """The names on at the next start: the ticked ones once the player chose
    (patches_chosen), else the game's own defaults."""
    names = [p['name'] for p in listed]
    if str(s.get('patches_chosen', '0')) == '1':
        chosen = set(s.get('rpcs3_patches') or [])
        return [n for n in names if n in chosen]
    return [p['name'] for p in listed if p.get('default')]


def list_patches():
    """[{name, kind, author, notes, file, games, hash}] for this executable.
    Raises OSError with the tool's message when it fails."""
    why = problem()
    if why:
        raise OSError(why)
    r = subprocess.run([python_exe(), str(TOOL), 'list-json', '--elf', str(PRISTINE)], cwd=str(ROOT),
                       capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=120,
                       creationflags=NO_WINDOW)
    if r.returncode != 0:
        raise OSError((r.stderr or r.stdout or f'rpcs3_patch.py exited with {r.returncode}').strip()[-600:])
    try:
        items = json.loads(r.stdout)
    except ValueError as e:
        raise OSError(f'rpcs3_patch.py printed something unexpected: {e}')
    for p in items:
        p['notes'] = clean_notes(p.get('notes'))
        p['author'] = str(p.get('author') or '').strip()
    return items


def built_state(exe):
    """(names, finished): the patches the last apply wrote into the
    executable, and whether the build after it finished (the game's .exe is
    newer than the summary). names is None when patches were never applied."""
    try:
        data = json.loads(SUMMARY.read_text(encoding='utf-8'))
        names = [str(p['name']) for p in data.get('applied', []) if isinstance(p, dict) and p.get('name')]
    except (OSError, ValueError, AttributeError, KeyError):
        return None, True
    try:
        finished = Path(exe).stat().st_mtime >= SUMMARY.stat().st_mtime - 2
    except OSError:
        finished = False
    return names, finished


def database_date():
    """When RPCS3's patch file was last downloaded (a time.struct_time), or None."""
    try:
        return time.localtime(PATCH_DB.stat().st_mtime)
    except OSError:
        return None


def start_script(*args):
    """apply-patches.ps1 with `args`, its output written to build/logs/
    apply-patches.log (the launcher reads it from there, so a build goes on
    if the launcher closes). Returns (process, log file)."""
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    cmd = ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(SCRIPT), *args]
    log = open(LOG, 'wb')
    log.write(f'# {time.strftime("%Y-%m-%d %H:%M:%S")}  apply-patches.ps1 {" ".join(args)}\r\n'.encode())
    log.flush()
    try:
        return subprocess.Popen(cmd, cwd=str(ROOT), stdin=subprocess.DEVNULL, stdout=log,
                                stderr=subprocess.STDOUT, creationflags=NO_WINDOW), log
    except OSError:
        log.close()
        raise
