/*
 * ps3recomp - NP identity, offline only (np_psnr.h)
 *
 * Public source release of the God of War: Ascension port: online play was
 * removed. The psnr client (a connection to a matchmaking / leaderboard
 * server) is gone; this file keeps the np_psnr.h API so the NP modules link,
 * and every call answers "offline": nothing connects, no request is sent, no
 * packet is routed. Only the local player name stays (--username or
 * PS3_NP_ONLINE_ID), because the console identity (OpenPSID, MAC) derives from
 * it and offline saves must keep working exactly as before.
 */
#include "np_psnr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* The fake username from sceNp.c. */
extern const char* np_fake_username(void);

/* PSN online IDs: 3-16 letters, digits, '-' and '_', starting with a letter.
 * Copies the allowed characters of `in`; 1 if the result is a valid ID. */
static int clean_online_id(const char* in, char out[17])
{
    int n = 0;
    for (; in && *in && n < 16; in++)
        if (isalnum((unsigned char)*in) || *in == '-' || *in == '_')
            out[n++] = *in;
    out[n] = 0;
    return n >= 3 && isalpha((unsigned char)out[0]);
}

static char s_id[17];
static int  s_id_state;     /* 0 unresolved, 1 a name applies, 2 none */

void np_psnr_setup(const char* username, const char* server)
{
    (void)server;   /* offline only: no server */
    const char* named[2] = { username, getenv("PS3_NP_ONLINE_ID") };
    int found = 0;
    for (int i = 0; i < 2 && !found; i++) {
        if (!named[i] || !*named[i]) continue;
        found = clean_online_id(named[i], s_id);
        if (!found)
            printf("[np] \"%s\" is not a valid online ID (3-16 letters, digits, - or _, "
                   "starting with a letter)\n", named[i]);
    }
    if (!found) { s_id_state = 2; return; }   /* "PS3Player" */
    s_id_state = 1;
    printf("[np] player \"%s\" (offline)\n", s_id);
}

const char* np_psnr_identity(void)
{
    if (!s_id_state) np_psnr_setup(NULL, NULL);
    return s_id_state == 1 ? s_id : NULL;
}

int np_psnr_enabled(void) { return 0; }

const char* np_psnr_online_id(void)
{
    const char* id = np_psnr_identity();
    return id ? id : np_fake_username();
}

uint16_t np_psnr_p2p_port(void) { return 3658; }

int np_psnr_connected(void) { return 0; }
int np_psnr_connect(const char* comm_id) { (void)comm_id; return -1; }

uint32_t np_psnr_request(uint8_t type, const void* body, uint32_t len, np_psnr_reply_fn fn, void* user)
{
    (void)type; (void)body; (void)len; (void)fn; (void)user;
    return 0;   /* not sent */
}

void np_psnr_on_push(void (*fn)(const psnr_msg* push)) { (void)fn; }
void np_psnr_on_tick(void (*fn)(void)) { (void)fn; }
void np_psnr_pump(void) { }

void np_psnr_set_p2p_sender(np_psnr_p2p_send_fn fn) { (void)fn; }
int  np_psnr_p2p_filter(void* buf, uint32_t* len, uint8_t from_ip[4], uint16_t* from_port)
{
    (void)buf; (void)len; (void)from_ip; (void)from_port;
    return 0;
}
void np_psnr_punch(uint32_t user, const uint8_t ip[4], uint16_t port) { (void)user; (void)ip; (void)port; }

int np_psnr_p2p_route(const void* buf, uint32_t len, const uint8_t ip[4], uint16_t port)
{
    (void)buf; (void)len; (void)ip; (void)port;
    return 0;
}
int64_t np_psnr_stream_connect(const uint8_t ip[4], uint16_t port) { (void)ip; (void)port; return -1; }
void    np_psnr_set_stream_sink(np_psnr_stream_sink_fn fn) { (void)fn; }
