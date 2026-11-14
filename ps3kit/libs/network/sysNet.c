/*
 * ps3recomp - sys_net module (offline only)
 *
 * Public source release of the God of War: Ascension port: online play was
 * removed, so this file no longer bridges the PS3 BSD socket API (libnet) to
 * host sockets. Every call answers the way a console with no network does:
 * initialisation succeeds, a socket cannot be created, and every other socket
 * call fails with EWOULDBLOCK (35). No host socket is ever opened.
 *
 * The game's own libnet imports are answered by the offline model in
 * runtime/ppu/ppu_sysprx.cpp (no data, no sockets); these functions only keep
 * the module's API (sysNet.h) for code that links against it.
 */

#include "sysNet.h"
#include <stdio.h>

#define SYS_NET_OFFLINE_EWOULDBLOCK 35

int32_t sys_net_initialize_network_ex(void* param) { (void)param; return 0; }
int32_t sys_net_finalize_network(void) { return 0; }

int32_t sys_net_bnet_socket(int32_t domain, int32_t type, int32_t protocol)
{
    (void)domain; (void)type; (void)protocol;
    return -1;
}
int32_t sys_net_bnet_bind(int32_t s, const sys_net_sockaddr* addr, uint32_t addrlen) { (void)s; (void)addr; (void)addrlen; return -1; }
int32_t sys_net_bnet_listen(int32_t s, int32_t backlog) { (void)s; (void)backlog; return -1; }
int32_t sys_net_bnet_accept(int32_t s, sys_net_sockaddr* addr, uint32_t* addrlen) { (void)s; (void)addr; (void)addrlen; return -1; }
int32_t sys_net_bnet_connect(int32_t s, const sys_net_sockaddr* addr, uint32_t addrlen) { (void)s; (void)addr; (void)addrlen; return -1; }
int32_t sys_net_bnet_send(int32_t s, const void* buf, uint32_t len, int32_t flags) { (void)s; (void)buf; (void)len; (void)flags; return -1; }
int32_t sys_net_bnet_sendto(int32_t s, const void* buf, uint32_t len, int32_t flags,
                            const sys_net_sockaddr* addr, uint32_t addrlen)
{
    (void)s; (void)buf; (void)len; (void)flags; (void)addr; (void)addrlen;
    return -1;
}
int32_t sys_net_bnet_recv(int32_t s, void* buf, uint32_t len, int32_t flags) { (void)s; (void)buf; (void)len; (void)flags; return -1; }
int32_t sys_net_bnet_recvfrom(int32_t s, void* buf, uint32_t len, int32_t flags,
                              sys_net_sockaddr* addr, uint32_t* addrlen)
{
    (void)s; (void)buf; (void)len; (void)flags; (void)addr; (void)addrlen;
    return -1;
}
int32_t sys_net_bnet_shutdown(int32_t s, int32_t how) { (void)s; (void)how; return -1; }
int32_t sys_net_bnet_close(int32_t s) { (void)s; return -1; }
int32_t sys_net_bnet_setsockopt(int32_t s, int32_t level, int32_t optname, const void* optval, uint32_t optlen)
{
    (void)s; (void)level; (void)optname; (void)optval; (void)optlen;
    return -1;
}
int32_t sys_net_bnet_getsockopt(int32_t s, int32_t level, int32_t optname, void* optval, uint32_t* optlen)
{
    (void)s; (void)level; (void)optname; (void)optval; (void)optlen;
    return -1;
}
int32_t sys_net_bnet_getsockname(int32_t s, sys_net_sockaddr* addr, uint32_t* addrlen) { (void)s; (void)addr; (void)addrlen; return -1; }
int32_t sys_net_bnet_getpeername(int32_t s, sys_net_sockaddr* addr, uint32_t* addrlen) { (void)s; (void)addr; (void)addrlen; return -1; }
int32_t sys_net_bnet_poll(sys_net_pollfd* fds, uint32_t nfds, int32_t timeout_ms) { (void)fds; (void)nfds; (void)timeout_ms; return 0; }
int32_t sys_net_bnet_select(int32_t nfds, void* readfds, void* writefds, void* exceptfds, void* timeout)
{
    (void)nfds; (void)readfds; (void)writefds; (void)exceptfds; (void)timeout;
    return 0;
}
int32_t  sys_net_bnet_inet_pton(int32_t family, const char* src, void* dst) { (void)family; (void)src; (void)dst; return 0; }
int32_t  sys_net_bnet_inet_aton(const char* cp, uint32_t* inp) { (void)cp; (void)inp; return 0; }
uint32_t sys_net_bnet_inet_addr(const char* cp) { (void)cp; return 0xFFFFFFFFu; }
uint32_t sys_net_bnet_inet_ntoa(uint32_t addr) { (void)addr; return 0; }
uint32_t sys_net_bnet_gethostbyname(const char* name) { (void)name; return 0; }
uint32_t sys_net_errno_loc(void) { return 0; }

void ps3_net_host_register(unsigned int (*guest_alloc)(unsigned int size, unsigned int align))
{
    (void)guest_alloc;   /* offline only: no host sockets */
}
