/* Minimal ucontext replacement for AArch64 Android: bionic ships no
 * getcontext/makecontext/swapcontext. Only what libs/spurs/cellFiber.c uses.
 * Callee-saved state only (x19-x30, sp, d8-d15): a switch is a function call. */
#ifndef PS3_UCONTEXT_H
#define PS3_UCONTEXT_H
#include <stdint.h>
#include <string.h>
#include <stddef.h>

typedef struct { void* ss_sp; size_t ss_size; } ps3_stack_t;
typedef struct ps3_ucontext {
    uint64_t regs[12];   /* x19..x30 */
    uint64_t sp;
    uint64_t fp[8];      /* d8..d15 */
    ps3_stack_t uc_stack;
    struct ps3_ucontext* uc_link;
} ps3_ucontext_t;

#ifdef __cplusplus
extern "C" {
#endif
void ps3_ctx_switch(ps3_ucontext_t* from, ps3_ucontext_t* to);   /* asm, ps3_ucontext.c */
int  ps3_getcontext(ps3_ucontext_t* c);
void ps3_makecontext(ps3_ucontext_t* c, void (*fn)(int, int), int lo, int hi);
int  ps3_swapcontext(ps3_ucontext_t* from, ps3_ucontext_t* to);
int  ps3_setcontext(ps3_ucontext_t* to);
#ifdef __cplusplus
}
#endif

#define ucontext_t     ps3_ucontext_t
#define getcontext     ps3_getcontext
#define setcontext     ps3_setcontext
#define swapcontext    ps3_swapcontext
#define PS3_MAKECONTEXT(c, fn, lo, hi) ps3_makecontext((c), (void (*)(int, int))(fn), (lo), (hi))
#endif
