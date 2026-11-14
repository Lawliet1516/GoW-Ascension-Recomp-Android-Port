// Game patches (RPCS3's, and the ones ported to 1.00) switched when the game
// starts, not built in.
//
// RPCS3 patches a game in memory as it runs. This port translates the code
// ahead of time, so a patch used to mean writing it into EBOOT.ELF and
// translating and building again. Now tools/rpcs3_patch.py runtime lists every
// patch's writes (host/ascension_patch_table.inc: guest address, original and
// patched bytes), the PPU instructions they change are lifted both ways
// (ppu_lifter.py --patch-sites: the word in guest memory picks the
// translation), and the SPU programs they change are lifted in both versions
// (matched by their bytes when the game loads them). Before the game runs,
// ascension_after_load() writes every patch over the loaded executable: the
// patched bytes for a patch that is on, the original bytes for one that is
// off, so the switch holds whether the disc's EBOOT.ELF was patched or not.
//
//   PS3_PATCHES=<name>|<name>...  the patches on, by name (the launcher's
//                                 list); "none" or empty: none. Unset: the
//                                 build's defaults (the table's default flags).
//   PS3_PATCH_LIST=1              print the patches this build can switch (JSON
//                                 between PATCH-LIST-BEGIN/END lines) and exit,
//                                 before anything starts: the launcher's
//                                 Patches page, where no patch files exist.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "ppu_recomp.h"   /* the generated header: vm_base and the guest loads */

namespace {

struct ascension_patch_write { uint32_t addr, len, orig, patched; };   // offsets into k_patch_bytes
struct ascension_patch {
    const char* name; const char* kind; int default_on; uint32_t first, count;
    const char* author; const char* file; const char* notes;
};
#include "ascension_patch_table.inc"

constexpr int kPatches = (int)(sizeof k_patches / sizeof k_patches[0]);

// PS3_PATCHES from the OS environment, not getenv(): the defaults code asks
// during static initialisation, before the boot harness's getenv cache exists
// (ascension_defaults.cpp reads its variables the same way).
bool patches_env(std::string& out)
{
#ifdef _WIN32
    SetLastError(0);
    const DWORD n = GetEnvironmentVariableA("PS3_PATCHES", nullptr, 0);
    if (!n) return GetLastError() != ERROR_ENVVAR_NOT_FOUND;   // set but empty
    out.resize(n);
    out.resize(GetEnvironmentVariableA("PS3_PATCHES", &out[0], n));
    return true;
#else
    const char* e = getenv("PS3_PATCHES");
    if (e) out = e;
    return e != nullptr;
#endif
}

// Which patches are on, decided once (the defaults code asks before main for
// Unlock FPS; the loader asks after the image is in memory).
const bool* patch_state()
{
    static bool on[kPatches];
    static bool done;
    if (done) return on;
    done = true;
    std::string list;
    const bool set = patches_env(list);
    for (int i = 0; i < kPatches; i++) on[i] = set ? false : k_patches[i].default_on != 0;
    if (!set) return on;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t bar = list.find('|', pos);
        if (bar == std::string::npos) bar = list.size();
        std::string name = list.substr(pos, bar - pos);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
        while (!name.empty() && (name[0] == ' ' || name[0] == '\t')) name.erase(0, 1);
        if (!name.empty() && name != "none") {
            int found = -1;
            for (int i = 0; i < kPatches; i++)
                if (name == k_patches[i].name) found = i;
            if (found >= 0) on[found] = true;
            else printf("[patches] unknown patch \"%s\" (not in this build's list)\n", name.c_str());
        }
        pos = bar + 1;
    }
    return on;
}

} // namespace

extern "C" int ascension_patch_on(const char* name)
{
    const bool* on = patch_state();
    for (int i = 0; i < kPatches; i++)
        if (!strcmp(name, k_patches[i].name)) return on[i] ? 1 : 0;
    return 0;
}

namespace {
void json_str(const char* s)
{
    putchar('"');
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c == '\n') fputs("\\n", stdout);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);   // UTF-8 passes through
    }
    putchar('"');
}
} // namespace

// ascension_pre_main (ascension_defaults.cpp), before anything starts:
// PS3_PATCH_LIST=1 prints the table and returns 0 (the process exits), else -1.
extern "C" int ascension_patch_list_main(void)
{
#ifdef _WIN32
    char v[8];
    const DWORD n = GetEnvironmentVariableA("PS3_PATCH_LIST", v, sizeof v);
    if (!n || n >= sizeof v || v[0] != '1') return -1;
#else
    const char* v = getenv("PS3_PATCH_LIST");
    if (!v || v[0] != '1') return -1;
#endif
    printf("PATCH-LIST-BEGIN\n[");
    for (int i = 0; i < kPatches; i++) {
        const ascension_patch& p = k_patches[i];
        printf("%s\n {\"name\": ", i ? "," : "");
        json_str(p.name);
        printf(", \"kind\": ");
        json_str(p.kind);
        printf(", \"default\": %s, \"author\": ", p.default_on ? "true" : "false");
        json_str(p.author);
        printf(", \"file\": ");
        json_str(p.file);
        printf(", \"notes\": ");
        json_str(p.notes);
        printf("}");
    }
    printf("\n]\nPATCH-LIST-END\n");
    fflush(stdout);
    return 0;
}

// boot_main.cpp, right after ppu_load_elf: the switches into guest memory.
extern "C" void ascension_after_load(void)
{
    if (!vm_base) return;
    const bool* on = patch_state();
    std::string list_on, list_off;
    // The patches that are off first, then the ones that are on (each in table
    // order): where two patches write the same data bytes, a patch that is on always wins over one that is
    // off, and of two that are on the later one. Code words are never shared
    // (rpcs3_patch.py refuses that), so for them the order does not matter.
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < kPatches; i++) {
            if (on[i] != (pass == 1)) continue;
            const ascension_patch& p = k_patches[i];
            for (uint32_t w = p.first; w < p.first + p.count; w++) {
                const ascension_patch_write& pw = k_patch_writes[w];
                memcpy(vm_base + pw.addr, k_patch_bytes + (on[i] ? pw.patched : pw.orig), pw.len);
            }
        }
    for (int i = 0; i < kPatches; i++) {
        const ascension_patch& p = k_patches[i];
        std::string& l = on[i] ? list_on : list_off;
        l += l.empty() ? "" : ", ";
        l += p.name;
    }
    std::string env;
    printf("[patches] on: %s; off: %s%s\n", list_on.empty() ? "none" : list_on.c_str(),
           list_off.empty() ? "none" : list_off.c_str(), patches_env(env) ? "" : " (build defaults)");
}
