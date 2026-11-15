/*
 * ps3recomp - sceNp HLE implementation
 *
 * Provides fake PSN identity so games can proceed through NP checks.
 * The username defaults to "PS3Player" but is configurable.
 */

#include "sceNp.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "np_psnr.h"
#include "mpnet.h"
#include "../system/cellSysutil.h"
#include "../filesystem/edat.h"


/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int  s_np_initialized = 0;
static int  s_score_initialized = 0;
static char s_fake_username[SCE_NP_ONLINEID_MAX_LENGTH + 1] = "PS3Player";

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/

void sceNpSetFakeUsername(const char* username)
{
    if (username) {
        strncpy(s_fake_username, GUEST_PTR(username, const char*),
            SCE_NP_ONLINEID_MAX_LENGTH);
        s_fake_username[SCE_NP_ONLINEID_MAX_LENGTH] = '\0';
    }
}

const char* np_fake_username(void)
{
    return s_fake_username;
}

/* Build a fake NP ID from the current username (PS3_NP_ONLINE_ID wins, so
 * two instances on one machine can be two players). */
static void np_build_fake_id(SceNpId* npId)
{
    memset(npId, 0, sizeof(SceNpId));
    strncpy(npId->handle.data, np_psnr_online_id(), SCE_NP_ONLINEID_MAX_LENGTH);
    npId->handle.term = '\0';
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 sceNpInit(u32 poolSize, void* poolPtr)
{
    (void)poolSize;
    (void)poolPtr;

    printf("[sceNp] Init(poolSize=%u, username=\"%s\")\n",
           poolSize, np_psnr_online_id());
    mpnet_init();

    if (s_np_initialized)
        return SCE_NP_ERROR_ALREADY_INITIALIZED;

    s_np_initialized = 1;
    return CELL_OK;
}

/* Whether sceNpInit has run; sceNpBasic follows it (see sceNpBasic.c). */
int np_is_initialized(void) { return s_np_initialized; }

s32 sceNpTerm(void)
{
    printf("[sceNp] Term()\n");

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    s_np_initialized = 0;
    return CELL_OK;
}

s32 sceNpGetNpId(s32 userId, SceNpId* npId)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!npId)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    /* A guest address, like every sibling getter's: writing through it
     * untranslated hit a host address and crashed Simpsons Arcade the first
     * time it asked for its NP ID online. */
    npId = GUEST_PTR(npId, SceNpId*);
    np_build_fake_id(npId);
    printf("[sceNp] GetNpId(user=%d) -> \"%s\"\n", userId, np_psnr_online_id());
    return CELL_OK;
}

s32 sceNpGetOnlineId(s32 userId, SceNpOnlineId* onlineId)
{
    onlineId = GUEST_PTR(onlineId, SceNpOnlineId*);
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!onlineId)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(onlineId, 0, sizeof(SceNpOnlineId));
    strncpy(onlineId->data, np_psnr_online_id(), SCE_NP_ONLINEID_MAX_LENGTH);
    onlineId->term = '\0';

    printf("[sceNp] GetOnlineId(user=%d) -> \"%s\"\n", userId, np_psnr_online_id());
    return CELL_OK;
}

s32 sceNpGetOnlineName(s32 userId, SceNpOnlineName* onlineName)
{
    onlineName = GUEST_PTR(onlineName, SceNpOnlineName*);
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!onlineName)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(onlineName, 0, sizeof(SceNpOnlineName));
    strncpy(onlineName->data, np_psnr_online_id(),
            SCE_NP_ONLINENAME_MAX_LENGTH - 1);

    printf("[sceNp] GetOnlineName(user=%d) -> \"%s\"\n",
           userId, np_psnr_online_id());
    return CELL_OK;
}

s32 sceNpGetUserProfile(s32 userId, SceNpUserInfo* userInfo)
{
    userInfo = GUEST_PTR(userInfo, SceNpUserInfo*);
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!userInfo)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(userInfo, 0, sizeof(SceNpUserInfo));
    np_build_fake_id(&userInfo->npId);
    strncpy(userInfo->onlineName.data, np_psnr_online_id(),
            SCE_NP_ONLINENAME_MAX_LENGTH - 1);
    /* Leave avatar URL empty */

    printf("[sceNp] GetUserProfile(user=%d) -> \"%s\"\n",
           userId, np_psnr_online_id());
    return CELL_OK;
}

s32 sceNpGetAccountRegion(s32 userId, u32* region)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!region)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    /* Region: US (SCEA) = 0x5553 ('US') */
    vm_write32((u32)(uintptr_t)region, (u32)0x5553);

    printf("[sceNp] GetAccountRegion(user=%d) -> US\n", userId);
    return CELL_OK;
}

s32 sceNpGetAccountAge(s32 userId, s32* age)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!age)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    vm_write32((u32)(uintptr_t)age, 25); /* default adult age */
    printf("[sceNp] GetAccountAge(user=%d) -> 25\n", userId);
    return CELL_OK;
}

s32 sceNpGetMyLanguages(SceNpMyLanguages* langs)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!langs)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    u32 langs_ea = (u32)(uintptr_t)langs;
    vm_write32(langs_ea + 0, SCE_NP_LANG_ENGLISH);
    vm_write32(langs_ea + 4, 0);
    vm_write32(langs_ea + 8, 0);

    printf("[sceNp] GetMyLanguages() -> English\n");
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NP Manager (sign-in state)
 *
 * The toolkit runs with a fake local profile but no live PSN connection. So the
 * manager reports OFFLINE — games gate their online flows on GetStatus and skip
 * them cleanly — while the identity getters still hand back the fake account,
 * matching how NP behaves on a real signed-in-but-disconnected console.
 * Imported by most online-capable titles.
 * -----------------------------------------------------------------------*/

static SceNpManagerCallback s_npmgr_cb = NULL;
static void*                s_npmgr_cb_arg = NULL;

s32 sceNpManagerGetStatus(s32* status)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!status)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    /* `status` is a GUEST address -- the HLE ABI adapter passes pointer
     * parameters straight through as guest values, so dereferencing one
     * writes to whatever host address shares that number. Same trap as
     * cellGcmSys had. Tokyo Jungle calls this during its online init. */
    /* Signed in when there is a psnr server to be signed in to. */
    s32 st = (np_psnr_enabled() || mpnet_on()) ? SCE_NP_MANAGER_STATUS_ONLINE : SCE_NP_MANAGER_STATUS_OFFLINE;
    vm_write32((uint32_t)(uintptr_t)status, (uint32_t)st);
    { static s32 last = -2;
      if (st != last) printf("[sceNp] ManagerGetStatus() -> %s\n", st < 0 ? "OFFLINE" : "ONLINE");
      last = st; }
    return CELL_OK;
}

s32 sceNpManagerRegisterCallback(SceNpManagerCallback callback, void* arg)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    s_npmgr_cb     = callback;
    s_npmgr_cb_arg = arg;
    printf("[sceNp] ManagerRegisterCallback()\n");
    /* Online: tell the title it is signed in, as the console does once the
     * manager reaches ONLINE. Delivered on its next cellSysutilCheckCallback. */
    if ((np_psnr_enabled() || mpnet_on()) && callback) {
        const u64 args[8] = { (u64)(u32)SCE_NP_MANAGER_STATUS_ONLINE, 0,
                              (u64)(u32)(uintptr_t)arg, 0, 0, 0, 0, 0 };
        cellSysutilQueueGuestCallbackArgs((u32)(uintptr_t)callback, args);
    }
    return CELL_OK;
}

s32 sceNpManagerUnregisterCallback(void)
{
    s_npmgr_cb     = NULL;
    s_npmgr_cb_arg = NULL;
    return CELL_OK;
}

/* Identity getters: reuse the fake-profile implementations (offline-with-account). */
s32 sceNpManagerGetNpId(SceNpId* npId)               { return sceNpGetNpId(0, npId); }
s32 sceNpManagerGetOnlineId(SceNpOnlineId* onlineId) { return sceNpGetOnlineId(0, onlineId); }
s32 sceNpManagerGetOnlineName(SceNpOnlineName* name) { return sceNpGetOnlineName(0, name); }
s32 sceNpManagerGetAccountAge(s32* age)              { return sceNpGetAccountAge(0, age); }

/* Parental controls: an unrestricted adult account. Unimplemented, these
 * returned OK without writing, and the title read whatever was in its stack. */
s32 sceNpManagerGetContentRatingFlag(s32* isRestricted, s32* age)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (isRestricted) vm_write32((u32)(uintptr_t)isRestricted, 0);
    if (age) vm_write32((u32)(uintptr_t)age, 25);
    return CELL_OK;
}

s32 sceNpManagerGetChatRestrictionFlag(s32* isRestricted)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (isRestricted) vm_write32((u32)(uintptr_t)isRestricted, 0);
    return CELL_OK;
}

/* Score setup is local even while the NP manager is offline. Games may
 * initialize it before starting the worker that decides whether to use PSN. */
s32 sceNpScoreInit(void)
{
    if (s_score_initialized)
        return SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED;
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    s_score_initialized = 1;
    return CELL_OK;
}

s32 sceNpScoreTerm(void)
{
    if (!s_score_initialized)
        return SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    s_score_initialized = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NPDRM
 *
 * On hardware sceNpDrmIsAvailable[2](klicensee, path) checks the licence for an
 * NPDRM file and leaves the klicensee with the kernel, so later cellFsOpen calls
 * on EDATs return plaintext. Free (license type 3) and disc-bound game data use a
 * title-specific klicensee that only the game knows; record it for the EDAT
 * reader (libs/filesystem/edat.c), which verifies it against each file's
 * dev_hash before using it. The two calls differ only in licence-check details
 * that do not apply here.
 * -----------------------------------------------------------------------*/

static s32 np_drm_is_available(const u8* k_licensee, const char* drm_path)
{
    const u8* k = GUEST_PTR(k_licensee, const u8*);
    const char* path = GUEST_PTR(drm_path, const char*);
    /* A NULL klicensee is legal for content that needs none; nothing to record. */
    if (k) edat_add_klicensee(k);
    fprintf(stderr, "[sceNp] DRM availability check: %s\n", path ? path : "(null)");
    return CELL_OK;
}

s32 sceNpDrmIsAvailable(const u8* k_licensee, const char* drm_path)
{
    return np_drm_is_available(k_licensee, drm_path);
}

s32 sceNpDrmIsAvailable2(const u8* k_licensee, const char* drm_path)
{
    return np_drm_is_available(k_licensee, drm_path);
}
/* ---------------------------------------------------------------------------
 * Tickets (mpnet). The game asks RequestTicket2, waits for the manager event
 * GOT_TICKET (0xFF, result = size), then reads the account id with
 * GetTicketParam(4) and later copies the raw ticket into its SAM login
 * (GetTicket). The ticket is our community server's own format (/ticket).
 * -----------------------------------------------------------------------*/

#define NP_MANAGER_EVENT_GOT_TICKET 0xFF

s32 sceNpManagerRequestTicket2(const SceNpId* npId, const void* version, const char* serviceId,
                               const void* cookie, u32 cookieSize, const char* entitlementId, u32 consumedCount)
{
    (void)npId; (void)version; (void)cookie; (void)cookieSize; (void)entitlementId; (void)consumedCount;
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    const char* sid = serviceId ? GUEST_PTR(serviceId, const char*) : "";
    if (!mpnet_on()) {
        printf("[sceNp] ManagerRequestTicket2(service=\"%s\") -> offline, no ticket\n", sid);
        return CELL_OK;
    }
    const unsigned char* t; uint64_t acct;
    int n = mpnet_ticket(&t, &acct);
    printf("[sceNp] ManagerRequestTicket2(service=\"%s\") -> %d bytes, account %llu\n", sid, n,
           (unsigned long long)acct);
    if (s_npmgr_cb && n > 0) {
        const u64 args[8] = { (u64)(u32)NP_MANAGER_EVENT_GOT_TICKET, (u64)(u32)n,
                              (u64)(u32)(uintptr_t)s_npmgr_cb_arg, 0, 0, 0, 0, 0 };
        cellSysutilQueueGuestCallbackArgs((u32)(uintptr_t)s_npmgr_cb, args);
    }
    return CELL_OK;
}

s32 sceNpManagerRequestTicket(const SceNpId* npId, const char* serviceId, const void* cookie,
                              u32 cookieSize, const char* entitlementId, u32 consumedCount)
{
    return sceNpManagerRequestTicket2(npId, NULL, serviceId, cookie, cookieSize, entitlementId, consumedCount);
}

s32 sceNpManagerGetTicket(void* buffer, u32* bufferSize)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!bufferSize) return SCE_NP_ERROR_INVALID_ARGUMENT;
    const unsigned char* t = NULL; uint64_t acct;
    int n = mpnet_on() ? mpnet_ticket(&t, &acct) : 0;
    u32 sz_ea = (u32)(uintptr_t)bufferSize;
    if (n <= 0) { vm_write32(sz_ea, 0); return (s32)0x8002A10Bu; /* ticket unavailable (best guess) */ }
    u32 cap = vm_read32(sz_ea);
    if (buffer) {
        if (cap < (u32)n) return (s32)0x8002A002u;   /* insufficient buffer (best guess) */
        memcpy(vm_ptr8((u32)(uintptr_t)buffer), t, (size_t)n);
    }
    vm_write32(sz_ea, (u32)n);
    printf("[sceNp] ManagerGetTicket(cap=%u) -> %d bytes\n", cap, n);
    return CELL_OK;
}

/* SceNpTicketParam is a 256-byte union; ids: 0 serial, 1 issuer, 2 issued, 3 expire, 4 subject account
 * id (u64), 5 subject online id, 6 region, 7 domain, 8 service id, 9 status, 10 duration, 11 DOB. */
s32 sceNpManagerGetTicketParam(s32 paramId, void* param)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!param) return SCE_NP_ERROR_INVALID_ARGUMENT;
    const unsigned char* t; uint64_t acct = 0;
    if (mpnet_on()) mpnet_ticket(&t, &acct);
    u32 p = (u32)(uintptr_t)param;
    memset(vm_ptr8(p), 0, 256);
    switch (paramId) {
        case 4: vm_write32(p, (u32)(acct >> 32)); vm_write32(p + 4, (u32)acct); break;
        case 5: strncpy((char*)vm_ptr8(p), np_psnr_online_id(), 16); break;
        case 6: memcpy(vm_ptr8(p), "us", 2); break;
        case 7: memcpy(vm_ptr8(p), "un", 2); break;
        default: break;
    }
    printf("[sceNp] ManagerGetTicketParam(%d)%s\n", paramId, paramId == 4 ? " -> account id" : "");
    return CELL_OK;
}
