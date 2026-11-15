/*
 * ps3recomp - sceNpTus HLE implementation (NP Title User Storage)
 *
 * TUS is Sony's per-account server storage for a title: numbered slots that
 * hold a signed 64-bit "variable" or a byte blob ("data"). Titles keep online
 * profiles there (God of War: Ascension stores its whole multiplayer profile,
 * a JSON text, in data slot 1). With PSN gone, this stand-in answers the
 * firmware API from files on this PC, so a title's own load/save code runs
 * unchanged:
 *
 *   <base>/<commId>_<nn>/slot<NNN>.dat    data slot: the bytes the title stored
 *   <base>/<commId>_<nn>/slot<NNN>.info   its SceNpTusDataInfo (when non-empty)
 *   <base>/<commId>_<nn>/var<NNN>.txt     variable slot: "<value> <lastChangedTick>"
 *
 *   <base> = PS3_NP_TUS_DIR, else <hdd0>/home/00000001/tus, <hdd0> =
 *            PS3_HDD0_ROOT, else ./gamedata/dev_hdd0 (beside the saves, where
 *            cellSaveData keeps them).
 *
 * One local account (user 00000001, as cellUserInfo reports): the target NpId
 * a title passes does not pick a store (offline it is empty anyway); it is
 * only copied into the status the title reads back.
 *
 * A write goes to <file>.tmp, is flushed to disk and renamed over the slot,
 * so a crash or power cut leaves the old copy or the new one, never half of
 * one. Earlier copies are kept beside the slot, because a title that loads a
 * damaged or missing slot falls back to its defaults and then saves those over
 * it (several times: titles re-save unchanged data too):
 *
 *   - a write of the bytes the slot already holds is skipped (no rotation);
 *   - <file>.bak1 .. .bak3     the last three copies replaced;
 *   - <file>.session .. .session2   the slot as it was when each of the last
 *                              three runs first changed it;
 *   - <file>.keep              never rotated: the LARGEST copy this slot has
 *                              had (a profile only grows as it is played), so
 *                              defaults saved over a lost profile, any number
 *                              of times, never push it out;
 *   - an empty data write over a non-empty slot is refused (the slot is kept).
 *
 * To get a copy back: quit the game, copy it over <file> (the log names them).
 *
 * GetData writes a NUL after the bytes it copies when the buffer has room, and
 * a slot larger than the title's buffer is cut to the buffer with its last
 * byte zeroed (logged): titles parse the text up to a NUL (the slot file is
 * not changed by this).
 *
 * Every operation completes when it is called (file I/O on this PC takes
 * microseconds); the async variants keep the result for PollAsync/WaitAsync,
 * which then always answer "done".
 *
 * PS3_NP_TUS_FILES=0: nothing is read or written (every data slot reads as
 * empty, writes are dropped): a title then starts from its defaults each run.
 *
 * The generic HLE adapter passes the PPC argument registers raw: every
 * pointer here is a GUEST address. Layouts: sceNpTus.h.
 */

#include "sceNpTus.h"
#include "mpnet.h"                         /* leaderboards: mpnet_tus_saved (online mode only) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_read*, vm_write*, vm_memcpy_* */

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <direct.h>
#  include <io.h>
#  define TUS_MKDIR(p) _mkdir(p)
#else
#  include <unistd.h>
#  include <pthread.h>
#  define TUS_MKDIR(p) mkdir((p), 0755)
#endif

/* ---------------------------------------------------------------------------
 * State
 * -----------------------------------------------------------------------*/

#define TUS_MAX_TITLE  SCE_NP_TUS_MAX_CTX_NUM
#define TUS_MAX_TRANS  64
#define TUS_MAX_FILE   (16u << 20)     /* a data slot larger than this is refused */
#define TUS_PATH       1100

typedef struct {
    int  used;
    char comm[16];                     /* "NPWR00802_00" */
    char dir[1024];                    /* <base>/<comm> */
    u8   npid[SCE_NP_TUS_NPID_SIZE];   /* the self NpId the title gave */
} tus_title;

typedef struct {
    int  used;
    int  title;                        /* index into s_title */
    int  aborted;
    s32  result;                       /* of the last operation (PollAsync) */
    u32  timeout;
    u8*  up_buf;                       /* SetData in pieces (sendSize < totalSize) */
    u32  up_total, up_have;
    s32  up_slot;
} tus_trans;

static int       s_init;
static tus_title s_title[TUS_MAX_TITLE];
static tus_trans s_trans[TUS_MAX_TRANS];
static char      s_base[1024];
static int       s_base_done;
static int       s_files = 1;
static u32       s_session[64];        /* FNV-1a of the paths that got their .session copy */
static int       s_session_n;

#ifdef _WIN32
static SRWLOCK s_lock = SRWLOCK_INIT;
#  define TUS_LOCK()   AcquireSRWLockExclusive(&s_lock)
#  define TUS_UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#  define TUS_LOCK()   pthread_mutex_lock(&s_lock)
#  define TUS_UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

/* ---------------------------------------------------------------------------
 * Files
 * -----------------------------------------------------------------------*/

static void tus_resolve_base(void)
{
    if (s_base_done)
        return;
    s_base_done = 1;
    const char* f = getenv("PS3_NP_TUS_FILES");
    s_files = !(f && f[0] == '0');
    const char* d = getenv("PS3_NP_TUS_DIR");
    if (d && *d) {
        snprintf(s_base, sizeof s_base, "%s", d);
    } else {
        const char* h = getenv("PS3_HDD0_ROOT");
        snprintf(s_base, sizeof s_base, "%s/home/00000001/tus", (h && *h) ? h : "./gamedata/dev_hdd0");
    }
    for (char* p = s_base; *p; p++)
        if (*p == '\\') *p = '/';
    size_t n = strlen(s_base);
    while (n > 1 && s_base[n - 1] == '/')
        s_base[--n] = 0;
    printf("[np-tus] store: %s%s\n", s_base,
           s_files ? "" : " (PS3_NP_TUS_FILES=0: nothing is read or written)");
    fflush(stdout);
}

static void tus_mkdirs(const char* path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            TUS_MKDIR(tmp);            /* "F:", "." and existing ones just fail */
            *p = '/';
        }
    }
    TUS_MKDIR(tmp);
}

static void tus_path(char* out, size_t cap, const tus_title* t, const char* kind, s32 slot, const char* ext)
{
    snprintf(out, cap, "%s/%s%03d%s", t->dir, kind, (int)slot, ext);
}

static u64 tus_tick_of(time_t t)       /* CellRtcTick: microseconds since 0001-01-01 */
{
    return ((u64)t + 62135596800ull) * 1000000ull;
}

static u64 tus_file_tick(const char* path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return tus_tick_of(st.st_mtime);
}

/* The whole file, or NULL when it does not exist (or is too large). */
static u8* tus_read(const char* path, u32* size)
{
    *size = 0;
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        n = ftell(f);
    if (n < 0 || (unsigned long)n > TUS_MAX_FILE || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        printf("[np-tus] %s: unreadable or larger than %u bytes\n", path, TUS_MAX_FILE);
        return NULL;
    }
    u8* b = (u8*)malloc((size_t)n + 1);
    if (!b) {
        fclose(f);
        return NULL;
    }
    size_t got = n ? fread(b, 1, (size_t)n, f) : 0;
    fclose(f);
    if (got != (size_t)n) {
        free(b);
        printf("[np-tus] %s: short read\n", path);
        return NULL;
    }
    *size = (u32)n;
    return b;
}

static int tus_exists(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int tus_move(const char* from, const char* to)   /* replaces "to" */
{
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

static int tus_put(const char* path, const void* data, u32 size)   /* path.tmp, flushed */
{
    FILE* f = fopen(path, "wb");
    if (!f)
        return -1;
    int ok = (size ? fwrite(data, 1, size, f) == size : 1) && fflush(f) == 0;
#ifdef _WIN32
    if (ok) ok = _commit(_fileno(f)) == 0;
#else
    if (ok) ok = fsync(fileno(f)) == 0;
#endif
    if (fclose(f) != 0)
        ok = 0;
    if (!ok)
        remove(path);
    return ok ? 0 : -1;
}

static int tus_copy(const char* from, const char* to)
{
    u32 n;
    u8* b = tus_read(from, &n);
    if (!b)
        return -1;
    char tmp[TUS_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", to);
    int rc = tus_put(tmp, b, n);
    free(b);
    return rc == 0 ? tus_move(tmp, to) : -1;
}

static u32 tus_fnv(const char* s)
{
    u32 h = 2166136261u;
    for (; *s; s++) h = (h ^ (u8)*s) * 16777619u;
    return h;
}

static long tus_size(const char* path)  /* -1: no such file */
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

/* <path>.keep: the largest copy the slot has had. A candidate that is at least
 * as large as the kept one replaces it. Copies larger than any buffer the title
 * read this slot with are not kept (they could not be loaded whole). */
static u32 s_recv_max;                 /* the largest GetData buffer this run */

static void tus_keep_largest(const char* path, const char* from, long size)
{
    if (size <= 0 || (s_recv_max && (u32)size > s_recv_max))
        return;
    char k[TUS_PATH + 16];
    snprintf(k, sizeof k, "%s.keep", path);
    if (size < tus_size(k))
        return;
    if (tus_copy(from, k) != 0)
        printf("[np-tus] could not keep %s\n", k);
}

/* Before the slot file is replaced (by newSize bytes) or removed (newSize 0):
 * .keep, the .session history (first change this run) and the .bak rotation. */
static void tus_keep_old(const char* path, u32 newSize)
{
    const long cur = tus_size(path);
    if (cur < 0)
        return;
    char a[TUS_PATH + 16], b[TUS_PATH + 16];
    tus_keep_largest(path, path, cur);
    if ((u32)cur > newSize && (u32)cur - newSize > (u32)cur / 10)
        printf("[np-tus] %s: %ld bytes replaced by %u (smaller); the largest copy is kept as %s.keep\n",
               path, cur, newSize, path);
    const u32 h = tus_fnv(path);
    int seen = 0;
    for (int i = 0; i < s_session_n; i++)
        if (s_session[i] == h) seen = 1;
    if (!seen) {                       /* first change this run: .session -> .session1 -> .session2 */
        snprintf(a, sizeof a, "%s.session1", path);
        snprintf(b, sizeof b, "%s.session2", path);
        if (tus_exists(a))
            tus_move(a, b);
        snprintf(b, sizeof b, "%s.session", path);
        if (tus_exists(b))
            tus_move(b, a);
        if (tus_copy(path, b) != 0)
            printf("[np-tus] could not keep %s\n", b);
        if (s_session_n < (int)(sizeof s_session / sizeof s_session[0]))
            s_session[s_session_n++] = h;
    }
    for (int i = 3; i > 1; i--) {
        snprintf(a, sizeof a, "%s.bak%d", path, i - 1);
        snprintf(b, sizeof b, "%s.bak%d", path, i);
        if (tus_exists(a))
            tus_move(a, b);
    }
    snprintf(a, sizeof a, "%s.bak1", path);
    if (tus_copy(path, a) != 0)
        printf("[np-tus] could not keep %s\n", a);
}

/* Replace the slot file: tmp + flush + rename, the old copy kept. The same
 * bytes again: nothing is written (and nothing rotated). */
static int tus_write(const tus_title* t, const char* path, const void* data, u32 size)
{
    const long cur = tus_size(path);
    if (cur >= 0 && (u32)cur == size) {
        u32 n;
        u8* old = tus_read(path, &n);
        const int same = old && n == size && (size == 0 || memcmp(old, data, size) == 0);
        free(old);
        if (same)
            return 0;
    }
    tus_mkdirs(t->dir);
    char tmp[TUS_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (tus_put(tmp, data, size) != 0) {
        printf("[np-tus] write %s failed\n", tmp);
        return -1;
    }
    tus_keep_old(path, size);
    if (tus_move(tmp, path) != 0) {
        printf("[np-tus] rename %s -> %s failed\n", tmp, path);
        remove(tmp);
        return -1;
    }
    tus_keep_largest(path, path, (long)size);   /* the new copy, when it is the largest so far */
    return 0;
}

static void tus_remove(const char* path)
{
    if (!tus_exists(path))
        return;
    tus_keep_old(path, 0);
    remove(path);
}

/* "no data": say where earlier copies of the slot are, if any. */
static void tus_note_copies(const char* path)
{
    static const char* const ext[] = { ".keep", ".bak1", ".session", ".session1" };
    char a[TUS_PATH + 16];
    for (int i = 0; i < 4; i++) {
        snprintf(a, sizeof a, "%s%s", path, ext[i]);
        const long n = tus_size(a);
        if (n > 0) {
            printf("[np-tus] NOTE: %s is missing, but earlier copies exist (largest: %s.keep). To get one back,\n"
                   "[np-tus]   quit the game and copy it over %s.\n", path, path, path);
            return;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Guest structures (built big-endian in a host buffer, then copied out)
 * -----------------------------------------------------------------------*/

static void tus_be32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static void tus_be64(u8* p, u64 v) { tus_be32(p, (u32)(v >> 32)); tus_be32(p + 4, (u32)v); }

static void tus_owner(u8* out, const tus_title* t, u32 npid)
{
    if (npid)
        vm_memcpy_from(out, npid, SCE_NP_TUS_NPID_SIZE);
    else
        memcpy(out, t->npid, SCE_NP_TUS_NPID_SIZE);
}

static void tus_status_out(u32 ea, u32 size, const tus_title* t, u32 npid, int has, u64 tick,
                           u32 data, u32 dataSize, const u8* info, u32 infoLen)
{
    if (!ea || !size)
        return;
    u8 img[SCE_NP_TUS_DATA_STATUS_SIZE];
    memset(img, 0, sizeof img);
    tus_owner(img + 0x00, t, npid);
    tus_be32(img + 0x24, has ? 1u : 0u);
    tus_be64(img + 0x28, has ? tick : 0);
    if (has)
        memcpy(img + 0x30, img + 0x00, SCE_NP_TUS_NPID_SIZE);   /* lastChangedAuthorId: the owner */
    tus_be32(img + 0x54, has ? data : 0);
    tus_be32(img + 0x58, has ? dataSize : 0);
    if (has && info && infoLen)
        memcpy(img + 0x5C, info, infoLen > SCE_NP_TUS_DATA_INFO_SIZE ? SCE_NP_TUS_DATA_INFO_SIZE : infoLen);
    vm_memcpy_to(ea, img, size < sizeof img ? size : sizeof img);
}

static void tus_variable_out(u32 ea, u32 size, const tus_title* t, u32 npid, int has, u64 tick,
                             s64 value, s64 old)
{
    if (!ea || !size)
        return;
    u8 img[SCE_NP_TUS_VARIABLE_SIZE];
    memset(img, 0, sizeof img);
    tus_owner(img + 0x00, t, npid);
    tus_be32(img + 0x24, has ? 1u : 0u);
    tus_be64(img + 0x28, has ? tick : 0);
    if (has)
        memcpy(img + 0x34, img + 0x00, SCE_NP_TUS_NPID_SIZE);
    tus_be64(img + 0x58, has ? (u64)value : 0);
    tus_be64(img + 0x60, (u64)old);
    vm_memcpy_to(ea, img, size < sizeof img ? size : sizeof img);
}

/* ---------------------------------------------------------------------------
 * Contexts
 * -----------------------------------------------------------------------*/

static tus_trans* tus_trans_get(s32 id)
{
    if (id < 1 || id > TUS_MAX_TRANS || !s_trans[id - 1].used)
        return NULL;
    return &s_trans[id - 1];
}

static void tus_trans_free(tus_trans* tr)
{
    free(tr->up_buf);
    memset(tr, 0, sizeof *tr);
}

/* Common start of an operation: the transaction, or an error code in *rc. */
static tus_trans* tus_begin(s32 transId, s32* rc)
{
    if (!s_init) { *rc = (s32)SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED; return NULL; }
    tus_trans* tr = tus_trans_get(transId);
    if (!tr) { *rc = (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ID; return NULL; }
    if (tr->aborted) { *rc = (s32)SCE_NP_COMMUNITY_ERROR_ABORTED; return NULL; }
    *rc = 0;
    return tr;
}

s32 sceNpTusInit(s32 prio)
{
    (void)prio;
    TUS_LOCK();
    tus_resolve_base();
    s32 rc = 0;
    if (s_init) {
        rc = (s32)SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED;
    } else {
        memset(s_title, 0, sizeof s_title);
        for (int i = 0; i < TUS_MAX_TRANS; i++)
            if (s_trans[i].used) tus_trans_free(&s_trans[i]);
        s_init = 1;
    }
    TUS_UNLOCK();
    printf("[np-tus] Init -> 0x%X\n", (u32)rc);
    return rc;
}

s32 sceNpTusTerm(void)
{
    TUS_LOCK();
    s32 rc = 0;
    if (!s_init) {
        rc = (s32)SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
    } else {
        for (int i = 0; i < TUS_MAX_TRANS; i++)
            if (s_trans[i].used) tus_trans_free(&s_trans[i]);
        memset(s_title, 0, sizeof s_title);
        s_init = 0;
    }
    TUS_UNLOCK();
    printf("[np-tus] Term -> 0x%X\n", (u32)rc);
    return rc;
}

s32 sceNpTusCreateTitleCtx(u32 communicationId, u32 passphrase, u32 selfNpId)
{
    (void)passphrase;
    if (!communicationId)
        return (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    TUS_LOCK();
    tus_resolve_base();
    s32 rc;
    if (!s_init) {
        rc = (s32)SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
    } else {
        rc = (s32)SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS;
        for (int i = 0; i < TUS_MAX_TITLE; i++) {
            if (s_title[i].used)
                continue;
            tus_title* t = &s_title[i];
            memset(t, 0, sizeof *t);
            t->used = 1;
            /* SceNpCommunicationId: char data[9], char term, u8 num, char dummy */
            char id[10];
            vm_memcpy_from(id, communicationId, 9);
            id[9] = 0;
            for (char* p = id; *p; p++)
                if (!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) *p = '_';
            snprintf(t->comm, sizeof t->comm, "%s_%02u", id, (unsigned)vm_read8(communicationId + 10));
            snprintf(t->dir, sizeof t->dir, "%s/%s", s_base, t->comm);
            if (selfNpId)
                vm_memcpy_from(t->npid, selfNpId, SCE_NP_TUS_NPID_SIZE);
            rc = i + 1;
            break;
        }
    }
    TUS_UNLOCK();
    printf("[np-tus] CreateTitleCtx(%s) -> 0x%X\n", rc > 0 ? s_title[rc - 1].comm : "?", (u32)rc);
    fflush(stdout);
    return rc;
}

s32 sceNpTusDestroyTitleCtx(s32 titleCtxId)
{
    TUS_LOCK();
    s32 rc = 0;
    if (!s_init)
        rc = (s32)SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
    else if (titleCtxId < 1 || titleCtxId > TUS_MAX_TITLE || !s_title[titleCtxId - 1].used)
        rc = (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ID;
    else {
        for (int i = 0; i < TUS_MAX_TRANS; i++)
            if (s_trans[i].used && s_trans[i].title == titleCtxId - 1) tus_trans_free(&s_trans[i]);
        memset(&s_title[titleCtxId - 1], 0, sizeof s_title[0]);
    }
    TUS_UNLOCK();
    return rc;
}

s32 sceNpTusCreateTransactionCtx(s32 titleCtxId)
{
    TUS_LOCK();
    s32 rc;
    if (!s_init)
        rc = (s32)SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
    else if (titleCtxId < 1 || titleCtxId > TUS_MAX_TITLE || !s_title[titleCtxId - 1].used)
        rc = (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ID;
    else {
        rc = (s32)SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS;
        for (int i = 0; i < TUS_MAX_TRANS; i++) {
            if (s_trans[i].used)
                continue;
            memset(&s_trans[i], 0, sizeof s_trans[i]);
            s_trans[i].used = 1;
            s_trans[i].title = titleCtxId - 1;
            rc = i + 1;
            break;
        }
    }
    TUS_UNLOCK();
    if (rc < 0)
        printf("[np-tus] CreateTransactionCtx(%d) -> 0x%X\n", (int)titleCtxId, (u32)rc);
    return rc;
}

s32 sceNpTusDestroyTransactionCtx(s32 transId)
{
    TUS_LOCK();
    s32 rc = 0;
    tus_trans* tr = s_init ? tus_trans_get(transId) : NULL;
    if (!s_init)
        rc = (s32)SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
    else if (!tr)
        rc = (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ID;
    else
        tus_trans_free(tr);
    TUS_UNLOCK();
    return rc;
}

s32 sceNpTusSetTimeout(s32 ctxId, u32 timeout)
{
    TUS_LOCK();
    s32 rc = 0;
    tus_trans* tr = s_init ? tus_trans_get(ctxId) : NULL;
    if (!s_init)
        rc = (s32)SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
    else if (tr)
        tr->timeout = timeout;
    else if (ctxId < 1 || ctxId > TUS_MAX_TITLE || !s_title[ctxId - 1].used)
        rc = (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ID;
    TUS_UNLOCK();
    return rc;
}

s32 sceNpTusAbortTransaction(s32 transId)
{
    TUS_LOCK();
    s32 rc = 0;
    tus_trans* tr = s_init ? tus_trans_get(transId) : NULL;
    if (!s_init)
        rc = (s32)SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
    else if (!tr)
        rc = (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ID;
    else
        tr->aborted = 1;               /* nothing is ever in flight: later operations fail */
    TUS_UNLOCK();
    return rc;
}

/* Every operation has already finished: answer "done" with its result. */
static s32 tus_result(s32 transId, u32 result)
{
    TUS_LOCK();
    s32 rc = 0, res = 0;
    tus_trans* tr = s_init ? tus_trans_get(transId) : NULL;
    if (!s_init)
        rc = (s32)SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
    else if (!tr)
        rc = (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ID;
    else
        res = tr->result;
    TUS_UNLOCK();
    if (rc == 0 && result)
        vm_write32(result, (u32)res);
    return rc;
}

s32 sceNpTusWaitAsync(s32 transId, u32 result) { return tus_result(transId, result); }
s32 sceNpTusPollAsync(s32 transId, u32 result) { return tus_result(transId, result); }

/* ---------------------------------------------------------------------------
 * Data
 * -----------------------------------------------------------------------*/

static s32 tus_get_data(tus_trans* tr, u32 npid, s32 slot, u32 st, u32 stSize, u32 data, u32 recv)
{
    const tus_title* t = &s_title[tr->title];
    if (slot < 0)
        return (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    char p[TUS_PATH], pi[TUS_PATH];
    tus_path(p, sizeof p, t, "slot", slot, ".dat");
    tus_path(pi, sizeof pi, t, "slot", slot, ".info");
    u32 n = 0, ni = 0;
    if (recv > s_recv_max)
        s_recv_max = recv;
    if (data && recv && (u64)data + recv > 0x100000000ull)
        return (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    u8* b = s_files ? tus_read(p, &n) : NULL;
    const int has = b != NULL;
    if (!has && s_files && tus_size(p) >= 0) {
        /* The file is there but could not be read (locked by another program,
         * no permission, larger than TUS_MAX_FILE): a real error, so the title
         * reports a profile error and does not load its default profile, which
         * it would then save over this file. Only a missing file is "no data". */
        printf("[np-tus] GetData %s slot %d: %s exists but cannot be read: error, the file is left as it is\n",
               t->comm, (int)slot, p);
        fflush(stdout);
        return (s32)SCE_NP_COMMUNITY_ERROR_OUT_OF_MEMORY;
    }
    u8* info = has ? tus_read(pi, &ni) : NULL;
    u32 copy = has ? (n < recv ? n : recv) : 0;
    const int cut = has && data && recv && n >= recv;
    if (cut)
        copy = recv - 1;               /* room for the NUL a title's text parser stops at */
    if (has && data && copy)
        vm_memcpy_to(data, b, copy);
    if (has && data && recv && copy < recv)
        vm_write8(data + copy, 0);
    tus_status_out(st, stSize, t, npid, has, has ? tus_file_tick(p) : 0, data, n, info, ni);
    free(b);
    free(info);
    printf("[np-tus] GetData %s slot %d: %s\n", t->comm, (int)slot, has ? "" : "no data");
    if (has)
        printf("[np-tus]   %u bytes, %u copied (buffer %u)\n", n, copy, recv);
    if (cut)
        printf("[np-tus] WARNING: %s is %u bytes, larger than the title's buffer (%u): cut to %u bytes + NUL.\n"
               "[np-tus]   The title will likely treat it as damaged; the file is not changed by this load.\n",
               p, n, recv, copy);
    if (!has && s_files)
        tus_note_copies(p);
    fflush(stdout);
    return has ? (s32)copy : (s32)SCE_NP_COMMUNITY_SERVER_ERROR_USER_STORAGE_DATA_NOT_FOUND;
}

static s32 tus_set_data(tus_trans* tr, s32 slot, u32 total, u32 send, u32 data, u32 info, u32 infoSize)
{
    const tus_title* t = &s_title[tr->title];
    if (slot < 0 || send > total || (send && !data))
        return (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    if (total > TUS_MAX_FILE)
        return (s32)SCE_NP_COMMUNITY_ERROR_BODY_TOO_LARGE;
    if ((send && !data) || (u64)data + send > 0x100000000ull)
        return (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    if (send == total && !tr->up_buf && total && !data)
        return (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    const u8* whole = NULL;
    if (send < total || tr->up_buf) {
        /* in pieces: collect them in the transaction until totalSize is there */
        if (!tr->up_buf || tr->up_slot != slot || tr->up_total != total) {
            free(tr->up_buf);
            tr->up_buf = (u8*)malloc(total ? total : 1);
            if (!tr->up_buf)
                return (s32)SCE_NP_COMMUNITY_ERROR_OUT_OF_MEMORY;
            tr->up_total = total;
            tr->up_have = 0;
            tr->up_slot = slot;
        }
        if (tr->up_have + send > total)
            return (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
        if (send)
            vm_memcpy_from(tr->up_buf + tr->up_have, data, send);
        tr->up_have += send;
        if (tr->up_have < total)
            return 0;
        whole = tr->up_buf;
    } else {
        whole = vm_base + data;        /* guest memory is one contiguous host block */
    }
    s32 rc = 0;
    int refused = 0;
    if (s_files) {
        char p[TUS_PATH], pi[TUS_PATH];
        tus_path(p, sizeof p, t, "slot", slot, ".dat");
        tus_path(pi, sizeof pi, t, "slot", slot, ".info");
        if (total == 0 && tus_size(p) > 0) {
            /* an empty profile over a stored one is a failed save, not a wish: keep the slot */
            refused = 1;
            printf("[np-tus] SetData %s slot %d: 0 bytes over %ld stored: refused, the slot is kept\n",
                   t->comm, (int)slot, tus_size(p));
        } else if (tus_write(t, p, whole, total) != 0)
            rc = (s32)SCE_NP_COMMUNITY_ERROR_OUT_OF_MEMORY;
        else {
            u32 isz = (info && infoSize >= 8) ? vm_read32(info) : 0;
            if (isz > SCE_NP_TUS_DATA_INFO_MAX_SIZE) isz = SCE_NP_TUS_DATA_INFO_MAX_SIZE;
            if (isz && 8 + isz <= infoSize)
                tus_write(t, pi, vm_base + info, 8 + isz);
            else
                tus_remove(pi);
        }
    }
    if (!refused)
        printf("[np-tus] SetData %s slot %d: %u bytes%s%s\n", t->comm, (int)slot, total,
               s_files ? "" : " (dropped: PS3_NP_TUS_FILES=0)", rc ? " FAILED" : "");
    fflush(stdout);
    if (!refused && !rc && total)
        mpnet_tus_saved(t->comm, (int)slot, whole, total);   /* online only: profile stats -> leaderboards */
    if (tr->up_buf) {
        free(tr->up_buf);
        tr->up_buf = NULL;
        tr->up_total = tr->up_have = 0;
    }
    return rc;
}

static s32 tus_data_status(tus_trans* tr, u32 npid, u32 slots, u32 st, u32 stSize, s32 num)
{
    const tus_title* t = &s_title[tr->title];
    if (!slots || num < 0 || num > SCE_NP_TUS_MAX_SLOT_NUM_PER_TRANS)
        return (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    if (st && stSize < (u32)num * SCE_NP_TUS_DATA_STATUS_SIZE)
        return (s32)SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT;
    for (s32 i = 0; i < num; i++) {
        const s32 slot = (s32)vm_read32(slots + 4u * (u32)i);
        char p[TUS_PATH], pi[TUS_PATH];
        tus_path(p, sizeof p, t, "slot", slot, ".dat");
        tus_path(pi, sizeof pi, t, "slot", slot, ".info");
        struct stat sb;
        const int has = s_files && slot >= 0 && stat(p, &sb) == 0;
        u32 ni = 0;
        u8* info = has ? tus_read(pi, &ni) : NULL;
        tus_status_out(st + (u32)i * SCE_NP_TUS_DATA_STATUS_SIZE, SCE_NP_TUS_DATA_STATUS_SIZE, t, npid,
                       has, has ? tus_tick_of(sb.st_mtime) : 0, 0, has ? (u32)sb.st_size : 0, info, ni);
        free(info);
    }
    return 0;
}

static s32 tus_delete_data(tus_trans* tr, u32 slots, s32 num)
{
    const tus_title* t = &s_title[tr->title];
    if (!slots || num < 0 || num > SCE_NP_TUS_MAX_SLOT_NUM_PER_TRANS)
        return (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    for (s32 i = 0; i < num && s_files; i++) {
        const s32 slot = (s32)vm_read32(slots + 4u * (u32)i);
        if (slot < 0)
            continue;
        char p[TUS_PATH];
        tus_path(p, sizeof p, t, "slot", slot, ".dat");
        tus_remove(p);
        tus_path(p, sizeof p, t, "slot", slot, ".info");
        tus_remove(p);
        printf("[np-tus] DeleteData %s slot %d\n", t->comm, (int)slot);
    }
    return 0;
}

/* sync: the result; async: 0 once started, the result goes to PollAsync */
#define TUS_RUN(ASYNC, CALL)                                         \
    TUS_LOCK();                                                      \
    s32 rc_;                                                         \
    tus_trans* tr = tus_begin(transId, &rc_);                        \
    if (tr) { rc_ = (CALL); tr->result = rc_; if (ASYNC) rc_ = 0; }  \
    TUS_UNLOCK();                                                    \
    return rc_

s32 sceNpTusSetData(s32 transId, u32 targetNpId, SceNpTusSlotId slotId, u32 totalSize, u32 sendSize,
                    u32 data, u32 info, u32 infoStructSize)
{
    (void)targetNpId;
    TUS_RUN(0, tus_set_data(tr, slotId, totalSize, sendSize, data, info, infoStructSize));
}

s32 sceNpTusSetDataAsync(s32 transId, u32 targetNpId, SceNpTusSlotId slotId, u32 totalSize, u32 sendSize,
                         u32 data, u32 info, u32 infoStructSize)
{
    (void)targetNpId;
    TUS_RUN(1, tus_set_data(tr, slotId, totalSize, sendSize, data, info, infoStructSize));
}

s32 sceNpTusGetData(s32 transId, u32 targetNpId, SceNpTusSlotId slotId, u32 dataStatus, u32 dataStatusSize,
                    u32 data, u32 recvSize, u32 option)
{
    (void)option;
    TUS_RUN(0, tus_get_data(tr, targetNpId, slotId, dataStatus, dataStatusSize, data, recvSize));
}

s32 sceNpTusGetDataAsync(s32 transId, u32 targetNpId, SceNpTusSlotId slotId, u32 dataStatus, u32 dataStatusSize,
                         u32 data, u32 recvSize, u32 option)
{
    (void)option;
    TUS_RUN(1, tus_get_data(tr, targetNpId, slotId, dataStatus, dataStatusSize, data, recvSize));
}

s32 sceNpTusGetMultiSlotDataStatus(s32 transId, u32 targetNpId, u32 slotIdArray, u32 statusArray,
                                   u32 statusArraySize, s32 arrayNum, u32 option)
{
    (void)option;
    TUS_RUN(0, tus_data_status(tr, targetNpId, slotIdArray, statusArray, statusArraySize, arrayNum));
}

s32 sceNpTusGetMultiSlotDataStatusAsync(s32 transId, u32 targetNpId, u32 slotIdArray, u32 statusArray,
                                        u32 statusArraySize, s32 arrayNum, u32 option)
{
    (void)option;
    TUS_RUN(1, tus_data_status(tr, targetNpId, slotIdArray, statusArray, statusArraySize, arrayNum));
}

s32 sceNpTusDeleteMultiSlotData(s32 transId, u32 targetNpId, u32 slotIdArray, s32 arrayNum, u32 option)
{
    (void)targetNpId; (void)option;
    TUS_RUN(0, tus_delete_data(tr, slotIdArray, arrayNum));
}

s32 sceNpTusDeleteMultiSlotDataAsync(s32 transId, u32 targetNpId, u32 slotIdArray, s32 arrayNum, u32 option)
{
    (void)targetNpId; (void)option;
    TUS_RUN(1, tus_delete_data(tr, slotIdArray, arrayNum));
}

/* ---------------------------------------------------------------------------
 * Variables
 * -----------------------------------------------------------------------*/

static int tus_var_read(const tus_title* t, s32 slot, s64* value, u64* tick)
{
    *value = 0; *tick = 0;
    if (!s_files || slot < 0)
        return 0;
    char p[TUS_PATH];
    tus_path(p, sizeof p, t, "var", slot, ".txt");
    u32 n;
    u8* b = tus_read(p, &n);
    if (!b)
        return 0;
    b[n] = 0;                          /* tus_read leaves room for it */
    long long v = 0; unsigned long long k = 0;
    const int ok = sscanf((const char*)b, "%lld %llu", &v, &k) >= 1;
    free(b);
    if (!ok)
        return 0;
    *value = (s64)v; *tick = (u64)k;
    return 1;
}

static int tus_var_write(const tus_title* t, s32 slot, s64 value, u64* tick)
{
    *tick = tus_tick_of(time(NULL));
    if (!s_files)
        return 0;
    char p[TUS_PATH], txt[64];
    tus_path(p, sizeof p, t, "var", slot, ".txt");
    const int n = snprintf(txt, sizeof txt, "%lld %llu\n", (long long)value, (unsigned long long)*tick);
    return tus_write(t, p, txt, (u32)n);
}

static s32 tus_set_vars(tus_trans* tr, u32 slots, u32 values, s32 num)
{
    const tus_title* t = &s_title[tr->title];
    if (!slots || !values || num < 0 || num > SCE_NP_TUS_MAX_SLOT_NUM_PER_TRANS)
        return (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    for (s32 i = 0; i < num; i++) {
        const s32 slot = (s32)vm_read32(slots + 4u * (u32)i);
        u64 tick;
        if (slot >= 0 && tus_var_write(t, slot, (s64)vm_read64(values + 8u * (u32)i), &tick) != 0)
            return (s32)SCE_NP_COMMUNITY_ERROR_OUT_OF_MEMORY;
    }
    return 0;
}

static s32 tus_get_vars(tus_trans* tr, u32 npid, u32 slots, u32 out, u32 outSize, s32 num)
{
    const tus_title* t = &s_title[tr->title];
    if (!slots || !out || num < 0 || num > SCE_NP_TUS_MAX_SLOT_NUM_PER_TRANS)
        return (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    if (outSize < (u32)num * SCE_NP_TUS_VARIABLE_SIZE)
        return (s32)SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT;
    for (s32 i = 0; i < num; i++) {
        s64 v; u64 tick;
        const int has = tus_var_read(t, (s32)vm_read32(slots + 4u * (u32)i), &v, &tick);
        tus_variable_out(out + (u32)i * SCE_NP_TUS_VARIABLE_SIZE, SCE_NP_TUS_VARIABLE_SIZE, t, npid, has, tick, v, 0);
    }
    return 0;
}

static s32 tus_add_var(tus_trans* tr, u32 npid, s32 slot, s64 add, u32 out, u32 outSize)
{
    const tus_title* t = &s_title[tr->title];
    if (slot < 0)
        return (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    s64 old; u64 tick;
    tus_var_read(t, slot, &old, &tick);
    const s64 now = old + add;
    if (tus_var_write(t, slot, now, &tick) != 0)
        return (s32)SCE_NP_COMMUNITY_ERROR_OUT_OF_MEMORY;
    tus_variable_out(out, outSize, t, npid, 1, tick, now, old);
    return 0;
}

/* ESTIMATE of the firmware rule: the stored value is replaced when "variable
 * <opeType> stored" holds (GREATER_THAN keeps a high score), or when the slot
 * is empty; the result holds the value after, oldVariable the one before. */
static s32 tus_try_var(tus_trans* tr, u32 npid, s32 slot, s32 ope, s64 value, u32 out, u32 outSize)
{
    const tus_title* t = &s_title[tr->title];
    if (slot < 0 || ope < SCE_NP_TUS_OPETYPE_EQUAL || ope > SCE_NP_TUS_OPETYPE_LESS_OR_EQUAL)
        return (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    s64 old; u64 tick;
    const int has = tus_var_read(t, slot, &old, &tick);
    int set = !has;
    switch (ope) {
    case SCE_NP_TUS_OPETYPE_EQUAL:            set |= value == old; break;
    case SCE_NP_TUS_OPETYPE_NOT_EQUAL:        set |= value != old; break;
    case SCE_NP_TUS_OPETYPE_GREATER_THAN:     set |= value >  old; break;
    case SCE_NP_TUS_OPETYPE_GREATER_OR_EQUAL: set |= value >= old; break;
    case SCE_NP_TUS_OPETYPE_LESS_THAN:        set |= value <  old; break;
    default:                                  set |= value <= old; break;
    }
    s64 now = old;
    if (set) {
        now = value;
        if (tus_var_write(t, slot, now, &tick) != 0)
            return (s32)SCE_NP_COMMUNITY_ERROR_OUT_OF_MEMORY;
    }
    tus_variable_out(out, outSize, t, npid, has || set, tick, now, old);
    return 0;
}

static s32 tus_delete_vars(tus_trans* tr, u32 slots, s32 num)
{
    const tus_title* t = &s_title[tr->title];
    if (!slots || num < 0 || num > SCE_NP_TUS_MAX_SLOT_NUM_PER_TRANS)
        return (s32)SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    for (s32 i = 0; i < num && s_files; i++) {
        const s32 slot = (s32)vm_read32(slots + 4u * (u32)i);
        if (slot < 0)
            continue;
        char p[TUS_PATH];
        tus_path(p, sizeof p, t, "var", slot, ".txt");
        tus_remove(p);
    }
    return 0;
}

s32 sceNpTusSetMultiSlotVariable(s32 transId, u32 targetNpId, u32 slotIdArray, u32 variableArray,
                                 s32 arrayNum, u32 option)
{
    (void)targetNpId; (void)option;
    TUS_RUN(0, tus_set_vars(tr, slotIdArray, variableArray, arrayNum));
}

s32 sceNpTusSetMultiSlotVariableAsync(s32 transId, u32 targetNpId, u32 slotIdArray, u32 variableArray,
                                      s32 arrayNum, u32 option)
{
    (void)targetNpId; (void)option;
    TUS_RUN(1, tus_set_vars(tr, slotIdArray, variableArray, arrayNum));
}

s32 sceNpTusGetMultiSlotVariable(s32 transId, u32 targetNpId, u32 slotIdArray, u32 variableArray,
                                 u32 variableArraySize, s32 arrayNum, u32 option)
{
    (void)option;
    TUS_RUN(0, tus_get_vars(tr, targetNpId, slotIdArray, variableArray, variableArraySize, arrayNum));
}

s32 sceNpTusGetMultiSlotVariableAsync(s32 transId, u32 targetNpId, u32 slotIdArray, u32 variableArray,
                                      u32 variableArraySize, s32 arrayNum, u32 option)
{
    (void)option;
    TUS_RUN(1, tus_get_vars(tr, targetNpId, slotIdArray, variableArray, variableArraySize, arrayNum));
}

s32 sceNpTusAddAndGetVariable(s32 transId, u32 targetNpId, SceNpTusSlotId slotId, s64 inVariable,
                              u32 outVariable, u32 outVariableSize, u32 option)
{
    (void)option;
    TUS_RUN(0, tus_add_var(tr, targetNpId, slotId, inVariable, outVariable, outVariableSize));
}

s32 sceNpTusAddAndGetVariableAsync(s32 transId, u32 targetNpId, SceNpTusSlotId slotId, s64 inVariable,
                                   u32 outVariable, u32 outVariableSize, u32 option)
{
    (void)option;
    TUS_RUN(1, tus_add_var(tr, targetNpId, slotId, inVariable, outVariable, outVariableSize));
}

s32 sceNpTusTryAndSetVariable(s32 transId, u32 targetNpId, SceNpTusSlotId slotId, s32 opeType,
                              s64 variable, u32 resultVariable, u32 resultVariableSize, u32 option)
{
    (void)option;
    TUS_RUN(0, tus_try_var(tr, targetNpId, slotId, opeType, variable, resultVariable, resultVariableSize));
}

s32 sceNpTusTryAndSetVariableAsync(s32 transId, u32 targetNpId, SceNpTusSlotId slotId, s32 opeType,
                                   s64 variable, u32 resultVariable, u32 resultVariableSize, u32 option)
{
    (void)option;
    TUS_RUN(1, tus_try_var(tr, targetNpId, slotId, opeType, variable, resultVariable, resultVariableSize));
}

s32 sceNpTusDeleteMultiSlotVariable(s32 transId, u32 targetNpId, u32 slotIdArray, s32 arrayNum, u32 option)
{
    (void)targetNpId; (void)option;
    TUS_RUN(0, tus_delete_vars(tr, slotIdArray, arrayNum));
}

s32 sceNpTusDeleteMultiSlotVariableAsync(s32 transId, u32 targetNpId, u32 slotIdArray, s32 arrayNum, u32 option)
{
    (void)targetNpId; (void)option;
    TUS_RUN(1, tus_delete_vars(tr, slotIdArray, arrayNum));
}
