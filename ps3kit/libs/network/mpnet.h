/*
 * mpnet (private kit for the GoW Ascension community-server prototype; design: ascension\host\mpnet\DESIGN.md)
 *
 *   PS3_MPNET=1                 online mode against OUR server (never Sony): NP status ONLINE, ticket, TSS,
 *                               EULA answered here; the game's sockets may reach only loopback + the server.
 *   PS3_MPNET_SERVER=host       the community server (default 127.0.0.1)
 *   PS3_MPNET_HTTP_PORT=n       its HTTP port (default 36101: /tss, /ticket)
 *   PS3_MPNET_SSL=1             keep the SAM client's SSL wrapper (default: off, source patch 1)
 *   The Online Pass licence check is never touched by this kit (host\mpnet\LICENCE.md).
 *
 * Functions here are named mpnet_* so gen_hle_nids.py does not register them as PRX exports.
 */
#ifndef PS3_MPNET_H
#define PS3_MPNET_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

int  mpnet_on(void);
int  mpnet_flag(int which);                 /* called from the private lifted sources */
void mpnet_init(void);                      /* once, from sceNpInit / cellNetCtlInit */
const char* mpnet_server_host(void);
/* HTTP GET from the server; returns body length (<= cap) or -1; *status = HTTP status. */
int  mpnet_http_get(const char* path, unsigned char* out, int cap, int* status);
/* 1 if the game may talk to this IPv4 address (network order) / resolve this name in mpnet mode. */
int  mpnet_ip_allowed(uint32_t ip_be);
int  mpnet_name_allowed(const char* name);
/* The ticket our server issued (fetched once); returns its size, 0 if none. */
int  mpnet_ticket(const unsigned char** data, uint64_t* acct);
void mpnet_log(const char* fmt, ...);
/* Peer endpoint from the server (/peer?id=); 0 = found (ip in host order). */
int  mpnet_peer_lookup(const char* online_id, uint32_t* ip, uint16_t* port);
void mpnet_udp_probe(uintptr_t host_fd);   /* internet play: tell the server this game socket's outside address */
/* relay (2026-11-13): wrap returns the wrapped length (> 0) and the server address in *via when this peer goes through
 * the relay, else 0; unwrap returns the payload length and the peer's address in *peer_from for a relayed packet,
 * -1 for any other packet, -2 for one to drop. Addresses are struct sockaddr_in. */
int  mpnet_p2p_wrap(const void* dst, const void* buf, int len, void* out, int cap, void* via);
int  mpnet_p2p_unwrap(const void* from, void* buf, int len, void* peer_from);
int  mpnet_server_is_loopback(void);
void mpnet_hmac_sha256(const void* key, size_t klen, const void* msg, size_t mlen, uint8_t out[32]);
void mpnet_hex(const uint8_t* b, size_t n, char* out);
/* leaderboards (2026-11-14): the game saved TUS data `slot` of title `comm` (bytes as written); sends the profile's
 * stats to the server in the background when signed in. */
void mpnet_tus_saved(const char* comm, int slot, const void* data, size_t len);
const char* mpnet_token(void);         /* 1 while the server is 127.x (or unknown) */

#ifdef __cplusplus
}
#endif
#endif
