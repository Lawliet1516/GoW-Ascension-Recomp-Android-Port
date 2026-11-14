"""Builds launcher/app/us_eboot.py: the difference between the decrypted EBOOT.ELF of the US disc
(BCUS98232 1.00) and the European one the port is built from (BCES01741 1.00).

    python -I tools/make_us_eboot_delta.py US-EBOOT.ELF EU-EBOOT.ELF launcher/app/us_eboot.py

The two discs carry the same multi-region program (the same title-ID tables, "LANGS_%s.TXT",
"%s-SYSDATA"): the US build's code sits 16 bytes later from 0x9700 on, so every address in it
differs by 16. Per 256-byte block of the European file the generator picks the US offset that
matches best (0, +-16, +-32) and stores the XOR of the two: 0.46% of the bytes are not zero
(the moved addresses), and the whole delta is ~42 KB with lzma. It holds no game data beyond
those differing bytes; the launcher applies it to the player's own US EBOOT and checks the md5.
"""
import base64
import hashlib
import lzma
import struct
import sys

B = 256
CANDIDATES = (0, 16, -16, 32, -32)


def build(us, eu):
    if len(us) != len(eu):
        raise SystemExit('the two ELFs differ in size')
    n = len(eu)
    deltas = bytearray()
    xor = bytearray(n)
    for o in range(0, n, B):
        blk = eu[o:o + B]
        best = None
        for d in CANDIDATES:
            s = o + d
            if s < 0 or s + len(blk) > len(us):
                continue
            src = us[s:s + len(blk)]
            eq = sum(1 for a, b in zip(blk, src) if a == b)
            if best is None or eq > best[0]:
                best = (eq, d)
        d = best[1]
        deltas.append(d & 0xFF)
        src = us[o + d:o + d + len(blk)]
        for j in range(len(blk)):
            xor[o + j] = blk[j] ^ src[j]
    head = struct.pack('<4sIII', b'GUE1', n, B, len(deltas))
    return lzma.compress(head + bytes(deltas) + bytes(xor), preset=9 | lzma.PRESET_EXTREME)


MODULE = '''"""The US disc (BCUS98232 1.00) as the European one the port is built from (BCES01741 1.00).

Both discs carry the same multi-region program; the US build's code sits 16 bytes later, so its
decrypted EBOOT.ELF differs from the European one only in where things are. DELTA (made by
tools/make_us_eboot_delta.py) is that difference: per 256-byte block an offset into the US file and
the XOR of the two (lzma). to_eu() turns the player's own US EBOOT.ELF into the European one and
checks the result's md5, so the port's code, patches and natives apply unchanged. The game data
needs nothing: the game builds its language table, boot screen and save folder names from the
disc's title ID (LANGS_BCUS98232.TXT, BCUS98232-SYSDATA, ...), all of which the US disc has.
"""
import base64
import hashlib
import lzma
import struct

US_MD5 = {us_md5!r}     # decrypted EBOOT.ELF of the US disc, BCUS98232 1.00
EU_MD5 = {eu_md5!r}     # decrypted EBOOT.ELF of the European disc, BCES01741 1.00

DELTA = (
{chunks}
)


def to_eu(us):
    """The European EBOOT.ELF from the US one (bytes). ValueError when `us` is not the US
    EBOOT of version 1.00 or the result is not the European one."""
    if hashlib.md5(us).hexdigest() != US_MD5:
        raise ValueError('not the decrypted EBOOT of the US disc, version 1.00')
    raw = lzma.decompress(base64.b85decode(DELTA))
    magic, n, block, count = struct.unpack_from('<4sIII', raw, 0)
    if magic != b'GUE1' or len(us) != n:
        raise ValueError('the US conversion data does not match this EBOOT')
    base = struct.calcsize('<4sIII')
    deltas = raw[base:base + count]
    xor = raw[base + count:base + count + n]
    parts = []
    for i in range(count):
        o = i * block
        d = deltas[i] - 256 if deltas[i] > 127 else deltas[i]
        parts.append(us[o + d:o + d + min(block, n - o)])
    src = b''.join(parts)
    out = (int.from_bytes(src, 'little') ^ int.from_bytes(xor, 'little')).to_bytes(n, 'little')
    if hashlib.md5(out).hexdigest() != EU_MD5:
        raise ValueError('the converted EBOOT is not the European one')
    return out
'''


def main(argv):
    us = open(argv[1], 'rb').read()
    eu = open(argv[2], 'rb').read()
    blob = base64.b85encode(build(us, eu)).decode('ascii')
    chunks = '\n'.join(f"    '{blob[i:i + 96]}'" for i in range(0, len(blob), 96))
    text = MODULE.format(us_md5=hashlib.md5(us).hexdigest(), eu_md5=hashlib.md5(eu).hexdigest(), chunks=chunks)
    with open(argv[3], 'w', encoding='ascii', newline='\r\n') as f:
        f.write(text)
    print(argv[3], len(text), 'chars; delta', len(blob), 'b85 chars')


if __name__ == '__main__':
    main(sys.argv)
