"""Carve God of War: Ascension's 33 Wws SPU job modules from a decrypted EBOOT.ELF.

The 15 modules named in the port's tools/jobmods_extra_funcs.json get the exact
lengths that reproduce their names (found by search); the other 18 use RULE.
Usage: python -I jobmods_carve.py EBOOT.ELF known.json OUTDIR [RULE] [--write]
"""
import hashlib, json, os, struct, sys

KNOWN_LEN = {  # module EA -> (length, zero-pad-to)
    0x6B2480: (0x2790, None), 0x6B4C80: (0x15A0, None), 0x6B6280: (0xCE30, None),
    0x6C3100: (0x13560, None), 0x70C180: (0xC10, None), 0x70EE80: (0x4440, None),
    0x761880: (0x8130, None), 0x769A00: (0x7040, None), 0x77C180: (0xC00, None),
    0x82B780: (0xAA70, None), 0x6DFF80: (0x400, None), 0x6FB300: (0xD000, None),
    0x794C00: (0x22000, None), 0x7B6B80: (0x1D800, None), 0x7ED580: (0x5E50, 0x6000),
}

def modules(elf):
    phoff, = struct.unpack_from('>Q', elf, 0x20)
    out = []
    for i in range(struct.unpack_from('>H', elf, 0x38)[0]):
        t, _f, off, va, _pa, fsz = struct.unpack_from('>IIQQQQ', elf, phoff + i * 56)
        if not (t == 1 and fsz):
            continue
        data = elf[off:off + fsz]
        st, hit = [], data.find(b'\xC0\xDE\xC0\xDE')
        while hit >= 0:
            s = hit - 0x20
            if s >= 0 and s % 16 == 0 and all((w >> 25) == 0x21 for w in struct.unpack_from('>4I', data, s)):
                st.append(s)
            hit = data.find(b'\xC0\xDE\xC0\xDE', hit + 1)
        for j, s in enumerate(st):
            out.append((va + s, data[s:(st[j + 1] if j + 1 < len(st) else len(data))], data[s:]))
    return out

def trailer(blob, extra):
    i = blob.rfind(b'\xff\xff\xff\xff')
    return len(blob) if i < 0 else min(len(blob), (i // 16) * 16 + 16 + extra)

RULES = {
    'gap': lambda b: len(b),
    'trailer+0': lambda b: trailer(b, 0),
    'trailer+0x10': lambda b: trailer(b, 0x10),
    'trailer+0x20': lambda b: trailer(b, 0x20),
    'stripzero16': lambda b: (len(b.rstrip(b'\0')) + 15) // 16 * 16,
    'gap_r1k': lambda b: (len(b) + 0x3FF) // 0x400 * 0x400,
    'size-32': lambda b: min(len(b), struct.unpack_from('>I', b, 0x14)[0] - 32),
}

def carve(elf, known, rule):
    res = []
    for ea, blob, full in modules(elf):
        if ea in KNOWN_LEN:
            L, pad = KNOWN_LEN[ea]
            m = full[:L] + (b'\0' * (pad - L) if pad else b'')
        else:
            m = full[:RULES[rule](blob)]
        name = f"jobmod_{hashlib.sha1(m).hexdigest()[:12]}_e{struct.unpack_from('>I', m, 0x10)[0]:04X}"
        res.append((ea, name, m))
    return res

if __name__ == '__main__':
    elf = open(sys.argv[1], 'rb').read()
    known = set(json.load(open(sys.argv[2])))
    out = sys.argv[3]
    rules = [sys.argv[4]] if len(sys.argv) > 4 and not sys.argv[4].startswith('--') else list(RULES)
    for rule in rules:
        res = carve(elf, known, rule)
        names = sorted(n for _, n, _ in res)
        miss = [k for k in known if k not in names]
        pos = {k: 200 + names.index(k) for k in ('jobmod_31107645db58_e16A80', 'jobmod_7868e9b847d1_e6D98') if k in names}
        print(f'{rule:14} modules {len(res)}  known missing {len(miss)}  ids {pos}  (want 204, 218)')
    if '--write' in sys.argv and len(rules) == 1:
        os.makedirs(out, exist_ok=True)
        for ea, n, m in res:
            open(os.path.join(out, n + '.bin'), 'wb').write(m)
        print('wrote', len(res), 'modules to', out)
