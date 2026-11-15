/*
 * ps3recomp - sceNpTus HLE
 *
 * NP Title User Storage (TUS): Sony's per-account server storage for a
 * title (variables = signed 64-bit counters, data = byte blobs, each in a
 * numbered slot). The firmware API (np_tus) is answered from files on this
 * PC, see sceNpTus.c.
 *
 * Signatures and layouts are the firmware's (the generic HLE adapter passes
 * the PPC argument registers raw, so every pointer below is a GUEST address).
 */

#ifndef PS3RECOMP_SCE_NP_TUS_H
#define PS3RECOMP_SCE_NP_TUS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes (the NP community library's, shared by TUS and Score)
 * -----------------------------------------------------------------------*/
#define SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED      0x8002A101
#define SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED          0x8002A102
#define SCE_NP_COMMUNITY_ERROR_OUT_OF_MEMORY            0x8002A103
#define SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT         0x8002A104
#define SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS         0x8002A107
#define SCE_NP_COMMUNITY_ERROR_ABORTED                  0x8002A109
#define SCE_NP_COMMUNITY_ERROR_BODY_TOO_LARGE           0x8002A10C
#define SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT    0x8002A1A1
#define SCE_NP_COMMUNITY_ERROR_INVALID_ID               0x8002A1A3
#define SCE_NP_COMMUNITY_ERROR_INVALID_TYPE             0x8002A1A5
/* What the server answered for an empty data slot (God of War: Ascension's
 * npTus.cpp treats exactly this value as "no data", not as an error). */
#define SCE_NP_COMMUNITY_SERVER_ERROR_USER_STORAGE_DATA_NOT_FOUND 0x8002A44A

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
#define SCE_NP_TUS_DATA_INFO_MAX_SIZE     384
#define SCE_NP_TUS_MAX_CTX_NUM            32
#define SCE_NP_TUS_MAX_SLOT_NUM_PER_TRANS 64
#define SCE_NP_TUS_MAX_USER_NUM_PER_TRANS 101

/* TryAndSetVariable comparisons */
#define SCE_NP_TUS_OPETYPE_EQUAL             1
#define SCE_NP_TUS_OPETYPE_NOT_EQUAL         2
#define SCE_NP_TUS_OPETYPE_GREATER_THAN      3
#define SCE_NP_TUS_OPETYPE_GREATER_OR_EQUAL  4
#define SCE_NP_TUS_OPETYPE_LESS_THAN         5
#define SCE_NP_TUS_OPETYPE_LESS_OR_EQUAL     6

/* Guest structure layouts (big-endian, PS3 32-bit pointers):
 *   SceNpId (0x24): online id {char data[16]; char term; char dummy[3]},
 *                   u8 opt[8], u8 reserved[8]
 *   SceNpTusVariable (0x78): ownerId 0x00, s32 hasData 0x24,
 *                   u64 lastChangedDate (CellRtcTick) 0x28, pad 0x30,
 *                   lastChangedAuthorId 0x34, s64 variable 0x58,
 *                   s64 oldVariable 0x60, reserved 0x68
 *   SceNpTusDataInfo (0x188): u32 infoSize 0x00, pad 0x04, u8 data[384] 0x08
 *   SceNpTusDataStatus (0x1E8): ownerId 0x00, s32 hasData 0x24,
 *                   u64 lastChangedDate 0x28, lastChangedAuthorId 0x30,
 *                   u32 data (guest ptr) 0x54, u32 dataSize 0x58,
 *                   SceNpTusDataInfo info 0x5C */
#define SCE_NP_TUS_NPID_SIZE          0x24
#define SCE_NP_TUS_VARIABLE_SIZE      0x78
#define SCE_NP_TUS_DATA_INFO_SIZE     0x188
#define SCE_NP_TUS_DATA_STATUS_SIZE   0x1E8

typedef s32 SceNpTusSlotId;

/* ---------------------------------------------------------------------------
 * Functions (guest addresses for every pointer)
 * -----------------------------------------------------------------------*/

/* Lifecycle */
s32 sceNpTusInit(s32 prio);
s32 sceNpTusTerm(void);

/* Contexts */
s32 sceNpTusCreateTitleCtx(u32 communicationId, u32 passphrase, u32 selfNpId);
s32 sceNpTusDestroyTitleCtx(s32 titleCtxId);
s32 sceNpTusCreateTransactionCtx(s32 titleCtxId);
s32 sceNpTusDestroyTransactionCtx(s32 transId);
s32 sceNpTusSetTimeout(s32 ctxId, u32 timeout);
s32 sceNpTusAbortTransaction(s32 transId);
s32 sceNpTusWaitAsync(s32 transId, u32 result);
s32 sceNpTusPollAsync(s32 transId, u32 result);

/* Variables */
s32 sceNpTusSetMultiSlotVariable(s32 transId, u32 targetNpId, u32 slotIdArray,
                                 u32 variableArray, s32 arrayNum, u32 option);
s32 sceNpTusSetMultiSlotVariableAsync(s32 transId, u32 targetNpId, u32 slotIdArray,
                                      u32 variableArray, s32 arrayNum, u32 option);
s32 sceNpTusGetMultiSlotVariable(s32 transId, u32 targetNpId, u32 slotIdArray,
                                 u32 variableArray, u32 variableArraySize,
                                 s32 arrayNum, u32 option);
s32 sceNpTusGetMultiSlotVariableAsync(s32 transId, u32 targetNpId, u32 slotIdArray,
                                      u32 variableArray, u32 variableArraySize,
                                      s32 arrayNum, u32 option);
s32 sceNpTusAddAndGetVariable(s32 transId, u32 targetNpId, SceNpTusSlotId slotId,
                              s64 inVariable, u32 outVariable, u32 outVariableSize,
                              u32 option);
s32 sceNpTusAddAndGetVariableAsync(s32 transId, u32 targetNpId, SceNpTusSlotId slotId,
                                   s64 inVariable, u32 outVariable, u32 outVariableSize,
                                   u32 option);
s32 sceNpTusTryAndSetVariable(s32 transId, u32 targetNpId, SceNpTusSlotId slotId,
                              s32 opeType, s64 variable, u32 resultVariable,
                              u32 resultVariableSize, u32 option);
s32 sceNpTusTryAndSetVariableAsync(s32 transId, u32 targetNpId, SceNpTusSlotId slotId,
                                   s32 opeType, s64 variable, u32 resultVariable,
                                   u32 resultVariableSize, u32 option);
s32 sceNpTusDeleteMultiSlotVariable(s32 transId, u32 targetNpId, u32 slotIdArray,
                                    s32 arrayNum, u32 option);
s32 sceNpTusDeleteMultiSlotVariableAsync(s32 transId, u32 targetNpId, u32 slotIdArray,
                                         s32 arrayNum, u32 option);

/* Data (the 9th argument, SceNpTusSetDataOptParam*, is on the guest stack and
 * not read: it only asks the server to compare dates/authors) */
s32 sceNpTusSetData(s32 transId, u32 targetNpId, SceNpTusSlotId slotId,
                    u32 totalSize, u32 sendSize, u32 data, u32 info,
                    u32 infoStructSize);
s32 sceNpTusSetDataAsync(s32 transId, u32 targetNpId, SceNpTusSlotId slotId,
                         u32 totalSize, u32 sendSize, u32 data, u32 info,
                         u32 infoStructSize);
s32 sceNpTusGetData(s32 transId, u32 targetNpId, SceNpTusSlotId slotId,
                    u32 dataStatus, u32 dataStatusSize, u32 data, u32 recvSize,
                    u32 option);
s32 sceNpTusGetDataAsync(s32 transId, u32 targetNpId, SceNpTusSlotId slotId,
                         u32 dataStatus, u32 dataStatusSize, u32 data, u32 recvSize,
                         u32 option);
s32 sceNpTusGetMultiSlotDataStatus(s32 transId, u32 targetNpId, u32 slotIdArray,
                                   u32 statusArray, u32 statusArraySize,
                                   s32 arrayNum, u32 option);
s32 sceNpTusGetMultiSlotDataStatusAsync(s32 transId, u32 targetNpId, u32 slotIdArray,
                                        u32 statusArray, u32 statusArraySize,
                                        s32 arrayNum, u32 option);
s32 sceNpTusDeleteMultiSlotData(s32 transId, u32 targetNpId, u32 slotIdArray,
                                s32 arrayNum, u32 option);
s32 sceNpTusDeleteMultiSlotDataAsync(s32 transId, u32 targetNpId, u32 slotIdArray,
                                     s32 arrayNum, u32 option);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_TUS_H */
