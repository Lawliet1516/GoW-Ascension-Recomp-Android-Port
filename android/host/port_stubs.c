/* Bring-up stand-ins for what the Windows port supplies from ascension/host and the per-game PPU loader sources.
 *
 * ps3kit's library (libps3kit_android.a) leaves these out on purpose: they belong to the port that runs a game.
 * Every symbol below is something a real Android port must provide properly; they are NOT behaviour to ship:
 *
 *   g_*_on, g_w2_*            A/B and diagnostic switches the Windows build toggles at run time (cellSpurs/cellGcmSys).
 *   host_ppu_main_thread_id   the id of the thread that runs the guest's main PPU thread (win32_compat's pseudo ids).
 *   __ImageBase               PE image base on Windows; the runtime uses its address to compress code pointers.
 *   ld_blit_ab, rsx_live_draw_*  the D3D12 live-draw engine (rsx_live_draw.c is Windows-only: no-ops off Windows).
 *   g_spu_iojrn_rt, t_spu_iojrn  hooks for the Ascension IO-journal on SPUs: absent = disabled.
 *   psnr_*                    community-server online client (libs/network/psnr/psnr.c is in the library; these are the
 *                             transport functions of ascension/host): online play disabled here.
 *   vm_base                   the guest address space base (set by whoever maps guest memory).
 */
#include <stdint.h>
#include <stddef.h>

typedef uint32_t DWORD;
volatile int g_edge6_on = 1, g_ld_blit_cpu_on = 1, g_ld_ppu_delay_on = 0, g_ld_prio_on = 1, g_ld_zocc_on = 0, g_q4_snd_on = 1;
volatile int g_w2_fifo_act = 0, g_w2_pkstats = 0;
DWORD host_ppu_main_thread_id(void) { return 0; }
char __ImageBase;
int ld_blit_ab(void) { return 0; }
void rsx_live_draw_vtx_join(void) {}
int rsx_live_draw_w2_nop(uint32_t count, uint32_t last_arg) { (void)count; (void)last_arg; return 0; }
int rsx_live_draw_w2_packet(uint32_t cls, uint32_t type, uint32_t method, uint32_t count, uint32_t dea0)
{ (void)cls; (void)type; (void)method; (void)count; (void)dea0; return 0; }
const void* g_spu_iojrn_rt = NULL;
__thread const void* t_spu_iojrn = NULL;
uint8_t* vm_base = NULL;

/* online: every call fails / reports "no server" */
typedef struct psnr_client psnr_client;
psnr_client* psnr_connect(const char* h, uint16_t p, const char* c, const char* o, const void* x) { (void)h; (void)p; (void)c; (void)o; (void)x; return NULL; }
void psnr_close(psnr_client* c) { (void)c; }
int  psnr_connect_error(const psnr_client* c) { (void)c; return -1; }
int  psnr_send(psnr_client* c, ...) { (void)c; return -1; }
int  psnr_poll(psnr_client* c, ...) { (void)c; return 0; }
void psnr_msg_free(void* m) { (void)m; }
void psnr_server_addr(const psnr_client* c, uint8_t ip[4], uint16_t* port) { (void)c; ip[0] = ip[1] = ip[2] = ip[3] = 0; *port = 0; }
void psnr_probe_packet(const psnr_client* c, uint8_t* out) { (void)c; (void)out; }
void psnr_punch_packet(const psnr_client* c, uint8_t* out) { (void)c; (void)out; }
int  psnr_is_control(const void* b, size_t n) { (void)b; (void)n; return 0; }
int  psnr_probe_reply(const void* b, size_t n, uint8_t ip[4], uint16_t* port) { (void)b; (void)n; (void)ip; (void)port; return 0; }
int  psnr_relay_available(const psnr_client* c) { (void)c; return 0; }
int  psnr_relay_wrap(const psnr_client* c, ...) { (void)c; return -1; }
int  psnr_relay_unwrap(const psnr_client* c, ...) { (void)c; return -1; }
int64_t psnr_stream_connect(const psnr_client* c, ...) { (void)c; return -1; }
int64_t psnr_stream_accept(const psnr_client* c, ...) { (void)c; return -1; }
