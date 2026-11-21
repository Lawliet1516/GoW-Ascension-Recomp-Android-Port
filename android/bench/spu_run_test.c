/* Integrated SPU run on the device: the real runtime (spu_channels.c, drain, policy, DMA...) running a lifted
 * program through spu_run_with_halt, then checks (1) the float results depend on the SPU float mode as the SPU's do
 * (truncation, denormals read as zero), (2) FPCR is exactly what it was before on every exit path, (3) the same
 * across many runs and threads, (4) a thread created from inside the SPU mode starts in the standard mode.
 * The program (spuprog/make_prog.py) is hand-assembled; its lift comes from ps3kit/tools/spu_lifter.py. */
#define _GNU_SOURCE
#include "spu_context.h"
#include "spu_helpers.h"
#include "win32_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>

/* The runtime surface comes from libps3kit_android.a; the few symbols the Windows port supplies from ascension/host
 * are in android/host/port_stubs.c (bring-up stand-ins, listed there). */
#ifdef SPU_A64_CHECK_MODE
void spu_a64_mode_violation(const char* op) { fprintf(stderr, "[spu-fp] FATAL: spu_%s outside the SPU float mode\n", op); abort(); }
#endif
extern void spu_func_00000000(spu_context*);
extern void spu_recomp_register(void);
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);

static uint64_t fpcr(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static void set_fpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" :: "r"(v)); }
static volatile int failures;
#define EXPECT(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint32_t ls_word(spu_context* c, uint32_t lsa) { const uint8_t* p = c->ls + lsa; return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void prep(spu_context* c)
{
    memset(c->gpr, 0, sizeof c->gpr); memset(c->ls + 0x1000, 0xEE, 0x100);
    c->status = 0; c->stop_code = 0; c->image_id = 0; c->host_depth = 0;
}
/* expected words of the program in the SPU's arithmetic */
static const uint32_t want[6] = { 0x40400000, 0x40C00000, 0x40E00000, 0x40A00000, 0x3F800000, 0x00000000 };
static int check_results(spu_context* c, const char* what)
{
    int ok = 1; for (int i = 0; i < 6; i++) { uint32_t g = ls_word(c, 0x1000 + 16 * i); if (g != want[i]) { ok = 0; printf("  %s: result %d = %08x, want %08x\n", what, i, g, want[i]); } }
    return ok;
}

static void* worker(void* arg)
{
    long id = (long)arg; spu_context* c = (spu_context*)calloc(1, sizeof(spu_context)); spu_context_init(c, (uint32_t)id + 1);
    const uint64_t before = fpcr();
    for (int i = 0; i < 20000; i++) {
        prep(c); spu_run_with_halt(spu_func_00000000, c);
        if (i % 1000 == 0 && !check_results(c, "thread")) { failures++; break; }
        if (fpcr() != before) { failures++; printf("FAIL: thread %ld FPCR drifted to 0x%llx after run %d\n", id, (unsigned long long)fpcr(), i); break; }
    }
    return NULL;
}
static volatile uint64_t child_fpcr;
static DWORD WINAPI child(LPVOID p) { (void)p; child_fpcr = fpcr(); return 0; }
static volatile uint64_t in_signal;
static void on_usr1(int s) { (void)s; in_signal = fpcr(); }

int main(void)
{
    extern uint8_t* vm_base; extern uint32_t ppu_vm_size;
    ppu_vm_size = 0x2000000u; vm_base = (uint8_t*)aligned_alloc(0x10000, ppu_vm_size); memset(vm_base, 0, ppu_vm_size);
    spu_recomp_register();
    spu_context* c = (spu_context*)calloc(1, sizeof(spu_context)); spu_context_init(c, 0);
    printf("FPCR at start: 0x%llx\n", (unsigned long long)fpcr());

    /* 1. the SPU arithmetic is what the program computes, and the mode is restored after a halt (stop) */
    const uint64_t before = fpcr(); prep(c); spu_run_with_halt(spu_func_00000000, c);
    EXPECT(check_results(c, "run 1"), "float results differ from the SPU's (truncation / denormals-as-zero)");
    EXPECT(fpcr() == before, "FPCR after run = 0x%llx, before = 0x%llx", (unsigned long long)fpcr(), (unsigned long long)before);
    printf("1. SPU results %s, FPCR restored after halt: %s (stop code 0x%x)\n", failures ? "WRONG" : "correct", fpcr() == before ? "yes" : "NO", c->stop_code);

    /* 2. a caller that already has odd FPCR bits (round-up, FZ off) gets exactly them back */
    set_fpcr(0x00400000u); prep(c); spu_run_with_halt(spu_func_00000000, c);
    EXPECT(check_results(c, "run 2"), "results depend on the caller's FPCR");
    EXPECT(fpcr() == 0x00400000u, "caller FPCR not restored: 0x%llx", (unsigned long long)fpcr());
    printf("2. caller FPCR 0x400000 -> after run 0x%llx\n", (unsigned long long)fpcr()); set_fpcr(before);

    /* 3. caller already in the SPU mode (nested use): stays exactly as it was */
    set_fpcr(0x01C00000u); prep(c); spu_run_with_halt(spu_func_00000000, c);
    EXPECT(fpcr() == 0x01C00000u, "nested: FPCR 0x%llx", (unsigned long long)fpcr());
    printf("3. caller already in SPU mode -> after run 0x%llx\n", (unsigned long long)fpcr()); set_fpcr(before);

    /* 4. negative control: the lifted code called directly, outside the runtime's mode, must NOT give the SPU's results */
    prep(c); c->host_depth = 1; spu_func_00000000(c);
    const int direct_ok = check_results(c, "direct (expected to differ)");
    printf("4. negative control (no SPU float mode): results %s\n", direct_ok ? "UNEXPECTEDLY correct (the test cannot tell the modes apart!)" : "differ, as they must");
    EXPECT(!direct_ok, "negative control passed: mode is not what makes the results right");
    EXPECT(fpcr() == before, "FPCR changed by the direct call");

    /* 5. signal handler runs while an SPU thread is in the mode: informational (inherits), main thread unaffected */
    signal(SIGUSR1, on_usr1); raise(SIGUSR1);
    printf("5. signal handler FPCR (outside SPU mode) = 0x%llx\n", (unsigned long long)in_signal);

    /* 6. a thread created while the creator is in the SPU mode starts in the standard mode (trampoline reset) */
    set_fpcr(0x01C00000u); HANDLE h = CreateThread(NULL, 0, child, NULL, 0, NULL); WaitForSingleObject(h, INFINITE); set_fpcr(before);
    printf("6. thread created from SPU mode starts with FPCR 0x%llx\n", (unsigned long long)child_fpcr);
    EXPECT(child_fpcr == 0, "new thread inherited the SPU mode (0x%llx)", (unsigned long long)child_fpcr);

    /* 7. many runs on four threads at once: correct results, no FPCR drift, main thread untouched */
    pthread_t th[4]; for (long i = 0; i < 4; i++) pthread_create(&th[i], NULL, worker, (void*)i);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    EXPECT(fpcr() == before, "main thread FPCR changed by the workers");
    printf("7. 4 threads x 20000 runs: %s\n", failures ? "problems above" : "all results correct, no drift");

    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
