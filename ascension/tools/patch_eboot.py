"""Prepares your own decrypted EBOOT.ELF for this build of the port.

This build was made with three RPCS3 game patches built in: Disable Motion
Blur, Unlock FPS and Disable SPU MLAA. The game's executable reads its data
from EBOOT.ELF, so that file needs the same 30 patched bytes. This script
writes them, after checking that the file is the right one (God of War:
Ascension, BCES01741, version 1.00, decrypted).

    python patch_eboot.py [path\\to\\EBOOT.ELF]

With no path it uses ..\\..\\work\\disc\\PS3_GAME\\USRDIR\\EBOOT.ELF. The
original is kept beside it as EBOOT.ELF.orig.
"""
import hashlib
import shutil
import sys
from pathlib import Path

ORIGINAL_MD5 = '4c12e201bd8579dbd4f9410b4c65af3c'   # decrypted EBOOT.ELF, BCES01741 v1.00
PATCHED_MD5 = 'e84f2bf7f782869b76d6376cd340d277'    # what this build expects
SIZE = 8584256
# (file offset, original bytes, patched bytes)
PATCHES = [
    (2744758, 'af14', '913c'), (2767848, 'd024af14', '60000000'), (2767932, '43c800', '461c40'),
    (2768086, 'af14', '913c'), (2768254, 'af14', '913c'), (3569300, '426fc28f', '447a0000'),
    (3900298, 'af14', '913c'), (4164336, '80bf', '38a0'), (4164339, '20', '00'),
    (6818496, '580243', '408000'), (6823236, '58268d', '408000'), (8433428, 'bf80', '4270'),
]


def main():
    default = Path(__file__).resolve().parents[2] / 'work' / 'disc' / 'PS3_GAME' / 'USRDIR' / 'EBOOT.ELF'
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else default
    if not path.is_file():
        print(f'Not found: {path}')
        print('Decrypt EBOOT.BIN to EBOOT.ELF with RPCS3 (Utilities > Decrypt PS3 Binaries) and put it there.')
        return 1
    data = bytearray(path.read_bytes())
    md5 = hashlib.md5(data).hexdigest()
    if md5 == PATCHED_MD5:
        print(f'Already prepared: {path}')
        return 0
    if len(data) != SIZE or md5 != ORIGINAL_MD5:
        print(f'This EBOOT.ELF is not the expected one (md5 {md5}, {len(data)} bytes).')
        print('It must be the decrypted EBOOT of God of War: Ascension BCES01741, version 1.00 (no update).')
        return 1
    for offset, old, new in PATCHES:
        old, new = bytes.fromhex(old), bytes.fromhex(new)
        assert data[offset:offset + len(old)] == old
        data[offset:offset + len(new)] = new
    if hashlib.md5(data).hexdigest() != PATCHED_MD5:
        print('Patching gave an unexpected result; nothing was written.')
        return 1
    backup = path.with_name(path.name + '.orig')
    if not backup.exists():
        shutil.copy2(path, backup)
    path.write_bytes(data)
    print(f'Done: {path} is ready (original kept as {backup.name}).')
    return 0


if __name__ == '__main__':
    sys.exit(main())
