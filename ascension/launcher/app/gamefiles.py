"""Prepares the game files from what the player picks: a game folder or a
disc image (.iso).

The player's files are never changed. The launcher:
  1. finds the folder holding PS3_GAME (the one picked, a parent of it, or
     the mounted .iso),
  2. checks PARAM.SFO is God of War: Ascension BCES01741 (Europe) or BCUS98232
     (USA: the same program 16 bytes on, see us_eboot.py),
  3. finds the decrypted EBOOT.ELF (RPCS3 writes it next to EBOOT.BIN with
     Utilities > Decrypt PS3 Binaries; a decrypted EBOOT.BIN is used as is),
     or decrypts the disc's EBOOT.BIN itself (unself.py); a US one is turned
     into the European one (us_eboot.to_eu, md5-checked),
  4. checks it is version 1.00 and writes the patched copy this build needs
     (Disable Motion Blur, Unlock FPS, Disable SPU MLAA: 30 bytes) into the
     port's own folder: work/eboot/EBOOT.ELF.

The game then reads the disc through PS3_VFS_ROOT and keeps its own data
(/dev_hdd0, /dev_hdd1) under ascension/gamedata, so a read-only .iso works.
"""

import hashlib
import json
import subprocess
from pathlib import Path

from .config import GAME_ID, GAME_IDS, ROOT, US_ID, WINDOWS, WORK
from . import us_eboot

ORIGINAL_MD5 = '4c12e201bd8579dbd4f9410b4c65af3c'   # decrypted EBOOT.ELF, BCES01741 v1.00
US_MD5 = us_eboot.US_MD5                            # decrypted EBOOT.ELF, BCUS98232 v1.00 (converted)
PATCHED_MD5 = 'e84f2bf7f782869b76d6376cd340d277'    # what this build expects
EBOOT_SIZE = 8584256
# (file offset, original bytes, patched bytes)
PATCHES = [
    (2744758, 'af14', '913c'), (2767848, 'd024af14', '60000000'), (2767932, '43c800', '461c40'),
    (2768086, 'af14', '913c'), (2768254, 'af14', '913c'), (3569300, '426fc28f', '447a0000'),
    (3900298, 'af14', '913c'), (4164336, '80bf', '38a0'), (4164339, '20', '00'),
    (6818496, '580243', '408000'), (6823236, '58268d', '408000'), (8433428, 'bf80', '4270'),
]
PREPARED = WORK / 'eboot' / 'EBOOT.ELF'
NO_WINDOW = 0x08000000 if WINDOWS else 0


class NotReady(Exception):
    """What is missing, in words for the player."""


def find_disc_root(folder):
    """The folder holding PS3_GAME, from the one picked (the disc folder
    itself, PS3_GAME, USRDIR, or a folder with the game one level down)."""
    p = Path(folder)
    for cand in (p, p.parent, p.parent.parent):
        if (cand / 'PS3_GAME' / 'PARAM.SFO').is_file():
            return cand
    try:
        for sub in sorted(p.iterdir()):
            if sub.is_dir() and (sub / 'PS3_GAME' / 'PARAM.SFO').is_file():
                return sub
    except OSError:
        pass
    return None


def check_title(root):
    from .game import read_sfo
    try:
        info = read_sfo(root / 'PS3_GAME' / 'PARAM.SFO')
    except (OSError, ValueError):
        raise NotReady(f"PARAM.SFO in {root / 'PS3_GAME'} could not be read.")
    title = str(info.get('TITLE_ID', ''))
    if title not in GAME_IDS:
        name = str(info.get('TITLE', '')).replace('\n', ' ')
        raise NotReady(f'This is {title or "?"} ({name}), not God of War: Ascension {GAME_ID} (Europe) '
                       f'or {US_ID} (USA).')
    return info


def _md5(path):
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def find_eboot(usrdir, extra=None):
    """The decrypted EBOOT (original or already patched) and its md5, or None."""
    names = []
    if extra:
        names.append(Path(extra))
    try:
        names += [f for f in usrdir.iterdir() if f.is_file() and f.name.lower().startswith('eboot')]
    except OSError:
        pass
    for f in names:
        try:
            if f.stat().st_size != EBOOT_SIZE:
                continue
            with open(f, 'rb') as fh:
                if fh.read(4) != b'\x7fELF':
                    continue
            md5 = _md5(f)
        except OSError:
            continue
        if md5 in (ORIGINAL_MD5, PATCHED_MD5, US_MD5):
            return f, md5
    return None


def write_prepared(src, md5):
    return write_prepared_data(Path(src).read_bytes(), md5)


def write_prepared_data(data, md5):
    if md5 == US_MD5:                                # the US disc: the European EBOOT from it
        try:
            data = us_eboot.to_eu(bytes(data))
        except ValueError as e:
            raise NotReady(f'The US EBOOT could not be converted: {e}; nothing was written.')
        md5 = ORIGINAL_MD5
    data = bytearray(data)
    if md5 == ORIGINAL_MD5:
        for offset, old, new in PATCHES:
            old, new = bytes.fromhex(old), bytes.fromhex(new)
            if data[offset:offset + len(old)] != old:
                raise NotReady('EBOOT.ELF did not have the expected bytes; nothing was written.')
            data[offset:offset + len(new)] = new
    if hashlib.md5(data).hexdigest() != PATCHED_MD5:
        raise NotReady('Preparing EBOOT.ELF gave an unexpected result; nothing was written.')
    PREPARED.parent.mkdir(parents=True, exist_ok=True)
    tmp = PREPARED.with_suffix('.tmp')
    tmp.write_bytes(data)
    tmp.replace(PREPARED)
    return PREPARED


# ---- disc images (Windows mounts them itself, read-only) ----------------------
def _ps(command):
    return subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', command],
                          capture_output=True, text=True, errors='replace', timeout=60, creationflags=NO_WINDOW)


def mount_iso(iso):
    """Mounts the .iso (or finds it already mounted) and returns its drive root."""
    if not WINDOWS:
        raise NotReady('Disc images are mounted by the launcher on Windows only.')
    iso = str(Path(iso).resolve()).replace("'", "''")
    cmd = (f"$i = Get-DiskImage -ImagePath '{iso}'; if (-not $i.Attached) {{ $i = Mount-DiskImage "
           f"-ImagePath '{iso}' -Access ReadOnly -StorageType ISO -PassThru }}; "
           f"($i | Get-Volume).DriveLetter")
    r = _ps(cmd)
    letter = (r.stdout or '').strip()[-1:]
    if r.returncode != 0 or not letter.isalpha():
        raise NotReady('Windows could not mount the disc image: ' + (r.stderr or r.stdout).strip()[:200])
    return Path(f'{letter}:\\')


def dismount_iso(iso):
    if WINDOWS and iso:
        _ps(f"Dismount-DiskImage -ImagePath '{str(Path(iso).resolve())}'")


# ---- the disc's own EBOOT.BIN ------------------------------------------------------
RPCS3_HELP = ('Your EBOOT is still encrypted. Open RPCS3 → Utilities → Decrypt PS3 Binaries and pick '
              'PS3_GAME\\USRDIR\\EBOOT.BIN in your game folder. RPCS3 saves EBOOT.elf in the same folder. '
              'Then in the launcher press Choose game folder and pick the same folder again.')
RPCS3_HELP_ISO = ('Your EBOOT is still encrypted. Copy PS3_GAME\\USRDIR\\EBOOT.BIN from the mounted disc image '
                  'into any folder and decrypt that copy with RPCS3 (Utilities → Decrypt PS3 Binaries; it saves '
                  'EBOOT.elf next to it). Then in the launcher press Advanced → Choose decrypted EBOOT.elf.')
# A 1:1 disc image (a Redump-style .iso) keeps the disc's own encryption: Windows mounts it and lists the files,
# but their contents are scrambled, so EBOOT.BIN (and every archive) reads as garbage (a tester, 2026-11-09).
ENCRYPTED_DISC_HELP = ('If your copy comes from a 1:1 disc image (a Redump .iso), that image is still encrypted '
                       'with the disc\'s own key: decrypt the .iso first with that key (for example with PS3Dec '
                       'and the matching disc key), then choose the decrypted .iso. Files extracted from a '
                       'still-encrypted .iso are scrambled and cannot work.')


def decrypt_disc_eboot(bin_, iso=''):
    """The decrypted EBOOT.ELF of disc version 1.00 from the disc's EBOOT.BIN,
    as bytes. Raises NotReady with what the player can do."""
    from . import unself
    try:
        data = Path(bin_).read_bytes()
    except FileNotFoundError:
        raise NotReady(f'{bin_} is missing: the copy of the game is not complete.')
    except OSError as e:
        raise NotReady(f'{bin_} could not be read: {e}')
    if data[:4] == b'\x7fELF':
        raise NotReady(f'{bin_} is already decrypted, but it is not version 1.00 of the disc. '
                       'The port needs the EBOOT of the disc itself (no game update).')
    if not unself.is_self(data):
        raise NotReady(f'{bin_} is unreadable: this copy of the disc is itself still encrypted. '
                       'Use a decrypted copy of your disc (version 1.00). ' + ENCRYPTED_DISC_HELP)
    try:
        elf = unself.decrypt_self(data)
    except unself.DamagedSelf as e:
        raise NotReady(f'{bin_} is damaged or incomplete: {e} Copy it from your disc again. ' + ENCRYPTED_DISC_HELP)
    except unself.LoaderEboot as e:
        raise NotReady(f'{bin_} cannot be used: {e} Copies like this keep the original EBOOT.BIN in a '
                       'folder named ORIG next to PS3_GAME: put that ORIG folder back beside PS3_GAME and choose '
                       'the game folder again. Without it, use an unchanged copy of your disc (version 1.00).')
    except unself.NotDiscEboot as e:
        raise NotReady(f'{bin_} is not the EBOOT of disc version 1.00: {e} '
                       'The port needs the EBOOT of the disc itself (no game update).')
    except unself.SelfError as e:
        raise NotReady(f'The launcher could not decrypt EBOOT.BIN: {e} ' + ENCRYPTED_DISC_HELP + ' '
                       + (RPCS3_HELP_ISO if iso else RPCS3_HELP))
    if hashlib.md5(elf).hexdigest() == US_MD5:      # the US disc: the same program, converted
        return us_eboot.to_eu(elf)
    if hashlib.md5(elf).hexdigest() != ORIGINAL_MD5:
        # Only the disc EBOOT of version 1.00 has this key revision, so a
        # wrong result means damaged data, not another version.
        raise NotReady(f'{bin_} decrypted, but not to the EBOOT of disc version 1.00: the file is damaged. '
                       'Copy it from your disc again. ' + ENCRYPTED_DISC_HELP)
    return elf


def original_eboot_candidates(root, usrdir):
    """Where a copy of the disc may keep the ORIGINAL EBOOT.BIN when PS3_GAME\\USRDIR\\EBOOT.BIN
    is not it. Scene releases for old firmware (e.g. DUPLEX) put a small loader SELF (key
    revision 0x4, ~130 KB once decrypted) in USRDIR and the disc's own EBOOT.BIN in ORIG\\
    next to PS3_GAME (a player's copy, 2026-11-08). Only files that exist."""
    names = (root / 'ORIG' / 'EBOOT.BIN', root / 'ORIG' / 'PS3_GAME' / 'USRDIR' / 'EBOOT.BIN',
             root / 'ORIG' / 'USRDIR' / 'EBOOT.BIN', usrdir / 'EBOOT.BIN.ORIG', usrdir / 'EBOOT.BIN.ORG',
             usrdir / 'EBOOT.BIN.BAK', usrdir / 'ORIG' / 'EBOOT.BIN')
    return [p for p in names if p.is_file()]


def decrypt_disc_eboot_any(root, usrdir, iso='', status=lambda m: None):
    """decrypt_disc_eboot on PS3_GAME\\USRDIR\\EBOOT.BIN; when that is not the disc's own EBOOT
    (a loader or a re-signed copy), the first original_eboot_candidates file that decrypts to
    the EBOOT of disc version 1.00. Raises the USRDIR file's NotReady when none does."""
    try:
        return decrypt_disc_eboot(usrdir / 'EBOOT.BIN', iso)
    except NotReady as first:
        for alt in original_eboot_candidates(root, usrdir):
            try:
                elf = decrypt_disc_eboot(alt, iso)
            except NotReady:
                continue
            status(f'Using the original EBOOT.BIN from {alt.parent}: the one in USRDIR is a loader or a re-signed copy.')
            return elf
        raise first


# ---- the whole step --------------------------------------------------------------
def prepare(source, eboot=None, status=lambda m: None):
    """From a picked folder or .iso (and optionally a picked EBOOT.ELF), make
    the game ready. Returns the settings to save. Raises NotReady."""
    source = Path(source)
    iso = ''
    if source.is_file() and source.suffix.lower() == '.iso':
        status('Mounting the disc image…')
        iso = str(source)
        root = find_disc_root(mount_iso(source))
    else:
        root = find_disc_root(source)
    if not root:
        raise NotReady(f'No PS3_GAME folder was found in {source}. Pick the folder that holds '
                       'PS3_GAME (your disc copy), or the .iso.')
    status('Checking the game…')
    title = str(check_title(root).get('TITLE_ID', GAME_ID))
    usrdir = root / 'PS3_GAME' / 'USRDIR'
    found = find_eboot(usrdir, eboot)
    if not found and PREPARED.is_file() and _md5(PREPARED) == PATCHED_MD5:
        found = (PREPARED, PATCHED_MD5)          # prepared before: the disc may hold only EBOOT.BIN
    if not found:
        status('Decrypting EBOOT.BIN…')
        elf = decrypt_disc_eboot_any(root, usrdir, iso, status)
        status('Writing the prepared EBOOT.ELF…')
        out = write_prepared_data(elf, ORIGINAL_MD5)
        return {'eboot': str(out), 'disc_dir': str(root), 'disc_iso': iso, 'game_source': str(source),
                'port_hdd': '1', 'title_id': title}
    src, md5 = found
    status('Writing the prepared EBOOT.ELF…')
    out = write_prepared(src, md5) if Path(src) != PREPARED else PREPARED
    return {'eboot': str(out), 'disc_dir': str(root), 'disc_iso': iso, 'game_source': str(source),
            'port_hdd': '1', 'title_id': title}


# Where each .iso was last mounted: {iso: [disc root, size, mtime of the image, md5 of its PARAM.SFO]}, so a PLAY
# while the image is still mounted reuses that drive at once instead of asking Windows (PowerShell) again.
_MOUNTS = WORK / 'iso-mounts.json'


def _sfo_md5(root):
    try:
        return hashlib.md5((Path(root) / 'PS3_GAME' / 'PARAM.SFO').read_bytes()).hexdigest()
    except OSError:
        return ''


def _image_id(iso):
    st = Path(iso).stat()
    return [st.st_size, int(st.st_mtime)]


def _known_mount(iso):
    """The disc root this .iso is still mounted at, from the last mount, or None."""
    try:
        root, size, mtime, md5 = json.loads(_MOUNTS.read_text(encoding='utf-8')).get(str(iso))
    except (OSError, ValueError, TypeError, AttributeError):
        return None
    if [size, mtime] != _image_id(iso) or not md5 or _sfo_md5(root) != md5:
        return None                     # another image, an image changed, or the drive is gone / holds another disc
    return Path(root)


def _remember_mount(iso, root):
    try:
        data = json.loads(_MOUNTS.read_text(encoding='utf-8')) if _MOUNTS.is_file() else {}
        if not isinstance(data, dict):
            data = {}
    except (OSError, ValueError):
        data = {}
    data[str(iso)] = [str(root)] + _image_id(iso) + [_sfo_md5(root)]
    try:
        _MOUNTS.parent.mkdir(parents=True, exist_ok=True)
        _MOUNTS.write_text(json.dumps(data, indent=1), encoding='utf-8')
    except OSError:
        pass


def ensure_mounted(s):
    """Windows: the .iso mounted before playing; returns the disc root. An image that is still mounted
    where it was last time is reused at once; Windows is asked (and mounts it) only when it is not."""
    iso = s.get('disc_iso', '')
    if not (WINDOWS and iso):
        return None
    if not Path(iso).is_file():
        raise NotReady(f'The disc image was not found: {iso}')
    root = _known_mount(iso)
    if root:
        return root
    root = find_disc_root(mount_iso(iso))
    if not root:
        raise NotReady(f'No PS3_GAME folder in the disc image {iso}.')
    _remember_mount(iso, root)
    return root
