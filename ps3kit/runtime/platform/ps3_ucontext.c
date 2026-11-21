#if defined(__aarch64__) && defined(__ANDROID__)
#include "ps3_ucontext.h"
#undef ucontext_t
#undef getcontext
#undef setcontext
#undef swapcontext

__asm__(
    ".text\n.align 4\n.globl ps3_ctx_switch\n.type ps3_ctx_switch,%function\nps3_ctx_switch:\n"
    "  stp x19,x20,[x0,#0]\n  stp x21,x22,[x0,#16]\n  stp x23,x24,[x0,#32]\n  stp x25,x26,[x0,#48]\n"
    "  stp x27,x28,[x0,#64]\n  stp x29,x30,[x0,#80]\n  mov x9, sp\n  str x9,[x0,#96]\n"
    "  stp d8,d9,[x0,#104]\n  stp d10,d11,[x0,#120]\n  stp d12,d13,[x0,#136]\n  stp d14,d15,[x0,#152]\n"
    "  ldp x19,x20,[x1,#0]\n  ldp x21,x22,[x1,#16]\n  ldp x23,x24,[x1,#32]\n  ldp x25,x26,[x1,#48]\n"
    "  ldp x27,x28,[x1,#64]\n  ldp x29,x30,[x1,#80]\n  ldr x9,[x1,#96]\n  mov sp, x9\n"
    "  ldp d8,d9,[x1,#104]\n  ldp d10,d11,[x1,#120]\n  ldp d12,d13,[x1,#136]\n  ldp d14,d15,[x1,#152]\n"
    "  ret\n"
    ".globl ps3_ctx_boot\n.type ps3_ctx_boot,%function\nps3_ctx_boot:\n"
    "  mov w0, w20\n  mov w1, w21\n  blr x19\n  mov x0, x22\n  bl ps3_setcontext\n  brk #0\n");
extern void ps3_ctx_boot(void);

int ps3_getcontext(ps3_ucontext_t* c) { memset(c, 0, sizeof *c); return 0; }

void ps3_makecontext(ps3_ucontext_t* c, void (*fn)(int, int), int lo, int hi)
{
    uint64_t top = ((uint64_t)(uintptr_t)c->uc_stack.ss_sp + c->uc_stack.ss_size) & ~15ull;
    memset(c->regs, 0, sizeof c->regs);
    c->regs[0] = (uint64_t)(uintptr_t)fn;            /* x19 */
    c->regs[1] = (uint32_t)lo;                       /* x20 */
    c->regs[2] = (uint32_t)hi;                       /* x21 */
    c->regs[3] = (uint64_t)(uintptr_t)c->uc_link;    /* x22 */
    c->regs[11] = (uint64_t)(uintptr_t)ps3_ctx_boot; /* x30 */
    c->sp = top;
}

int ps3_swapcontext(ps3_ucontext_t* from, ps3_ucontext_t* to) { ps3_ctx_switch(from, to); return 0; }

int ps3_setcontext(ps3_ucontext_t* to)
{
    ps3_ucontext_t dead;
    ps3_ctx_switch(&dead, to);
    return 0;
}
#endif
