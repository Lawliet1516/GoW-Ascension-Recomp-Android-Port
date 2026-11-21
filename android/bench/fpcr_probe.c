/* What does FPCR do on this device/kernel/libc? Run on the phone. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <setjmp.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>
static uint64_t get(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static void set(uint64_t v) { __asm__ volatile("msr fpcr, %0" :: "r"(v)); }
static uint64_t getsr(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v; }
static jmp_buf jb; static volatile uint64_t in_handler, in_thread;
static void h(int s) { (void)s; in_handler = get(); }
static void* th(void* a) { (void)a; in_thread = get(); return 0; }
int main(void)
{
    const uint64_t SPU = 0x01C00000u, d = get();
    printf("default FPCR = 0x%llx (RMode=%llu FZ=%llu DN=%llu AH=%llu trap-enable bits=0x%llx)\n", (unsigned long long)d,
           (unsigned long long)((d >> 22) & 3), (unsigned long long)((d >> 24) & 1), (unsigned long long)((d >> 25) & 1),
           (unsigned long long)((d >> 1) & 1), (unsigned long long)(d & 0x9F00));
    /* writable trap bits? (IOE..IXE bits 8..12, IDE 15) -- Linux/Android do not deliver FP traps */
    set(d | 0x9F00); printf("trap-enable bits after write: 0x%llx (0 => RAZ/WI on this CPU)\n", (unsigned long long)(get() & 0x9F00)); set(d);
    set(d | SPU); printf("SPU mode FPCR = 0x%llx\n", (unsigned long long)get());
    /* setjmp/longjmp: does the mode seen at setjmp come back? */
    if (!setjmp(jb)) { set(d); longjmp(jb, 1); }
    printf("after longjmp (setjmp taken in SPU mode, mode cleared before): FPCR = 0x%llx -> %s\n", (unsigned long long)get(),
           (get() & 0x01C00000u) == SPU ? "RESTORED by longjmp" : "NOT restored (must be re-set by hand)");
    /* signal handler */
    signal(SIGUSR1, h); set(d | SPU); raise(SIGUSR1);
    printf("inside a signal handler: FPCR = 0x%llx -> %s; after return: 0x%llx\n", (unsigned long long)in_handler,
           (in_handler & 0x01C00000u) ? "inherits the mode" : "handler runs in default mode", (unsigned long long)get());
    /* new thread */
    pthread_t t; pthread_create(&t, 0, th, 0); pthread_join(t, 0);
    printf("a thread created while in SPU mode starts with FPCR = 0x%llx -> %s\n", (unsigned long long)in_thread,
           (in_thread & 0x01C00000u) ? "inherits the mode (must reset)" : "default mode");
    set(d);
    printf("FPSR = 0x%llx (cumulative flags only; nothing reads them)\n", (unsigned long long)getsr());
    return 0;
}
