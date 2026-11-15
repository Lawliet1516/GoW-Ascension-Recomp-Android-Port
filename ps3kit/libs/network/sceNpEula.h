/* ps3recomp - sceNpEula (cellSysutilNpEula), mpnet private kit */
#ifndef PS3_SCENPEULA_H
#define PS3_SCENPEULA_H
#include "ps3emu/ps3types.h"
#ifdef __cplusplus
extern "C" {
#endif
s32 sceNpEulaCheckEulaStatus(u32 communicationId, u32 arg1, u64 arg2, u32 cbFunc, u32 userdata);
s32 sceNpEulaShowCurrentEula(u32 communicationId, u64 arg1, u32 cbFunc, u32 userdata);
s32 sceNpEulaAbort(void);
#ifdef __cplusplus
}
#endif
#endif
