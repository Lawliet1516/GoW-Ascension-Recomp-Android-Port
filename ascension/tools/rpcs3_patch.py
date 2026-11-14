#!/usr/bin/env python3
"""RPCS3 game patches for the recompiled God of War: Ascension.

RPCS3 patches the game in memory as it runs. Here the game's code is translated
to C++ ahead of time, so a patch has to be written into the executable BEFORE
the translation: this tool applies the chosen patches to a pristine decrypted
EBOOT.ELF and says what changed, so the build re-translates only what it must
(the main PPU code, or the embedded SPU programs).

Patches are matched exactly as RPCS3 matches them:
  PPU-<sha1>  the executable (SHA-1 over each segment's type and flags, and the
              address, size and bytes of each loaded segment)
  SPU-<sha1>  an SPU program embedded in the executable (SHA-1 over each of its
              segments' address and sizes, and its loaded and NOTE bytes)

    python tools/rpcs3_patch.py info  --elf EBOOT.elf [--patches patch.yml ...]
    python tools/rpcs3_patch.py apply --elf EBOOT.elf --out patched.ELF
                                      [--patches ...] --enable "Patch name" ...
                                      [--summary changes.json]
    python tools/rpcs3_patch.py runtime --elf EBOOT.elf --table patch_table.inc
                                      --sites sites.json --out all-on.ELF
                                      [--enable "default patch" ...]

runtime: every patch for this executable as a switch read when the game
starts (PS3_PATCHES, host/ascension_patches.cpp) instead of a translation: the
table of each patch's writes (guest address, original and patched bytes), the
PPU instructions they change for ppu_lifter.py --patch-sites (each lifted both
ways, picked by the word in guest memory), and the executable with every patch
applied, whose changed SPU programs are lifted beside the originals.

Supported operations: byte, be16, be32, be64, le16, le32, le64, bef32, bef64,
lef32, lef64, utf8, c_utf8 and load (anchor lists). Operations that need new
code at run time (code_alloc, jump, jump_link, jump_func, alloc, bp_exec) are
reported and skipped.
"""
import argparse
import hashlib
import json
import os
import struct
import sys

try:
    import yaml
except ImportError:
    sys.exit("PyYAML is needed: python -m pip install --user pyyaml")


# --------------------------------------------------------------------------
# ELF helpers
# --------------------------------------------------------------------------

def ppu_segments(elf):
    """(type, flags, offset, vaddr, filesz, memsz) of each program header."""
    if elf[:4] != b"\x7fELF" or elf[4] != 2:
        sys.exit("not a decrypted 64-bit PPU ELF")
    phoff, = struct.unpack_from(">Q", elf, 0x20)
    phent, phnum = struct.unpack_from(">HH", elf, 0x36)
    out = []
    for i in range(phnum):
        p_type, p_flags, off, va, _pa, filesz, memsz, _al = struct.unpack_from(">IIQQQQQQ", elf, phoff + i * phent)
        out.append((p_type, p_flags, off, va, filesz, memsz))
    return out


def ppu_hash(elf):
    sha = hashlib.sha1()
    for p_type, p_flags, off, va, filesz, memsz in ppu_segments(elf):
        sha.update(struct.pack(">I", p_type))
        sha.update(struct.pack(">I", p_flags))
        if p_type == 1 and memsz:
            sha.update(struct.pack(">Q", va))
            sha.update(struct.pack(">Q", memsz))
            sha.update(elf[off:off + filesz])
    return "PPU-" + sha.hexdigest()


def ppu_file_offset(elf, addr, size):
    for p_type, _f, off, va, filesz, _m in ppu_segments(elf):
        if p_type == 1 and va <= addr and addr + size <= va + filesz:
            return off + addr - va
    return None


# End of the lifted PPU code in God of War: Ascension 1.00 (the import stubs
# end here; read-only data and the embedded SPU programs follow it).
CODE_END = 0x62C4CC


def spu_images(elf):
    """Each SPU ELF embedded in the executable: (file offset, progs, hash, name)."""
    out = []
    pos = elf.find(b"\x7fELF\x01\x02")
    while pos >= 0:
        try:
            if struct.unpack_from(">H", elf, pos + 18)[0] == 23:   # EM_SPU
                phoff, = struct.unpack_from(">I", elf, pos + 0x1C)
                phent, phnum = struct.unpack_from(">HH", elf, pos + 0x2A)
                progs, sha, name = [], hashlib.sha1(), ""
                for i in range(phnum):
                    t, off, va, _pa, filesz, memsz, _fl, _al = struct.unpack_from(">8I", elf, pos + phoff + i * phent)
                    progs.append((t, off, va, filesz, memsz))
                    sha.update(struct.pack(">III", va, memsz, filesz))
                    if t == 1 and filesz:
                        sha.update(elf[pos + off:pos + off + filesz])
                    elif t == 4 and filesz:
                        sha.update(elf[pos + off:pos + off + filesz])
                        name = elf[pos + off + 0x14:pos + off + 0x14 + 64].split(b"\0")[0].decode("latin1")
                out.append((pos, progs, "SPU-" + sha.hexdigest(), name))
        except struct.error:
            pass
        pos = elf.find(b"\x7fELF\x01\x02", pos + 1)
    return out


# --------------------------------------------------------------------------
# Patch file
# --------------------------------------------------------------------------

class PatchLoader(getattr(yaml, "CSafeLoader", yaml.SafeLoader)):
    """RPCS3's YAML reader lets a later anchor of the same name replace an
    earlier one (patch.yml relies on it); PyYAML's refuses."""


def _compose_node(self, parent, index):
    if not self.check_event(yaml.events.AliasEvent):
        anchor = getattr(self.peek_event(), "anchor", None)
        if anchor is not None and anchor in self.anchors:
            del self.anchors[anchor]
    return yaml.composer.Composer.compose_node(self, parent, index)


if PatchLoader.__mro__[1] is yaml.SafeLoader:   # the pure-Python composer
    PatchLoader.compose_node = _compose_node
else:                                             # libyaml: use the Python one
    class PatchLoader(yaml.SafeLoader):           # noqa: F811
        pass
    PatchLoader.compose_node = _compose_node


def load_patches(paths):
    """{hash: {patch name: info}} merged from every patch file."""
    db = {}
    for path in paths:
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as f:
            data = yaml.load(f, Loader=PatchLoader) or {}
        for key, entries in data.items():
            if not (isinstance(key, str) and (key.startswith("PPU-") or key.startswith("SPU-"))):
                continue
            if not isinstance(entries, dict):
                continue
            for name, info in entries.items():
                if isinstance(info, dict) and "Patch" in info:
                    info = dict(info)
                    info["_file"] = os.path.basename(path)
                    db.setdefault(key, {})[name] = info
    return db


def flatten(ops):
    """Expand `load` entries (YAML anchors arrive as nested lists)."""
    for op in ops or []:
        if isinstance(op, list) and op and op[0] == "load":
            yield from flatten(op[1])
        else:
            yield op


def encode(kind, value):
    if kind == "byte":
        return struct.pack(">B", int(value) & 0xFF)
    ints = {"be16": ">H", "be32": ">I", "be64": ">Q", "le16": "<H", "le32": "<I", "le64": "<Q"}
    if kind in ints:
        bits = {"H": 16, "I": 32, "Q": 64}[ints[kind][1]]
        return struct.pack(ints[kind], int(value) & ((1 << bits) - 1))
    floats = {"bef32": ">f", "bef64": ">d", "lef32": "<f", "lef64": "<d"}
    if kind in floats:
        return struct.pack(floats[kind], float(value))
    if kind == "utf8":
        return str(value).encode("utf-8")
    if kind == "c_utf8":
        return str(value).encode("utf-8") + b"\0"
    return None


def games_text(info):
    games = info.get("Games") or {}
    parts = []
    if isinstance(games, dict):
        for title, serials in games.items():
            if isinstance(serials, dict):
                parts.append(f"{title} " + ", ".join(f"{s} {v}" for s, v in serials.items()))
    return "; ".join(parts)


def applicable(elf, db):
    """[(hash, kind, name, info, spu image or None)] for this executable."""
    out = []
    h = ppu_hash(elf)
    for name, info in sorted(db.get(h, {}).items()):
        out.append((h, "PPU", name, info, None))
    for img in spu_images(elf):
        for name, info in sorted(db.get(img[2], {}).items()):
            out.append((img[2], "SPU", name, info, img))
    return out


def apply(elf, chosen):
    """Write the chosen patches into a copy of elf. Returns (bytes, report)."""
    out = bytearray(elf)
    code_end = CODE_END
    report = {"applied": [], "skipped": [], "ppu_code": False, "ppu_data": False, "spu_images": []}
    for h, kind, name, info, img in chosen:
        n_ok = 0
        for op in flatten(info.get("Patch")):
            if not isinstance(op, list) or len(op) < 3:
                continue
            what, addr, value = op[0], int(op[1]), op[2]
            data = encode(what, value)
            if data is None:
                report["skipped"].append(f"{name}: {what} at 0x{addr:X} (needs new code at run time)")
                continue
            if kind == "PPU":
                off = ppu_file_offset(elf, addr, len(data))
                if off is None:
                    report["skipped"].append(f"{name}: 0x{addr:X} is not in the executable's file data")
                    continue
                if addr < code_end:
                    report["ppu_code"] = True
                else:
                    report["ppu_data"] = True
            else:
                pos, progs, _h, _n = img
                off = None
                for t, p_off, va, filesz, _m in progs:
                    if t == 1 and va <= addr and addr + len(data) <= va + filesz:
                        off = pos + p_off + addr - va
                if off is None:
                    report["skipped"].append(f"{name}: LS 0x{addr:X} is outside the SPU program")
                    continue
                if pos not in report["spu_images"]:
                    report["spu_images"].append(pos)
            out[off:off + len(data)] = data
            n_ok += 1
        report["applied"].append({"name": name, "kind": kind, "hash": h, "writes": n_ok})
    report["spu_images"] = [f"0x{p:08X}" for p in report["spu_images"]]
    return bytes(out), report


def runtime_writes(elf, found):
    """[(name, kind, [(guest address, original bytes, patched bytes, op)])] for
    every patch: SPU program writes as the guest address of their bytes in the
    loaded executable."""
    def va_of(off):
        for p_type, _f, p_off, va, filesz, _m in ppu_segments(elf):
            if p_type == 1 and p_off <= off < p_off + filesz:
                return va + off - p_off
        return None
    out = []
    for h, kind, name, info, img in found:
        writes = []
        for op in flatten(info.get("Patch")):
            if not isinstance(op, list) or len(op) < 3:
                continue
            what, addr, value = op[0], int(op[1]), op[2]
            data = encode(what, value)
            if data is None:
                continue
            if kind == "PPU":
                off = ppu_file_offset(elf, addr, len(data))
                va = addr if off is not None else None
            else:
                pos, progs, _h, _n = img
                off = va = None
                for t, p_off, lva, filesz, _m in progs:
                    if t == 1 and lva <= addr and addr + len(data) <= lva + filesz:
                        off = pos + p_off + addr - lva
                        va = va_of(off)
            if off is None or va is None:
                continue
            writes.append((va, elf[off:off + len(data)], data, what))
        notes = info.get("Notes") or ""
        if isinstance(notes, (list, tuple)):
            notes = " ".join(str(n) for n in notes)
        notes = "\n".join(" ".join(line.split()) for line in str(notes).replace("\r", "").split("\n")).strip()
        while "\n\n" in notes:
            notes = notes.replace("\n\n", "\n")
        meta = {"author": str(info.get("Author") or "").strip(), "notes": notes, "file": info.get("_file", "")}
        out.append((name, kind, writes, meta))
    return out


def runtime_sites(elf, patches):
    """{word address: patched word} for the PPU code words the patches change
    (float constants in the code range are data: written, not lifted twice)."""
    words = {}
    owner = {}
    for name, kind, writes, _meta in patches:
        if kind != "PPU":
            continue
        for va, old, new, what in writes:
            if va >= CODE_END or what in ("bef32", "bef64", "lef32", "lef64"):
                continue
            for i in range(len(new)):
                a = va + i
                w = a & ~3
                if owner.setdefault(w, name) != name:
                    sys.exit(f"0x{w:08X} is changed by both {owner[w]!r} and {name!r}: "
                             "one runtime switch per instruction word")
                if w not in words:
                    off = ppu_file_offset(elf, w, 4)
                    words[w] = bytearray(elf[off:off + 4])
                words[w][a - w] = new[i]
    return {f"0x{w:08X}": f"0x{struct.unpack('>I', bytes(b))[0]:08X}" for w, b in sorted(words.items())}


def runtime_table(elf, patches, default_on, sources):
    """The C table host/ascension_patches.cpp includes (with each patch's
    author, notes and file, which the game prints for the launcher)."""
    def c_str(t):
        t = t.replace("\\", "\\\\").replace('"', '\\"').replace("\t", " ").replace("\r", "").replace("\n", "\\n")
        return '"' + t + '"'
    blob, rows, pats = bytearray(), [], []
    for name, kind, writes, meta in patches:
        first = len(rows)
        for va, old, new, _what in writes:
            rows.append(f"    {{ 0x{va:08X}u, {len(new)}u, {len(blob)}u, {len(blob) + len(old)}u }},")
            blob += old + new
        pats.append(f"    {{ {c_str(name)}, {c_str(kind)}, {1 if name in default_on else 0}, "
                    f"{first}u, {len(rows) - first}u,\n"
                    f"      {c_str(meta['author'])}, {c_str(meta['file'])},\n"
                    f"      {c_str(meta['notes'])} }},")
    lines = [f"/* Generated by tools/rpcs3_patch.py runtime for {ppu_hash(elf)}",
             " * from " + ", ".join(os.path.basename(p) for p in sources) + ". Do not edit. */",
             f'#define ASCENSION_PATCH_ELF "{ppu_hash(elf)}"',
             "static const unsigned char k_patch_bytes[] = {"]
    for i in range(0, len(blob), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in blob[i:i + 16]) + ",")
    lines += ["};", "static const ascension_patch_write k_patch_writes[] = {"] + rows + ["};",
              "static const ascension_patch k_patches[] = {"] + pats + ["};", ""]
    return "\n".join(lines)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_patches = [os.path.join(here, "..", "..", "work", "patches", "patch.yml"),
                       os.path.join(here, "..", "patches-rpcs3", "ascension-1.00.yml")]
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["info", "apply", "list-json", "runtime"])
    ap.add_argument("--elf", required=True, help="pristine decrypted EBOOT.ELF")
    ap.add_argument("--patches", nargs="*", default=default_patches)
    ap.add_argument("--out")
    ap.add_argument("--enable", nargs="*", default=[])
    ap.add_argument("--summary")
    ap.add_argument("--table", help="runtime: the C table to write")
    ap.add_argument("--sites", help="runtime: the --patch-sites JSON to write")
    a = ap.parse_args()

    elf = open(a.elf, "rb").read()
    db = load_patches(a.patches)
    found = applicable(elf, db)

    if a.command == "info":
        print(f"executable {ppu_hash(elf)}")
        for pos, _p, h, n in spu_images(elf):
            print(f"  SPU program at 0x{pos:08X} {h} {n!r}")
        print(f"{len(found)} patch(es) for this executable:")
        for h, kind, name, info, _img in found:
            print(f"  [{kind}] {name}  ({info.get('Author', '?')}; {info['_file']})")
        return
    if a.command == "list-json":
        print(json.dumps([{"name": name, "kind": kind, "hash": h, "author": str(info.get("Author", "")),
                           "notes": str(info.get("Notes") or ""), "file": info["_file"],
                           "games": games_text(info)} for h, kind, name, info, _i in found], indent=1))
        return

    if a.command == "runtime":
        if not (a.table and a.sites and a.out):
            sys.exit("runtime needs --table, --sites and --out")
        patches = runtime_writes(elf, found)
        sites = runtime_sites(elf, patches)
        for path, text in ((a.table, runtime_table(elf, patches, set(a.enable), a.patches)),
                           (a.sites, json.dumps(sites, indent=1))):
            with open(path + ".tmp", "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            os.replace(path + ".tmp", path)
        data, _report = apply(elf, found)
        with open(a.out + ".tmp", "wb") as f:
            f.write(data)
        os.replace(a.out + ".tmp", a.out)
        for name, kind, writes, _meta in patches:
            print(f"[{kind}] {name}: {len(writes)} write(s){' (default on)' if name in a.enable else ''}")
        print(f"{len(sites)} PPU instruction(s) to lift both ways; all patches applied: {a.out}")
        return

    want = set(a.enable)
    chosen = [f for f in found if f[2] in want]
    missing = want - {f[2] for f in chosen}
    if missing:
        print("not found for this executable: " + ", ".join(sorted(missing)), file=sys.stderr)
    data, report = apply(elf, chosen)
    if not a.out:
        sys.exit("--out is required for apply")
    tmp = a.out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, a.out)
    report["out"] = os.path.abspath(a.out)
    report["sha256"] = hashlib.sha256(data).hexdigest()
    if a.summary:
        with open(a.summary, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=1)
    for r in report["applied"]:
        print(f"applied [{r['kind']}] {r['name']} ({r['writes']} write(s))")
    for s in report["skipped"]:
        print("skipped " + s)
    print(f"PPU code changed: {report['ppu_code']}, PPU data changed: {report['ppu_data']}, "
          f"SPU programs changed: {len(report['spu_images'])}")


if __name__ == "__main__":
    main()
