#!/usr/bin/env python3
"""cellGcm IO mapping table: many mappings, no leak on refusal, no slot reuse,
freed IO space reused.

Builds the real populate/clear_offset_table, find_*_mapping, gcm_io_alloc,
cellGcmMapMainMemory, cellGcmUnmapIoAddress and cellGcm_syscall_iomap/iounmap
(pulled verbatim out of libs/video/cellGcmSys.c, with CELL_GCM_MAX_IO_MAPPINGS
from cellGcmSys.h) against small stubs for guest memory, and checks:

1. a title mapping separate 1 MB chunks gets at least 40 distinct IO offsets
   before the first refusal (Killzone 3 maps up to 38);
2. a refused cellGcmMapMainMemory does not consume IO address space;
3. a sys_rsx iomap after an unmap does not overwrite a live mapping;
4. map/unmap cycles reuse the IO space an unmap freed: 1,000 cycles of 70 MB
   stay inside the 256 MB aperture (God of War: Ascension's area loads ran a
   bump allocator past 512 MB, 2026-11-03), at io = ea - ioAddress;
5. sys_rsx iounmap frees the mapping slot as well as the table pages.

Run: python tools/test_gcm_io_mappings.py   (CC=<compiler>; on Windows,
e.g. CC=clang, else it is skipped)
"""
import os
import re
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt' and not os.environ.get('CC'):
    print('SKIP: set CC to a C compiler (e.g. CC=clang) to run this fixture on Windows')
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]
src = (root / 'libs/video/cellGcmSys.c').read_text(encoding='utf-8').replace('\r\n', '\n')
hdr = (root / 'libs/video/cellGcmSys.h').read_text(encoding='utf-8')


def function(signature):
    m = re.search(signature, src)
    assert m, f'{signature} not found'
    i, depth = src.index('{', m.start()) + 1, 1
    while depth:
        depth += {'{': 1, '}': -1}.get(src[i], 0)
        i += 1
    return src[m.start():i]


slots = re.search(r'^#define CELL_GCM_MAX_IO_MAPPINGS\s+(\d+)', hdr, re.M).group(1)
parts = [function(r'static void populate_offset_table\(u32 ea, u32 io, u32 size\)'),
         function(r'static void clear_offset_table\(u32 ea, u32 io, u32 size\)'),
         function(r'static IoMapping\* find_mapping_by_io\(u32 io\)'),
         function(r'static IoMapping\* find_free_mapping\(void\)'),
         function(r'static int gcm_io_pages_free\(u32 p, u32 pages\)'),
         function(r'static u32 gcm_io_alloc\(u32 ea, u32 size\)\n\{'),
         function(r's32 cellGcmMapMainMemory\(u32 ea, u32 size, u32\* offset\)'),
         function(r's32 cellGcmUnmapIoAddress\(u32 io\)'),
         function(r'void cellGcm_syscall_iomap\(u32 ea, u32 io, u32 size\)'),
         function(r'void cellGcm_syscall_iounmap\(u32 io, u32 size\)')]

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ps3emu/ps3types.h>
#include <ps3emu/error_codes.h>
#ifndef CELL_OK
#define CELL_OK 0
#endif
#ifndef CELL_GCM_MAX_IO_MAPPINGS
#define CELL_GCM_MAX_IO_MAPPINGS %s
#endif
typedef struct { u32 ioAddress, ioSize; } CellGcmConfig;
static CellGcmConfig s_config = { 0x100000u, 0x200000u };
typedef struct IoMapping { u32 ea; u32 io; u32 size; int active; } IoMapping;
static IoMapping s_io_mappings[CELL_GCM_MAX_IO_MAPPINGS];
static int s_io_mapping_count = 0;
static u16 s_io_address_table[65536];
static u16 s_ea_address_table[65536];
static u16 s_io_last_ea[65536];
volatile u32 g_gcm_io_remap_gen;
static void gcm_io_lock(void) {}
static void gcm_io_unlock(void) {}
static void offset_table_write_through(u32 ea_page, u32 io_page) { (void)ea_page; (void)io_page; }
#define GCM_EXTRA_IO_BASE 0x10000000u
static u32 s_gcm_extra_io_size;
#define GCM_IO_NONE 0xFFFFFFFFu
#define GCM_IO_PAGES (0x10000000u >> 20)
static unsigned char mem[0x1000];
static void vm_write32(u32 a, u32 v) { for (int i = 0; i < 4; i++) mem[a + i] = (unsigned char)(v >> (24 - 8 * i)); }
static u32 rd32(u32 a) { return (u32)mem[a] << 24 | mem[a + 1] << 16 | mem[a + 2] << 8 | mem[a + 3]; }
static u32 pages_used(void) { u32 n = 0; for (u32 i = 0; i < 65536; i++) n += s_ea_address_table[i] != 0xFFFF; return n; }
%s
int main(void)
{
    u32* out = (u32*)(uintptr_t)0x100;     /* guest out-param address */
    int ok = 1;
    memset(s_io_address_table, 0xFF, sizeof s_io_address_table);
    memset(s_ea_address_table, 0xFF, sizeof s_ea_address_table);
    populate_offset_table(s_config.ioAddress, 0, s_config.ioSize);   /* the Init region */

    /* 3: map A and B, unmap A, then iomap C at A's io: B stays, C gets a slot. */
    cellGcmMapMainMemory(0x20000000u, 0x100000u, out);
    const u32 io_a = rd32(0x100);
    cellGcmMapMainMemory(0x20100000u, 0x100000u, out);
    cellGcmUnmapIoAddress(io_a);
    cellGcm_syscall_iomap(0x30000000u, io_a, 0x100000u);
    int b_kept = 0, c_found = 0;
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        b_kept  |= s_io_mappings[i].active && s_io_mappings[i].ea == 0x20100000u;
        c_found |= s_io_mappings[i].active && s_io_mappings[i].ea == 0x30000000u;
    }
    printf("%%s live mapping kept after unmap + iomap: %%s\n",
           b_kept && c_found ? "ok  " : "FAIL", b_kept ? (c_found ? "yes" : "new one lost") : "overwritten");
    ok &= b_kept && c_found;

    /* 5: sys_rsx iounmap frees the slot too. */
    int live = s_io_mapping_count;
    cellGcm_syscall_iounmap(io_a, 0x100000u);
    int c_gone = 1;
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) c_gone &= !(s_io_mappings[i].active && s_io_mappings[i].ea == 0x30000000u);
    printf("%%s sys_rsx iounmap frees the mapping slot: %%s\n", c_gone && s_io_mapping_count == live - 1 ? "ok  " : "FAIL",
           c_gone ? "yes" : "still active");
    ok &= c_gone && s_io_mapping_count == live - 1;

    /* 4: 1,000 cycles of map 70 MB / unmap: freed space reused, inside 256 MB, at io = ea - ioAddress. */
    u32 hi = 0, paired = 1, fails = 0;
    for (int i = 0; i < 1000; i++) {
        const u32 ea = 0x00500000u + (u32)(i %% 3) * 0x04600000u;   /* three 70 MB areas, in turn (past B's page) */
        if (cellGcmMapMainMemory(ea, 0x04600000u, out) != CELL_OK) { fails++; continue; }
        const u32 io = rd32(0x100);
        if (io + 0x04600000u > hi) hi = io + 0x04600000u;
        paired &= io == ea - s_config.ioAddress;
        if (cellGcmUnmapIoAddress(io) != CELL_OK) fails++;
    }
    printf("%%s 1000 map/unmap cycles of 70 MB: highest io end 0x%%08X, %%u failures, io = ea - ioAddress: %%s\n",
           hi <= 0x10000000u && !fails && paired ? "ok  " : "FAIL", hi, fails, paired ? "yes" : "no");
    ok &= hi <= 0x10000000u && !fails && paired;

    /* 1 + 2: map 1 MB chunks until the first refusal. */
    static u32 offs[4096];
    int n = 0, distinct = 1, leaked = -1;
    for (int i = 0; i < 4096; i++) {
        u32 before = pages_used();
        if (cellGcmMapMainMemory(0x40000000u + (u32)i * 0x100000u, 0x100000u, out) != CELL_OK) {
            leaked = pages_used() != before;
            break;
        }
        offs[n++] = rd32(0x100);
    }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < i; j++)
            if (offs[i] == offs[j]) distinct = 0;
    printf("%%s 1 MB chunks mapped before the first refusal: %%d (want >= 40, distinct offsets: %%s)\n",
           n >= 40 && distinct ? "ok  " : "FAIL", n, distinct ? "yes" : "no");
    ok &= n >= 40 && distinct;
    printf("%%s refused call leaves the IO allocator alone: %%s\n",
           leaked == 0 ? "ok  " : "FAIL", leaked < 0 ? "no refusal" : leaked ? "leaked" : "yes");
    ok &= leaked == 0;
    return ok ? 0 : 1;
}
'''

with tempfile.TemporaryDirectory(prefix='gcm-io-') as tmp:
    c = Path(tmp) / 'io.c'
    exe = Path(tmp) / ('io.exe' if os.name == 'nt' else 'io')
    c.write_text(HARNESS % (slots, '\n'.join(parts)))
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-w',
                    '-I' + str(root / 'include'), str(c), '-o', str(exe)], check=True)
    r = subprocess.run([str(exe)], timeout=10, stdout=subprocess.PIPE, text=True)
    print(''.join(l for l in r.stdout.splitlines(True) if not l.startswith('[cellGcmSys]')), end='')
    print('PASS' if r.returncode == 0 else 'FAIL')
    raise SystemExit(r.returncode)
