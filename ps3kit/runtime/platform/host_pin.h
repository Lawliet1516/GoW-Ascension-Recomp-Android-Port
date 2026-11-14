/* host_pin.h -- topology-aware placement of the host threads that carry the
 * frame (PS3_HOST_PIN, off by default). See host_pin.c. */
#ifndef PS3_HOST_PIN_H
#define PS3_HOST_PIN_H

#ifdef __cplusplus
extern "C" {
#endif

enum host_role {
    HOST_ROLE_LIGHT = 0,      /* anything not below: guest PPU threads, audio, PSO builders */
    HOST_ROLE_PPU_MAIN,       /* the guest main thread (boot_main.cpp, ppu_run) */
    HOST_ROLE_WALKER,         /* the present thread: FIFO walker + D3D12 submit (vblank_ticker) */
    HOST_ROLE_SPU,            /* SPURS policy-module worker, index = SPU number 0..5 */
    HOST_ROLE_SPURS_KERNEL,   /* the SPURS kernel (admission) thread */
    HOST_ROLE_AUDIO,          /* cellAudio mixer: light, but MMCSS "Audio" if PS3_MMCSS */
    HOST_ROLE_COUNT
};

/* Process-wide, once, as early as possible in main(): EcoQoS opt-out
 * (PS3_ECOQOS_OFF=1) and, with PS3_HOST_PIN=1, the topology plan plus the
 * process default CPU sets (every thread not pinned below, the GPU driver's
 * own threads included, goes to the "light" logical processors). Safe to call
 * more than once; does nothing when every switch is off. */
void host_qos_init(void);

/* Place the CALLING thread for its role (index: SPU number for HOST_ROLE_SPU,
 * else 0). No-op unless PS3_HOST_PIN=1 (and the role's own switch). Also
 * applies the per-thread EcoQoS opt-out and MMCSS when those are on. */
void host_pin_thread(enum host_role role, int index);

/* 1 when the calling thread was given a logical processor no other thread of
 * this process is assigned to -- its spins may use PAUSE instead of
 * SwitchToThread (sys_timer.c SYS_USLEEP_PAUSE, cellSpurs.c). */
int host_pin_self_exclusive(void);

/* 1 when the process may use fewer than 8 physical cores (PS3_SMALL_CPU=0/1
 * forces it): placement and the SPURS kernel's yield loop are then off. On a
 * hybrid CPU (host_cpu_hybrid): fewer than 8 P-cores. */
int host_small_cpu(void);

/* SPURS_KERNEL_SPIN=auto's answer (cellSpurs.c): 1 = the yield loop, i.e.
 * !host_small_cpu(); under the hybrid A/B (PS3_HOST_PIN_AB on a hybrid CPU) the
 * side in force decides, and host_kernel_spin_auto_varies() is then 1. */
int host_kernel_spin_auto(void);
int host_kernel_spin_auto_varies(void);

/* 1 on a hybrid CPU (P-cores + E-cores) with PS3_HYBRID != 0. */
int host_cpu_hybrid(void);

#ifdef __cplusplus
}
#endif
#endif
