/*
 * mpnet: runtime side of the GoW Ascension community-server prototype (see mpnet.h).
 * Private kit only (work\ps3kit-mpnet); nothing here runs unless PS3_MPNET=1.
 */
#ifdef _WIN32
    #define _CRT_RAND_S
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef SOCKET mp_sock;
    #define MP_BAD INVALID_SOCKET
    #define mp_close closesocket
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/select.h>
    #include <sys/time.h>
    typedef int mp_sock;
    #define MP_BAD (-1)
    #define mp_close close
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include "mpnet.h"
#include "np_psnr.h"
#include "../../runtime/ppu/ppu_memory.h"

static int s_on = -1;

int mpnet_on(void)
{
    if (s_on < 0) {
        const char* e = getenv("PS3_MPNET"); s_on = (e && e[0] == '1') ? 1 : 0;
        /* Licence rule (host\mpnet\LICENCE.md): the offline patch passes the lobby gate (Online Pass included)
         * with one word; it must never be combined with the online path. */
        const char* p = getenv("PS3_PATCHES");
        if (s_on && p && strstr(p, "Offline Multiplayer")) {
            s_on = 0;
            printf("[mpnet] OFF: PS3_PATCHES names the Offline Multiplayer patch; online mode needs the game's own gate\n");
            fflush(stdout);
        }
        /* Fair play (2026-11-13): online, only visual/performance patches may be on. Every other patch (the
         * Cheat: ones, the offline ones, anything added later) turns online mode off. An allowlist, so a new
         * patch is refused until someone decides it is fair. */
        if (s_on && p && *p) {
            static const char* fair[] = { "Unlock FPS", "Disable Depth of Field", "Disable Motion Blur", "Disable MLAA",
                                          "Disable SPU MLAA - God of War: Ascension (01)" };
            char list[2048]; snprintf(list, sizeof list, "%s", p);
            for (char* it = strtok(list, "|"); it && s_on; it = strtok(NULL, "|")) {
                while (*it == ' ') it++;
                int ok = 0;
                for (size_t i = 0; i < sizeof fair / sizeof fair[0]; i++) if (!strcmp(it, fair[i])) ok = 1;
                if (!ok && *it) {
                    s_on = 0;
                    printf("[mpnet] OFF: the patch \"%s\" is not allowed online (only visual patches are)\n", it);
                    fflush(stdout);
                }
            }
        }
    }
    return s_on;
}

void mpnet_log(const char* fmt, ...)
{
    char b[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("[mpnet] %s\n", b); fflush(stdout);
}

static int env1(const char* n) { const char* e = getenv(n); return e && e[0] == '1'; }

int mpnet_flag(int which)
{
    static int said[8];
    int v = 0;
    if (mpnet_on()) {
        switch (which) {
            case 1: v = !env1("PS3_MPNET_SSL"); break;            /* SSL wrapper off */
            case 2: v = 1; break;                                 /* TSS plaintext */
        }
    }
    if (which > 0 && which < 8 && !said[which]) {
        said[which] = 1;
        static const char* names[] = { "", "SSL off (0x324798)", "TSS plaintext (0x3319BC)" };
        if (mpnet_on()) mpnet_log("source patch %d %s: %s", which, which < 3 ? names[which] : "?", v ? "ON" : "off");
    }
    return v;
}

const char* mpnet_server_host(void)
{
    static char host[128];
    if (!host[0]) {
        const char* e = getenv("PS3_MPNET_SERVER");
        snprintf(host, sizeof host, "%s", (e && *e) ? e : "127.0.0.1");
        char* c = strchr(host, ':'); if (c) *c = 0;
    }
    return host;
}

static int http_port(void)
{
    const char* e = getenv("PS3_MPNET_HTTP_PORT");
    return (e && atoi(e) > 0) ? atoi(e) : 36101;
}

static void net_up(void)
{
#ifdef _WIN32
    static int done; if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = 1; }
#endif
}

static uint32_t s_server_ip;   /* network order, 0 = unresolved */

static uint32_t server_ip(void)
{
    if (!s_server_ip) {
        net_up();
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(mpnet_server_host(), NULL, &hints, &res) == 0 && res) {
            s_server_ip = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
            freeaddrinfo(res);
        }
    }
    return s_server_ip;
}

/* Peers the server named (mpnet_peer_lookup), network order. Two PCs (2026-11-13): the game may also talk to the
 * players our server matched it with; nobody else. */
static uint32_t s_peers[16];
static int s_npeers;

static int ip_private(uint32_t ip_be)
{
    const uint8_t* b = (const uint8_t*)&ip_be;
    return b[0] == 10 || (b[0] == 172 && (b[1] & 0xF0) == 16) || (b[0] == 192 && b[1] == 168);
}

int mpnet_ip_allowed(uint32_t ip_be)
{
    const uint8_t* b = (const uint8_t*)&ip_be;
    if (b[0] == 127) return 1;                       /* loopback */
    if (ip_be == 0) return 1;                        /* INADDR_ANY (bind) */
    uint32_t s = server_ip();
    if (s && ip_be == s) return 1;
    for (int i = 0; i < s_npeers; i++) if (s_peers[i] == ip_be) return 1;
    return 0;
}

/* The P2P socket stays pinned to 127.0.0.1 while the server itself is on loopback (one-PC tests). */
int mpnet_server_is_loopback(void)
{
    uint32_t s = server_ip();
    return !s || ((const uint8_t*)&s)[0] == 127;
}

int mpnet_name_allowed(const char* name)
{
    if (!name || !*name) return 0;
    if (!strcmp(name, "localhost")) return 1;
    if (!strcmp(name, mpnet_server_host())) return 1;
    int dots = 0; const char* p = name;
    for (; *p; p++) { if (*p == '.') dots++; else if (*p < '0' || *p > '9') break; }
    if (!*p && dots == 3) { uint32_t a = inet_addr(name); return mpnet_ip_allowed(a); }
    return 0;
}

static uint32_t s_local_ip;          /* network order; set by mpnet_http_get */
static char     s_udp_token[40];     /* from /hello: "ok <token> <udp port>" */
static unsigned s_udp_port;

#ifdef _WIN32
/* HTTPS (2026-11-14): PS3_MPNET_HTTPS=<name> (the launcher sets it for servers it knows have a certificate, e.g.
 * <ip-with-dashes>.sslip.io) sends every request of mpnet_http_get over HTTPS to that name, through Windows' own WinHTTP
 * (system certificate check; no fallback to plain HTTP). The game's own traffic (SAM, P2P) is unchanged. */
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

static void local_ip_toward_server(uint32_t ip)
{   /* this PC's address toward the server, for /hello's lan= (a UDP "connect" sends nothing) */
    mp_sock u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u == MP_BAD) return;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(36101); a.sin_addr.s_addr = ip;
    if (connect(u, (struct sockaddr*)&a, sizeof a) == 0) {
        struct sockaddr_in me; int ml = sizeof me;
        if (getsockname(u, (struct sockaddr*)&me, &ml) == 0) s_local_ip = me.sin_addr.s_addr;
    }
    mp_close(u);
}

static int https_get(const char* host, const char* path, unsigned char* out, int cap, int* status)
{
    wchar_t whost[256], wpath[700];
    if (!MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, 256) || !MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 700))
        return -1;
    int len = -1;
    HINTERNET h = WinHttpOpen(L"ps3kit-mpnet", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = h ? WinHttpConnect(h, whost, INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", wpath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE) : NULL;
    if (h) WinHttpSetTimeouts(h, 3000, 3000, 4000, 4000);
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, NULL)) {
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status) *status = (int)code;
        len = 0;
        for (;;) {
            DWORD got = 0;
            if (len >= cap || !WinHttpReadData(r, out + len, (DWORD)(cap - len), &got) || got == 0) break;
            len += (int)got;
        }
    } else {
        mpnet_log("https: request to %s failed (error %lu)", host, (unsigned long)GetLastError());
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (h) WinHttpCloseHandle(h);
    mpnet_log("https GET %s -> %d, %d bytes", path, status ? *status : 0, len);
    return len;
}
#endif

int mpnet_http_get(const char* path, unsigned char* out, int cap, int* status)
{
    if (status) *status = 0;
    uint32_t ip = server_ip();
    if (!ip) { mpnet_log("http: cannot resolve server %s", mpnet_server_host()); return -1; }
    net_up();
#ifdef _WIN32
    {   const char* hs = getenv("PS3_MPNET_HTTPS");
        if (hs && *hs) {
            if (!s_local_ip) local_ip_toward_server(ip);
            return https_get(hs, path, out, cap, status);
        }
    }
#endif
    mp_sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == MP_BAD) return -1;
#ifdef _WIN32
    DWORD tmo = 4000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof tmo);
#else
    struct timeval tv = { 4, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)http_port()); a.sin_addr.s_addr = ip;
    /* 2026-11-13 (laptop test): a server that does not answer froze the game ~21 s per attempt (the OS's own connect
     * retries) -> black screen. Connect without blocking and give up after 3 s. */
    int connected = 0;
#ifdef _WIN32
    { u_long nb = 1; ioctlsocket(s, FIONBIO, &nb); }
#else
    { int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl | O_NONBLOCK); }
#endif
    if (connect(s, (struct sockaddr*)&a, sizeof a) == 0) connected = 1;
    else {
        fd_set wf, ef; FD_ZERO(&wf); FD_ZERO(&ef); FD_SET(s, &wf); FD_SET(s, &ef);
        struct timeval ct = { 3, 0 };
        if (select((int)s + 1, NULL, &wf, &ef, &ct) > 0 && FD_ISSET(s, &wf) && !FD_ISSET(s, &ef)) {
            int err = 0; socklen_t el = sizeof err;
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
            connected = (err == 0);
        }
    }
#ifdef _WIN32
    { u_long nb = 0; ioctlsocket(s, FIONBIO, &nb); }
#else
    { int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl & ~O_NONBLOCK); }
#endif
    if (!connected) {
        mpnet_log("http: connect %s:%d failed", mpnet_server_host(), http_port());
        mp_close(s); return -1;
    }
    {   /* internet play (2026-11-13): this PC's home-network address toward the server, sent in /hello (lan=) */
        struct sockaddr_in me; socklen_t ml = sizeof me;
        if (getsockname(s, (struct sockaddr*)&me, &ml) == 0) s_local_ip = me.sin_addr.s_addr;
    }
    char req[600];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ps3kit-mpnet\r\n\r\n",
                     path, mpnet_server_host());
    if (send(s, req, n, 0) != n) { mp_close(s); return -1; }
    int bufcap = cap + 4096, got = 0;
    char* buf = (char*)malloc((size_t)bufcap);
    if (!buf) { mp_close(s); return -1; }
    for (;;) {
        int r = recv(s, buf + got, bufcap - got, 0);
        if (r <= 0) break;
        got += r;
        if (got >= bufcap) break;
    }
    mp_close(s);
    int body = -1;
    for (int i = 0; i + 3 < got; i++)
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') { body = i + 4; break; }
    int st = 0;
    if (got > 12 && !strncmp(buf, "HTTP/1.", 7)) st = atoi(buf + 9);
    if (status) *status = st;
    int len = -1;
    if (body >= 0) {
        len = got - body;
        if (len > cap) len = cap;
        memcpy(out, buf + body, (size_t)len);
    }
    free(buf);
    mpnet_log("http GET %s -> %d, %d bytes", path, st, len);
    return len;
}

/* ---- ticket ------------------------------------------------------------------------------------------- */
static unsigned char s_ticket[512];
static int s_ticket_len;
static uint64_t s_acct;
static int s_signed;      /* 1 once the server signed our ticket (leaderboards, 2026-11-14) */

static uint64_t fnv1a64(const char* s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 0x100000001b3ULL; }
    return h;
}

/* Stage 0 (2026-11-10): the ticket is made HERE, never fetched (the client only checks that the SAM server
 * echoes the account id it sent). Layout (ours, unsigned, 64 bytes): "MPN1", u32 version 1 (BE), u64 account (BE),
 * char online_id[32], zero pad. Account = FNV-1a-64(online id) with the top bit clear (never 0). A later stage
 * moves issuing to the server (authenticated HELLO, signed, short expiry: review finding "tickets"). */
/* The player's secret token for the community server (2026-11-13 accounts): 32 hex chars made once and kept in
 * PS3_MPNET_TOKEN_FILE, else <PS3_HDD0_ROOT>\..\mpnet-token.txt, else mpnet-token.txt. The server stores only a hash;
 * the first sign-in with a free name registers it to this token. */
const char* mpnet_token(void)
{
    static char tok[65];
    if (tok[0]) return tok;
    char path[600];
    const char* e = getenv("PS3_MPNET_TOKEN_FILE");
    const char* h = getenv("PS3_HDD0_ROOT");
    if (e && *e) snprintf(path, sizeof path, "%s", e);
    else if (h && *h) snprintf(path, sizeof path, "%s\\..\\mpnet-token.txt", h);
    else snprintf(path, sizeof path, "mpnet-token.txt");
    FILE* f = fopen(path, "rb");
    if (f) {
        size_t n = fread(tok, 1, 64, f); fclose(f);
        tok[n] = 0;
        while (n && (unsigned char)tok[n - 1] <= ' ') tok[--n] = 0;   /* trailing newline / spaces */
        int ok = n >= 32;
        for (size_t i = 0; i < n; i++) if (!strchr("0123456789abcdefABCDEF", tok[i])) ok = 0;
        if (ok) return tok;
        mpnet_log("token file %s is not valid: making a new token", path);
    }
    unsigned char r[16];
#ifdef _WIN32
    for (int i = 0; i < 16; i += 4) { unsigned int v = 0; rand_s(&v); memcpy(r + i, &v, 4); }
#else
    FILE* u = fopen("/dev/urandom", "rb"); if (u) { fread(r, 1, 16, u); fclose(u); }
#endif
    for (int i = 0; i < 16; i++) snprintf(tok + 2 * i, 3, "%02x", r[i]);
    f = fopen(path, "wb");
    if (f) { fputs(tok, f); fclose(f); mpnet_log("new player token written to %s", path); }
    else mpnet_log("cannot write the token file %s (the name stays bound to this session's token)", path);
    return tok;
}

/* The account key the server knows us by (2026-11-13 challenge sign-in): K = HMAC-SHA256(token, "gowa-acct:" + name in
 * lower case), as 64 hex chars. The token itself never leaves this PC. */
static void account_key(const char* name, uint8_t k[32], char khex[65])
{
    char msg[64]; int n = snprintf(msg, sizeof msg, "gowa-acct:%s", name);
    for (int i = 10; i < n; i++) if (msg[i] >= 'A' && msg[i] <= 'Z') msg[i] = (char)(msg[i] + 32);
    const char* t = mpnet_token();
    mpnet_hmac_sha256(t, strlen(t), msg, (size_t)n, k);
    mpnet_hex(k, 32, khex);
}

/* "&nonce=..&proof=.." for a fresh server challenge, or "" if the server gives none. */
static void challenge_args(const char* name, char* out, size_t cap)
{
    unsigned char body[80]; int st = 0;
    out[0] = 0;
    int n = mpnet_http_get("/nonce", body, (int)sizeof body - 1, &st);
    if (st != 200 || n < 16 || n > 64) return;
    body[n] = 0;
    uint8_t k[32], pr[32]; char khex[65], phex[65], msg[120];
    account_key(name, k, khex);
    int m = snprintf(msg, sizeof msg, "%s:%s", (char*)body, name);
    for (int i = n + 1; i < m; i++) if (msg[i] >= 'A' && msg[i] <= 'Z') msg[i] = (char)(msg[i] + 32);
    mpnet_hmac_sha256(k, 32, msg, (size_t)m, pr);
    mpnet_hex(pr, 32, phex);
    snprintf(out, cap, "&nonce=%s&proof=%s", (char*)body, phex);
}

int mpnet_ticket(const unsigned char** data, uint64_t* acct)
{
    if (!s_ticket_len) {
        const char* id = np_psnr_online_id();
        /* a ticket signed by the server: challenge sign-in; a free name is registered once with K (reg=);
         * servers with accounts off answer 404 and the old local ticket is used */
        char path[400], ch[200]; int st = 0, n;
        challenge_args(id, ch, sizeof ch);
        snprintf(path, sizeof path, "/ticket?id=%s%s", id, ch[0] ? ch : "&nonce=x&proof=x");
        n = mpnet_http_get(path, s_ticket, (int)sizeof s_ticket, &st);
        if (st == 403 && n > 0 && n < 40 && !memcmp(s_ticket, "unknown name", 12)) {
            uint8_t k[32]; char khex[65];
            account_key(id, k, khex);
            snprintf(path, sizeof path, "/ticket?id=%s&reg=%s", id, khex);
            n = mpnet_http_get(path, s_ticket, (int)sizeof s_ticket, &st);
            if (st == 200) mpnet_log("name \"%s\" registered on the server (first sign-in)", id);
        }
        if (st == 200 && n == 64 && !memcmp(s_ticket, "MPN1", 4)) {
            s_acct = 0;
            for (int i = 0; i < 8; i++) s_acct = (s_acct << 8) | s_ticket[8 + i];
            s_ticket_len = 64;
            s_signed = 1;
            mpnet_log("ticket for \"%s\": 64 bytes signed by the server, account %llu", id, (unsigned long long)s_acct);
            goto have;
        }
        if (st == 403) {
            mpnet_log("the server refused the name \"%s\" (taken by another player or banned): no ticket", id);
            memset(s_ticket, 0, sizeof s_ticket);
            s_acct = 0;
            s_ticket_len = 64;   /* an empty ticket: the server answers the sign-in with "failed" */
            goto have;
        }
        memset(s_ticket, 0, sizeof s_ticket);
        memcpy(s_ticket, "MPN1", 4);
        s_ticket[7] = 1;
        s_acct = fnv1a64(id) & 0x7FFFFFFFFFFFFFFFULL;
        if (!s_acct) s_acct = 1;
        for (int i = 0; i < 8; i++) s_ticket[8 + i] = (unsigned char)(s_acct >> (56 - 8 * i));
        snprintf((char*)s_ticket + 16, 32, "%s", id);
        s_ticket_len = 64;
        mpnet_log("ticket for \"%s\": %d bytes (made locally), account %llu", id, s_ticket_len,
                  (unsigned long long)s_acct);
    }
have:
    if (data) *data = s_ticket;
    if (acct) *acct = s_acct;
    return s_ticket_len;
}

/* ---- leaderboards (2026-11-14) ------------------------------------------------------------------------------
 * The game has no leaderboard screen and never calls sceNpScore (host\mpnet\NOTES.md 2026-11-14). Its multiplayer
 * stats are in the TUS profile it saves (data slot 1, JSON "profile.scores": [{"name": .., "score": ..}, ..]).
 * After such a save, a few of those counters go to our server (/api/report, challenge sign-in like /hello), on a
 * thread of their own so the game never waits for the network. Only with a server-signed ticket (signed-in account);
 * at most one report per 35 s (the newest one waits); the same numbers twice are not sent again. The game's data
 * and behaviour are unchanged: this only reads the bytes the game already wrote. PS3_MPNET_LB=0 turns it off. */
static const char* const k_lb_stats[] = { "XP", "Favor", "PlayerKill", "Death", "DeathByPlayer", "Assist", "GameOver",
                                          "TeamVictory", "FFA1stPlace", "WinsTFR", "WinsCTF", "WinsHun", "AIKill",
                                          "Waves", "GoldWave", "FlagCapture", "KillStreak10" };

/* "XP:800,Favor:590,..." from the profile JSON (any whitespace; keys in any order inside each score object). */
static int lb_extract(const char* js, size_t len, char* out, size_t cap)
{
    const char* end = js + len;
    const char* p = NULL;
    for (const char* q = js; q + 8 <= end; q++) if (!memcmp(q, "\"scores\"", 8)) { p = q + 8; break; }
    if (!p) return 0;
    while (p < end && *p != '[') p++;
    size_t o = 0; out[0] = 0;
    int n = 0;
    while (p < end && *p != ']') {
        while (p < end && *p != '{' && *p != ']') p++;
        if (p >= end || *p == ']') break;
        const char* ob = p; int depth = 0;
        const char* oe = p;
        for (; oe < end; oe++) { if (*oe == '{') depth++; else if (*oe == '}' && --depth == 0) break; }
        if (oe >= end) break;
        char name[32] = ""; long long val = -1;
        for (const char* k = ob; k < oe; k++) {
            if (k + 6 <= oe && !memcmp(k, "\"name\"", 6)) {
                const char* v = k + 6; while (v < oe && (*v == ' ' || *v == ':' || *v == '\t' || *v == '\r' || *v == '\n')) v++;
                if (v < oe && *v == '"') { v++; size_t i = 0; while (v < oe && *v != '"' && i < sizeof name - 1) name[i++] = *v++; name[i] = 0; }
            } else if (k + 7 <= oe && !memcmp(k, "\"score\"", 7)) {
                const char* v = k + 7; while (v < oe && (*v == ' ' || *v == ':' || *v == '\t' || *v == '\r' || *v == '\n')) v++;
                if (v < oe && *v >= '0' && *v <= '9') { val = 0; while (v < oe && *v >= '0' && *v <= '9' && val < 100000000000LL) val = val * 10 + (*v++ - '0'); }
            }
        }
        p = oe + 1;
        if (!name[0] || val < 0) continue;
        for (size_t i = 0; i < sizeof k_lb_stats / sizeof k_lb_stats[0]; i++)
            if (!strcmp(name, k_lb_stats[i]) && o + strlen(name) + 24 < cap) {
                o += (size_t)snprintf(out + o, cap - o, "%s%s:%lld", n ? "," : "", name, val);
                n++;
                break;
            }
    }
    return n;
}

static char s_lb_pending[400], s_lb_sent[400];
static volatile long s_lb_busy;
static time_t s_lb_last;

static void lb_send(const char* stats)
{
    const char* id = np_psnr_online_id();
    char ch[200], path[700]; unsigned char body[128]; int st = 0;
    challenge_args(id, ch, sizeof ch);
    if (!ch[0]) { mpnet_log("leaderboard: no sign-in challenge from the server; stats not sent"); return; }
    snprintf(path, sizeof path, "/api/report?id=%s%s&s=%s", id, ch, stats);
    int n = mpnet_http_get(path, body, (int)sizeof body - 1, &st);
    if (n < 0) n = 0;
    body[n] = 0;
    while (n && (body[n - 1] == '\n' || body[n - 1] == '\r')) body[--n] = 0;
    mpnet_log("leaderboard: stats sent (%s) -> %d %s", stats, st, (char*)body);
}

#ifdef _WIN32
static SRWLOCK s_lb_lock = SRWLOCK_INIT;
static DWORD WINAPI lb_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        char now_stats[400];
        AcquireSRWLockExclusive(&s_lb_lock);
        snprintf(now_stats, sizeof now_stats, "%s", s_lb_pending);
        int same = !strcmp(now_stats, s_lb_sent);
        if (same) { InterlockedExchange(&s_lb_busy, 0); ReleaseSRWLockExclusive(&s_lb_lock); return 0; }
        ReleaseSRWLockExclusive(&s_lb_lock);
        time_t t = time(NULL);
        if (s_lb_last && t - s_lb_last < 35) { Sleep((DWORD)(35 - (t - s_lb_last)) * 1000); continue; }
        s_lb_last = time(NULL);
        lb_send(now_stats);
        AcquireSRWLockExclusive(&s_lb_lock);
        snprintf(s_lb_sent, sizeof s_lb_sent, "%s", now_stats);
        ReleaseSRWLockExclusive(&s_lb_lock);
    }
}
#endif

void mpnet_tus_saved(const char* comm, int slot, const void* data, size_t len)
{
    if (!mpnet_on() || slot != 1 || !comm || strncmp(comm, "NPWR00802", 9) || !data || !len) return;
    { const char* e = getenv("PS3_MPNET_LB"); if (e && e[0] == '0') return; }
    if (!s_signed) { mpnet_log("leaderboard: not signed in with a server-signed ticket; stats not sent"); return; }
    char stats[400];
    if (!lb_extract((const char*)data, len, stats, sizeof stats)) {
        mpnet_log("leaderboard: no known stats in the saved profile (%u bytes)", (unsigned)len);
        return;
    }
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_lb_lock);
    snprintf(s_lb_pending, sizeof s_lb_pending, "%s", stats);
    int start = !strcmp(stats, s_lb_sent) ? 0 : InterlockedExchange(&s_lb_busy, 1) == 0;
    ReleaseSRWLockExclusive(&s_lb_lock);
    if (start) {
        HANDLE h = CreateThread(NULL, 0, lb_thread, NULL, 0, NULL);
        if (h) CloseHandle(h); else InterlockedExchange(&s_lb_busy, 0);
    }
#else
    if (strcmp(stats, s_lb_sent)) { lb_send(stats); snprintf(s_lb_sent, sizeof s_lb_sent, "%s", stats); }
#endif
}

/* ---- init (no guest memory pokes: the game's licence checks stay as shipped, see host\mpnet\LICENCE.md) ---- */
void mpnet_init(void)
{
    static int done;
    if (done || !mpnet_on()) return;
    done = 1;
    mpnet_log("on: server %s, http %d, online id \"%s\", p2p port %u", mpnet_server_host(), http_port(),
              np_psnr_online_id(), (unsigned)np_psnr_p2p_port());
    /* tell the server where this player's P2P socket is, for sceNpSignaling peer lookups (2026-11-13) */
    {   /* self-check of the sign-in maths against the server's Python (test_sam.py KNOWN_K / KNOWN_PROOF) */
        uint8_t k[32], pr[32]; char khex[65], phex[65];
        const char* t = "00112233445566778899aabbccddeeff"; const char* m = "gowa-acct:mptesta";
        const char* m2 = "0123456789abcdef0123456789abcdef:mptesta";
        mpnet_hmac_sha256(t, strlen(t), m, strlen(m), k); mpnet_hex(k, 32, khex);
        mpnet_hmac_sha256(k, 32, m2, strlen(m2), pr); mpnet_hex(pr, 32, phex);
        int ok = !strcmp(khex, "8505da6637238b6e5966d0fc1429e1e2857cd12cc730126419491e65c0701a8c") &&
                 !strcmp(phex, "428717eb20fef482f3412b5584e06437830bc3553048ee4d7347d59be0e1a17f");
        mpnet_log("sign-in self-check: %s", ok ? "ok" : "MISMATCH (HMAC-SHA256 differs from the server)");
    }
    /* sign in first (registers a new name), then tell the server where this player's P2P socket is, for
     * sceNpSignaling peer lookups; /hello proves the name with its own fresh challenge */
    mpnet_ticket(NULL, NULL);
    char path[460], ch[200], party[40] = ""; unsigned char body[256]; int st = 0;
    challenge_args(np_psnr_online_id(), ch, sizeof ch);
    /* private match (2026-11-13): PS3_MPNET_PARTY=<code> (launcher / Discord invite) -> the server matches this
     * player only with players who sent the same code. Only A-Z and 0-9 are passed on. */
    { const char* e = getenv("PS3_MPNET_PARTY"); size_t n = 0;
      if (e) for (; *e && n < 12; e++) { char c = *e; if (c >= 'a' && c <= 'z') c -= 32;
                                         if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) { if (!n) { strcpy(party, "&party="); n = 0; } party[7 + n++] = c; party[7 + n] = 0; } }
      if (n) mpnet_log("private party code %s", party + 7); }
    char lan[40] = "";
    if (s_local_ip) {
        const uint8_t* b = (const uint8_t*)&s_local_ip;
        snprintf(lan, sizeof lan, "&lan=%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    }
    snprintf(path, sizeof path, "/hello?id=%s&port=%u%s%s%s", np_psnr_online_id(), (unsigned)np_psnr_p2p_port(), ch, party, lan);
    int n = mpnet_http_get(path, body, (int)sizeof body - 1, &st);
    /* internet play (2026-11-13): "ok <token> <udp port>" -> the game socket sends the token to that UDP port
     * (mpnet_udp_probe), so the server sees this PC's outside address + port behind a home router. */
    if (n > 0 && st == 200) {
        body[n] = 0;
        char tok[40] = ""; unsigned up = 0;
        if (sscanf((const char*)body, "ok %39s %u", tok, &up) == 2 && up > 0 && up < 65536) {
            snprintf(s_udp_token, sizeof s_udp_token, "%s", tok);
            s_udp_port = up;
            mpnet_log("address probe: udp port %u", up);
        }
    }
}

/* The server's answer to /peer?id=<online id>: "<a.b.c.d> <port>". 0 on success, ip in host order. */
/* Internet play (2026-11-13): from the game's own P2P socket, send "MPN1UDP <token>" to the server's UDP probe port
 * (at most every 15 s; the sysNet P2P bind and sendto call this). The server records where it came from: the
 * router's outside address + port for this socket, which it then gives to the other players (/peer). It never
 * answers, so the game never sees anything from it. */
void mpnet_udp_probe(uintptr_t host_fd)
{
    static time_t last;
    time_t now = time(NULL);
    if (!mpnet_on() || !s_udp_token[0] || !s_udp_port || (last && now - last < 15)) return;
    uint32_t ip = server_ip();
    if (!ip) return;
    last = now;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)s_udp_port); a.sin_addr.s_addr = ip;
    char msg[64];
    int n = snprintf(msg, sizeof msg, "MPN1UDP %s", s_udp_token);
    sendto((mp_sock)host_fd, msg, n, 0, (struct sockaddr*)&a, sizeof a);
}

/* ---- relay (2026-11-13): for players whose routers block a direct path. A peer the game keeps sending to without
 * hearing back for 3 s (the game itself gives up after 10 s) is switched to the server's relay: its packets go to the
 * server's UDP port as "MPR1" + token + peer name + payload; the server forwards them (same match only) as "MPR2" +
 * sender name + payload, which arrive here looking like they came from that peer's own address. PS3_MPNET_RELAY=1
 * relays every peer from the start (tests). */
typedef struct { char name[17]; uint32_t ip_be; uint16_t port; int relay; uint64_t first_send, last_recv; } mp_rpeer;
static mp_rpeer s_rpeers[16];
static int s_nrpeers;

static uint64_t mp_now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timeval tv; gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
#endif
}

static int relay_forced(void)
{
    const char* e = getenv("PS3_MPNET_RELAY");
    return e && e[0] == '1';
}

static void rpeer_note(const char* name, uint32_t ip_be, uint16_t port)
{
    mp_rpeer* p = NULL;
    for (int i = 0; i < s_nrpeers; i++) if (!strcmp(s_rpeers[i].name, name)) p = &s_rpeers[i];
    if (!p) {
        if (s_nrpeers >= (int)(sizeof s_rpeers / sizeof s_rpeers[0])) return;
        p = &s_rpeers[s_nrpeers++];
        memset(p, 0, sizeof *p);
        snprintf(p->name, sizeof p->name, "%s", name);
    }
    p->ip_be = ip_be; p->port = port; p->first_send = 0; p->last_recv = 0;
    p->relay = relay_forced();
    if (p->relay) mpnet_log("relay: %s through the server from the start (PS3_MPNET_RELAY)", name);
}

int mpnet_p2p_wrap(const void* dst, const void* buf, int len, void* out, int cap, void* via)
{
    const struct sockaddr_in* d = (const struct sockaddr_in*)dst;
    if (!mpnet_on() || !s_udp_token[0] || !s_udp_port || len <= 0 || len > 1400 || cap < len + 52) return 0;
    mp_rpeer* p = NULL;
    for (int i = 0; i < s_nrpeers; i++)
        if (s_rpeers[i].ip_be == d->sin_addr.s_addr && s_rpeers[i].port == ntohs(d->sin_port)) p = &s_rpeers[i];
    if (!p) return 0;
    uint64_t now = mp_now_ms();
    if (!p->first_send) p->first_send = now;
    if (!p->relay && now - p->first_send > 3000 && (!p->last_recv || now - p->last_recv > 3000)) {
        p->relay = 1;
        mpnet_log("relay: nothing from %s for 3 s, its packets now go through the server", p->name);
    }
    if (!p->relay) return 0;
    char* o = (char*)out;
    memcpy(o, "MPR1", 4);
    memcpy(o + 4, s_udp_token, 32);
    memset(o + 36, 0, 16);
    memcpy(o + 36, p->name, strlen(p->name));
    memcpy(o + 52, buf, (size_t)len);
    struct sockaddr_in* v = (struct sockaddr_in*)via;
    memset(v, 0, sizeof *v);
    v->sin_family = AF_INET; v->sin_port = htons((uint16_t)s_udp_port); v->sin_addr.s_addr = server_ip();
    return len + 52;
}

int mpnet_p2p_unwrap(const void* from, void* buf, int len, void* peer_from)
{
    const struct sockaddr_in* f = (const struct sockaddr_in*)from;
    if (!mpnet_on() || !s_nrpeers) return -1;
    uint64_t now = mp_now_ms();
    if (s_udp_port && f->sin_addr.s_addr == server_ip() && ntohs(f->sin_port) == s_udp_port) {
        char* b = (char*)buf;
        if (len < 20 || memcmp(b, "MPR2", 4)) return -2;          /* the server sends nothing else: drop */
        char name[17]; memcpy(name, b + 4, 16); name[16] = 0;
        for (int i = 0; i < s_nrpeers; i++) {
            if (strcmp(s_rpeers[i].name, name)) continue;
            /* MEASURED n22: a relayed packet must not count as "the direct path works" (last_recv is direct only),
             * and a peer that relays to us cannot hear us directly either: relay back at once, so both sides switch
             * together well before the game's 10 s timeout. */
            if (!s_rpeers[i].relay) {
                s_rpeers[i].relay = 1;
                mpnet_log("relay: %s reaches us through the server, so we answer the same way", name);
            }
            struct sockaddr_in* pf = (struct sockaddr_in*)peer_from;
            memset(pf, 0, sizeof *pf);
            pf->sin_family = AF_INET; pf->sin_addr.s_addr = s_rpeers[i].ip_be; pf->sin_port = htons(s_rpeers[i].port);
            memmove(b, b + 20, (size_t)(len - 20));
            return len - 20;
        }
        return -2;                                                  /* a sender we were not matched with */
    }
    for (int i = 0; i < s_nrpeers; i++)
        if (s_rpeers[i].ip_be == f->sin_addr.s_addr && s_rpeers[i].port == ntohs(f->sin_port)) {
            /* test only: PS3_MPNET_TEST_BLOCK_DIRECT=1 drops direct packets from peers, like a router that blocks
             * them, so the automatic switch to the relay can be tested on one PC */
            const char* tb = getenv("PS3_MPNET_TEST_BLOCK_DIRECT");
            if (tb && tb[0] == '1') return -2;
            s_rpeers[i].last_recv = now;
        }
    return -1;
}

int mpnet_peer_lookup(const char* online_id, uint32_t* ip, uint16_t* port)
{
    char path[160]; char body[256]; int st = 0;
    snprintf(path, sizeof path, "/peer?id=%s", online_id);
    int n = mpnet_http_get(path, (unsigned char*)body, (int)sizeof body - 1, &st);
    if (n <= 0 || st != 200) return -1;
    body[n] = 0;
    unsigned a, b, c, d, p;
    if (sscanf(body, "%u.%u.%u.%u %u", &a, &b, &c, &d, &p) != 5 || a > 255 || b > 255 || c > 255 || d > 255 || p > 65535)
        return -1;
    uint32_t v = (a << 24) | (b << 16) | (c << 8) | d;
    uint32_t be = htonl(v);
    if (!mpnet_ip_allowed(be)) {
        /* A peer on another PC, named by our signed-in server. Internet play (2026-11-13): an outside address is fine
         * too (the server saw that player's own game socket there); never loopback-like, multicast or broadcast
         * addresses from the server, and PS3_MPNET_LAN_ONLY=1 keeps the old home-network-only rule. */
        const uint8_t* q = (const uint8_t*)&be;
        int lan_only = getenv("PS3_MPNET_LAN_ONLY") && getenv("PS3_MPNET_LAN_ONLY")[0] == '1';
        if (q[0] == 0 || q[0] >= 224 || (lan_only && !ip_private(be))) {
            mpnet_log("peer %s at %s refused", online_id, body); return -1;
        }
        if (s_npeers < (int)(sizeof s_peers / sizeof s_peers[0])) s_peers[s_npeers++] = be;
        mpnet_log("peer %s at %u.%u.%u.%u:%u allowed (named by the server)", online_id, a, b, c, d, p);
    }
    *ip = v; *port = (uint16_t)p;
    rpeer_note(online_id, be, (uint16_t)p);
    return 0;
}
