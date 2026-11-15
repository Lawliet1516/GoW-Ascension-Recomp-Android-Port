/*
 * ps3recomp - sceNpEula (cellSysutilNpEula), mpnet private kit
 *
 * God of War: Ascension's lobby gate waits for CheckEulaStatus's callback (func_0032C5B8): event 1 or 2
 * with r5 = the accepted version marks the EULA done; events 3..5 are errors. Online (PS3_MPNET=1) the
 * EULA of our community server counts as accepted: event 1, error 0, version 1. Offline the call fails,
 * as an unsigned-in console does, so the game keeps its old path.
 */
#include "sceNpEula.h"
#include "mpnet.h"
#include "../system/cellSysutil.h"
#include <stdio.h>

#define EULA_EVENT_STATUS 1

s32 sceNpEulaCheckEulaStatus(u32 communicationId, u32 arg1, u64 arg2, u32 cbFunc, u32 userdata)
{
    (void)communicationId; (void)arg1; (void)arg2;
    if (!mpnet_on()) {
        printf("[sceNpEula] CheckEulaStatus -> offline\n");
        return (s32)0x8002E501u;   /* best guess: not signed in */
    }
    printf("[sceNpEula] CheckEulaStatus(cb=0x%08X) -> accepted (version 1)\n", cbFunc);
    if (cbFunc) {
        const u64 args[8] = { EULA_EVENT_STATUS, 0, 1, (u64)userdata, 0, 0, 0, 0 };
        cellSysutilQueueGuestCallbackArgs(cbFunc, args);
    }
    return 0;
}

s32 sceNpEulaShowCurrentEula(u32 communicationId, u64 arg1, u32 cbFunc, u32 userdata)
{
    (void)communicationId; (void)arg1;
    printf("[sceNpEula] ShowCurrentEula -> accepted\n");
    if (mpnet_on() && cbFunc) {
        const u64 args[8] = { EULA_EVENT_STATUS, 0, 1, (u64)userdata, 0, 0, 0, 0 };
        cellSysutilQueueGuestCallbackArgs(cbFunc, args);
    }
    return 0;
}

s32 sceNpEulaAbort(void) { return 0; }
