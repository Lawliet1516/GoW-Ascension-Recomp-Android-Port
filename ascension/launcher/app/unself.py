"""Decrypts the disc's EBOOT.BIN (a PS3 SELF) into the EBOOT.ELF the port needs,
so the player only picks the game folder or .iso.

A port of the part of RPCS3's Crypto/unself.cpp (GPL-2.0) a disc game's SELF
uses: an APP SELF without an NPDRM layer. The metadata info is AES-256-CBC
under the APP key of the SELF's key revision; the metadata headers and every
segment are AES-128-CTR; a segment may be zlib-compressed. AES comes from
Windows (CNG, bcrypt.dll). gamefiles.py uses the result only when its md5 is
the known decrypted EBOOT of disc version 1.00.
"""

import ctypes
import struct
import zlib

import os
from pathlib import Path

# No console keys ship with this source. They are read from a scetool-format
# key file that you supply (the same file tools/decrypt_self.py takes): the
# path in PS3_KEYS, else keys.txt next to the launcher. Needed entries:
#   [appldr] type=SELF revision=001C self_type=APP erk=... riv=...
#     (the APP key of God of War: Ascension's disc EBOOT, key revision 0x1C)
#   [gpkg] key=...   (only for the optional 1.04 sound mix, app/upd104.py)


def _key_file_entries():
    """The [section] blocks of the key file as dicts ([] when there is none)."""
    path = os.environ.get('PS3_KEYS') or str(Path(__file__).resolve().parent.parent / 'keys.txt')
    try:
        text = Path(path).read_text(encoding='utf-8', errors='replace')
    except OSError:
        return []
    entries, cur = [], None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith('['):
            cur = {'name': line[1:-1]}
            entries.append(cur)
        elif '=' in line and cur is not None:
            k, _, v = line.partition('=')
            cur[k.strip()] = v.strip()
    for e in entries:
        if 'category' in e:
            e.setdefault('type', e['category'])
        if 'program_type' in e:
            e.setdefault('self_type', e['program_type'])
    return entries


def app_keys():
    """{key revision: (erk hex, riv hex)} of the APP SELF keys in the key file."""
    out = {}
    for e in _key_file_entries():
        try:
            if e.get('type') == 'SELF' and e.get('self_type') == 'APP' and e.get('erk') and e.get('riv'):
                out.setdefault(int(e.get('revision', '-1'), 16), (e['erk'], e['riv']))
        except ValueError:
            pass
    return out


def named_key(name):
    """A bare key= entry of the key file (e.g. [gpkg]), as bytes, or None."""
    for e in _key_file_entries():
        if e.get('name') == name and e.get('key'):
            try:
                return bytes.fromhex(e['key'])
            except ValueError:
                return None
    return None


KEY_APP = 4          # program_identification_header.program_type
SUPPLEMENTAL_NPDRM = 3


class SelfError(Exception):
    """Why this EBOOT.BIN cannot be decrypted here."""


class NotDiscEboot(SelfError):
    """A readable SELF, but not the disc game's own EBOOT (an update, a PSN
    or debug build): decrypting it elsewhere would not help either."""


class LoaderEboot(NotDiscEboot):
    """Not the disc's program at all: a small loader signed with firmware 3.55's
    key (revision 0x4), as copies made for old custom firmware carry. The game
    behind it is packed in the loader's own format, so only the original helps."""


class DamagedSelf(SelfError):
    """The disc EBOOT, but cut short or damaged (a copy or dump that went
    wrong): only a new copy helps."""


def is_self(head):
    return head[:4] == b'SCE\0'


# ---- AES (Windows CNG) ---------------------------------------------------------
def _cng(key, chaining, data, iv=None, decrypt=False):
    """AES over whole 16-byte blocks: chaining 'CBC' or 'ECB', no padding."""
    try:
        bc = ctypes.WinDLL('bcrypt')
    except (AttributeError, OSError):
        raise SelfError('AES is not available (bcrypt.dll).')
    vp, ul = ctypes.c_void_p, ctypes.c_ulong
    alg, hkey = vp(), vp()
    if bc.BCryptOpenAlgorithmProvider(ctypes.byref(alg), ctypes.c_wchar_p('AES'), None, 0):
        raise SelfError('AES is not available (BCryptOpenAlgorithmProvider).')
    try:
        mode = ctypes.create_unicode_buffer('ChainingMode' + chaining)
        if bc.BCryptSetProperty(alg, ctypes.c_wchar_p('ChainingMode'), mode, ctypes.sizeof(mode), 0):
            raise SelfError('AES chaining mode could not be set.')
        kb = ctypes.create_string_buffer(bytes(key), len(key))
        if bc.BCryptGenerateSymmetricKey(alg, ctypes.byref(hkey), None, 0, kb, len(key), 0):
            raise SelfError('AES key could not be made.')
        try:
            src = ctypes.create_string_buffer(bytes(data), len(data))
            dst = ctypes.create_string_buffer(len(data))
            ivb = ctypes.create_string_buffer(bytes(iv), len(iv)) if iv else None
            done = ul(0)
            fn = bc.BCryptDecrypt if decrypt else bc.BCryptEncrypt
            if fn(hkey, src, ul(len(data)), None, ivb, ul(len(iv) if iv else 0), dst, ul(len(data)),
                  ctypes.byref(done), 0) or done.value != len(data):
                raise SelfError('AES failed.')
            return dst.raw
        finally:
            bc.BCryptDestroyKey(hkey)
    finally:
        bc.BCryptCloseAlgorithmProvider(alg, 0)


def _cbc_decrypt(key, iv, data):
    return _cng(key, 'CBC', data, iv, decrypt=True)


def _ctr(key, iv, data):
    """AES-CTR as RPCS3 (mbedtls) does it: the whole 16-byte IV is a
    big-endian counter, one per block."""
    n = (len(data) + 15) // 16
    c0 = int.from_bytes(iv, 'big')
    mask = (1 << 128) - 1
    counters = b''.join(((c0 + i) & mask).to_bytes(16, 'big') for i in range(n))
    stream = _cng(key, 'ECB', counters)[:len(data)]
    return (int.from_bytes(data, 'big') ^ int.from_bytes(stream, 'big')).to_bytes(len(data), 'big')


# ---- the SELF -------------------------------------------------------------------
def decrypt_self(data):
    """The ELF inside a disc game's SELF, as bytes. Raises SelfError."""
    try:
        return _decrypt_self(bytes(data))
    except (struct.error, IndexError, ValueError, OverflowError, MemoryError):
        raise DamagedSelf('Its headers point past the end of the file.')


def _decrypt_self(data):
    if len(data) < 0x80 or not is_self(data):
        raise SelfError('Not a PS3 executable (no SCE header).')
    _, _, key_rev, se_type, meta, hsize, _ = struct.unpack_from('>IIHHIQQ', data, 0)
    if se_type != 1:
        raise SelfError(f'Not a SELF (header type {se_type}).')
    if key_rev & 0x8000:
        raise NotDiscEboot('It is a debug build, not the disc EBOOT.')
    (_, prog_off, ehdr_off, phdr_off, shdr_off, _, _, supp_off, supp_size, _) = \
        struct.unpack_from('>10Q', data, 0x20)
    _, _, prog_type, _ = struct.unpack_from('>QIIQ', data, prog_off)
    if prog_type != KEY_APP:
        raise NotDiscEboot(f'It is not a disc game executable (program type {prog_type}).')
    o = supp_off
    while o < supp_off + supp_size:
        stype, ssize = struct.unpack_from('>II', data, o)
        if stype == SUPPLEMENTAL_NPDRM:
            raise NotDiscEboot('It is a PSN (NPDRM) executable, not the disc EBOOT.')
        if ssize == 0:
            break
        o += ssize
    if key_rev == 0x0004:
        raise LoaderEboot('This copy was changed to run on old custom firmware: its EBOOT.BIN is a loader '
                          '(key revision 0x4), not the game program of the disc (key revision 0x1C).')
    APP_KEYS = app_keys()
    if not APP_KEYS:
        raise SelfError('No PS3 key file: put a scetool-format keys.txt next to the launcher (or set PS3_KEYS), '
                        'or decrypt EBOOT.BIN yourself (RPCS3 or tools/decrypt_self.py) and pick the EBOOT.ELF.')
    if key_rev not in APP_KEYS:
        raise NotDiscEboot(f'Its key revision is 0x{key_rev:X}, the disc EBOOT of version 1.00 has 0x1C '
                           "(a game update's EBOOT?).")
    erk, riv = (bytes.fromhex(h) for h in APP_KEYS[key_rev])

    # Metadata info (key, iv) under the APP key, then the headers under those.
    info_off = 0x20 + meta
    info = _cbc_decrypt(erk, riv, data[info_off:info_off + 0x40])
    if info[0x10] != 0 or info[0x30] != 0:
        raise SelfError('The metadata did not decrypt (wrong key).')
    headers = _ctr(info[0x00:0x10], info[0x20:0x30], data[info_off + 0x40:hsize])
    _, _, nsec, nkeys, _, _, _ = struct.unpack_from('>QIIIIII', headers, 0)
    sections = [struct.unpack_from('>QQ8I', headers, 0x20 + 0x30 * i) for i in range(nsec)]
    keys_off = 0x20 + 0x30 * nsec
    keys = [headers[keys_off + 16 * k:keys_off + 16 * (k + 1)] for k in range(nkeys)]

    ehdr = data[ehdr_off:ehdr_off + 0x40]
    if ehdr[:4] != b'\x7fELF' or ehdr[4] != 2:
        raise SelfError('The executable is not a 64-bit ELF.')
    phoff, shoff = struct.unpack_from('>QQ', ehdr, 0x20)
    phentsize, phnum, shentsize, shnum = struct.unpack_from('>HHHH', ehdr, 0x36)
    phdr_raw = data[phdr_off:phdr_off + phnum * 0x38]
    phdrs = [struct.unpack_from('>IIQQQQQQ', phdr_raw, 0x38 * i) for i in range(phnum)]

    out = bytearray()

    def put(offset, blob):
        end = offset + len(blob)
        if len(out) < end:
            out.extend(bytes(end - len(out)))
        out[offset:end] = blob

    put(0, ehdr)
    for (d_off, d_size, typ, prog_idx, _, _, encrypted, key_idx, iv_idx, compressed) in sections:
        if typ != 2:                       # program segments only, as RPCS3's WriteElf
            continue
        if encrypted != 3 or key_idx >= nkeys or iv_idx >= nkeys or prog_idx >= phnum:
            raise SelfError('An unexpected segment layout.')
        if d_off + d_size > len(data):
            raise DamagedSelf('It ends before its last segment.')
        seg = _ctr(keys[key_idx], keys[iv_idx], data[d_off:d_off + d_size])
        p_offset, p_filesz = phdrs[prog_idx][2], phdrs[prog_idx][5]
        if compressed == 2:
            z = zlib.decompressobj()
            try:
                seg = z.decompress(seg, p_filesz)
            except zlib.error:
                raise DamagedSelf('A segment did not decompress.')
            if len(seg) < p_filesz and not z.eof:
                raise DamagedSelf('A segment is cut short.')
        put(p_offset, seg)
    if shdr_off:
        put(shoff, data[shdr_off:shdr_off + shnum * shentsize])
    if phdrs:                              # last, so segment data cannot overwrite them
        put(phoff, phdr_raw[:phnum * phentsize])
    return bytes(out)
