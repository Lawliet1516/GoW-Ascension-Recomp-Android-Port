/*
 * ps3recomp - sceNpSignaling HLE implementation
 *
 * Offline: init/term and contexts work, connections report NOT_CONNECTED.
 * mpnet (PS3_MPNET=1, private kit, 2026-11-13): connections are answered through the community server on
 * 127.0.0.1: ActivateConnection looks the peer's online id up there (GET /peer?id=), and the title's handlers get
 * ESTABLISHED. Signatures and event numbers follow how GoW Ascension calls them (static reading of its EBOOT,
 * host\mpnet\NOTES.md "2026-11-13 signaling"):
 *   CreateCtx(npId, handler (the game passes 0), arg, &ctxId)
 *   AddExtendedHandler(ctxId, handler, arg)        the game's real event handler (OPD 0x80D338 -> 0x1A8BF8)
 *   SetCtxOpt(ctxId, opt, value)                   the game: opt 1 (bandwidth probe) = 0
 *   ActivateConnection(ctxId, &peerNpId, &connId)
 *   GetConnectionStatus(ctxId, connId, &status, &addr, &port)    status 2 = active; the game reads the peer here
 *   GetConnectionInfo(ctxId, connId, code, &info)  the game asks codes 1 (RTT), 6 (packet loss), 2 (bandwidth)
 *   GetLocalNetInfo(ctxId, &netInfo)               {u32 size, local addr, mapped addr, nat_status (0..3), upnp,
 *                                                   npport_status, u16 npport}
 * Events the game handles (0x1A8BF8): 0 DEAD, 1 ESTABLISHED, 0x0A peer activated, 0x0B peer deactivated,
 * 0x0C mutual activated. Handlers are called as handler(ctxId, connId, event, errorCode, arg) on the title's next
 * cellSysutilCheckCallback.
 */

#include "sceNpSignaling.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../system/cellSysutil.h"           /* cellSysutilQueueGuestCallbackArgs */
#include "mpnet.h"
#include "np_psnr.h"

#define EV_DEAD         0
#define EV_ESTABLISHED  1
#define EV_MUTUAL       12   /* EXT_MUTUAL_ACTIVATED: the only event that builds the peer into the game's mesh
                                (0x1A86A8 type 2: IP/port lookup, then the mesh's new-peer call). MEASURED need:
                                with ESTABLISHED alone both windows never sent a P2P packet (run 122423). */

static int s_initialized = 0;

typedef struct {
    int in_use;
    u32 handler, arg;          /* guest OPD / guest pointer */
    u32 ext_handler, ext_arg;
} SignalingCtx;

typedef struct {
    int in_use;
    u32 ctx;
    char name[17];
    u32 ip;                    /* host order */
    u16 port;
} SignalingConn;

static SignalingCtx  s_ctx[SCE_NP_SIGNALING_CTX_MAX];
static SignalingConn s_conn[SCE_NP_SIGNALING_CONN_MAX];

static u32 gaddr(const void* p) { return (u32)(uintptr_t)p; }

static void post_event(u32 ctx, u32 conn, u32 event, s32 err)
{
    if (ctx >= SCE_NP_SIGNALING_CTX_MAX || !s_ctx[ctx].in_use) return;
    u32 hs[2][2] = { { s_ctx[ctx].ext_handler, s_ctx[ctx].ext_arg }, { s_ctx[ctx].handler, s_ctx[ctx].arg } };
    for (int i = 0; i < 2; i++) {
        if (!hs[i][0]) continue;
        const u64 args[8] = { ctx, conn, event, (u64)(u32)err, hs[i][1], 0, 0, 0 };
        cellSysutilQueueGuestCallbackArgs(hs[i][0], args);
        printf("[sceNpSignaling] event %u for conn %u (err 0x%X) -> %s handler 0x%08X\n", event, conn, (u32)err,
               i == 0 ? "extended" : "main", hs[i][0]);
    }
}

/* Lifecycle */

s32 sceNpSignalingInit(void)
{
    printf("[sceNpSignaling] Init()\n");
    if (s_initialized)
        return (s32)SCE_NP_SIGNALING_ERROR_ALREADY_INITIALIZED;
    memset(s_ctx, 0, sizeof(s_ctx));
    memset(s_conn, 0, sizeof(s_conn));
    s_initialized = 1;
    return CELL_OK;
}

s32 sceNpSignalingTerm(void)
{
    printf("[sceNpSignaling] Term()\n");
    s_initialized = 0;
    return CELL_OK;
}

/* Context management */

s32 sceNpSignalingCreateCtx(const void* npId, SceNpSignalingHandler handler,
                              void* arg, SceNpSignalingCtxId* ctxId)
{
    (void)npId;
    printf("[sceNpSignaling] CreateCtx(handler 0x%08X)\n", gaddr((const void*)handler));

    /* mpnet (MEASURED run 114933): the game never calls sceNpSignalingInit (on a PS3 the NP library sets
     * signaling up itself), so CreateCtx returned NOT_INITIALIZED (0x80022A01). Initialize on first use. */
    if (!s_initialized && mpnet_on()) {
        memset(s_ctx, 0, sizeof(s_ctx));
        memset(s_conn, 0, sizeof(s_conn));
        s_initialized = 1;
        printf("[sceNpSignaling] initialized on first CreateCtx (mpnet)\n");
    }
    if (!s_initialized) return (s32)SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED;
    if (!ctxId) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < SCE_NP_SIGNALING_CTX_MAX; i++) {
        if (!s_ctx[i].in_use) {
            memset(&s_ctx[i], 0, sizeof(SignalingCtx));
            s_ctx[i].in_use = 1;
            s_ctx[i].handler = gaddr((const void*)handler);
            s_ctx[i].arg = gaddr(arg);
            vm_write32(gaddr(ctxId), (u32)i);
            return CELL_OK;
        }
    }
    return (s32)SCE_NP_SIGNALING_ERROR_OUT_OF_MEMORY;
}

s32 sceNpSignalingDestroyCtx(SceNpSignalingCtxId ctxId)
{
    printf("[sceNpSignaling] DestroyCtx(%u)\n", ctxId);
    if (ctxId >= SCE_NP_SIGNALING_CTX_MAX || !s_ctx[ctxId].in_use)
        return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
    for (int c = 0; c < SCE_NP_SIGNALING_CONN_MAX; c++)
        if (s_conn[c].in_use && s_conn[c].ctx == ctxId) s_conn[c].in_use = 0;
    s_ctx[ctxId].in_use = 0;
    return CELL_OK;
}

s32 sceNpSignalingAddExtendedHandler(SceNpSignalingCtxId ctxId, SceNpSignalingHandler handler, void* arg)
{
    printf("[sceNpSignaling] AddExtendedHandler(ctx %u, handler 0x%08X)\n", ctxId, gaddr((const void*)handler));
    if (ctxId >= SCE_NP_SIGNALING_CTX_MAX || !s_ctx[ctxId].in_use)
        return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
    s_ctx[ctxId].ext_handler = gaddr((const void*)handler);
    s_ctx[ctxId].ext_arg = gaddr(arg);
    return CELL_OK;
}

s32 sceNpSignalingSetCtxOpt(SceNpSignalingCtxId ctxId, s32 optname, s32 optval)
{
    printf("[sceNpSignaling] SetCtxOpt(ctx %u, opt %d, %d)\n", ctxId, optname, optval);
    if (ctxId >= SCE_NP_SIGNALING_CTX_MAX || !s_ctx[ctxId].in_use)
        return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
    return CELL_OK;
}

/* Connections */

s32 sceNpSignalingActivateConnection(SceNpSignalingCtxId ctxId,
                                       const void* peerNpId,
                                       SceNpSignalingConnId* connId)
{
    if (!mpnet_on()) {
        printf("[sceNpSignaling] ActivateConnection() - offline\n");
        return (s32)SCE_NP_SIGNALING_ERROR_NOT_CONNECTED;
    }
    if (ctxId >= SCE_NP_SIGNALING_CTX_MAX || !s_ctx[ctxId].in_use)
        return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
    if (!peerNpId || !connId) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;

    char name[17] = {0};
    for (int i = 0; i < 16; i++) name[i] = (char)vm_read8(gaddr(peerNpId) + i);

    for (int c = 0; c < SCE_NP_SIGNALING_CONN_MAX; c++)
        if (s_conn[c].in_use && s_conn[c].ctx == ctxId && !strcmp(s_conn[c].name, name)) {
            vm_write32(gaddr(connId), (u32)c);
            printf("[sceNpSignaling] ActivateConnection(\"%s\") -> existing conn %d\n", name, c);
            post_event(ctxId, (u32)c, EV_ESTABLISHED, 0);
        post_event(ctxId, (u32)c, EV_MUTUAL, 0);
            return CELL_OK;
        }

    u32 ip = 0; u16 port = 0;
    if (mpnet_peer_lookup(name, &ip, &port) != 0) {
        printf("[sceNpSignaling] ActivateConnection(\"%s\"): the server does not know this player\n", name);
        return (s32)SCE_NP_SIGNALING_ERROR_NOT_CONNECTED;
    }
    for (int c = 0; c < SCE_NP_SIGNALING_CONN_MAX; c++) {
        if (s_conn[c].in_use) continue;
        memset(&s_conn[c], 0, sizeof s_conn[c]);
        s_conn[c].in_use = 1; s_conn[c].ctx = ctxId; s_conn[c].ip = ip; s_conn[c].port = port;
        memcpy(s_conn[c].name, name, sizeof name);
        vm_write32(gaddr(connId), (u32)c);
        printf("[sceNpSignaling] ActivateConnection(\"%s\") -> conn %d at %u.%u.%u.%u:%u\n", name, c,
               ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port);
        post_event(ctxId, (u32)c, EV_ESTABLISHED, 0);
        post_event(ctxId, (u32)c, EV_MUTUAL, 0);
        return CELL_OK;
    }
    return (s32)SCE_NP_SIGNALING_ERROR_OUT_OF_MEMORY;
}

s32 sceNpSignalingDeactivateConnection(SceNpSignalingCtxId ctxId,
                                         SceNpSignalingConnId connId)
{
    if (connId < SCE_NP_SIGNALING_CONN_MAX && s_conn[connId].in_use && s_conn[connId].ctx == ctxId) {
        printf("[sceNpSignaling] DeactivateConnection(conn %u \"%s\")\n", connId, s_conn[connId].name);
        s_conn[connId].in_use = 0;
    }
    return CELL_OK;
}

static SignalingConn* find_conn(SceNpSignalingCtxId ctxId, SceNpSignalingConnId connId)
{
    if (connId >= SCE_NP_SIGNALING_CONN_MAX || !s_conn[connId].in_use || s_conn[connId].ctx != ctxId) return NULL;
    return &s_conn[connId];
}

s32 sceNpSignalingGetConnectionStatus(SceNpSignalingCtxId ctxId,
                                        SceNpSignalingConnId connId,
                                        s32* connStatus,
                                        u32* peerAddr, u16* peerPort)
{
    SignalingConn* c = mpnet_on() ? find_conn(ctxId, connId) : NULL;
    if (connStatus) vm_write32(gaddr(connStatus), c ? (u32)SCE_NP_SIGNALING_CONN_STATUS_ACTIVE
                                                    : (u32)SCE_NP_SIGNALING_CONN_STATUS_INACTIVE);
    if (peerAddr) vm_write32(gaddr(peerAddr), c ? c->ip : 0u);      /* in_addr: big-endian in guest memory */
    if (peerPort) vm_write16(gaddr(peerPort), c ? c->port : (u16)0);
    return CELL_OK;
}

s32 sceNpSignalingGetConnectionInfo(SceNpSignalingCtxId ctxId,
                                      SceNpSignalingConnId connId,
                                      s32 code, void* info)
{
    if (!info) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    SignalingConn* c = mpnet_on() ? find_conn(ctxId, connId) : NULL;
    if (!c) return (s32)SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND;
    u32 ea = gaddr(info);
    switch (code) {
        case 1: vm_write32(ea, 1000); break;            /* RTT, microseconds (loopback) */
        case 2: vm_write32(ea, 10000000); break;        /* bandwidth, bits/s */
        case 4: case 5: vm_write32(ea, c->ip); vm_write16(ea + 4, c->port); break;   /* peer / mapped address */
        case 6: vm_write32(ea, 0); break;               /* packet loss */
        default: for (u32 o = 0; o < 36; o += 4) vm_write32(ea + o, 0); break;
    }
    return CELL_OK;
}

s32 sceNpSignalingGetLocalNetInfo(SceNpSignalingCtxId ctxId, void* netInfo)
{
    (void)ctxId;
    if (!netInfo) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    u32 ea = gaddr(netInfo);
    vm_write32(ea + 0x04, 0x7F000001u);                 /* local address */
    vm_write32(ea + 0x08, 0x7F000001u);                 /* mapped address */
    vm_write32(ea + 0x0C, 2);                           /* nat_status: type 2 (the game accepts 0..3) */
    vm_write32(ea + 0x10, 0);                           /* upnp_status */
    vm_write32(ea + 0x14, 0);                           /* npport_status */
    vm_write16(ea + 0x18, np_psnr_p2p_port());           /* npport */
    return CELL_OK;
}

s32 sceNpSignalingGetPeerNetInfo(SceNpSignalingCtxId ctxId,
                                   SceNpSignalingConnId connId,
                                   u32* peerAddr, u16* peerPort)
{
    SignalingConn* c = find_conn(ctxId, connId);
    if (peerAddr) vm_write32(gaddr(peerAddr), c ? c->ip : 0u);
    if (peerPort) vm_write16(gaddr(peerPort), c ? c->port : (u16)0);
    return CELL_OK;
}
