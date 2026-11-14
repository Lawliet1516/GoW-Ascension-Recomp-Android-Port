"""The 1.04 sound mix: MASTER_MIXER.GMX from the player's own official 1.04
update package (.pkg), for the launcher to put into its GAME.PSARC copy.

The port runs the 1.00 game code, which cannot load update packs. This one
data file drops into 1.00 as is: same size, the same 227 sound groups; 17 mix
levels changed (15 louder), Sony's "music too quiet" / Hecaton mix fix.

Nothing is downloaded and nothing from the game ships with the launcher: the
file is read from the player's package (decrypted with the public PS3 package
key, AES through Windows CNG, as unself.py does for the EBOOT), checked
against the known 1.04 file, and kept in the launcher's own folder.
"""
import hashlib
import json
import struct
import threading
import zlib
from pathlib import Path

from .config import CONFIG_DIR
from .i18n import tr

GMX_PATH = '/SOUND/PS3_BE/MASTER_MIXER.GMX'     # inside GAME.PSARC (disc) and TPPS*.PSARC (updates)
GMX_SIZE = 63592
# SHA-256 of the 1.04 file (the same in the US and EU 1.04 packages) and of the disc's 1.00 file (US = EU)
GMX_104 = '79498c20a1d1283f6e9dd7240752f961420f2300b6c26eb5808f29d7990e8484'
GMX_100 = '583afd2d6ad7f0c51a09825c66ca4a75a2df4cc3aa77ca1abd8e0d7f5bcba757'
# The later updates' mixes have groups for content the 1.00 code does not have: refused, by version
GMX_LATER = {
    '39812c5e6610bd91cb4260bcb71bb7dc0acbcaac29521f309eab384641cd1780': '1.06',
    '694e32574b7ec4169c37b6efd55a029d090147ca00d37d92447cf0e849f440a7': '1.08',
    '88ecfb3a4eddb27d9065e2c63fb962a6de35c5c01537d601e7caacd66dcc71ef': '1.09',
    'e321b5c6c1b34a8d268064e697a5be9a974a3382d294f0f22e5c7092857c0cdd': '1.10',
    'd94a2bce0eca4c06073d67ef72e01268bf22e0424c4d953af427da0dcfe8a519': '1.11',
}
CACHE = CONFIG_DIR / 'update-1.04'          # not under generated/: that folder is a PS3_MODS overlay
_LOCK = threading.Lock()                    # the Mods page's check and the build after Save share the cache


class UpdateError(Exception):
    """Why the package cannot give the 1.04 sound mix (text for the player)."""


class _Pkg:
    """A finalized PS3 .pkg: AES-128-CTR, counter = the header's IV + block number."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        hdr = self.f.read(0xA0)
        if len(hdr) < 0xA0 or hdr[:4] != b'\x7fPKG':
            raise UpdateError(tr('{name} is not a PS3 package (.pkg).', name=Path(path).name))
        self.items, self.total_size = struct.unpack('>IQ', hdr[0x14:0x20])
        self.data_off, self.data_size = struct.unpack('>QQ', hdr[0x20:0x30])
        self.content_id = hdr[0x30:0x60].rstrip(b'\0').decode('ascii', 'replace')
        self.riv = int.from_bytes(hdr[0x70:0x80], 'big')

    def close(self):
        self.f.close()

    def read(self, start, length):
        from . import unself
        if length <= 0:
            return b''
        b0 = start // 16
        skip = start - b0 * 16
        n = (skip + length + 15) // 16
        self.f.seek(self.data_off + b0 * 16)
        raw = self.f.read(n * 16)
        raw += bytes(n * 16 - len(raw))
        try:
            gpkg = unself.named_key('gpkg')
            if gpkg is None:
                raise OSError('no [gpkg] key in the key file (PS3_KEYS or keys.txt next to the launcher)')
            plain = unself._ctr(gpkg, ((self.riv + b0) & ((1 << 128) - 1)).to_bytes(16, 'big'), raw)
        except unself.SelfError as e:
            raise UpdateError(tr('The package could not be decrypted: {error}', error=e))
        return plain[skip:skip + length]

    def files(self):
        """[(name, offset, size)] of the package's files."""
        if not 0 < self.items < 100000:
            raise UpdateError(tr('The package could not be read (its file table is damaged).'))
        table = self.read(0, self.items * 0x20)
        out = []
        for i in range(self.items):
            no, ns, fo, fs, _flags = struct.unpack('>IIQQI', table[i * 0x20:i * 0x20 + 0x1C])
            name = self.read(no, ns).rstrip(b'\0')
            if not name or any(c < 0x20 or c > 0x7E for c in name):
                raise UpdateError(tr('The package could not be read (not a finished retail package?).'))
            out.append((name.decode('ascii'), fo, fs))
        return out


def _psarc_file(pkg, base, size, inner):
    """One file of a PSARC that sits at `base` in the package (PSAR 1.4, zlib), or None."""
    head = pkg.read(base, 32)
    if len(head) < 32 or head[:4] != b'PSAR' or head[8:12] != b'zlib':
        return None
    toc_len, ent_size, n, block = struct.unpack('>IIII', head[12:28])
    if not (32 < toc_len <= size and 30 <= ent_size and 0 < n and block):
        return None
    toc = pkg.read(base + 32, toc_len - 32)
    zb = 2 if block <= 0x10000 else 3 if block <= 0x1000000 else 4
    table = toc[n * ent_size:]
    zsizes = [int.from_bytes(table[i:i + zb], 'big') for i in range(0, len(table) - zb + 1, zb)]

    def entry(i):
        e = toc[i * ent_size:(i + 1) * ent_size]
        b0, length, off = struct.unpack('>I', e[16:20])[0], int.from_bytes(e[20:25], 'big'), int.from_bytes(e[25:30], 'big')
        data, pos = b'', off
        for b in range(b0, b0 + max(1, -(-length // block)) if length else b0):
            z = zsizes[b]
            raw = pkg.read(base + pos, z or block)
            pos += z or block
            left = length - len(data)
            if z == 0 or (len(raw) == min(block, left) and raw[:1] != b'\x78'):
                data += raw
            else:
                try:
                    data += zlib.decompress(raw)
                except zlib.error:
                    data += raw
        return data[:length]

    names = entry(0).decode('utf-8', 'replace').split('\n')
    want = inner.upper()
    for i, name in enumerate(names, 1):
        if name.upper() == want and i < n:
            return entry(i)
    return None


def gmx_from_pkg(path):
    """(the GMX bytes, the package's content ID) from an update package. Raises UpdateError
    with the reason when the package has no fitting sound mix."""
    path = Path(path)
    if not path.is_file():
        raise UpdateError(tr('The update package was not found: {path}', path=path))
    pkg = _Pkg(path)
    try:
        packs = [(n, o, s) for n, o, s in pkg.files() if n.upper().endswith('.PSARC') and
                 Path(n).name.upper().startswith('TPPS') and '-' not in Path(n).name]
        found = None
        for name, off, size in packs:
            found = _psarc_file(pkg, off, size, GMX_PATH)
            if found is not None:
                break
        cid = pkg.content_id
        short = path.stat().st_size < pkg.total_size
    except (struct.error, IndexError, ValueError, OverflowError, MemoryError, UnicodeError):
        raise UpdateError(tr('The package could not be read (its file table is damaged).'))
    finally:
        pkg.close()
    if found is None and short:
        raise UpdateError(tr('{name} is incomplete (its download did not finish). Download it again.', name=path.name))
    if found is None:
        raise UpdateError(tr('{name} has no sound mix (update 1.05, 1.07 and 1.12 carry only the program). '
                             'Choose the 1.04 package (its name has A0104).', name=path.name))
    digest = hashlib.sha256(found).hexdigest()
    if digest in GMX_LATER:
        raise UpdateError(tr('{name} is update {version}: its sound mix has groups for content the port '
                             'does not have. Choose the 1.04 package (its name has A0104).',
                             name=path.name, version=GMX_LATER[digest]))
    if digest != GMX_104 or len(found) != GMX_SIZE:
        raise UpdateError(tr('The sound mix in {name} is not the known 1.04 one; it is not used.', name=path.name))
    return found, cid


def gmx_file(pkg_path):
    """The checked 1.04 GMX in the launcher's own folder, taken from the package the first
    time (and again when the package changes). Once taken, the package may be moved or
    deleted: the checked copy is used while the setting still names that package.
    Returns (path, content ID)."""
    with _LOCK:
        return _gmx_file(Path(pkg_path))


def _gmx_file(p):
    out, keyf = CACHE / 'MASTER_MIXER.GMX', CACHE / 'source.json'
    try:
        saved = json.loads(keyf.read_text(encoding='utf-8'))
        good = hashlib.sha256(out.read_bytes()).hexdigest() == GMX_104
    except (OSError, ValueError):
        saved, good = {}, False
    if not isinstance(saved, dict):
        saved, good = {}, False
    try:
        st = p.stat()
    except OSError:
        if good and saved.get('path') == str(p.absolute()):
            return out, saved.get('content_id', '')
        raise UpdateError(tr('The update package was not found: {path}', path=p))
    key = json.dumps([str(p.resolve()), st.st_size, int(st.st_mtime)])
    if good and saved.get('key') == key:
        return out, saved.get('content_id', '')
    data, cid = gmx_from_pkg(p)
    CACHE.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix('.tmp')
    tmp.write_bytes(data)
    tmp.replace(out)
    keyf.write_text(json.dumps({'key': key, 'path': str(p.absolute()), 'content_id': cid}), encoding='utf-8')
    return out, cid


def psarc_ops(s):
    """The GAME.PSARC change for the setting, as compose_mods takes them: {} when it is off.
    Raises OSError with the reason when it is on but the package cannot give the file."""
    if str(s.get('sound_mix_104', '0')) != '1':
        return {}
    pkg = str(s.get('update_pkg') or '').strip()
    if not pkg:
        raise OSError(tr('"1.04 sound mix" is on, but no update package is chosen. Choose your 1.04 update '
                         '(.pkg) on the Mods page, or turn the option off.'))
    try:
        path, _cid = gmx_file(pkg)
    except UpdateError as e:
        raise OSError(tr('1.04 sound mix: {reason}', reason=e))
    except OSError as e:
        raise OSError(tr('1.04 sound mix: the package could not be read ({error}).', error=e))
    return {'GAME.PSARC': {GMX_PATH: {'data_file': str(path), 'sha256': GMX_104}}}


def describe(pkg_path):
    """A line for the Mods page: what the chosen package gives."""
    if not str(pkg_path or '').strip():
        return tr('No update package chosen.')
    try:
        _path, cid = gmx_file(pkg_path)
    except (UpdateError, OSError) as e:
        return '✗ ' + str(e)
    except Exception as e:      # the page's line must never stay on "Checking the package…"
        return '✗ ' + tr('1.04 sound mix: the package could not be read ({error}).', error=e)
    return '✓ ' + tr('1.04 sound mix found ({id}).', id=cid or '?')
